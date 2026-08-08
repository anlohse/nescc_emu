/*
 * testApu.cpp -- channels, counters, the frame sequencer and its IRQ.
 *
 * Sound is hard to assert on, so these test the machinery underneath it: the
 * counters that decide when a note stops, the sequencer that clocks them, and
 * the status register a game polls to find out.
 */

#include "TestRom.h"
#include "nes/Nes.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace nes;

namespace {

// Frame counter boundaries, in CPU cycles. A half-frame lands on step 2.
const int HALF_FRAME = 14913;
const int FULL_SEQUENCE = 29830;

/** An APU with sample generation on, so mix() actually runs. */
Apu makeApu() {
	Apu apu;
	apu.setSampleOutput(true);
	return apu;
}

/**
 * Peak absolute sample over a run -- "is this channel making any sound".
 *
 * Settles first, because the DC-blocking high-pass carries state: a channel
 * switched off a moment ago still has a decaying tail worth several thousand
 * samples, and measuring straight away reads that tail as sound.
 */
float peakOver(Apu& apu, int cycles) {
	apu.setSampleOutput(false);
	apu.tick(40000);
	apu.setSampleOutput(true);

	apu.clearSamples();
	apu.tick(cycles);
	float peak = 0.0f;
	for (float s : apu.samples())
		peak = std::max(peak, std::fabs(s));
	return peak;
}

/**
 * Number of LFSR steps before the noise register returns to where it started.
 * @return -1 if it did not come back around in the budget.
 */
int lfsrPeriod(Apu& apu) {
	const std::uint16_t initial = apu.noiseShiftRegister();
	std::uint16_t prev = initial;
	int steps = 0;
	for (int i = 0; i < 400000; i++) {
		apu.tick(2);
		const std::uint16_t now = apu.noiseShiftRegister();
		if (now == prev)
			continue;
		prev = now;
		steps++;
		if (now == initial)
			return steps;
	}
	return -1;
}

} // namespace

/* ------------------------------------------------------------------------ */
/* Length counters                                                           */
/* ------------------------------------------------------------------------ */

TEST_CASE("length_counter_loads_from_the_table_and_counts_down") {
	Apu apu;
	apu.writeRegister(0x4015, Apu::ENABLE_PULSE1);
	// Index 0 in the length table is 10, which is not the value written.
	apu.writeRegister(0x4003, 0x00);
	CHECK((apu.peekStatus() & Apu::ENABLE_PULSE1) != 0);

	// Ten half-frames to run it out; each full sequence delivers two.
	for (int i = 0; i < 5; i++)
		apu.tick(FULL_SEQUENCE);
	CHECK_EQ(apu.peekStatus() & Apu::ENABLE_PULSE1, 0);
}

TEST_CASE("the_halt_flag_freezes_the_length_counter") {
	Apu apu;
	apu.writeRegister(0x4015, Apu::ENABLE_PULSE1);
	apu.writeRegister(0x4000, 0x20);          // bit 5: halt / envelope loop
	apu.writeRegister(0x4003, 0x00);          // length 10

	// One bit doing two jobs: this also makes the envelope loop.
	for (int i = 0; i < 20; i++)
		apu.tick(FULL_SEQUENCE);
	CHECK((apu.peekStatus() & Apu::ENABLE_PULSE1) != 0);   // still sounding
}

TEST_CASE("disabling_a_channel_clears_its_length_counter") {
	Apu apu;
	apu.writeRegister(0x4015, Apu::ENABLE_PULSE1 | Apu::ENABLE_NOISE);
	apu.writeRegister(0x4003, 0xF8);          // long length
	apu.writeRegister(0x400F, 0xF8);
	REQUIRE((apu.peekStatus() & Apu::ENABLE_PULSE1) != 0);
	REQUIRE((apu.peekStatus() & Apu::ENABLE_NOISE) != 0);

	apu.writeRegister(0x4015, Apu::ENABLE_NOISE);   // pulse 1 off
	CHECK_EQ(apu.peekStatus() & Apu::ENABLE_PULSE1, 0);
	CHECK((apu.peekStatus() & Apu::ENABLE_NOISE) != 0);
}

