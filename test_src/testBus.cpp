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

/* ------------------------------------------------------------------------ */
/* Per-access timing                                                         */
/* ------------------------------------------------------------------------ */

/*
 * The bus is where time passes. Every access the CPU makes during an
 * instruction advances the PPU and APU a cycle first, so a device register is
 * sampled at the cycle its access happens rather than at the instruction
 * boundary before it. Whatever the accesses did not account for -- a taken
 * branch, a page-crossing read, the internal cycles of a stack operation --
 * is charged when the instruction ends, so the totals stay exact.
 */

TEST_CASE("each_access_advances_the_ppu_a_cycle") {
	Nes console;
	NesBus& bus = console.bus();
	const Ppu& ppu = console.ppu();

	const int start = ppu.dot();
	bus.beginInstruction();
	bus.read(0x0000);
	CHECK_EQ(ppu.dot(), start + 3);
	bus.read(0x0000);
	CHECK_EQ(ppu.dot(), start + 6);
	bus.write(0x0000, 0);
	CHECK_EQ(ppu.dot(), start + 9);
}

TEST_CASE("the_instruction_pays_for_the_cycles_it_did_not_access_in") {
	Nes console;
	NesBus& bus = console.bus();
	const Ppu& ppu = console.ppu();

	// Three accesses inside an instruction that cost five cycles: the missing
	// two are charged at the end, so the PPU still lands 15 dots on.
	const int start = ppu.dot();
	bus.beginInstruction();
	bus.read(0x0000);
	bus.read(0x0000);
	bus.read(0x0000);
	bus.endInstruction(5);
	CHECK_EQ(ppu.dot(), start + 15);
	CHECK_EQ(bus.overrunInstructions(), 0);
}

TEST_CASE("nothing_outside_an_instruction_charges_time") {
	// The reset vector read, a debugger poking around, an OAM DMA copy: all
	// reach this bus, and none of them is time the CPU is spending here.
	Nes console;
	NesBus& bus = console.bus();
	const Ppu& ppu = console.ppu();

	const int start = ppu.dot();
	bus.read(0xFFFC);
	bus.read(0x0000);
	bus.write(0x0000, 0);
	CHECK_EQ(ppu.dot(), start);
}

TEST_CASE("an_oam_dma_advances_the_ppu_once_not_twice") {
	// The copy makes 256 reads through this same bus, and the transfer is paid
	// for by the 513-cycle stall. Charging both would run the PPU at roughly
	// one and a half times speed for the duration.
	Nes console;
	NesBus& bus = console.bus();

	const std::uint64_t before = console.ppu().frame() * 1000000
			+ static_cast<std::uint64_t>(console.ppu().scanline()) * 341
			+ console.ppu().dot();

	bus.beginInstruction();
	bus.write(0x4014, 0x02);       // start a DMA out of page $0200
	bus.endInstruction(4);

	const std::uint64_t after = console.ppu().frame() * 1000000
			+ static_cast<std::uint64_t>(console.ppu().scanline()) * 341
			+ console.ppu().dot();

	// Only the write's own instruction has been charged so far; the stall is
	// still owed and Nes::step() spends it separately.
	CHECK_EQ(after - before, 4 * 3);
	CHECK_EQ(bus.pendingDmaStall(), 513);
}

TEST_CASE("a_running_console_never_overruns_its_cycle_budget") {
	// The whole scheme rests on accesses being no more numerous than cycles.
	// emu6502 pins that over all 256 opcodes; this checks it end to end, with
	// real instructions running out of a real cartridge.
	std::vector<std::uint8_t> prg;
	testrom::setResetVector(prg, 0xC000);
	prg[0x0000] = 0xEE;            // INC $0200  -- two writes, the worst case
	prg[0x0001] = 0x00;
	prg[0x0002] = 0x02;
	prg[0x0003] = 0x4C;            // JMP $C000
	prg[0x0004] = 0x00;
	prg[0x0005] = 0xC0;

	auto cart = Cartridge::fromINes(testrom::build(testrom::Options(), prg));
	REQUIRE(cart != nullptr);

	Nes console;
	console.setCartridge(std::move(cart));
	console.reset();
	for (int i = 0; i < 20000; i++)
		console.step();

	CHECK_EQ(console.bus().overrunInstructions(), 0);
}

