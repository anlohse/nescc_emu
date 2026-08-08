/*
 * testController.cpp -- the pad's shift register, and the CPU reading it.
 */

#include "TestRom.h"
#include "nes/Nes.h"

#include <doctest/doctest.h>

#include <vector>

using namespace nes;

namespace {

/** Strobe the pad and clock out all eight bits, in report order. */
std::vector<std::uint8_t> readAllButtons(NesBus& bus, std::uint16_t port = 0x4016) {
	bus.write(0x4016, 1);
	bus.write(0x4016, 0);
	std::vector<std::uint8_t> bits;
	for (int i = 0; i < 8; i++)
		bits.push_back(bus.read(port) & 1);
	return bits;
}

} // namespace

TEST_CASE("shift_register_reports_buttons_in_hardware_order") {
	Controller pad;
	pad.setButtons(Controller::BUTTON_A | Controller::BUTTON_START | Controller::BUTTON_RIGHT);

	pad.writeStrobe(1);
	pad.writeStrobe(0);

	// A, B, Select, Start, Up, Down, Left, Right.
	const std::uint8_t expected[8] = { 1, 0, 0, 1, 0, 0, 0, 1 };
	for (int i = 0; i < 8; i++)
		CHECK_MESSAGE(pad.read() == expected[i], "bit ", i);
}

TEST_CASE("reads_past_the_eighth_return_one") {
	// An official pad shifts in high bits once its eight are spent, and some
	// games use that to tell a controller from an empty port.
	Controller pad;
	pad.setButtons(0);
	pad.writeStrobe(1);
	pad.writeStrobe(0);

	for (int i = 0; i < 8; i++)
		CHECK_EQ(pad.read(), 0);
	CHECK_EQ(pad.read(), 1);
	CHECK_EQ(pad.read(), 1);
}

TEST_CASE("strobe_high_keeps_the_register_transparent") {
	Controller pad;
	pad.writeStrobe(1);
	pad.setButtons(Controller::BUTTON_A);

	// No shifting while the strobe is high: every read is A, right now.
	CHECK_EQ(pad.read(), 1);
	CHECK_EQ(pad.read(), 1);
	CHECK_EQ(pad.read(), 1);

	pad.setButtons(Controller::BUTTON_B);
	CHECK_EQ(pad.read(), 0);                 // A released, and it shows at once
}

TEST_CASE("the_latch_freezes_input_for_the_whole_read") {
	// This is the point of the latch: everything a game reads in one pass comes
	// from the same instant, so a button released mid-sequence still reports
	// pressed until the next latch.
	Controller pad;
	pad.setButtons(Controller::BUTTON_A | Controller::BUTTON_START);
	pad.writeStrobe(1);
	pad.writeStrobe(0);

	CHECK_EQ(pad.read(), 1);                 // A
	pad.setButtons(0);                       // released between reads
	CHECK_EQ(pad.read(), 0);                 // B
	CHECK_EQ(pad.read(), 0);                 // Select
	CHECK_EQ(pad.read(), 1);                 // Start -- still the latched state

	// The next latch picks up the release.
	pad.writeStrobe(1);
	pad.writeStrobe(0);
	for (int i = 0; i < 8; i++)
		CHECK_EQ(pad.read(), 0);
}

TEST_CASE("peek_does_not_clock_the_register") {
	Controller pad;
	pad.setButtons(Controller::BUTTON_B);
	pad.writeStrobe(1);
	pad.writeStrobe(0);

	CHECK_EQ(pad.peek(), 0);                 // A, not pressed
	CHECK_EQ(pad.peek(), 0);                 // still A: peeking never shifts
	CHECK_EQ(pad.read(), 0);
	CHECK_EQ(pad.peek(), 1);                 // now B
}

TEST_CASE("button_names_parse") {
	CHECK_EQ(Controller::buttonFromName("a"), Controller::BUTTON_A);
	CHECK_EQ(Controller::buttonFromName("START"), Controller::BUTTON_START);
	CHECK_EQ(Controller::buttonFromName("Select"), Controller::BUTTON_SELECT);
	CHECK_EQ(Controller::buttonFromName("down"), Controller::BUTTON_DOWN);
	CHECK_EQ(Controller::buttonFromName("turbo"), 0);
	CHECK_EQ(Controller::buttonFromName(""), 0);
	CHECK_EQ(Controller::buttonFromName("st"), 0);       // prefix is not a match
	CHECK_EQ(Controller::buttonFromName("starts"), 0);
}

TEST_CASE("cpu_reads_port_one_at_4016") {
	Nes console;
	NesBus& bus = console.bus();
	console.controller(0).setButtons(Controller::BUTTON_UP | Controller::BUTTON_B);

	const std::vector<std::uint8_t> bits = readAllButtons(bus);
	const std::uint8_t expected[8] = { 0, 1, 0, 0, 1, 0, 0, 0 };
	for (int i = 0; i < 8; i++)
		CHECK_MESSAGE(bits[i] == expected[i], "bit ", i);
}

