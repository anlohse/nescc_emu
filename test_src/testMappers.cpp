/*
 * testMappers.cpp -- bank switching, board by board.
 *
 * Every ROM here is stamped: byte N of bank B holds B. So reading $8000 and
 * getting 3 means bank 3 is mapped there, and a test can say which bank is
 * where without caring what is in it.
 */

#include "TestRom.h"
#include "nes/Nes.h"

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace nes;

namespace {

const std::size_t BANK_16K = 16 * 1024;
const std::size_t BANK_8K = 8 * 1024;

/** @p totalSize bytes in which every byte of bank B holds the value B. */
std::vector<std::uint8_t> stamped(std::size_t totalSize, std::size_t bankSize) {
	std::vector<std::uint8_t> data(totalSize, 0);
	for (std::size_t i = 0; i < totalSize; i++)
		data[i] = static_cast<std::uint8_t>(i / bankSize);
	return data;
}

/** A cartridge on @p mapper with stamped PRG and CHR. */
std::unique_ptr<Cartridge> makeCart(int mapper, int prgBanks16k, int chrBanks8k,
		std::size_t prgStampSize = BANK_16K, std::size_t chrStampSize = 1024) {
	testrom::Options o;
	o.mapper = mapper;
	o.prgBanks = prgBanks16k;
	o.chrBanks = chrBanks8k;

	const std::vector<std::uint8_t> prg =
			stamped(static_cast<std::size_t>(prgBanks16k) * BANK_16K, prgStampSize);
	const std::vector<std::uint8_t> chr =
			stamped(static_cast<std::size_t>(chrBanks8k) * BANK_8K, chrStampSize);

	std::string error;
	auto cart = Cartridge::fromINes(testrom::build(o, prg, chr), &error);
	REQUIRE_MESSAGE(cart != nullptr, error);
	return cart;
}

std::string mirroringOf(const Cartridge& cart) {
	return toString(cart.mirroring());
}

/** MMC1 takes five writes to load one register, one bit at a time. */
void mmc1Write(Cartridge& cart, std::uint16_t address, std::uint8_t value) {
	for (int i = 0; i < 5; i++)
		cart.cpuWrite(address, static_cast<std::uint8_t>((value >> i) & 1));
}

} // namespace

/* ------------------------------------------------------------------------ */
/* Mapper 2: UxROM                                                           */
/* ------------------------------------------------------------------------ */

TEST_CASE("uxrom_switches_the_low_half_and_fixes_the_high_half") {
	auto cart = makeCart(2, 8, 0);            // 128 KB PRG, CHR RAM
	CHECK_EQ(cart->mapperNumber(), 2);

	CHECK_EQ(cart->cpuRead(0x8000), 0);       // powers up on bank 0
	CHECK_EQ(cart->cpuRead(0xC000), 7);       // last bank, always

	cart->cpuWrite(0x8000, 3);
	CHECK_EQ(cart->cpuRead(0x8000), 3);
	CHECK_EQ(cart->cpuRead(0xBFFF), 3);
	// The fixed half is what makes the board work: the vectors at $FFFA-$FFFF
	// have to stay readable whichever bank the game switched in.
	CHECK_EQ(cart->cpuRead(0xC000), 7);
	CHECK_EQ(cart->cpuRead(0xFFFF), 7);
}

TEST_CASE("uxrom_selects_from_any_address_in_the_rom_window") {
	auto cart = makeCart(2, 8, 0);
	// There is no address decoding on this board -- only the data matters.
	cart->cpuWrite(0xFFFF, 5);
	CHECK_EQ(cart->cpuRead(0x8000), 5);
}

TEST_CASE("uxrom_has_chr_ram") {
	auto cart = makeCart(2, 8, 0);            // no CHR banks declared
	cart->ppuWrite(0x0000, 0x42);
	CHECK_EQ(cart->ppuRead(0x0000), 0x42);
}

/* ------------------------------------------------------------------------ */
/* Mapper 3: CNROM                                                           */
/* ------------------------------------------------------------------------ */

