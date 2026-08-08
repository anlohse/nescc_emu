#ifndef NES_APU_H
#define NES_APU_H

#include "Region.h"

#include <6502cc/emu_bus.h>

#include <cstdint>
#include <vector>

namespace nes {

/**
 * The 2A03's audio processing unit.
 *
 * Five channels mixed into one mono signal: two pulses, a triangle, a noise
 * generator and a delta-modulation channel that plays sampled audio straight
 * out of cartridge ROM.
 *
 * Everything is driven by a divider hanging off the CPU clock, so this is
 * ticked with CPU cycles and nothing else. Two clock rates matter:
 *
 *   - The channel timers run at half the CPU rate (the triangle at the full
 *     rate, which is why it can reach frequencies the pulses cannot).
 *   - The frame counter divides down to roughly 240 Hz and issues the
 *     "quarter frame" and "half frame" pulses that clock envelopes, sweeps and
 *     length counters. It has nothing to do with a video frame, despite the
 *     name, and in its four-step mode it also raises an IRQ.
 *
 * The mixer is deliberately nonlinear. The hardware sums its channels through
 * a resistor ladder, so a channel gets quieter as the others get louder; the
 * usual polynomial approximations of that curve are used here. A linear sum
 * sounds noticeably wrong -- brittle and too loud with everything playing.
 *
 * Output is accumulated into a buffer at one sample per CPU cycle and
 * decimated by the caller, which is the simplest thing that does not alias
 * audibly at NES frequencies.
 */
class Apu {
public:
	// $4015 channel-enable and status bits.
	static const std::uint8_t ENABLE_PULSE1   = 0x01;
	static const std::uint8_t ENABLE_PULSE2   = 0x02;
	static const std::uint8_t ENABLE_TRIANGLE = 0x04;
	static const std::uint8_t ENABLE_NOISE    = 0x08;
	static const std::uint8_t ENABLE_DMC      = 0x10;
	static const std::uint8_t STATUS_FRAME_IRQ = 0x40;
	static const std::uint8_t STATUS_DMC_IRQ   = 0x80;

	/** CPU cycles per second on an NTSC 2A03, and on its PAL counterpart. */
	static const int CPU_CLOCK_HZ = 1789773;
	static const int PAL_CPU_CLOCK_HZ = 1662607;

	static int cpuClockHz(Region region) {
		return region == Region::Pal ? PAL_CPU_CLOCK_HZ : CPU_CLOCK_HZ;
	}

	Apu();

	/** The DMC fetches its samples over the CPU bus, so it needs one. */
	void setBus(Bus* bus) { m_bus = bus; }

	/**
	 * Retune for a region. Changes the frame sequencer's step boundaries and
	 * the noise and DMC period tables, all of which are derived from the CPU
	 * clock and so differ between the two consoles.
	 */
	void setRegion(Region region);
	Region region() const { return m_region; }

	void reset();

	/** Advance by @p cycles CPU cycles, generating one sample per cycle. */
	void tick(int cycles);

	void writeRegister(std::uint16_t address, std::uint8_t value);
	/** $4015 only. Reading it acknowledges the frame IRQ. */
	std::uint8_t readStatus();
	/** Side-effect-free view of $4015, for debuggers. */
	std::uint8_t peekStatus() const;

	/** True while either the frame counter or the DMC is asserting IRQ. */
	bool irqAsserted() const { return m_frameIrq || m_dmcIrq; }

	/**
	 * CPU cycles the DMC stole for sample fetches since the last call.
	 *
	 * The DMC reads through the same bus as the CPU and halts it while it does,
	 * exactly like OAM DMA. Not charging for it makes DPCM-heavy games run fast.
	 */
	int takeDmcStall();

	/**
	 * Samples generated since the last call, one per CPU cycle, in [-1, 1].
	 *
	 * The caller decimates to its device rate and clears this. Left as raw
	 * CPU-rate output because resampling policy belongs to whoever owns the
	 * audio device.
	 */
	const std::vector<float>& samples() const { return m_samples; }
	void clearSamples() { m_samples.clear(); }

	/**
	 * Turn sample generation on or off -- off for headless runs and turbo.
	 *
	 * While off, the mixer and its filters do not run at all, which is most of
	 * the APU's cost. Turning it back on clears the filter state, because the
	 * filters have been frozen meanwhile and their stale contents would come
	 * out as a click.
	 */
	void setSampleOutput(bool enabled);

	/** Inspection only: the noise channel's 15-bit shift register. */
	std::uint16_t noiseShiftRegister() const { return m_noise.shift; }

private:
	/** Envelope generator, shared by the pulses and the noise channel. */
	struct Envelope {
		bool start = false;
		bool loop = false;         // doubles as the length counter's halt flag
		bool constant = false;
		std::uint8_t volume = 0;   // period when generating, level when constant
		std::uint8_t divider = 0;
		std::uint8_t decay = 0;

