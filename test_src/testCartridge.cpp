/*
 * testCartridge.cpp -- iNES container parsing and mapper 0 behaviour.
 */

#include "TestRom.h"
#include "nes/Cartridge.h"

#include <doctest/doctest.h>

#include <string>

using namespace nes;

namespace {
// doctest cannot stringify a scoped enum, and comparing the names gives a far
// more readable failure than two integers would.
std::string mirroringOf(const Cartridge& c) { return toString(c.mirroring()); }
} // namespace

TEST_CASE("rejects_images_that_are_not_ines") {
	std::string error;

	SUBCASE("too short for a header") {
		std::vector<std::uint8_t> tiny(4, 0);
		CHECK(Cartridge::fromINes(tiny, &error) == nullptr);
		CHECK(error.find("header") != std::string::npos);
	}
	SUBCASE("bad magic") {
		std::vector<std::uint8_t> image(32, 0);
		image[0] = 'X';
		CHECK(Cartridge::fromINes(image, &error) == nullptr);
		CHECK(error.find("magic") != std::string::npos);
	}
	SUBCASE("no PRG banks declared") {
		testrom::Options o;
		o.prgBanks = 0;
		CHECK(Cartridge::fromINes(testrom::build(o), &error) == nullptr);
		CHECK(error.find("PRG") != std::string::npos);
	}
	SUBCASE("truncated body") {
		testrom::Options o;
		o.truncate = true;
		CHECK(Cartridge::fromINes(testrom::build(o), &error) == nullptr);
		CHECK(error.find("truncated") != std::string::npos);
	}
	SUBCASE("unsupported mapper") {
		testrom::Options o;
		o.mapper = 9;                       // MMC2, which switches banks on reads
		CHECK(Cartridge::fromINes(testrom::build(o), &error) == nullptr);
		CHECK(error.find("mapper 9") != std::string::npos);
		// The message should say what *is* available, not just what is not.
		CHECK(error.find("MMC3") != std::string::npos);
	}
}

TEST_CASE("parses_header_fields") {
	testrom::Options o;
	o.prgBanks = 2;
	o.chrBanks = 1;
	o.battery = true;
	o.verticalMirroring = true;

	std::string error;
	auto cart = Cartridge::fromINes(testrom::build(o), &error);
	REQUIRE_MESSAGE(cart != nullptr, error);

	CHECK_EQ(cart->mapperNumber(), 0);
	CHECK_EQ(cart->prgSize(), 32u * 1024u);
	CHECK_EQ(cart->chrSize(), 8u * 1024u);
	CHECK(cart->hasBattery());
	CHECK_EQ(mirroringOf(*cart), "vertical");
	CHECK_FALSE(cart->isNes20());
}

TEST_CASE("mirroring_flags") {
	testrom::Options o;

	SUBCASE("horizontal by default") {
		auto cart = Cartridge::fromINes(testrom::build(o));
		REQUIRE(cart != nullptr);
		CHECK_EQ(mirroringOf(*cart), "horizontal");
	}
	SUBCASE("vertical bit") {
		o.verticalMirroring = true;
		auto cart = Cartridge::fromINes(testrom::build(o));
		REQUIRE(cart != nullptr);
		CHECK_EQ(mirroringOf(*cart), "vertical");
	}
	SUBCASE("four-screen overrides the vertical bit") {
		o.verticalMirroring = true;
		o.fourScreen = true;
		auto cart = Cartridge::fromINes(testrom::build(o));
		REQUIRE(cart != nullptr);
		CHECK_EQ(mirroringOf(*cart), "four-screen");
	}
}

TEST_CASE("mapper_number_spans_both_nibbles") {
	// The low nibble lives in the top of byte 6, the high nibble in the top of
	// byte 7. Mapper 0 is the only one implemented, so check the decode via the
	// error message for a mapper that needs both nibbles.
	testrom::Options o;
	o.mapper = 0x40;                        // high nibble only
	std::string error;
	CHECK(Cartridge::fromINes(testrom::build(o), &error) == nullptr);
	CHECK_MESSAGE(error.find("mapper 64") != std::string::npos, error);
}

TEST_CASE("nes20_images_are_read_as_ines1") {
	testrom::Options o;
	o.nes20 = true;
	auto cart = Cartridge::fromINes(testrom::build(o));
	REQUIRE(cart != nullptr);
	CHECK(cart->isNes20());
	CHECK_EQ(cart->mapperNumber(), 0);
}