TEST_CASE("cnrom_switches_character_banks_and_leaves_program_rom_alone") {
	auto cart = makeCart(3, 2, 4, BANK_16K, BANK_8K);   // 32 KB PRG, 32 KB CHR
	CHECK_EQ(cart->mapperNumber(), 3);

	CHECK_EQ(cart->ppuRead(0x0000), 0);
	CHECK_EQ(cart->cpuRead(0x8000), 0);
	CHECK_EQ(cart->cpuRead(0xC000), 1);       // 32 KB PRG, fixed, both banks visible

	cart->cpuWrite(0x8000, 2);
	CHECK_EQ(cart->ppuRead(0x0000), 2);
	CHECK_EQ(cart->ppuRead(0x1FFF), 2);
	// Program ROM does not move on this board.
	CHECK_EQ(cart->cpuRead(0x8000), 0);
	CHECK_EQ(cart->cpuRead(0xC000), 1);
}

TEST_CASE("cnrom_chr_rom_ignores_writes") {
	auto cart = makeCart(3, 2, 4, BANK_16K, BANK_8K);
	cart->ppuWrite(0x0000, 0x99);
	CHECK_EQ(cart->ppuRead(0x0000), 0);       // it is ROM
}

/* ------------------------------------------------------------------------ */
/* Mapper 7: AxROM                                                           */
/* ------------------------------------------------------------------------ */

TEST_CASE("axrom_switches_the_whole_program_space_at_once") {
	// Stamped in 32 KB units, because that is the bank size on this board.
	auto cart = makeCart(7, 8, 0, 32 * 1024);
	CHECK_EQ(cart->mapperNumber(), 7);

	CHECK_EQ(cart->cpuRead(0x8000), 0);
	CHECK_EQ(cart->cpuRead(0xFFFF), 0);

	cart->cpuWrite(0x8000, 2);
	CHECK_EQ(cart->cpuRead(0x8000), 2);
	// No fixed bank anywhere: every bank has to carry its own copy of the
	// vectors, which is a real constraint on how these games are built.
	CHECK_EQ(cart->cpuRead(0xFFFF), 2);
}

TEST_CASE("axrom_selects_the_single_screen") {
	auto cart = makeCart(7, 8, 0, 32 * 1024);
	CHECK_EQ(mirroringOf(*cart), "single-screen A");

	cart->cpuWrite(0x8000, 0x10);
	CHECK_EQ(mirroringOf(*cart), "single-screen B");

	cart->cpuWrite(0x8000, 0x00);
	CHECK_EQ(mirroringOf(*cart), "single-screen A");
}

/* ------------------------------------------------------------------------ */
/* Bus conflicts                                                             */
/* ------------------------------------------------------------------------ */

namespace {

/**
 * A mapper-3 cartridge whose PRG is stamped in 16 KB units, so the byte at any
 * address in the low bank is 0 and in the high bank is 1 -- easy to reason
 * about when checking what a write gets ANDed with.
 */
std::unique_ptr<Cartridge> conflictCart(int submapper, std::uint8_t byteAt8000) {
	testrom::Options o;
	o.mapper = 3;
	o.nes20 = true;
	o.submapper = submapper;
	o.prgBanks = 2;
	o.chrBanks = 4;

	std::vector<std::uint8_t> prg(2 * BANK_16K, 0xFF);
	prg[0] = byteAt8000;                       // the byte at $8000
	const std::vector<std::uint8_t> chr = stamped(4 * BANK_8K, BANK_8K);

	std::string error;
	auto cart = Cartridge::fromINes(testrom::build(o, prg, chr), &error);
	REQUIRE_MESSAGE(cart != nullptr, error);
	return cart;
}

} // namespace

TEST_CASE("the_header_decides_whether_a_board_has_bus_conflicts") {
	CHECK(conflictCart(2, 0xFF)->hasBusConflicts());        // submapper 2: yes
	CHECK_FALSE(conflictCart(1, 0xFF)->hasBusConflicts());  // submapper 1: no
	CHECK_FALSE(conflictCart(0, 0xFF)->hasBusConflicts());  // unstated: assume not

	// Nothing is inferred from the mapper number: an iNES 1.0 image of the same
    // board says nothing, so nothing is claimed.
	testrom::Options o;
	o.mapper = 3;
	auto plain = Cartridge::fromINes(testrom::build(o));
	REQUIRE(plain != nullptr);
	CHECK_FALSE(plain->hasBusConflicts());
	CHECK_EQ(plain->submapper(), 0);
}