TEST_CASE("a_disabled_channel_ignores_length_writes") {
	Apu apu;
	apu.writeRegister(0x4015, 0x00);
	apu.writeRegister(0x4003, 0xF8);
	CHECK_EQ(apu.peekStatus() & Apu::ENABLE_PULSE1, 0);
}

/* ------------------------------------------------------------------------ */
/* Frame counter                                                             */
/* ------------------------------------------------------------------------ */

TEST_CASE("a_game_that_never_writes_4017_gets_no_frame_irq") {
	// A deliberate deviation from the documented power-up state, and the reason
	// is a real game: Ikinari Musician never writes $4017 and never reads
	// $4015, so an interrupt enabled at power-up is asserted forever. Its
	// handler runs about 113 times a frame and writes $4015 = 0 every time,
	// turning off every channel -- a music program with no sound.
	//
	// Writing $4017 is the only way to select the sequence or know its phase,
	// so a game that never writes it cannot be relying on the interrupt.
	Apu apu;
	apu.tick(FULL_SEQUENCE * 4);
	CHECK_FALSE(apu.irqAsserted());
	CHECK_EQ(apu.peekStatus() & Apu::STATUS_FRAME_IRQ, 0);
}

TEST_CASE("writing_4017_with_the_inhibit_clear_turns_the_irq_on") {
	// The other half of the deviation: asking for it still works.
	Apu apu;
	apu.tick(FULL_SEQUENCE * 2);
	REQUIRE_FALSE(apu.irqAsserted());

	apu.writeRegister(0x4017, 0x00);
	apu.tick(FULL_SEQUENCE + 10);
	CHECK(apu.irqAsserted());
}

TEST_CASE("four_step_mode_raises_an_irq_at_the_end_of_the_sequence") {
	Apu apu;
	apu.writeRegister(0x4017, 0x00);          // 4-step, IRQ enabled
	apu.tick(4);                              // let the write's delay expire

	apu.tick(FULL_SEQUENCE - 100);
	CHECK_FALSE(apu.irqAsserted());
	apu.tick(200);
	CHECK(apu.irqAsserted());
	CHECK((apu.peekStatus() & Apu::STATUS_FRAME_IRQ) != 0);
}

TEST_CASE("reading_status_acknowledges_the_frame_irq") {
	Apu apu;
	apu.writeRegister(0x4017, 0x00);
	apu.tick(FULL_SEQUENCE + 10);
	REQUIRE(apu.irqAsserted());

	const std::uint8_t status = apu.readStatus();
	CHECK((status & Apu::STATUS_FRAME_IRQ) != 0);   // the read still reports it
	CHECK_FALSE(apu.irqAsserted());                 // but clears it
}

TEST_CASE("the_inhibit_bit_suppresses_and_clears_the_frame_irq") {
	Apu apu;
	apu.writeRegister(0x4017, 0x00);
	apu.tick(FULL_SEQUENCE + 10);
	REQUIRE(apu.irqAsserted());

	apu.writeRegister(0x4017, 0x40);          // inhibit
	CHECK_FALSE(apu.irqAsserted());           // clears the pending one too

	apu.tick(FULL_SEQUENCE * 2);
	CHECK_FALSE(apu.irqAsserted());           // and no more arrive
}

TEST_CASE("five_step_mode_never_raises_an_irq") {
	Apu apu;
	apu.writeRegister(0x4017, 0x80);          // 5-step, IRQ enabled bit clear
	apu.tick(FULL_SEQUENCE * 3);
	CHECK_FALSE(apu.irqAsserted());
}

