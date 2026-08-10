/*
 * testState.cpp -- save states, checked by determinism rather than by reading.
 *
 * The interesting test here is not "does a field come back". It is: run a
 * while, save, run on, load, run the same distance again, and compare. If the
 * two runs diverge by a single pixel then something the machine depends on was
 * not in the state, and the test says so without anybody having to guess which
 * field it was.
 *
 * That is worth more than a field-by-field check, because the failure mode of a
 * save state is always an omission, and an omission is exactly what a
 * field-by-field test cannot see.
 */

#include "nes/Nes.h"
#include "testRom.h"

#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace nes;

namespace {

/**
 * A ROM that keeps changing something, so that two runs can disagree.
 *
 * A program that sits in a loop doing nothing produces identical frames whether
 * or not the state was restored, which would make this test pass on an empty
 * implementation.
 */
std::vector<std::uint8_t> busyProgram() {
	std::vector<std::uint8_t> prg;
	// Turn rendering on, then walk the palette and the scroll registers forever,
	// so the picture, the PPU's address registers and the CPU's own registers
	// are all in motion when the state is taken.
	const std::uint8_t code[] = {
		0xA9, 0x1E,       // LDA #$1E
		0x8D, 0x01, 0x20, // STA $2001   -- show background and sprites
		0xA9, 0x00,       // LDA #$00
		0x8D, 0x00, 0x20, // STA $2000
		// loop:
		0xEE, 0x10, 0x00, // INC $0010
		0xAD, 0x10, 0x00, // LDA $0010
		0x8D, 0x06, 0x20, // STA $2006
		0x8D, 0x05, 0x20, // STA $2005
		0x69, 0x07,       // ADC #$07
		0x8D, 0x07, 0x20, // STA $2007
		0xCA,             // DEX
		0x88,             // DEY
		0x4C, 0x0A, 0x80  // JMP loop  ($800A)
	};
	prg.assign(code, code + sizeof(code));
	testrom::setResetVector(prg, 0x8000);
	return prg;
}

std::unique_ptr<Cartridge> busyCart(int mapper = 0, int prgBanks = 1) {
	testrom::Options o;
	o.mapper = mapper;
	o.prgBanks = prgBanks;
	o.chrBanks = 1;
	return Cartridge::fromINes(testrom::build(o, busyProgram()));
}

std::string tempPath() {
	static int counter = 0;
	return "nes_state_test_" + std::to_string(counter++) + ".tmp";
}

/** Every pixel of the picture, for comparing two runs. */
std::vector<std::uint8_t> screen(const Nes& console) {
	const std::uint8_t* frame = console.ppu().framebuffer();
	return std::vector<std::uint8_t>(frame,
			frame + Ppu::SCREEN_WIDTH * Ppu::SCREEN_HEIGHT);
}

} // namespace

TEST_CASE("a_restored_state_runs_the_same_as_the_run_it_came_from") {
	const std::string path = tempPath();

	// One run straight through: save at frame 20, keep going to 40, and
	// remember what the screen looked like.
	std::vector<std::uint8_t> expected;
	{
		Nes console;
		auto cart = busyCart();
		REQUIRE(cart != nullptr);
		console.setCartridge(std::move(cart));
		console.powerOn();
		for (int i = 0; i < 20; i++)
			console.stepFrame();

		std::string why;
		REQUIRE_MESSAGE(console.saveState(path, &why), why);

		for (int i = 0; i < 20; i++)
			console.stepFrame();
		expected = screen(console);
	}

	// A second console, brought to the same point by loading rather than by
	// running. If the state is complete these are identical; if anything was
	// left out, they are not.
	{
		Nes console;
		auto cart = busyCart();
		REQUIRE(cart != nullptr);
		console.setCartridge(std::move(cart));
		console.powerOn();

		std::string why;
		REQUIRE_MESSAGE(console.loadState(path, &why), why);

		for (int i = 0; i < 20; i++)
			console.stepFrame();
		CHECK(screen(console) == expected);
	}
	std::remove(path.c_str());
}