TEST_CASE("a_write_is_anded_with_the_rom_underneath_it") {
	// Both the CPU and the ROM drive the bus during the write, and a line
	// pulled low by either side reads low.
	auto cart = conflictCart(2, 0x01);
	cart->cpuWrite(0x8000, 0x03);              // 0x03 AND 0x01 = 0x01
	CHECK_EQ(cart->ppuRead(0x0000), 1);

	// Without the conflict the same write would have selected bank 3.
	auto clean = conflictCart(1, 0x01);
	clean->cpuWrite(0x8000, 0x03);
	CHECK_EQ(clean->ppuRead(0x0000), 3);
}

TEST_CASE("the_safe_write_idiom_is_unaffected") {
	// How games written for these boards avoid the problem: write the bank
	// number to an address that already holds it, so the AND changes nothing.
	// Ikinari Musician does exactly this -- STA $808B with A = $21, where the
	// ROM byte at $808B is also $21.
	auto cart = conflictCart(2, 0x02);
	cart->cpuWrite(0x8000, 0x02);
	CHECK_EQ(cart->ppuRead(0x0000), 2);
}

TEST_CASE("a_write_against_all_ones_passes_through") {
	auto cart = conflictCart(2, 0xFF);
	cart->cpuWrite(0x8000, 0x02);
	CHECK_EQ(cart->ppuRead(0x0000), 2);
}

TEST_CASE("the_conflict_uses_the_bank_currently_mapped") {
	// The ROM byte that fights the write is whichever one is visible at that
	// address right now, not a fixed byte of the image.
	testrom::Options o;
	o.mapper = 2;                              // UxROM: the low half switches
	o.nes20 = true;
	o.submapper = 2;
	o.prgBanks = 4;
	o.chrBanks = 0;

	std::vector<std::uint8_t> prg(4 * BANK_16K, 0xFF);
	prg[0 * BANK_16K] = 0xFF;                  // bank 0 lets everything through
	prg[1 * BANK_16K] = 0x01;                  // bank 1 masks all but bit 0
	auto cart = Cartridge::fromINes(testrom::build(o, prg));
	REQUIRE(cart != nullptr);

	cart->cpuWrite(0x8000, 0x01);              // 0x01 AND 0xFF -> bank 1
	REQUIRE_EQ(cart->cpuRead(0x8000), 0x01);   // bank 1 is now mapped low

	// Now the byte at $8000 is 0x01, so a request for bank 3 is masked to 1.
	cart->cpuWrite(0x8000, 0x03);
	CHECK_EQ(cart->cpuRead(0x8000), 0x01);     // still bank 1
}

TEST_CASE("boards_with_decoded_registers_never_conflict") {
	// MMC1 and MMC3 decode their registers away from the ROM chip, and mapper
	// 87's register is in the work-RAM window where no ROM is driving. Even a
	// header claiming submapper 2 must not make those AND anything.
	for (int mapper : { 1, 4, 87 }) {
		testrom::Options o;
		o.mapper = mapper;
		o.nes20 = true;
		o.submapper = 2;
		o.prgBanks = 8;
		o.chrBanks = 4;
		auto cart = Cartridge::fromINes(testrom::build(o));
		REQUIRE(cart != nullptr);
		CHECK_MESSAGE(!cart->hasBusConflicts(), "mapper ", mapper);
	}
}

/* ------------------------------------------------------------------------ */
/* Mapper 87: a bank register in the work-RAM window                         */
/* ------------------------------------------------------------------------ */

TEST_CASE("mapper87_selects_chr_banks_from_the_work_ram_window") {
	auto cart = makeCart(87, 2, 4, BANK_16K, BANK_8K);   // 32 KB PRG, 32 KB CHR
	CHECK_EQ(cart->mapperNumber(), 87);

	CHECK_EQ(cart->ppuRead(0x0000), 0);
	// $6000 is work RAM on most boards; on this one it is the bank register,
	// which is only safe because the board carries no RAM there.
	cart->cpuWrite(0x6000, 0x02);
	CHECK_EQ(cart->ppuRead(0x0000), 1);
	CHECK_EQ(cart->ppuRead(0x1FFF), 1);
}

TEST_CASE("mapper87_crosses_the_two_bank_bits") {
	// The board wires value bit 0 to bank bit 1 and vice versa. Writing 1 gives
	// bank 2, not bank 1 -- the one thing separating this from plain CNROM.
	auto cart = makeCart(87, 2, 4, BANK_16K, BANK_8K);

	cart->cpuWrite(0x6000, 0x00);
	CHECK_EQ(cart->ppuRead(0x0000), 0);
	cart->cpuWrite(0x6000, 0x01);
	CHECK_EQ(cart->ppuRead(0x0000), 2);
	cart->cpuWrite(0x6000, 0x02);
	CHECK_EQ(cart->ppuRead(0x0000), 1);
	cart->cpuWrite(0x6000, 0x03);
	CHECK_EQ(cart->ppuRead(0x0000), 3);
}