TEST_CASE("a_register_is_read_at_the_cycle_its_access_happens") {
	// The point of all of it. LDA $2002 is four cycles, and the data read is
	// the fourth -- so a vblank flag raised during the first three is already
	// up by the time the CPU looks, and one raised after the instruction ends
	// is not. Under whole-instruction timing both reads happened at the
	// instruction's first cycle and neither could tell the difference.
	//
	// The PPU raises vblank at scanline 241, dot 1. Parking the PPU at dot D of
	// scanline 240 puts that (341 - D) + 1 dots away.
	struct Case { int parkDot; int dotsToVblank; bool expectFlag; };
	const Case cases[] = {
		{ 337,  5, true  },   // raised during cycle 2 of the instruction
		{ 332, 10, true  },   // raised two dots before the read
		{ 329, 13, false },   // raised one dot after the read
		{ 320, 22, false },   // raised after the instruction is over
	};

	for (const Case& c : cases) {
		CAPTURE(c.parkDot);
		CAPTURE(c.dotsToVblank);

		std::vector<std::uint8_t> prg;
		testrom::setResetVector(prg, 0xC000);
		prg[0x0000] = 0xAD;        // LDA $2002
		prg[0x0001] = 0x02;
		prg[0x0002] = 0x20;

		auto cart = Cartridge::fromINes(testrom::build(testrom::Options(), prg));
		REQUIRE(cart != nullptr);

		Nes console;
		console.setCartridge(std::move(cart));
		console.reset();

		// Park the PPU on the dot this case wants. Rendering is off, so no
		// odd-frame skip complicates the count.
		while (!(console.ppu().scanline() == 240 && console.ppu().dot() == c.parkDot))
			console.ppu().tick(1);

		console.step();
		CHECK_EQ((console.cpuRegisters().a & 0x80) != 0, c.expectFlag);
	}
}

TEST_CASE("an_unmapped_read_returns_whatever_the_bus_last_carried") {
	// Open bus, which is not zero. Nothing drives the data lines when an address
	// decodes to no device, so the read comes back with whatever was last on them.
	// $4018-$40FF is the clearest case: the 2A03 never decoded it and no board
	// implemented here does either.
	//
	// The value is arranged deliberately rather than hoped for. A write puts a byte
	// on the bus; the read that follows finds it still there.
	std::vector<std::uint8_t> program;
	program.push_back(0xA9);          // LDA #$5A
	program.push_back(0x5A);
	program.push_back(0x8D);          // STA $0300 -- drives $5A onto the bus
	program.push_back(0x00);
	program.push_back(0x03);
	program.push_back(0xAD);          // LDA $4018 -- unmapped
	program.push_back(0x18);
	program.push_back(0x40);

	auto console = makeConsole(program, 0xC000);
	console->step();                  // LDA #$5A
	console->step();                  // STA $0300
	console->step();                  // LDA $4018

	// Not $5A: the read's own address bytes crossed the bus after the store, so
	// what lingers is the high byte of $4018. What matters is that it is the last
	// thing carried rather than a hardcoded zero.
	CHECK_EQ(console->cpuRegisters().a, 0x40);
}

TEST_CASE("a_write_only_apu_register_reads_as_open_bus_too") {
	// $4000-$4013 and $4014 answer nothing, so they behave the same way. Reading
	// one used to give zero, which is a value the hardware never returns.
	std::vector<std::uint8_t> program;
	program.push_back(0xAD);          // LDA $4000
	program.push_back(0x00);
	program.push_back(0x40);

	auto console = makeConsole(program, 0xC000);
	console->step();
	CHECK_EQ(console->cpuRegisters().a, 0x40);
}