TEST_CASE("cpu_reads_port_two_at_4017") {
	Nes console;
	NesBus& bus = console.bus();
	console.controller(1).setButtons(Controller::BUTTON_LEFT);

	const std::vector<std::uint8_t> bits = readAllButtons(bus, 0x4017);
	const std::uint8_t expected[8] = { 0, 0, 0, 0, 0, 0, 1, 0 };
	for (int i = 0; i < 8; i++)
		CHECK_MESSAGE(bits[i] == expected[i], "bit ", i);

	// The ports are independent -- port 1 saw nothing.
	bus.write(0x4016, 1);
	bus.write(0x4016, 0);
	for (int i = 0; i < 8; i++)
		CHECK_EQ(bus.read(0x4016) & 1, 0);
}

TEST_CASE("one_strobe_write_latches_both_ports") {
	// $4017 is the APU frame counter on writes, so a game latches both pads with
	// a single write to $4016. If the second port did not follow that strobe it
	// would report stale input.
	Nes console;
	NesBus& bus = console.bus();
	console.controller(0).setButtons(Controller::BUTTON_A);
	console.controller(1).setButtons(Controller::BUTTON_A);

	bus.write(0x4016, 1);
	bus.write(0x4016, 0);
	CHECK_EQ(bus.read(0x4016) & 1, 1);
	CHECK_EQ(bus.read(0x4017) & 1, 1);
}

TEST_CASE("controller_reads_carry_open_bus_in_the_high_bits") {
	Nes console;
	NesBus& bus = console.bus();
	console.controller(0).setButtons(Controller::BUTTON_A);
	bus.write(0x4016, 1);
	bus.write(0x4016, 0);

	// Bit 0 is the data; bit 6 is the lingering high byte of the $40xx address.
	CHECK_EQ(bus.read(0x4016), 0x41);
	CHECK_EQ(bus.read(0x4016), 0x40);
}

TEST_CASE("peeking_a_port_does_not_clock_it") {
	// Same contract as the PPU registers: a memory viewer must not consume input.
	Nes console;
	NesBus& bus = console.bus();
	console.controller(0).setButtons(Controller::BUTTON_A);
	bus.write(0x4016, 1);
	bus.write(0x4016, 0);

	CHECK_EQ(bus.peek(0x4016), 0x41);
	CHECK_EQ(bus.peek(0x4016), 0x41);        // unchanged
	CHECK_EQ(bus.read(0x4016), 0x41);        // the real read consumes it
	CHECK_EQ(bus.peek(0x4016), 0x40);
}

TEST_CASE("controller_ports_no_longer_count_as_stubs") {
	Nes console;
	NesBus& bus = console.bus();

	const unsigned long readsBefore = bus.stubReads();
	const unsigned long writesBefore = bus.stubWrites();
	bus.write(0x4016, 1);
	bus.read(0x4016);
	bus.read(0x4017);
	CHECK_EQ(bus.stubReads(), readsBefore);
	CHECK_EQ(bus.stubWrites(), writesBefore);

	bus.read(0x4015);                        // APU status is still a stub
	CHECK_EQ(bus.stubReads(), readsBefore + 1);
}

TEST_CASE("a_program_reads_the_pad_the_way_a_game_does") {
	// The standard read loop, as written by roughly every NES game:
	//
	//   LDA #$01 / STA $4016 / LDA #$00 / STA $4016   latch
	//   LDX #$08
	// loop:
	//   LDA $4016 / LSR A / ROL $10                   shift bit 0 into $10
	//   DEX / BNE loop
	//
	// $10 ends up holding all eight buttons, but MSB first: the first bit read
	// (A) is shifted the furthest, so it lands in bit 7 and the packed byte is
	// the reverse of this class's mask. Games live with that; it is why so many
	// of them test input with BMI and BPL.
	const std::vector<std::uint8_t> program = {
		0xA9, 0x01,             // LDA #$01
		0x8D, 0x16, 0x40,       // STA $4016
		0xA9, 0x00,             // LDA #$00
		0x8D, 0x16, 0x40,       // STA $4016
		0xA2, 0x08,             // LDX #$08
		0xAD, 0x16, 0x40,       // loop: LDA $4016
		0x4A,                   // LSR A
		0x26, 0x10,             // ROL $10
		0xCA,                   // DEX
		0xD0, 0xF7,             // BNE loop
		0x4C, 0x15, 0xC0        // JMP self
	};

	std::vector<std::uint8_t> prg;
	testrom::setResetVector(prg, 0xC000);
	for (std::size_t i = 0; i < program.size(); i++)
		prg[i] = program[i];

	auto cart = Cartridge::fromINes(testrom::build(testrom::Options(), prg));
	REQUIRE(cart != nullptr);

	Nes console;
	console.setCartridge(std::move(cart));
	console.reset();
	console.controller(0).setButtons(Controller::BUTTON_A | Controller::BUTTON_START);

	for (int i = 0; i < 100 && console.cpuRegisters().pc != 0xC015; i++)
		console.step();

	REQUIRE_EQ(console.cpuRegisters().pc, 0xC015);   // reached the spin
	// A in bit 7, Start in bit 4: 1001 0000.
	CHECK_EQ(console.bus().peek(0x0010), 0x90);
}