TEST_CASE("mapper87_leaves_program_rom_fixed") {
	auto cart = makeCart(87, 2, 4, BANK_16K, BANK_8K);
	cart->cpuWrite(0x6000, 0x03);
	CHECK_EQ(cart->cpuRead(0x8000), 0);
	CHECK_EQ(cart->cpuRead(0xC000), 1);      // 32 KB, both halves visible
}

/* ------------------------------------------------------------------------ */
/* Mapper 1: MMC1                                                            */
/* ------------------------------------------------------------------------ */

TEST_CASE("mmc1_needs_five_writes_to_load_a_register") {
	auto cart = makeCart(1, 8, 0);
	REQUIRE_EQ(cart->cpuRead(0x8000), 0);

	// Four writes shift bits in but commit nothing.
	for (int i = 0; i < 4; i++)
		cart->cpuWrite(0xE000, 1);
	CHECK_EQ(cart->cpuRead(0x8000), 0);

	cart->cpuWrite(0xE000, 1);                // the fifth commits 0b11111 -> 15
	CHECK_EQ(cart->cpuRead(0x8000), 7);       // 15 wraps into 8 banks
}

TEST_CASE("mmc1_powers_up_with_the_last_bank_fixed_high") {
	// Nothing has configured the board yet, so the reset vector must still be
	// readable. That is only true in PRG mode 3.
	auto cart = makeCart(1, 8, 0);
	CHECK_EQ(cart->cpuRead(0x8000), 0);
	CHECK_EQ(cart->cpuRead(0xC000), 7);

	mmc1Write(*cart, 0xE000, 3);
	CHECK_EQ(cart->cpuRead(0x8000), 3);
	CHECK_EQ(cart->cpuRead(0xC000), 7);       // still fixed
}

TEST_CASE("mmc1_reset_abandons_a_partial_sequence") {
	auto cart = makeCart(1, 8, 0);
	mmc1Write(*cart, 0xE000, 3);
	REQUIRE_EQ(cart->cpuRead(0x8000), 3);

	// Three bits of a new value, then a reset.
	cart->cpuWrite(0xE000, 1);
	cart->cpuWrite(0xE000, 1);
	cart->cpuWrite(0xE000, 1);
	cart->cpuWrite(0xE000, 0x80);             // bit 7: reset
	CHECK_EQ(cart->cpuRead(0x8000), 3);       // nothing committed

	// The next sequence starts from scratch: if the three abandoned bits were
	// still in the register this would commit early, and on something else.
	mmc1Write(*cart, 0xE000, 5);
	CHECK_EQ(cart->cpuRead(0x8000), 5);
}

TEST_CASE("mmc1_reset_restores_the_fixed_high_bank") {
	auto cart = makeCart(1, 8, 0);
	mmc1Write(*cart, 0x8000, 0x00);           // 32 KB PRG mode
	REQUIRE_EQ(cart->cpuRead(0xC000), 1);     // second half of bank pair 0

	// A reset forces PRG mode 3 back on, which is how a game guarantees it can
	// reach the vectors after any state.
	cart->cpuWrite(0x8000, 0x80);
	CHECK_EQ(cart->cpuRead(0xC000), 7);
}

TEST_CASE("mmc1_switches_thirty_two_kilobytes_at_a_time") {
	auto cart = makeCart(1, 8, 0);
	mmc1Write(*cart, 0x8000, 0x00);           // control: PRG mode 0
	mmc1Write(*cart, 0xE000, 4);

	// The low bit of the bank number is ignored in this mode, so bank 4 and
	// bank 5 both select the pair starting at 4.
	CHECK_EQ(cart->cpuRead(0x8000), 4);
	CHECK_EQ(cart->cpuRead(0xC000), 5);

	mmc1Write(*cart, 0xE000, 5);
	CHECK_EQ(cart->cpuRead(0x8000), 4);
	CHECK_EQ(cart->cpuRead(0xC000), 5);
}

