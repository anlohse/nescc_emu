/*
 * testBus.cpp -- CPU address decoding, and the CPU running out of a cartridge.
 */

#include "TestRom.h"
#include "nes/Nes.h"

#include <doctest/doctest.h>

using namespace nes;

namespace {

/** A console with a one-bank NROM cartridge holding @p prg and resetting to @p pc. */
std::unique_ptr<Nes> makeConsole(const std::vector<std::uint8_t>& program, std::uint16_t pc) {
	std::vector<std::uint8_t> prg;
	testrom::setResetVector(prg, pc);
	for (std::size_t i = 0; i < program.size(); i++)
		prg[(pc - 0xC000) + i] = program[i];   // one bank: $C000 mirrors PRG offset 0

	std::string error;
	auto cart = Cartridge::fromINes(testrom::build(testrom::Options(), prg), &error);
	REQUIRE_MESSAGE(cart != nullptr, error);

	std::unique_ptr<Nes> console(new Nes());
	console->setCartridge(std::move(cart));
	console->reset();
	return console;
}

} // namespace

TEST_CASE("internal_ram_mirrors_every_2kb") {
	Nes console;
	NesBus& bus = console.bus();

	bus.write(0x0000, 0x5A);
	CHECK_EQ(bus.read(0x0800), 0x5A);
	CHECK_EQ(bus.read(0x1000), 0x5A);
	CHECK_EQ(bus.read(0x1800), 0x5A);

	// Writing through a mirror lands in the same byte.
	bus.write(0x1801, 0xA5);
	CHECK_EQ(bus.read(0x0001), 0xA5);

	// $07FF is the top of real RAM; $0800 wraps back to $0000.
	bus.write(0x07FF, 0x11);
	CHECK_EQ(bus.read(0x0FFF), 0x11);
	CHECK_EQ(bus.read(0x17FF), 0x11);
}

TEST_CASE("ppu_registers_mirror_every_8_bytes") {
	Nes console;
	NesBus& bus = console.bus();

	// Register 0 (PPUCTRL) is reachable at $2000 and every alias 8 bytes apart.
	bus.write(0x2000, Ppu::CTRL_NMI_ENABLE);
	CHECK_EQ(bus.peek(0x2000), Ppu::CTRL_NMI_ENABLE);
	CHECK_EQ(bus.peek(0x2008), Ppu::CTRL_NMI_ENABLE);
	CHECK_EQ(bus.peek(0x3FF8), Ppu::CTRL_NMI_ENABLE);

	bus.write(0x3FF8, 0x00);                  // same register, far mirror
	CHECK_EQ(console.ppu().peekRegister(0), 0x00);

	// Reaching PPUADDR/PPUDATA through mirrors still addresses VRAM.
	bus.write(0x2FFE, 0x21);                  // $2FFE & 7 == 6, high byte
	bus.write(0x2FFE, 0x08);                  // low byte -> $2108
	bus.write(0x3FFF, 0x77);                  // $3FFF & 7 == 7, PPUDATA
	CHECK_EQ(console.ppu().vramRead(0x2108), 0x77);
}

TEST_CASE("peek_does_not_disturb_device_state") {
	// This is the whole reason peek() exists: a memory viewer must not count as
	// a CPU access. The PPU registers are covered in testPpu.cpp, the controller
	// ports in testController.cpp and the APU in testApu.cpp; what is left in
	// $4000-$401F is write-only or unmapped, and $4018+ are the disabled test
	// registers -- nothing answers there.
	Nes console;
	NesBus& bus = console.bus();

	const unsigned long readsBefore = bus.stubReads();
	bus.peek(0x4018);
	bus.peek(0x4000);
	CHECK_EQ(bus.stubReads(), readsBefore);   // peek is invisible to devices

	bus.read(0x4018);
	CHECK_EQ(bus.stubReads(), readsBefore + 1);
}