TEST_CASE("five_step_mode_clocks_everything_immediately") {
	// Switching to the five-step sequence issues a quarter and half frame at
	// once. A game uses that to restart an envelope in step with its own code,
	// so it has to be observable -- here, by the length counter dropping.
	Apu apu;
	apu.writeRegister(0x4015, Apu::ENABLE_PULSE1);
	apu.writeRegister(0x4000, 0x00);
	apu.writeRegister(0x4003, 0x08);          // length index 1 -> 254

	apu.writeRegister(0x4017, 0x80);
	apu.tick(4);                              // the write's delay
	// The immediate half-frame took one off; without it the counter would need
	// a full sequence to move at all.
	CHECK((apu.peekStatus() & Apu::ENABLE_PULSE1) != 0);

	// Length 2 runs out after two half frames; from 254 it would not.
	apu.writeRegister(0x4003, 0x18);          // index 3 -> 2
	apu.tick(HALF_FRAME + 100);
	apu.writeRegister(0x4017, 0x80);
	apu.tick(4);
	CHECK_EQ(apu.peekStatus() & Apu::ENABLE_PULSE1, 0);
}

/* ------------------------------------------------------------------------ */
/* Channels                                                                  */
/* ------------------------------------------------------------------------ */

TEST_CASE("a_pulse_channel_makes_a_sound") {
	Apu apu = makeApu();
	CHECK_EQ(peakOver(apu, 10000), doctest::Approx(0.0f).epsilon(0.001));

	apu.writeRegister(0x4015, Apu::ENABLE_PULSE1);
	apu.writeRegister(0x4000, 0xBF);          // constant volume 15, halt set
	apu.writeRegister(0x4002, 0xFD);          // period 0x0FD -- audible
	apu.writeRegister(0x4003, 0xF8);
	CHECK(peakOver(apu, 20000) > 0.01f);
}

TEST_CASE("a_pulse_is_muted_below_period_eight") {
	// Not a volume choice -- hardware gates the channel off entirely, because
	// the frequency would be past anything the console can reproduce.
	Apu apu = makeApu();
	apu.writeRegister(0x4015, Apu::ENABLE_PULSE1);
	apu.writeRegister(0x4000, 0xBF);
	apu.writeRegister(0x4002, 0x07);          // period 7
	apu.writeRegister(0x4003, 0xF8);
	CHECK_EQ(peakOver(apu, 20000), doctest::Approx(0.0f).epsilon(0.001));

	apu.writeRegister(0x4002, 0x08);          // period 8: allowed
	apu.writeRegister(0x4003, 0xF8);
	CHECK(peakOver(apu, 20000) > 0.01f);
}

TEST_CASE("constant_volume_and_the_envelope_differ") {
	Apu apu = makeApu();
	apu.writeRegister(0x4015, Apu::ENABLE_PULSE1);
	apu.writeRegister(0x4002, 0xFD);

	apu.writeRegister(0x4000, 0xBF);          // halt, constant, volume 15
	apu.writeRegister(0x4003, 0xF8);
	const float constant = peakOver(apu, 20000);

	// Envelope mode instead, with the fastest decay and no loop. The level
	// walks down one step per quarter frame and then stays at zero, which is
	// how a note ends without the game touching the channel again.
	apu.writeRegister(0x4000, 0x20);          // halt/loop set, envelope, period 0
	apu.writeRegister(0x4003, 0xF8);
	const float sustained = peakOver(apu, 20000);

	apu.writeRegister(0x4000, 0x00);          // no loop: decays to silence
	apu.writeRegister(0x4003, 0xF8);
	apu.tick(FULL_SEQUENCE * 2);
	const float decayed = peakOver(apu, 20000);

	CHECK(constant > 0.01f);
	CHECK(sustained > 0.0f);                  // a looping envelope keeps going
	CHECK(decayed < sustained);               // a one-shot does not
}