TEST_CASE("mmc1_fixes_the_first_bank_in_prg_mode_two") {
	auto cart = makeCart(1, 8, 0);
	mmc1Write(*cart, 0x8000, 0x08);           // PRG mode 2
	mmc1Write(*cart, 0xE000, 6);
	CHECK_EQ(cart->cpuRead(0x8000), 0);       // first bank, fixed
	CHECK_EQ(cart->cpuRead(0xC000), 6);       // switchable
}

TEST_CASE("mmc1_controls_mirroring") {
	auto cart = makeCart(1, 8, 0);
	mmc1Write(*cart, 0x8000, 0x00);
	CHECK_EQ(mirroringOf(*cart), "single-screen A");
	mmc1Write(*cart, 0x8000, 0x01);
	CHECK_EQ(mirroringOf(*cart), "single-screen B");
	mmc1Write(*cart, 0x8000, 0x02);
	CHECK_EQ(mirroringOf(*cart), "vertical");
	mmc1Write(*cart, 0x8000, 0x03);
	CHECK_EQ(mirroringOf(*cart), "horizontal");
}

TEST_CASE("mmc1_switches_character_banks_in_both_modes") {
	auto cart = makeCart(1, 2, 4, BANK_16K, 4096);   // stamped in 4 KB units

	// 8 KB mode: one bank, low bit ignored.
	mmc1Write(*cart, 0x8000, 0x03);           // CHR 8 KB, horizontal
	mmc1Write(*cart, 0xA000, 2);
	CHECK_EQ(cart->ppuRead(0x0000), 2);
	CHECK_EQ(cart->ppuRead(0x1000), 3);       // the pair's upper half

	// 4 KB mode: two independent banks.
	mmc1Write(*cart, 0x8000, 0x13);
	mmc1Write(*cart, 0xA000, 6);
	mmc1Write(*cart, 0xC000, 1);
	CHECK_EQ(cart->ppuRead(0x0000), 6);
	CHECK_EQ(cart->ppuRead(0x1000), 1);
}

/* ------------------------------------------------------------------------ */
/* MMC1's consecutive-write rule                                             */
/* ------------------------------------------------------------------------ */

/*
 * The board takes one write per five-bit sequence and needs a gap between them.
 * A read-modify-write instruction does not leave one: it writes the unmodified
 * byte and then the result, on back-to-back cycles, and MMC1 keeps only the
 * first. So an RMW against $8000-$FFFF loads a bit of what was already at that
 * address rather than of what the instruction computed.
 */

TEST_CASE("mmc1_ignores_the_second_write_of_a_pair") {
	auto cart = makeCart(1, 8, 0);

	// Five instructions, each writing twice. Only the first of each pair lands,
	// so the register loads 0b11111 rather than something built from all ten.
	for (int i = 0; i < 5; i++) {
		cart->beginInstruction();
		cart->cpuWrite(0xE000, 1);
		cart->cpuWrite(0xE000, 0);            // one cycle later: dropped
	}
	CHECK_EQ(cart->cpuRead(0x8000), 7);       // 31 wrapped into 8 banks

	// Take the pairs apart and the same ten writes mean something else, which
	// is what makes the rule observable at all.
	auto other = makeCart(1, 8, 0);
	for (int i = 0; i < 5; i++) {
		other->beginInstruction();
		other->cpuWrite(0xE000, 1);
		other->beginInstruction();
		other->cpuWrite(0xE000, 0);
	}
	CHECK_NE(other->cpuRead(0x8000), 7);
}

TEST_CASE("mmc1_takes_writes_at_face_value_until_told_about_instructions") {
	// Nothing here counts cycles; the rule needs a driver that reports where
	// instructions begin. Without one there is no evidence two writes were
	// adjacent, and inventing some would break every direct caller.
	auto cart = makeCart(1, 8, 0);
	for (int i = 0; i < 5; i++)
		cart->cpuWrite(0xE000, 1);
	CHECK_EQ(cart->cpuRead(0x8000), 7);
}

TEST_CASE("boards_with_decoded_registers_take_both_writes") {
	// MMC3 decodes its registers properly and has no serial protocol to
	// protect, so the pair is two ordinary writes and the second one wins.
	auto cart = makeCart(4, 8, 0, BANK_8K);
	cart->beginInstruction();
	cart->cpuWrite(0x8000, 6);                // select R6
	cart->cpuWrite(0x8001, 3);                // same instruction, still lands
	CHECK_EQ(cart->cpuRead(0x8000), 3);
}

