/*
 * testRegion.cpp -- PAL versus NTSC.
 *
 * The region is not a label on the box. It changes the CPU clock, the number of
 * scanlines, the ratio between the two chips, and every table in the APU that
 * was derived from the clock. These check that the header is read correctly and
 * that the timing actually changes underneath.
 */

#include "TestRom.h"
#include "nes/Nes.h"

#include <doctest/doctest.h>

#include <memory>
#include <string>

using namespace nes;

namespace {

// doctest cannot stringify a scoped enum, so comparisons go through the name.
std::string name(Region region) {
	return toString(region);
}

std::string regionOf(const Cartridge& cart) {
	return name(cart.region());
}

/** A cartridge whose header declares @p timing in NES 2.0 byte 12. */
std::unique_ptr<Cartridge> nes20Cart(int timing) {
	testrom::Options o;
	o.nes20 = true;
	o.nes20Timing = timing;
	auto cart = Cartridge::fromINes(testrom::build(o));
	REQUIRE(cart != nullptr);
	return cart;
}

} // namespace

/* ------------------------------------------------------------------------ */
/* Reading the header                                                        */
/* ------------------------------------------------------------------------ */

TEST_CASE("nes20_byte_twelve_gives_the_region") {
	CHECK_EQ(regionOf(*nes20Cart(0)), "NTSC");
	CHECK_EQ(regionOf(*nes20Cart(1)), "PAL");
	// Multi-region images run on either console; NTSC is the sane default.
	CHECK_EQ(regionOf(*nes20Cart(2)), "NTSC");
	// Dendy is a PAL-region clone and uses PAL video timing.
	CHECK_EQ(regionOf(*nes20Cart(3)), "PAL");
}

TEST_CASE("ines_one_point_zero_falls_back_to_flags9") {
	testrom::Options o;
	o.inesPalFlag = true;
	auto pal = Cartridge::fromINes(testrom::build(o));
	REQUIRE(pal != nullptr);
	CHECK_EQ(regionOf(*pal), "PAL");
	CHECK_FALSE(pal->isNes20());

	o.inesPalFlag = false;
	auto ntsc = Cartridge::fromINes(testrom::build(o));
	REQUIRE(ntsc != nullptr);
	CHECK_EQ(regionOf(*ntsc), "NTSC");
}

TEST_CASE("nes20_timing_wins_over_the_old_flag") {
	// A NES 2.0 image carries both fields. Byte 12 is the authoritative one;
	// byte 9 predates it and many dumps leave it at zero regardless.
	testrom::Options o;
	o.nes20 = true;
	o.nes20Timing = 1;      // PAL
	o.inesPalFlag = false;  // the old flag disagrees
	auto cart = Cartridge::fromINes(testrom::build(o));
	REQUIRE(cart != nullptr);
	CHECK_EQ(regionOf(*cart), "PAL");
}

TEST_CASE("an_image_with_no_region_information_is_ntsc") {
	auto cart = Cartridge::fromINes(testrom::build(testrom::Options()));
	REQUIRE(cart != nullptr);
	CHECK_EQ(regionOf(*cart), "NTSC");
}

/* ------------------------------------------------------------------------ */
/* What the region changes                                                   */
/* ------------------------------------------------------------------------ */

TEST_CASE("pal_draws_fifty_extra_scanlines") {
	Ppu ppu;
	CHECK_EQ(ppu.scanlinesPerFrame(), 262);
	CHECK_EQ(ppu.preRenderScanline(), 261);

	ppu.setRegion(Region::Pal);
	// The extra lines are all blanking below the picture: the visible area and
	// the vblank scanline are unchanged, which is why most games just work.
	CHECK_EQ(ppu.scanlinesPerFrame(), 312);
	CHECK_EQ(ppu.preRenderScanline(), 311);
}

TEST_CASE("a_pal_frame_takes_312_scanlines_of_dots") {
	Ppu ppu;
	ppu.setRegion(Region::Pal);
	ppu.reset();

	const std::uint64_t frame = ppu.frame();
	ppu.tick(Ppu::DOTS_PER_SCANLINE * 312);
	CHECK_EQ(ppu.frame(), frame + 1);
	CHECK_EQ(ppu.scanline(), 0);
}