TEST_CASE("a_state_taken_mid_frame_resumes_mid_frame") {
	// Frames are the easy case. A state taken partway down the picture has to
	// carry the beam position, the half-drawn framebuffer and everything the
	// rest of the line depends on.
	const std::string path = tempPath();

	std::vector<std::uint8_t> expected;
	int savedScanline = -1;
	{
		Nes console;
		console.setCartridge(busyCart());
		console.powerOn();
		for (int i = 0; i < 12; i++)
			console.stepFrame();
		// Partway into a visible line, not at a frame boundary.
		while (console.ppu().scanline() != 120 || console.ppu().dot() < 100)
			console.step();
		savedScanline = console.ppu().scanline();

		std::string why;
		REQUIRE_MESSAGE(console.saveState(path, &why), why);
		for (int i = 0; i < 10; i++)
			console.stepFrame();
		expected = screen(console);
	}
	{
		Nes console;
		console.setCartridge(busyCart());
		console.powerOn();
		std::string why;
		REQUIRE_MESSAGE(console.loadState(path, &why), why);
		CHECK_EQ(console.ppu().scanline(), savedScanline);
		for (int i = 0; i < 10; i++)
			console.stepFrame();
		CHECK(screen(console) == expected);
	}
	std::remove(path.c_str());
}

TEST_CASE("an_mmc3_state_keeps_its_irq_counter") {
	// The board with the most state, and the one where losing it is most
	// visible: a stale counter puts the screen split on the wrong line.
	const std::string path = tempPath();

	std::vector<std::uint8_t> expected;
	{
		Nes console;
		console.setCartridge(busyCart(4, 8));
		console.powerOn();
		// Give the board a counter to remember.
		console.bus().write(0xC000, 32);
		console.bus().write(0xC001, 0);
		console.bus().write(0xE001, 0);
		for (int i = 0; i < 15; i++)
			console.stepFrame();

		std::string why;
		REQUIRE_MESSAGE(console.saveState(path, &why), why);
		for (int i = 0; i < 10; i++)
			console.stepFrame();
		expected = screen(console);
	}
	{
		Nes console;
		console.setCartridge(busyCart(4, 8));
		console.powerOn();
		std::string why;
		REQUIRE_MESSAGE(console.loadState(path, &why), why);
		for (int i = 0; i < 10; i++)
			console.stepFrame();
		CHECK(screen(console) == expected);
	}
	std::remove(path.c_str());
}

TEST_CASE("work_ram_survives_a_state") {
	const std::string path = tempPath();
	Nes console;
	console.setCartridge(busyCart(1, 2));    // MMC1 has work RAM
	console.powerOn();
	console.bus().write(0x6000, 0x5A);
	console.bus().write(0x0123, 0xC3);

	std::string why;
	REQUIRE_MESSAGE(console.saveState(path, &why), why);

	console.bus().write(0x6000, 0x00);
	console.bus().write(0x0123, 0x00);
	REQUIRE_MESSAGE(console.loadState(path, &why), why);

	CHECK_EQ(console.bus().peek(0x6000), 0x5A);   // the cartridge's RAM
	CHECK_EQ(console.bus().peek(0x0123), 0xC3);   // the console's own
	std::remove(path.c_str());
}

TEST_CASE("a_state_from_another_rom_is_refused") {
	const std::string path = tempPath();
	{
		Nes console;
		console.setCartridge(busyCart(0, 1));
		console.powerOn();
		console.stepFrame();
		REQUIRE(console.saveState(path));
	}

	// A different cartridge: same program, twice the PRG, different mapper.
	Nes other;
	other.setCartridge(busyCart(4, 8));
	other.powerOn();
	other.stepFrame();
	const std::vector<std::uint8_t> before = screen(other);

	std::string why;
	CHECK_FALSE(other.loadState(path, &why));
	CHECK(why.find("different ROM") != std::string::npos);
	// And it refused without disturbing anything, which is the part that
	// matters: a rejected file must leave the machine running.
	CHECK(screen(other) == before);
	std::remove(path.c_str());
}

TEST_CASE("a_truncated_state_is_refused_rather_than_half_loaded") {
	const std::string path = tempPath();
	{
		Nes console;
		console.setCartridge(busyCart());
		console.powerOn();
		for (int i = 0; i < 5; i++)
			console.stepFrame();
		REQUIRE(console.saveState(path));
	}

	// Chop it in half, the way a full disk or a killed process would.
	std::vector<std::uint8_t> bytes;
	{
		std::ifstream in(path.c_str(), std::ios::binary);
		bytes.assign(std::istreambuf_iterator<char>(in),
				std::istreambuf_iterator<char>());
	}
	{
		std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(bytes.data()),
				static_cast<std::streamsize>(bytes.size() / 2));
	}

	Nes console;
	console.setCartridge(busyCart());
	console.powerOn();
	std::string why;
	CHECK_FALSE(console.loadState(path, &why));
	CHECK(why.find("truncated") != std::string::npos);
	std::remove(path.c_str());
}

TEST_CASE("saving_needs_a_cartridge") {
	Nes console;
	std::string why;
	CHECK_FALSE(console.saveState(tempPath(), &why));
	CHECK_EQ(why, "no cartridge loaded");
}