TEST_CASE("a_read_modify_write_loads_the_byte_that_was_already_there") {
	// The end-to-end version: a real MMC1 cartridge, real INC instructions, and
	// the bank that comes out the other side.
	//
	// Code sits in the last bank, which mode 3 fixes at $C000 so it stays
	// reachable whatever the switchable half is doing. Each INC targets a byte
	// of that same bank whose bit 0 is the bit being shifted in.
	const int banks = 8;
	std::vector<std::uint8_t> prg = stamped(banks * BANK_16K, BANK_16K);
	const std::size_t last = static_cast<std::size_t>(banks - 1) * BANK_16K;

	// $E000-$E004 hold 1,1,1,0,0. Shifted in low bit first that is 0b00111 = 7.
	// Incremented first they would be 2,2,2,1,1 -- bits 0,0,0,1,1 = 24, which
	// wraps to bank 0. The two readings are as far apart as eight banks allow.
	const std::uint8_t bits[5] = { 1, 1, 1, 0, 0 };
	for (int i = 0; i < 5; i++)
		prg[last + 0x2000 + i] = bits[i];

	std::size_t p = last;                     // $C000, where execution starts
	for (int i = 0; i < 5; i++) {
		prg[p++] = 0xEE;                      // INC $E00i, absolute
		prg[p++] = static_cast<std::uint8_t>(i);
		prg[p++] = 0xE0;
	}
	prg[p++] = 0x4C;                          // JMP to itself
	prg[p++] = static_cast<std::uint8_t>((0xC000 + 15) & 0xFF);
	prg[p++] = 0xC0;

	prg[last + 0x3FFC] = 0x00;                // reset vector -> $C000
	prg[last + 0x3FFD] = 0xC0;

	testrom::Options o;
	o.mapper = 1;
	o.prgBanks = banks;
	o.chrBanks = 0;

	std::string error;
	auto cart = Cartridge::fromINes(testrom::build(o, prg), &error);
	REQUIRE_MESSAGE(cart != nullptr, error);
	Cartridge* raw = cart.get();

	Nes nes;
	nes.setCartridge(std::move(cart));
	nes.reset();
	REQUIRE_EQ(nes.cpuRegisters().pc, 0xC000);

	for (int i = 0; i < 5; i++)
		nes.step();

	// $9000 is a stamped byte of whichever bank is switched in low; the code and
	// the data bytes are elsewhere in the bank, so this still reads the stamp.
	CHECK_EQ(raw->cpuRead(0x9000), 7);

	// And the ROM is untouched: the writes reached the register, not the chip.
	CHECK_EQ(raw->cpuRead(0xE000), 1);
}

/* ------------------------------------------------------------------------ */
/* Mapper 4: MMC3                                                            */
/* ------------------------------------------------------------------------ */

TEST_CASE("mmc3_banks_program_rom_eight_kilobytes_at_a_time") {
	auto cart = makeCart(4, 8, 0, BANK_8K);   // 128 KB = 16 banks of 8 KB
	CHECK_EQ(cart->mapperNumber(), 4);

	cart->cpuWrite(0x8000, 6);                // select R6
	cart->cpuWrite(0x8001, 3);
	cart->cpuWrite(0x8000, 7);                // select R7
	cart->cpuWrite(0x8001, 5);

	CHECK_EQ(cart->cpuRead(0x8000), 3);       // R6
	CHECK_EQ(cart->cpuRead(0xA000), 5);       // R7
	CHECK_EQ(cart->cpuRead(0xC000), 14);      // second-to-last, fixed
	CHECK_EQ(cart->cpuRead(0xE000), 15);      // last, always: the vectors
}

TEST_CASE("mmc3_prg_mode_swaps_the_switchable_and_fixed_windows") {
	auto cart = makeCart(4, 8, 0, BANK_8K);
	cart->cpuWrite(0x8000, 6);
	cart->cpuWrite(0x8001, 3);

	cart->cpuWrite(0x8000, 0x40 | 6);         // PRG mode 1, still selecting R6
	CHECK_EQ(cart->cpuRead(0x8000), 14);      // now second-to-last is fixed low
	CHECK_EQ(cart->cpuRead(0xC000), 3);       // and R6 moved up
	CHECK_EQ(cart->cpuRead(0xE000), 15);      // the last bank never moves
}

