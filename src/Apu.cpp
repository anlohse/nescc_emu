#include "nes/Apu.h"

namespace nes {

namespace {

// Indexed by the top five bits of $4003/$4007/$400B/$400F. The values look
// arbitrary because they are: musically useful note lengths at the frame
// counter's rate, tabulated in silicon.
const std::uint8_t LENGTH_TABLE[32] = {
	 10, 254,  20,   2,  40,   4,  80,   6, 160,   8,  60,  10,  14,  12,  26,  14,
	 12,  16,  24,  18,  48,  20,  96,  22, 192,  24,  72,  26,  16,  28,  32,  30
};

// 12.5%, 25%, 50%, and 25% inverted. The fourth is the third's complement, not
// a distinct duty -- it sounds identical, only phase-shifted.
const std::uint8_t DUTY_TABLE[4][8] = {
	{ 0, 1, 0, 0, 0, 0, 0, 0 },
	{ 0, 1, 1, 0, 0, 0, 0, 0 },
	{ 0, 1, 1, 1, 1, 0, 0, 0 },
	{ 1, 0, 0, 1, 1, 1, 1, 1 }
};

// A triangle drawn in 16 steps up and 16 down. Four bits of resolution is why
// the NES triangle has its characteristic edge rather than a pure tone.
const std::uint8_t TRIANGLE_TABLE[32] = {
	15, 14, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,  3,  2,  1,  0,
	 0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15
};

// Noise periods, in APU cycles. Both tables aim at the same pitches, so the PAL
// values are not simply scaled -- they are what the designers picked to land on
// the same notes from a slower clock.
const std::uint16_t NOISE_PERIODS_NTSC[16] = {
	4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068
};
const std::uint16_t NOISE_PERIODS_PAL[16] = {
	4, 8, 14, 30, 60, 88, 118, 148, 188, 236, 354, 472, 708, 944, 1890, 3778
};

// DMC rates, in CPU cycles per output bit.
const std::uint16_t DMC_RATES_NTSC[16] = {
	428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54
};
const std::uint16_t DMC_RATES_PAL[16] = {
	398, 354, 316, 298, 276, 236, 210, 198, 176, 148, 132, 118, 98, 78, 66, 50
};

// The CPU is held off the bus while the DMC fetches a sample byte.
const int DMC_FETCH_STALL = 4;

} // namespace

/* ------------------------------------------------------------------------ */
/* Envelope                                                                  */
/* ------------------------------------------------------------------------ */

void Apu::Envelope::clock() {
	if (start) {
		start = false;
		decay = 15;
		divider = volume;
		return;
	}
	if (divider == 0) {
		divider = volume;
		if (decay > 0)
			decay--;
		else if (loop)
			decay = 15;   // looping is what makes a sustained note possible
	} else {
		divider--;
	}
}

/* ------------------------------------------------------------------------ */
/* Pulse                                                                     */
/* ------------------------------------------------------------------------ */

void Apu::Pulse::clockTimer() {
	if (timer == 0) {
		timer = timerPeriod;
		dutyStep = static_cast<std::uint8_t>((dutyStep + 1) & 7);
	} else {
		timer--;
	}
}

int Apu::Pulse::targetPeriod() const {
	const int change = timerPeriod >> sweepShift;
	if (!sweepNegate)
		return timerPeriod + change;
	// Pulse 1 negates with one's complement, so it subtracts one too many.
	// A real asymmetry between the two channels, and audible: a downward sweep
	// on pulse 1 lands slightly sharp of the same sweep on pulse 2.
	return timerPeriod - change - (onesComplement ? 1 : 0);
}

bool Apu::Pulse::muted() const {
	// Below 8 the frequency is past audible; above $7FF the counter would wrap.
	// Hardware silences both cases outright rather than producing something.
	return timerPeriod < 8 || targetPeriod() > 0x7FF;
}

void Apu::Pulse::clockSweep() {
	if (sweepDivider == 0 && sweepEnabled && sweepShift > 0 && !muted()) {
		const int target = targetPeriod();
		if (target >= 0 && target <= 0x7FF)
			timerPeriod = static_cast<std::uint16_t>(target);
	}
	if (sweepDivider == 0 || sweepReload) {
		sweepDivider = sweepPeriod;
		sweepReload = false;
	} else {
		sweepDivider--;
	}
}

std::uint8_t Apu::Pulse::output() const {
	if (!enabled || lengthCounter == 0 || muted())
		return 0;
	if (DUTY_TABLE[duty][dutyStep] == 0)
		return 0;
	return envelope.output();
}

/* ------------------------------------------------------------------------ */
/* Triangle                                                                  */
/* ------------------------------------------------------------------------ */

void Apu::Triangle::clockTimer() {
	if (timer == 0) {
		timer = timerPeriod;
		// The sequence only advances while both counters are alive. That is why
		// the triangle stops mid-waveform rather than snapping to silence, and
		// why it clicks far less than the other channels.
		if (lengthCounter > 0 && linearCounter > 0)
			step = static_cast<std::uint8_t>((step + 1) & 31);
	} else {
		timer--;
	}
}

void Apu::Triangle::clockLinear() {
	if (linearReloadFlag)
		linearCounter = linearReload;
	else if (linearCounter > 0)
		linearCounter--;
	if (!control)
		linearReloadFlag = false;
}

std::uint8_t Apu::Triangle::output() const {
	if (!enabled)
		return 0;
	// Periods below two put the fundamental above 55 kHz. Hardware really does
	// emit it; reproducing that only folds ultrasonic energy back down as
	// aliasing noise, and games park the timer here specifically to shut the
	// channel up.
	if (timerPeriod < 2)
		return 0;
	return TRIANGLE_TABLE[step];
}

/* ------------------------------------------------------------------------ */
/* Noise                                                                     */
/* ------------------------------------------------------------------------ */

void Apu::Noise::clockTimer() {
	if (timer > 0) {
		timer--;
		return;
	}
	timer = timerPeriod;
	// A 15-bit LFSR. Tapping bit 1 gives a long pseudo-random sequence that
	// sounds like white noise; tapping bit 6 gives a 93-step loop that sounds
	// metallic and pitched, which games use for snares and lasers.
	const std::uint16_t tap = mode ? 6 : 1;
	const std::uint16_t feedback =
			static_cast<std::uint16_t>((shift & 1) ^ ((shift >> tap) & 1));
	shift = static_cast<std::uint16_t>((shift >> 1) | (feedback << 14));
}

std::uint8_t Apu::Noise::output() const {
	// Bit 0 set means silence -- inverted from what you would expect.
	if (!enabled || lengthCounter == 0 || (shift & 1))
		return 0;
	return envelope.output();
}

/* ------------------------------------------------------------------------ */
/* Apu                                                                       */
/* ------------------------------------------------------------------------ */

void Apu::setRegion(Region region) {
	m_region = region;
	if (region == Region::Pal) {
		m_noisePeriods = NOISE_PERIODS_PAL;
		m_dmcRates = DMC_RATES_PAL;
		m_step1 = 8313;  m_step2 = 16627; m_step3 = 24939;
		m_step4Irq = 33252; m_step4Clock = 33253; m_step4Wrap = 33254;
		m_step5Clock = 41565; m_step5Wrap = 41566;
	} else {
		m_noisePeriods = NOISE_PERIODS_NTSC;
		m_dmcRates = DMC_RATES_NTSC;
		m_step1 = 7457;  m_step2 = 14913; m_step3 = 22371;
		m_step4Irq = 29828; m_step4Clock = 29829; m_step4Wrap = 29830;
		m_step5Clock = 37281; m_step5Wrap = 37282;
	}
}

Apu::Apu() : m_bus(nullptr), m_pulse1(), m_pulse2(), m_triangle(), m_noise(),
		m_dmc(), m_frameCounter(0), m_frameMode(4), m_frameIrqInhibit(false),
		m_frameIrq(false), m_dmcIrq(false), m_frameResetDelay(0),
		m_evenCycle(false), m_dmcStall(0),
		m_hpPrevIn(0.0f), m_hpPrevOut(0.0f), m_lpPrev(0.0f),
		m_generateSamples(false), m_samples() {
	m_pulse1.onesComplement = true;
	setRegion(Region::Ntsc);
	reset();
}

void Apu::reset() {
	Bus* bus = m_bus;
	const bool generate = m_generateSamples;

	m_pulse1 = Pulse();
	m_pulse2 = Pulse();
	m_triangle = Triangle();
	m_noise = Noise();
	m_dmc = Dmc();
	m_pulse1.onesComplement = true;

	m_bus = bus;
	m_generateSamples = generate;
	m_frameCounter = 0;
	m_frameMode = 4;
	m_frameIrqInhibit = false;
	m_frameIrq = false;
	m_dmcIrq = false;
	m_frameResetDelay = 0;
	m_evenCycle = false;
	m_dmcStall = 0;
	m_hpPrevIn = 0.0f;
	m_hpPrevOut = 0.0f;
	m_lpPrev = 0.0f;
	m_samples.clear();
}

void Apu::setSampleOutput(bool enabled) {
	if (enabled && !m_generateSamples) {
		// The filters have been standing still while output was off, so their
		// state describes whatever was playing back then. Emitting it now would
		// be an audible click on the first sample.
		m_hpPrevIn = 0.0f;
		m_hpPrevOut = 0.0f;
		m_lpPrev = 0.0f;
	}
	m_generateSamples = enabled;
}

int Apu::takeDmcStall() {
	const int stall = m_dmcStall;
	m_dmcStall = 0;
	return stall;
}

void Apu::tick(int cycles) {
	for (int i = 0; i < cycles; i++) {
		m_evenCycle = !m_evenCycle;

		// The pulses and the noise run off a divide-by-two of the CPU clock.
		// The triangle does not, which is the whole reason it reaches an octave
		// lower than the pulses can.
		if (m_evenCycle) {
			m_pulse1.clockTimer();
			m_pulse2.clockTimer();
			m_noise.clockTimer();
		}
		m_triangle.clockTimer();
		clockDmcTimer();

		if (m_frameResetDelay > 0 && --m_frameResetDelay == 0) {
			m_frameCounter = 0;
			// Switching to the five-step sequence clocks everything once
			// immediately. Games use that to force an envelope to restart in
			// step with their own timing.
			if (m_frameMode == 5) {
				clockQuarterFrame();
				clockHalfFrame();
			}
		} else {
			clockFrameCounter();
		}

		if (m_generateSamples)
			m_samples.push_back(mix());
	}
}

void Apu::clockFrameCounter() {
	m_frameCounter++;

	// Comparisons rather than a switch: the boundaries are runtime values now,
	// because PAL clocks the same 240 Hz sequence from a slower CPU.
	if (m_frameCounter == m_step1) {
		clockQuarterFrame();
		return;
	}
	if (m_frameCounter == m_step2) {
		clockQuarterFrame();
		clockHalfFrame();
		return;
	}
	if (m_frameCounter == m_step3) {
		clockQuarterFrame();
		return;
	}

	if (m_frameMode == 4) {
		// The interrupt is asserted across three consecutive cycles, which is
		// what lets a game catch it however its polling happens to line up.
		if (m_frameCounter == m_step4Irq || m_frameCounter == m_step4Clock
				|| m_frameCounter == m_step4Wrap) {
			if (!m_frameIrqInhibit)
				m_frameIrq = true;
		}
		if (m_frameCounter == m_step4Clock) {
			clockQuarterFrame();
			clockHalfFrame();
		} else if (m_frameCounter == m_step4Wrap) {
			m_frameCounter = 0;
		}
	} else {
		if (m_frameCounter == m_step5Clock) {
			clockQuarterFrame();
			clockHalfFrame();
		} else if (m_frameCounter == m_step5Wrap) {
			m_frameCounter = 0;
		}
	}
}

void Apu::clockQuarterFrame() {
	m_pulse1.envelope.clock();
	m_pulse2.envelope.clock();
	m_noise.envelope.clock();
	m_triangle.clockLinear();
}

void Apu::clockHalfFrame() {
	clockLengthCounters();
	m_pulse1.clockSweep();
	m_pulse2.clockSweep();
}

void Apu::clockLengthCounters() {
	// The halt flag shares a bit with the envelope's loop flag, and with the
	// triangle's linear-counter control. One bit, two jobs, on every channel.
	if (!m_pulse1.envelope.loop && m_pulse1.lengthCounter > 0)
		m_pulse1.lengthCounter--;
	if (!m_pulse2.envelope.loop && m_pulse2.lengthCounter > 0)
		m_pulse2.lengthCounter--;
	if (!m_triangle.control && m_triangle.lengthCounter > 0)
		m_triangle.lengthCounter--;
	if (!m_noise.envelope.loop && m_noise.lengthCounter > 0)
		m_noise.lengthCounter--;
}

void Apu::clockDmcTimer() {
	fillDmcBuffer();

	if (m_dmc.timer > 0) {
		m_dmc.timer--;
		return;
	}
	m_dmc.timer = m_dmc.timerPeriod;

	if (!m_dmc.silence) {
		// Delta modulation: each bit nudges the level up or down by two, and
		// the level is what you hear. It cannot move faster than that, which is
		// why DPCM samples are muffled.
		if (m_dmc.shift & 1) {
			if (m_dmc.output <= 125)
				m_dmc.output += 2;
		} else {
			if (m_dmc.output >= 2)
				m_dmc.output -= 2;
		}
	}
	m_dmc.shift = static_cast<std::uint8_t>(m_dmc.shift >> 1);

	if (m_dmc.bitsRemaining > 0)
		m_dmc.bitsRemaining--;
	if (m_dmc.bitsRemaining == 0) {
		m_dmc.bitsRemaining = 8;
		if (m_dmc.bufferFilled) {
			m_dmc.shift = m_dmc.buffer;
			m_dmc.bufferFilled = false;
			m_dmc.silence = false;
		} else {
			m_dmc.silence = true;
		}
	}
}

void Apu::fillDmcBuffer() {
	if (m_dmc.bufferFilled || m_dmc.bytesRemaining == 0)
		return;

	// Samples always live in $C000-$FFFF, so this can only ever hit cartridge
	// ROM -- no device with read side effects is reachable from here.
	m_dmc.buffer = m_bus ? m_bus->read(m_dmc.currentAddress) : 0;
	m_dmc.bufferFilled = true;
	m_dmcStall += DMC_FETCH_STALL;

	m_dmc.currentAddress = (m_dmc.currentAddress == 0xFFFF)
			? 0x8000
			: static_cast<std::uint16_t>(m_dmc.currentAddress + 1);

	if (--m_dmc.bytesRemaining == 0) {
		if (m_dmc.loop) {
			m_dmc.currentAddress = m_dmc.sampleAddress;
			m_dmc.bytesRemaining = m_dmc.sampleLength;
		} else if (m_dmc.irqEnabled) {
			m_dmcIrq = true;
		}
	}
}

float Apu::mix() {
	const int p1 = m_pulse1.output();
	const int p2 = m_pulse2.output();
	const int t = m_triangle.output();
	const int n = m_noise.output();
	const int d = m_dmc.output;

	// The hardware sums through a resistor ladder, so channels are not
	// independent: adding a second pulse at full volume does not double the
	// amplitude of the first. These are the standard approximations of the
	// resulting curve. A linear sum is noticeably wrong -- harsh, and too loud
	// once more than two channels are playing.
	float pulseOut = 0.0f;
	if (p1 + p2 > 0)
		pulseOut = 95.88f / (8128.0f / static_cast<float>(p1 + p2) + 100.0f);

	float tndOut = 0.0f;
	const float tnd = static_cast<float>(t) / 8227.0f
			+ static_cast<float>(n) / 12241.0f
			+ static_cast<float>(d) / 22638.0f;
	if (tnd > 0.0f)
		tndOut = 159.79f / (1.0f / tnd + 100.0f);

	const float raw = pulseOut + tndOut;

	// One-pole high-pass at about 90 Hz to strip the DC offset, then a one-pole
	// low-pass at about 14 kHz. Coefficients are for the CPU sample rate.
	const float hp = raw - m_hpPrevIn + 0.999684f * m_hpPrevOut;
	m_hpPrevIn = raw;
	m_hpPrevOut = hp;

	m_lpPrev += 0.047f * (hp - m_lpPrev);
	return m_lpPrev;
}

/* ------------------------------------------------------------------------ */
/* Registers                                                                 */
/* ------------------------------------------------------------------------ */

void Apu::writeRegister(std::uint16_t address, std::uint8_t value) {
	switch (address) {
	/* --- Pulse 1 --------------------------------------------------------- */
	case 0x4000:
		m_pulse1.duty = static_cast<std::uint8_t>((value >> 6) & 3);
		m_pulse1.envelope.loop = (value & 0x20) != 0;
		m_pulse1.envelope.constant = (value & 0x10) != 0;
		m_pulse1.envelope.volume = static_cast<std::uint8_t>(value & 0x0F);
		break;
	case 0x4001:
		m_pulse1.sweepEnabled = (value & 0x80) != 0;
		m_pulse1.sweepPeriod = static_cast<std::uint8_t>((value >> 4) & 7);
		m_pulse1.sweepNegate = (value & 0x08) != 0;
		m_pulse1.sweepShift = static_cast<std::uint8_t>(value & 7);
		m_pulse1.sweepReload = true;
		break;
	case 0x4002:
		m_pulse1.timerPeriod = static_cast<std::uint16_t>((m_pulse1.timerPeriod & 0x700) | value);
		break;
	case 0x4003:
		m_pulse1.timerPeriod =
				static_cast<std::uint16_t>((m_pulse1.timerPeriod & 0x0FF) | ((value & 7) << 8));
		if (m_pulse1.enabled)
			m_pulse1.lengthCounter = LENGTH_TABLE[(value >> 3) & 0x1F];
		m_pulse1.dutyStep = 0;
		m_pulse1.envelope.start = true;
		break;

	/* --- Pulse 2 --------------------------------------------------------- */
	case 0x4004:
		m_pulse2.duty = static_cast<std::uint8_t>((value >> 6) & 3);
		m_pulse2.envelope.loop = (value & 0x20) != 0;
		m_pulse2.envelope.constant = (value & 0x10) != 0;
		m_pulse2.envelope.volume = static_cast<std::uint8_t>(value & 0x0F);
		break;
	case 0x4005:
		m_pulse2.sweepEnabled = (value & 0x80) != 0;
		m_pulse2.sweepPeriod = static_cast<std::uint8_t>((value >> 4) & 7);
		m_pulse2.sweepNegate = (value & 0x08) != 0;
		m_pulse2.sweepShift = static_cast<std::uint8_t>(value & 7);
		m_pulse2.sweepReload = true;
		break;
	case 0x4006:
		m_pulse2.timerPeriod = static_cast<std::uint16_t>((m_pulse2.timerPeriod & 0x700) | value);
		break;
	case 0x4007:
		m_pulse2.timerPeriod =
				static_cast<std::uint16_t>((m_pulse2.timerPeriod & 0x0FF) | ((value & 7) << 8));
		if (m_pulse2.enabled)
			m_pulse2.lengthCounter = LENGTH_TABLE[(value >> 3) & 0x1F];
		m_pulse2.dutyStep = 0;
		m_pulse2.envelope.start = true;
		break;

	/* --- Triangle -------------------------------------------------------- */
	case 0x4008:
		m_triangle.control = (value & 0x80) != 0;
		m_triangle.linearReload = static_cast<std::uint8_t>(value & 0x7F);
		break;
	case 0x400A:
		m_triangle.timerPeriod =
				static_cast<std::uint16_t>((m_triangle.timerPeriod & 0x700) | value);
		break;
	case 0x400B:
		m_triangle.timerPeriod =
				static_cast<std::uint16_t>((m_triangle.timerPeriod & 0x0FF) | ((value & 7) << 8));
		if (m_triangle.enabled)
			m_triangle.lengthCounter = LENGTH_TABLE[(value >> 3) & 0x1F];
		m_triangle.linearReloadFlag = true;
		break;

	/* --- Noise ----------------------------------------------------------- */
	case 0x400C:
		m_noise.envelope.loop = (value & 0x20) != 0;
		m_noise.envelope.constant = (value & 0x10) != 0;
		m_noise.envelope.volume = static_cast<std::uint8_t>(value & 0x0F);
		break;
	case 0x400E:
		m_noise.mode = (value & 0x80) != 0;
		m_noise.timerPeriod = m_noisePeriods[value & 0x0F];
		break;
	case 0x400F:
		if (m_noise.enabled)
			m_noise.lengthCounter = LENGTH_TABLE[(value >> 3) & 0x1F];
		m_noise.envelope.start = true;
		break;

	/* --- DMC ------------------------------------------------------------- */
	case 0x4010:
		m_dmc.irqEnabled = (value & 0x80) != 0;
		m_dmc.loop = (value & 0x40) != 0;
		m_dmc.timerPeriod = m_dmcRates[value & 0x0F];
		// Clearing the enable also acknowledges any pending interrupt, which is
		// how a handler shuts one off.
		if (!m_dmc.irqEnabled)
			m_dmcIrq = false;
		break;
	case 0x4011:
		// Writing the level directly is how games play PCM through the DMC by
		// brute force, one CPU write at a time.
		m_dmc.output = static_cast<std::uint8_t>(value & 0x7F);
		break;
	case 0x4012:
		m_dmc.sampleAddress = static_cast<std::uint16_t>(0xC000 + (value * 64));
		break;
	case 0x4013:
		m_dmc.sampleLength = static_cast<std::uint16_t>((value * 16) + 1);
		break;

	/* --- Status / enables ------------------------------------------------ */
	case 0x4015:
		m_pulse1.enabled = (value & ENABLE_PULSE1) != 0;
		m_pulse2.enabled = (value & ENABLE_PULSE2) != 0;
		m_triangle.enabled = (value & ENABLE_TRIANGLE) != 0;
		m_noise.enabled = (value & ENABLE_NOISE) != 0;

		// Disabling a channel silences it now, by clearing its length counter.
		if (!m_pulse1.enabled) m_pulse1.lengthCounter = 0;
		if (!m_pulse2.enabled) m_pulse2.lengthCounter = 0;
		if (!m_triangle.enabled) m_triangle.lengthCounter = 0;
		if (!m_noise.enabled) m_noise.lengthCounter = 0;

		m_dmc.enabled = (value & ENABLE_DMC) != 0;
		if (!m_dmc.enabled) {
			m_dmc.bytesRemaining = 0;
		} else if (m_dmc.bytesRemaining == 0) {
			// Only restarts if it had finished; writing mid-sample does not
			// interrupt one that is still playing.
			m_dmc.currentAddress = m_dmc.sampleAddress;
			m_dmc.bytesRemaining = m_dmc.sampleLength;
		}
		m_dmcIrq = false;
		break;

	/* --- Frame counter --------------------------------------------------- */
	case 0x4017:
		m_frameMode = (value & 0x80) ? 5 : 4;
		m_frameIrqInhibit = (value & 0x40) != 0;
		if (m_frameIrqInhibit)
			m_frameIrq = false;
		// The reset lands three or four CPU cycles later depending on which
		// half of the APU cycle the write fell in. Games time writes here
		// against their own code, so the delay is not decorative.
		m_frameResetDelay = m_evenCycle ? 3 : 4;
		break;

	default:
		break;
	}
}

std::uint8_t Apu::readStatus() {
	const std::uint8_t status = peekStatus();
	// Reading acknowledges the frame interrupt. The DMC's is not cleared here --
	// that one needs a write to $4010 or $4015.
	m_frameIrq = false;
	return status;
}

std::uint8_t Apu::peekStatus() const {
	std::uint8_t status = 0;
	if (m_pulse1.lengthCounter > 0)   status |= ENABLE_PULSE1;
	if (m_pulse2.lengthCounter > 0)   status |= ENABLE_PULSE2;
	if (m_triangle.lengthCounter > 0) status |= ENABLE_TRIANGLE;
	if (m_noise.lengthCounter > 0)    status |= ENABLE_NOISE;
	if (m_dmc.bytesRemaining > 0)     status |= ENABLE_DMC;
	if (m_frameIrq)                   status |= STATUS_FRAME_IRQ;
	if (m_dmcIrq)                     status |= STATUS_DMC_IRQ;
	return status;
}

} // namespace nes