TEST_CASE("the_triangle_needs_both_its_counters") {
	Apu apu = makeApu();
	apu.writeRegister(0x4015, Apu::ENABLE_TRIANGLE);
	apu.writeRegister(0x400A, 0x40);          // audible period
	apu.writeRegister(0x400B, 0xF8);          // length + reload the linear counter

	// The linear counter only loads on a quarter frame, so nothing happens
	// until the sequencer runs.
	apu.writeRegister(0x4008, 0xFF);          // control set, reload 127
	apu.tick(HALF_FRAME);
	CHECK(peakOver(apu, 20000) > 0.001f);

	// Clearing the length counter stops it even though the linear counter is
	// still loaded.
	apu.writeRegister(0x4015, 0x00);
	CHECK_EQ(peakOver(apu, 20000), doctest::Approx(0.0f).epsilon(0.0001));
}

TEST_CASE("the_triangle_is_silent_at_ultrasonic_periods") {
	Apu apu = makeApu();
	apu.writeRegister(0x4015, Apu::ENABLE_TRIANGLE);
	apu.writeRegister(0x4008, 0xFF);
	apu.writeRegister(0x400A, 0x01);          // period 1 -- above 50 kHz
	apu.writeRegister(0x400B, 0xF8);
	apu.tick(HALF_FRAME);
	CHECK_EQ(peakOver(apu, 20000), doctest::Approx(0.0f).epsilon(0.0001));
}

TEST_CASE("the_noise_lfsr_runs_its_full_period_in_long_mode") {
	// A maximal 15-bit LFSR: 32767 states before it repeats. That is what makes
	// it sound like white noise rather than a tone.
	Apu apu;
	apu.writeRegister(0x400E, 0x00);          // long mode, shortest period
	CHECK_EQ(lfsrPeriod(apu), 32767);
}

TEST_CASE("short_mode_shortens_the_lfsr_to_93_steps") {
	// Tapping bit 6 instead of bit 1 collapses the period to 93, which is short
	// enough to hear as a pitch. Games use it for snares and laser sounds, so
	// getting the tap wrong is audible rather than subtle.
	Apu apu;
	apu.writeRegister(0x400E, 0x80);          // short mode
	CHECK_EQ(lfsrPeriod(apu), 93);
}

TEST_CASE("the_noise_channel_makes_a_sound") {
	Apu apu = makeApu();
	apu.writeRegister(0x4015, Apu::ENABLE_NOISE);
	apu.writeRegister(0x400C, 0x3F);          // halt, constant volume 15
	apu.writeRegister(0x400E, 0x04);
	apu.writeRegister(0x400F, 0xF8);
	CHECK(peakOver(apu, 40000) > 0.005f);
}

TEST_CASE("writing_4011_moves_the_dmc_level_directly") {
	// Games play PCM through this by brute force, one CPU write at a time --
	// no sample, no DMA, just the output level.
	Apu apu = makeApu();
	apu.clearSamples();
	apu.writeRegister(0x4011, 0x7F);
	apu.tick(200);
	float high = 0.0f;
	for (float s : apu.samples())
		high = std::max(high, std::fabs(s));
	CHECK(high > 0.005f);
}

/* ------------------------------------------------------------------------ */
/* Mixing                                                                    */
/* ------------------------------------------------------------------------ */

TEST_CASE("the_mixer_is_nonlinear") {
	// Two identical pulses must not be twice as loud as one. The channels sum
	// through a resistor ladder, so the second one adds less than the first.
	Apu one = makeApu();
	one.writeRegister(0x4015, Apu::ENABLE_PULSE1);
	one.writeRegister(0x4000, 0xBF);
	one.writeRegister(0x4002, 0xFD);
	one.writeRegister(0x4003, 0xF8);
	const float single = peakOver(one, 40000);

	Apu two = makeApu();
	two.writeRegister(0x4015, Apu::ENABLE_PULSE1 | Apu::ENABLE_PULSE2);
	two.writeRegister(0x4000, 0xBF);
	two.writeRegister(0x4002, 0xFD);
	two.writeRegister(0x4003, 0xF8);
	two.writeRegister(0x4004, 0xBF);
	two.writeRegister(0x4006, 0xFD);
	two.writeRegister(0x4007, 0xF8);
	const float pair = peakOver(two, 40000);

	CHECK(pair > single);                     // louder...
	CHECK(pair < single * 2.0f);              // ...but not twice as loud
}