TEST_CASE("mmc3_banks_character_rom_in_one_and_two_kilobyte_windows") {
	auto cart = makeCart(4, 2, 4, BANK_16K, 1024);   // 32 KB CHR = 32 banks

	cart->cpuWrite(0x8000, 0);                // R0: a 2 KB window at $0000
	cart->cpuWrite(0x8001, 4);
	CHECK_EQ(cart->ppuRead(0x0000), 4);
	CHECK_EQ(cart->ppuRead(0x0400), 5);       // the second half follows

	cart->cpuWrite(0x8000, 2);                // R2: a 1 KB window at $1000
	cart->cpuWrite(0x8001, 9);
	CHECK_EQ(cart->ppuRead(0x1000), 9);
	CHECK_EQ(cart->ppuRead(0x1400), 0);       // R3, still unset
}

TEST_CASE("mmc3_inversion_swaps_the_two_pattern_halves") {
	auto cart = makeCart(4, 2, 4, BANK_16K, 1024);
	cart->cpuWrite(0x8000, 0);
	cart->cpuWrite(0x8001, 4);                // R0 -> 2 KB at $0000
	cart->cpuWrite(0x8000, 2);
	cart->cpuWrite(0x8001, 9);                // R2 -> 1 KB at $1000
	REQUIRE_EQ(cart->ppuRead(0x0000), 4);
	REQUIRE_EQ(cart->ppuRead(0x1000), 9);

	// One write exchanges the background and sprite tile sets.
	cart->cpuWrite(0x8000, 0x80);
	CHECK_EQ(cart->ppuRead(0x1000), 4);
	CHECK_EQ(cart->ppuRead(0x0000), 9);
}

TEST_CASE("mmc3_controls_mirroring") {
	auto cart = makeCart(4, 8, 0, BANK_8K);
	cart->cpuWrite(0xA000, 0);
	CHECK_EQ(mirroringOf(*cart), "vertical");
	cart->cpuWrite(0xA000, 1);
	CHECK_EQ(mirroringOf(*cart), "horizontal");
}

TEST_CASE("a_four_screen_board_ignores_the_mirroring_register") {
	// The extra VRAM is soldered on; no register can undo that.
	testrom::Options o;
	o.mapper = 4;
	o.prgBanks = 8;
	o.chrBanks = 0;
	o.fourScreen = true;
	auto cart = Cartridge::fromINes(testrom::build(o));
	REQUIRE(cart != nullptr);

	CHECK_EQ(mirroringOf(*cart), "four-screen");
	cart->cpuWrite(0xA000, 1);
	CHECK_EQ(mirroringOf(*cart), "four-screen");
}

/* ------------------------------------------------------------------------ */
/* MMC3's scanline counter                                                   */
/* ------------------------------------------------------------------------ */

TEST_CASE("mmc3_irq_fires_after_the_latched_number_of_scanlines") {
	auto cart = makeCart(4, 8, 0, BANK_8K);

	cart->cpuWrite(0xC000, 4);                // latch
	cart->cpuWrite(0xC001, 0);                // request a reload
	cart->cpuWrite(0xE001, 0);                // enable

	// The first clock reloads rather than counting, so a latch of 4 fires on
	// the fifth scanline, not the fourth.
	for (int i = 0; i < 4; i++) {
		cart->ppuScanline();
		CHECK_FALSE(cart->irqAsserted());
	}
	cart->ppuScanline();
	CHECK(cart->irqAsserted());
}

TEST_CASE("mmc3_irq_is_acknowledged_by_disabling_it") {
	auto cart = makeCart(4, 8, 0, BANK_8K);
	cart->cpuWrite(0xC000, 1);
	cart->cpuWrite(0xC001, 0);
	cart->cpuWrite(0xE001, 0);
	cart->ppuScanline();
	cart->ppuScanline();
	REQUIRE(cart->irqAsserted());

	cart->cpuWrite(0xE000, 0);                // disable, which also acknowledges
	CHECK_FALSE(cart->irqAsserted());

	// And it stays quiet while disabled.
	for (int i = 0; i < 10; i++)
		cart->ppuScanline();
	CHECK_FALSE(cart->irqAsserted());
}