TEST_CASE("trainer_shifts_the_prg_offset") {
	// With a trainer present, PRG starts 512 bytes later. If the loader ignored
	// it, the first PRG byte would read as the trainer's filler.
	std::vector<std::uint8_t> prg;
	testrom::setResetVector(prg, 0xC000);
	prg[0] = 0x5A;

	testrom::Options o;
	o.trainer = true;

	auto cart = Cartridge::fromINes(testrom::build(o, prg));
	REQUIRE(cart != nullptr);
	CHECK_EQ(cart->cpuRead(0x8000), 0x5A);   // not 0xAA, the trainer filler
}

TEST_CASE("nrom_16k_mirrors_across_both_halves") {
	std::vector<std::uint8_t> prg;
	testrom::setResetVector(prg, 0xC123);
	prg[0] = 0x11;
	prg[0x3FFF] = 0x22;

	testrom::Options o;
	o.prgBanks = 1;
	auto cart = Cartridge::fromINes(testrom::build(o, prg));
	REQUIRE(cart != nullptr);

	CHECK_EQ(cart->cpuRead(0x8000), 0x11);
	CHECK_EQ(cart->cpuRead(0xC000), 0x11);   // same bank, mirrored
	CHECK_EQ(cart->cpuRead(0xBFFF), 0x22);
	CHECK_EQ(cart->cpuRead(0xFFFF), 0x22);

	// The reset vector must resolve through the mirror.
	CHECK_EQ(cart->cpuRead(0xFFFC), 0x23);
	CHECK_EQ(cart->cpuRead(0xFFFD), 0xC1);
}

TEST_CASE("nrom_32k_maps_straight_through") {
	std::vector<std::uint8_t> prg(32768, 0);
	prg[0] = 0x11;
	prg[0x4000] = 0x33;                      // start of the second 16 KB
	prg[0x7FFF] = 0x44;

	testrom::Options o;
	o.prgBanks = 2;
	auto cart = Cartridge::fromINes(testrom::build(o, prg));
	REQUIRE(cart != nullptr);

	CHECK_EQ(cart->cpuRead(0x8000), 0x11);
	CHECK_EQ(cart->cpuRead(0xC000), 0x33);   // distinct halves, no mirroring
	CHECK_EQ(cart->cpuRead(0xFFFF), 0x44);
}

TEST_CASE("nrom_program_rom_is_read_only") {
	std::vector<std::uint8_t> prg;
	testrom::setResetVector(prg, 0xC000);
	prg[0] = 0x11;

	auto cart = Cartridge::fromINes(testrom::build(testrom::Options(), prg));
	REQUIRE(cart != nullptr);

	cart->cpuWrite(0x8000, 0xFF);
	CHECK_EQ(cart->cpuRead(0x8000), 0x11);   // NROM has no registers to hit
}

TEST_CASE("nrom_work_ram_at_6000") {
	auto cart = Cartridge::fromINes(testrom::build(testrom::Options()));
	REQUIRE(cart != nullptr);

	cart->cpuWrite(0x6000, 0xAB);
	cart->cpuWrite(0x7FFF, 0xCD);
	CHECK_EQ(cart->cpuRead(0x6000), 0xAB);
	CHECK_EQ(cart->cpuRead(0x7FFF), 0xCD);

	CHECK_EQ(cart->cpuRead(0x4020), 0);      // unmapped: open bus
}

TEST_CASE("chr_rom_is_read_only_but_chr_ram_is_writable") {
	SUBCASE("with CHR ROM") {
		std::vector<std::uint8_t> chr(8192, 0);
		chr[0] = 0x77;
		testrom::Options o;
		o.chrBanks = 1;
		auto cart = Cartridge::fromINes(testrom::build(o, std::vector<std::uint8_t>(), chr));
		REQUIRE(cart != nullptr);
		CHECK_EQ(cart->chrSize(), 8192u);
		CHECK_EQ(cart->ppuRead(0x0000), 0x77);
		cart->ppuWrite(0x0000, 0xFF);
		CHECK_EQ(cart->ppuRead(0x0000), 0x77);
	}
	SUBCASE("zero CHR banks means CHR RAM") {
		testrom::Options o;
		o.chrBanks = 0;
		auto cart = Cartridge::fromINes(testrom::build(o));
		REQUIRE(cart != nullptr);
		CHECK_EQ(cart->chrSize(), 0u);
		cart->ppuWrite(0x0123, 0xEE);
		CHECK_EQ(cart->ppuRead(0x0123), 0xEE);
	}
}