TEST_CASE("cartridge_space_routes_to_the_mapper") {
	std::vector<std::uint8_t> prg;
	testrom::setResetVector(prg, 0xC000);
	prg[0] = 0x42;

	auto cart = Cartridge::fromINes(testrom::build(testrom::Options(), prg));
	REQUIRE(cart != nullptr);

	Nes console;
	console.setCartridge(std::move(cart));
	NesBus& bus = console.bus();

	CHECK_EQ(bus.read(0x8000), 0x42);
	CHECK_EQ(bus.read(0xC000), 0x42);        // 16 KB mirror

	// Work RAM on the cartridge, not console RAM.
	bus.write(0x6000, 0x99);
	CHECK_EQ(bus.read(0x6000), 0x99);
	CHECK_EQ(bus.read(0x0000), 0);           // console RAM untouched
}

TEST_CASE("bus_without_a_cartridge_reads_open_bus") {
	Nes console;
	CHECK_EQ(console.bus().read(0x8000), 0);
	CHECK_EQ(console.bus().peek(0xFFFC), 0);
}

TEST_CASE("reset_loads_pc_from_the_cartridge_vector") {
	auto console = makeConsole({ 0xEA }, 0xC000);
	CHECK_EQ(console->cpuRegisters().pc, 0xC000);
	CHECK_EQ(console->cpuRegisters().sp, 0xFD);
	CHECK(console->cpuRegisters().getStatus(FLAG_I));
	CHECK_EQ(console->cycles(), 7);          // the reset sequence
}

TEST_CASE("cpu_executes_a_program_out_of_cartridge_rom") {
	// LDA #$42 / STA $0200 / LDX #$05 / INX / JMP self
	const std::vector<std::uint8_t> program = {
		0xA9, 0x42,
		0x8D, 0x00, 0x02,
		0xA2, 0x05,
		0xE8,
		0x4C, 0x08, 0xC0
	};
	auto console = makeConsole(program, 0xC000);

	CHECK_EQ(console->step(), 2);            // LDA #
	CHECK_EQ(console->cpuRegisters().a, 0x42);

	CHECK_EQ(console->step(), 4);            // STA abs
	CHECK_EQ(console->bus().peek(0x0200), 0x42);

	CHECK_EQ(console->step(), 2);            // LDX #
	CHECK_EQ(console->cpuRegisters().x, 0x05);

	CHECK_EQ(console->step(), 2);            // INX
	CHECK_EQ(console->cpuRegisters().x, 0x06);

	CHECK_EQ(console->step(), 3);            // JMP abs
	CHECK_EQ(console->cpuRegisters().pc, 0xC008);
	console->step();
	CHECK_EQ(console->cpuRegisters().pc, 0xC008);   // spins in place

	// 7 for reset plus the instructions above.
	CHECK_EQ(console->cycles(), 7 + 2 + 4 + 2 + 2 + 3 + 3);
}

TEST_CASE("nmi_vectors_through_the_cartridge") {
	// Proves the interrupt path added to emu6502 works through a real mapper:
	// the vector has to be fetched from PRG ROM.
	std::vector<std::uint8_t> prg;
	testrom::setResetVector(prg, 0xC000);
	prg[0x0000] = 0xEA;                      // NOP at $C000
	prg[0x3FFA] = 0x00;                      // NMI vector -> $C100
	prg[0x3FFB] = 0xC1;
	prg[0x0100] = 0x40;                      // RTI at $C100

	auto cart = Cartridge::fromINes(testrom::build(testrom::Options(), prg));
	REQUIRE(cart != nullptr);

	Nes console;
	console.setCartridge(std::move(cart));
	console.reset();
	REQUIRE_EQ(console.cpuRegisters().pc, 0xC000);

	console.cpu().nmi();
	CHECK_EQ(console.step(), 7);
	CHECK_EQ(console.cpuRegisters().pc, 0xC100);

	console.step();                          // RTI
	CHECK_EQ(console.cpuRegisters().pc, 0xC000);
}