TEST_CASE("a_disabled_mmc3_counter_never_asserts") {
	auto cart = makeCart(4, 8, 0, BANK_8K);
	cart->cpuWrite(0xC000, 2);
	cart->cpuWrite(0xC001, 0);
	// $E001 never written: the counter runs but the line stays up.
	for (int i = 0; i < 20; i++)
		cart->ppuScanline();
	CHECK_FALSE(cart->irqAsserted());
}

TEST_CASE("the_ppu_clocks_the_counter_only_while_rendering") {
	auto cart = makeCart(4, 8, 0, BANK_8K);
	cart->cpuWrite(0xC000, 4);
	cart->cpuWrite(0xC001, 0);
	cart->cpuWrite(0xE001, 0);

	Ppu ppu(cart.get());
	ppu.reset();

	// Rendering off: A12 never toggles on real hardware, so nothing counts.
	ppu.writeRegister(1, 0x00);
	ppu.tick(341 * 300);
	CHECK_FALSE(cart->irqAsserted());

	// Rendering on: the counter runs down and the line comes up within a frame.
	ppu.writeRegister(1, Ppu::MASK_SHOW_BACKGROUND);
	ppu.tick(341 * 12);
	CHECK(cart->irqAsserted());
}

TEST_CASE("an_mmc3_irq_reaches_the_cpu") {
	// End to end: the board asserts, Nes::step merges it with the APU's line,
	// and the CPU vectors through $FFFE.
	std::vector<std::uint8_t> prg = stamped(8 * BANK_16K, BANK_8K);
	// The last 8 KB bank is always at $E000, so put the code and vectors there.
	const std::size_t last = prg.size() - BANK_8K;
	prg[last + 0x0000] = 0x58;                // CLI at $E000
	prg[last + 0x0001] = 0x4C;                // JMP $E001
	prg[last + 0x0002] = 0x01;
	prg[last + 0x0003] = 0xE0;
	prg[last + 0x0100] = 0xEA;                // NOP at $E100, the handler
	prg[last + 0x1FFC] = 0x00;                // reset  -> $E000
	prg[last + 0x1FFD] = 0xE0;
	prg[last + 0x1FFE] = 0x00;                // IRQ    -> $E100
	prg[last + 0x1FFF] = 0xE1;

	testrom::Options o;
	o.mapper = 4;
	o.prgBanks = 8;
	o.chrBanks = 0;
	auto cart = Cartridge::fromINes(testrom::build(o, prg));
	REQUIRE(cart != nullptr);

	Nes console;
	console.setCartridge(std::move(cart));
	console.reset();
	REQUIRE_EQ(console.cpuRegisters().pc, 0xE000);

	console.bus().write(0x2001, Ppu::MASK_SHOW_BACKGROUND);   // rendering on
	console.bus().write(0xC000, 4);           // latch
	console.bus().write(0xC001, 0);           // reload
	console.bus().write(0xE001, 0);           // enable

	bool vectored = false;
	for (int i = 0; i < 20000 && !vectored; i++) {
		console.step();
		if (console.cpuRegisters().pc >= 0xE100 && console.cpuRegisters().pc < 0xE110)
			vectored = true;
	}
	CHECK(vectored);
}

/* ------------------------------------------------------------------------ */
/* Mapper-controlled mirroring, through to the PPU                           */
/* ------------------------------------------------------------------------ */

TEST_CASE("a_mirroring_change_reaches_the_ppu_immediately") {
	// The PPU asks the cartridge on every nametable access rather than caching,
	// which is what lets a board change mirroring mid-frame.
	auto cart = makeCart(7, 8, 0, 32 * 1024);      // AxROM: single-screen
	Nes console;
	console.setCartridge(std::move(cart));
	console.reset();

	Ppu& ppu = console.ppu();
	ppu.vramWrite(0x2000, 0x11);
	ppu.vramWrite(0x2400, 0x22);
	// Single-screen A: both nametables are the same kilobyte, so the second
	// write landed on top of the first.
	CHECK_EQ(ppu.vramRead(0x2000), 0x22);
	CHECK_EQ(ppu.vramRead(0x2400), 0x22);

	console.bus().write(0x8000, 0x10);             // switch to screen B
	ppu.vramWrite(0x2000, 0x33);
	CHECK_EQ(ppu.vramRead(0x2400), 0x33);
	// Screen A still holds what it held; the board only changed which one the
	// PPU is looking at.
	console.bus().write(0x8000, 0x00);
	CHECK_EQ(ppu.vramRead(0x2000), 0x22);
}