		void clock();
		std::uint8_t output() const { return constant ? volume : decay; }
	};

	struct Pulse {
		bool enabled = false;
		std::uint8_t duty = 0;
		std::uint8_t dutyStep = 0;
		std::uint16_t timerPeriod = 0;
		std::uint16_t timer = 0;
		std::uint8_t lengthCounter = 0;
		Envelope envelope;

		bool sweepEnabled = false;
		bool sweepNegate = false;
		bool sweepReload = false;
		std::uint8_t sweepPeriod = 0;
		std::uint8_t sweepDivider = 0;
		std::uint8_t sweepShift = 0;
		// Pulse 1 subtracts one extra when negating; pulse 2 does not. A real
		// difference between two otherwise identical channels.
		bool onesComplement = false;

		void clockTimer();
		void clockSweep();
		/** Signed: a negating sweep can drive the target below zero. */
		int targetPeriod() const;
		bool muted() const;
		std::uint8_t output() const;
	};

	struct Triangle {
		bool enabled = false;
		bool control = false;      // also the length counter's halt flag
		std::uint8_t linearReload = 0;
		std::uint8_t linearCounter = 0;
		bool linearReloadFlag = false;
		std::uint16_t timerPeriod = 0;
		std::uint16_t timer = 0;
		std::uint8_t lengthCounter = 0;
		std::uint8_t step = 0;

		void clockTimer();
		void clockLinear();
		std::uint8_t output() const;
	};

	struct Noise {
		bool enabled = false;
		bool mode = false;         // taps bit 6 instead of bit 1
		std::uint16_t timerPeriod = 0;
		std::uint16_t timer = 0;
		std::uint16_t shift = 1;   // 15-bit LFSR, never legitimately zero
		std::uint8_t lengthCounter = 0;
		Envelope envelope;

		void clockTimer();
		std::uint8_t output() const;
	};

	struct Dmc {
		bool enabled = false;
		bool irqEnabled = false;
		bool loop = false;
		std::uint16_t timerPeriod = 0;
		std::uint16_t timer = 0;
		std::uint8_t output = 0;   // 7 bits, and writable directly at $4011

		std::uint16_t sampleAddress = 0;
		std::uint16_t sampleLength = 0;
		std::uint16_t currentAddress = 0;
		std::uint16_t bytesRemaining = 0;

		std::uint8_t shift = 0;
		std::uint8_t bitsRemaining = 0;
		bool bufferFilled = false;
		std::uint8_t buffer = 0;
		// Set when the output unit runs dry. The level then holds rather than
		// snapping to zero, which is what stops DPCM from clicking between
		// samples.
		bool silence = true;
	};

	void clockQuarterFrame();
	void clockHalfFrame();
	void clockLengthCounters();
	void clockFrameCounter();
	void clockDmcTimer();
	void fillDmcBuffer();
	/** Not const: the output filters carry state from sample to sample. */
	float mix();

	Bus* m_bus;
	Region m_region;
	// Pointers into the static per-region tables, so a lookup costs nothing.
	const std::uint16_t* m_noisePeriods;
	const std::uint16_t* m_dmcRates;
	// Frame-sequencer boundaries, in CPU cycles. The first three are shared by
	// both sequences; the rest depend on which mode is selected. None of them
	// divide evenly, because a 240 Hz sequence off a 1.79 MHz clock cannot.
	std::uint32_t m_step1, m_step2, m_step3;
	std::uint32_t m_step4Irq, m_step4Clock, m_step4Wrap;
	std::uint32_t m_step5Clock, m_step5Wrap;

	Pulse m_pulse1;
	Pulse m_pulse2;
	Triangle m_triangle;
	Noise m_noise;
	Dmc m_dmc;

	// The frame counter divides the CPU clock; its step boundaries fall on
	// fractional CPU cycles, so they are counted in APU cycles (CPU / 2) with
	// the odd half-cycle tracked separately.
	std::uint32_t m_frameCounter;
	std::uint8_t m_frameMode;      // 4 or 5 steps
	bool m_frameIrqInhibit;
	bool m_frameIrq;
	bool m_dmcIrq;
	// A write to $4017 takes effect after a short delay; games rely on it to
	// line the sequence up with their own timing.
	int m_frameResetDelay;

	bool m_evenCycle;
	int m_dmcStall;

	// The console filters its own output, and both filters earn their place
	// here rather than in the front-end. The high-pass removes the DC offset
	// the mixer's positive-only output would otherwise carry -- audible as a
	// click when audio starts. The low-pass is an anti-aliasing filter: samples
	// come out at 1.79 MHz and the caller decimates roughly 40:1, so without it
	// everything above the device's Nyquist folds back down as noise.
	float m_hpPrevIn;
	float m_hpPrevOut;
	float m_lpPrev;

	bool m_generateSamples;
	std::vector<float> m_samples;
};

} // namespace nes

#endif // NES_APU_H