TEST_CASE("silence_really_is_silence") {
	Apu apu = makeApu();
	CHECK_EQ(peakOver(apu, 50000), doctest::Approx(0.0f).epsilon(0.0001));
}

/* ------------------------------------------------------------------------ */
/* Integration with the console                                              */
/* ------------------------------------------------------------------------ */

TEST_CASE("the_bus_routes_apu_registers") {
	Nes console;
	NesBus& bus = console.bus();

	bus.write(0x4015, Apu::ENABLE_PULSE1);
	bus.write(0x4003, 0x00);                  // length 10
	CHECK((bus.peek(0x4015) & Apu::ENABLE_PULSE1) != 0);

	// $4015 is a real register now, not a stub.
	const unsigned long stubs = bus.stubReads();
	bus.read(0x4015);
	CHECK_EQ(bus.stubReads(), stubs);
}

TEST_CASE("peeking_4015_does_not_acknowledge_the_irq") {
	// Same contract as the PPU registers and the controller ports: a debugger
	// must not clear an interrupt the game has not seen yet.
	Nes console;
	console.bus().write(0x4017, 0x00);
	console.apu().tick(FULL_SEQUENCE + 10);
	REQUIRE(console.apu().irqAsserted());

	CHECK((console.bus().peek(0x4015) & Apu::STATUS_FRAME_IRQ) != 0);
	CHECK(console.apu().irqAsserted());       // still pending

	console.bus().read(0x4015);
	CHECK_FALSE(console.apu().irqAsserted()); // the real read clears it
}

TEST_CASE("the_frame_irq_reaches_the_cpu") {
	// The end-to-end path: the APU raises its level-triggered line, Nes::step
	// forwards it, and the CPU vectors through $FFFE.
	std::vector<std::uint8_t> prg;
	testrom::setResetVector(prg, 0xC000);
	prg[0x0000] = 0x58;                       // CLI -- allow IRQs
	prg[0x0001] = 0x4C;                       // JMP $C001, spin
	prg[0x0002] = 0x01;
	prg[0x0003] = 0xC0;
	prg[0x3FFE] = 0x00;                       // IRQ vector -> $C100
	prg[0x3FFF] = 0xC1;
	prg[0x0100] = 0xEA;                       // NOP at $C100

	auto cart = Cartridge::fromINes(testrom::build(testrom::Options(), prg));
	REQUIRE(cart != nullptr);

	Nes console;
	console.setCartridge(std::move(cart));
	console.reset();
	console.bus().write(0x4017, 0x00);        // 4-step mode, IRQ enabled

	bool vectored = false;
	for (int i = 0; i < 40000 && !vectored; i++) {
		console.step();
		if (console.cpuRegisters().pc >= 0xC100 && console.cpuRegisters().pc < 0xC110)
			vectored = true;
	}
	CHECK(vectored);
}

TEST_CASE("an_apu_irq_is_ignored_while_the_i_flag_is_set") {
	// Level-triggered, unlike the NMI: it waits rather than being lost.
	std::vector<std::uint8_t> prg;
	testrom::setResetVector(prg, 0xC000);
	prg[0x0000] = 0x4C;                       // JMP $C000, spin with I still set
	prg[0x0001] = 0x00;
	prg[0x0002] = 0xC0;
	prg[0x3FFE] = 0x00;
	prg[0x3FFF] = 0xC1;
	prg[0x0100] = 0xEA;

	auto cart = Cartridge::fromINes(testrom::build(testrom::Options(), prg));
	REQUIRE(cart != nullptr);

	Nes console;
	console.setCartridge(std::move(cart));
	console.reset();                          // reset leaves I set
	console.bus().write(0x4017, 0x00);

	for (int i = 0; i < 40000; i++)
		console.step();
	CHECK_EQ(console.cpuRegisters().pc, 0xC000);   // never vectored
	CHECK(console.apu().irqAsserted());            // but the line is still held
}