TEST_CASE("pal_has_no_odd_frame_dot_skip") {
	// NTSC drops one dot on odd frames to keep its colour phase aligned. PAL's
	// carrier does not divide the same way and has no such skip, so every PAL
	// frame is exactly the same length.
	Ppu ppu;
	ppu.setRegion(Region::Pal);
	ppu.reset();
	ppu.writeRegister(1, Ppu::MASK_SHOW_BACKGROUND);   // skip only applies when rendering

	for (int i = 0; i < 4; i++) {
		const std::uint64_t before = ppu.frame();
		ppu.tick(Ppu::DOTS_PER_SCANLINE * 312);
		CHECK_EQ(ppu.frame(), before + 1);
		CHECK_EQ(ppu.dot(), 0);
	}
}

TEST_CASE("the_console_runs_pal_at_the_right_speed") {
	// The whole point: 16 PPU dots per 5 CPU cycles instead of 3 per 1, and a
	// 312-line frame. That works out to 33247.5 CPU cycles per frame -- which
	// is not a whole number, so the leftover has to carry between steps.
	std::vector<std::uint8_t> prg;
	testrom::setResetVector(prg, 0xC000);
	prg[0] = 0x4C;          // JMP $C000, a 3-cycle spin
	prg[1] = 0x00;
	prg[2] = 0xC0;

	testrom::Options o;
	o.nes20 = true;
	o.nes20Timing = 1;      // PAL
	auto cart = Cartridge::fromINes(testrom::build(o, prg));
	REQUIRE(cart != nullptr);

	Nes console;
	console.setCartridge(std::move(cart));
	console.reset();
	REQUIRE_EQ(name(console.region()), "PAL");

	const std::uint64_t startFrame = console.ppu().frame();
	const std::uint64_t startCycles = console.cycles();
	while (console.ppu().frame() < startFrame + 100)
		console.step();

	const double perFrame =
			static_cast<double>(console.cycles() - startCycles) / 100.0;
	// Within a few cycles: the loop can only stop on instruction boundaries.
	CHECK(perFrame > 33240.0);
	CHECK(perFrame < 33255.0);

	CHECK_EQ(console.cpuClockHz(), Apu::PAL_CPU_CLOCK_HZ);
	CHECK(console.frameRate() > 50.0);
	CHECK(console.frameRate() < 50.1);
}

TEST_CASE("an_ntsc_console_is_unchanged") {
	std::vector<std::uint8_t> prg;
	testrom::setResetVector(prg, 0xC000);
	prg[0] = 0x4C;
	prg[1] = 0x00;
	prg[2] = 0xC0;

	auto cart = Cartridge::fromINes(testrom::build(testrom::Options(), prg));
	REQUIRE(cart != nullptr);

	Nes console;
	console.setCartridge(std::move(cart));
	console.reset();
	CHECK_EQ(name(console.region()), "NTSC");
	CHECK_EQ(console.cpuClockHz(), Apu::CPU_CLOCK_HZ);

	const std::uint64_t startFrame = console.ppu().frame();
	const std::uint64_t startCycles = console.cycles();
	while (console.ppu().frame() < startFrame + 100)
		console.step();

	const double perFrame =
			static_cast<double>(console.cycles() - startCycles) / 100.0;
	CHECK(perFrame > 29775.0);
	CHECK(perFrame < 29786.0);
}

TEST_CASE("the_apu_frame_sequencer_is_retuned_for_pal") {
	// Same 240 Hz sequence, slower clock, so the boundaries move. The
	// observable is the frame IRQ: it must not arrive at the NTSC time.
	Apu apu;
	apu.setRegion(Region::Pal);
	apu.writeRegister(0x4017, 0x00);        // 4-step, IRQ enabled

	apu.tick(29830 + 20);                   // where NTSC would have fired
	CHECK_FALSE(apu.irqAsserted());

	apu.tick(33254 - 29830);                // on to the PAL boundary
	CHECK(apu.irqAsserted());
}

TEST_CASE("pal_noise_periods_differ_from_ntsc") {
	// The tables aim at the same pitches from a slower clock, so they are not
	// a simple scaling of each other -- entry 4 is 64 on NTSC and 60 on PAL.
	// Counting LFSR steps over a fixed window shows the difference.
	auto stepsIn = [](Region region, int cycles) {
		Apu apu;
		apu.setRegion(region);
		apu.writeRegister(0x400E, 0x04);    // period index 4
		std::uint16_t prev = apu.noiseShiftRegister();
		int steps = 0;
		for (int i = 0; i < cycles; i += 2) {
			apu.tick(2);
			if (apu.noiseShiftRegister() != prev) {
				prev = apu.noiseShiftRegister();
				steps++;
			}
		}
		return steps;
	};

	const int ntsc = stepsIn(Region::Ntsc, 100000);
	const int pal = stepsIn(Region::Pal, 100000);
	CHECK(pal > ntsc);                      // shorter period, more steps
}
