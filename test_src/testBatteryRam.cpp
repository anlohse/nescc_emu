/*
 * testBatteryRam.cpp -- cartridge RAM that survives a power cycle.
 *
 * The failure that matters is silent: a game writes its save, the emulator
 * closes, and the bytes are gone. So these check the round trip end to end, and
 * that the awkward cases -- no battery, no file yet, a truncated file -- fail
 * safely rather than losing or refusing data.
 */

#include "TestRom.h"
#include "nes/Nes.h"

#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace nes;

namespace {

/** A scratch path that does not collide between test cases. */
std::string tempPath(const char* stem) {
	static int counter = 0;
	return std::string("nes_test_") + stem + "_" + std::to_string(counter++) + ".tmp";
}

std::unique_ptr<Cartridge> makeCart(bool battery) {
	testrom::Options o;
	o.battery = battery;
	std::string error;
	auto cart = Cartridge::fromINes(testrom::build(o), &error);
	REQUIRE_MESSAGE(cart != nullptr, error);
	return cart;
}

bool fileExists(const std::string& path) {
	std::ifstream is(path.c_str(), std::ifstream::binary);
	return is.good();
}

std::vector<std::uint8_t> readFile(const std::string& path) {
	std::ifstream is(path.c_str(), std::ifstream::binary);
	return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(is)),
			std::istreambuf_iterator<char>());
}

void writeFile(const std::string& path, const std::vector<std::uint8_t>& data) {
	std::ofstream os(path.c_str(), std::ofstream::binary | std::ofstream::trunc);
	os.write(reinterpret_cast<const char*>(data.data()),
			static_cast<std::streamsize>(data.size()));
}

} // namespace

/* ------------------------------------------------------------------------ */
/* Which cartridges have something to keep                                   */
/* ------------------------------------------------------------------------ */

TEST_CASE("only_a_cartridge_with_a_battery_has_persistent_ram") {
	CHECK(makeCart(true)->hasPersistentRam());
	// The board still has work RAM; it just was not wired to a battery, so its
	// contents are not ours to keep.
	CHECK_FALSE(makeCart(false)->hasPersistentRam());
}

TEST_CASE("the_save_path_replaces_the_rom_extension") {
	CHECK_EQ(Cartridge::batteryRamPathFor("zelda.nes"), "zelda.sav");
	CHECK_EQ(Cartridge::batteryRamPathFor("/roms/zelda.nes"), "/roms/zelda.sav");
	CHECK_EQ(Cartridge::batteryRamPathFor("C:\\roms\\zelda.nes"), "C:\\roms\\zelda.sav");
	// No extension: append rather than mangle.
	CHECK_EQ(Cartridge::batteryRamPathFor("zelda"), "zelda.sav");
	// A dot in a directory name is not an extension.
	CHECK_EQ(Cartridge::batteryRamPathFor("/my.roms/zelda"), "/my.roms/zelda.sav");
}

/* ------------------------------------------------------------------------ */
/* The round trip                                                            */
/* ------------------------------------------------------------------------ */

TEST_CASE("save_ram_survives_a_power_cycle") {
	const std::string path = tempPath("roundtrip");

	{
		auto cart = makeCart(true);
		cart->cpuWrite(0x6000, 0x4C);
		cart->cpuWrite(0x6001, 0x49);
		cart->cpuWrite(0x7FFF, 0x21);
		REQUIRE(cart->saveBatteryRam(path));
	}

	{
		// A brand new cartridge, as if the emulator had been restarted.
		auto cart = makeCart(true);
		REQUIRE_EQ(cart->cpuRead(0x6000), 0);
		REQUIRE(cart->loadBatteryRam(path));
		CHECK_EQ(cart->cpuRead(0x6000), 0x4C);
		CHECK_EQ(cart->cpuRead(0x6001), 0x49);
		CHECK_EQ(cart->cpuRead(0x7FFF), 0x21);
	}

	CHECK_EQ(readFile(path).size(), 0x2000);   // the whole 8 KB window
	std::remove(path.c_str());
}

TEST_CASE("a_cartridge_without_a_battery_writes_nothing") {
	const std::string path = tempPath("nobattery");
	auto cart = makeCart(false);
	cart->cpuWrite(0x6000, 0x99);

	// Succeeds, because there is nothing to do -- not because it failed.
	CHECK(cart->saveBatteryRam(path));
	CHECK_FALSE(fileExists(path));
}

/* ------------------------------------------------------------------------ */
/* The awkward cases                                                         */
/* ------------------------------------------------------------------------ */

TEST_CASE("a_missing_save_file_is_not_an_error") {
	// This is what every first run looks like, so it must not be reported as a
	// failure or the front-end will print an error on a perfectly normal start.
	auto cart = makeCart(true);
	std::string error = "untouched";
	CHECK(cart->loadBatteryRam(tempPath("absent"), &error));
	CHECK_EQ(error, "untouched");
	CHECK_EQ(cart->cpuRead(0x6000), 0);
}

TEST_CASE("a_short_save_file_loads_as_far_as_it_goes") {
	// A truncated save is still better than no save: keep what is there rather
	// than discarding a player's progress over a partial write.
	const std::string path = tempPath("short");
	writeFile(path, std::vector<std::uint8_t>{ 0x11, 0x22, 0x33 });

	auto cart = makeCart(true);
	CHECK(cart->loadBatteryRam(path));
	CHECK_EQ(cart->cpuRead(0x6000), 0x11);
	CHECK_EQ(cart->cpuRead(0x6001), 0x22);
	CHECK_EQ(cart->cpuRead(0x6002), 0x33);
	CHECK_EQ(cart->cpuRead(0x6003), 0);        // the rest stays as it was
	std::remove(path.c_str());
}

TEST_CASE("an_oversized_save_file_is_truncated_rather_than_overrunning") {
	const std::string path = tempPath("long");
	writeFile(path, std::vector<std::uint8_t>(0x4000, 0xAB));   // twice the window

	auto cart = makeCart(true);
	CHECK(cart->loadBatteryRam(path));
	CHECK_EQ(cart->cpuRead(0x6000), 0xAB);
	CHECK_EQ(cart->cpuRead(0x7FFF), 0xAB);
	std::remove(path.c_str());
}

TEST_CASE("save_ram_is_not_disturbed_by_a_reset") {
	// Reset clears console RAM but not the cartridge's -- a battery survives
	// the power switch, which is the entire point of it.
	auto cart = makeCart(true);
	Nes console;
	console.setCartridge(std::move(cart));
	console.bus().write(0x6000, 0x5A);
	console.reset();
	CHECK_EQ(console.bus().peek(0x6000), 0x5A);
	CHECK_EQ(console.bus().peek(0x0000), 0);   // console RAM was cleared
}

/* ------------------------------------------------------------------------ */
/* Through the console                                                       */
/* ------------------------------------------------------------------------ */

TEST_CASE("the_console_saves_and_reloads_beside_the_rom") {
	const std::string romPath = tempPath("cart") + ".nes";
	const std::string savePath = Cartridge::batteryRamPathFor(romPath);

	testrom::Options o;
	o.battery = true;
	std::vector<std::uint8_t> prg;
	testrom::setResetVector(prg, 0xC000);
	writeFile(romPath, testrom::build(o, prg));

	{
		Nes console;
		std::string error;
		REQUIRE_MESSAGE(console.loadRom(romPath, &error), error);
		CHECK_EQ(console.batteryRamPath(), savePath);

		console.bus().write(0x6000, 0xC0);
		console.bus().write(0x6001, 0xDE);
		REQUIRE(console.saveBatteryRam(&error));
	}

	{
		Nes console;
		std::string error;
		REQUIRE_MESSAGE(console.loadRom(romPath, &error), error);
		// Loaded automatically: a player should not have to ask for their save.
		CHECK_EQ(console.bus().peek(0x6000), 0xC0);
		CHECK_EQ(console.bus().peek(0x6001), 0xDE);
	}

	std::remove(savePath.c_str());
	std::remove(romPath.c_str());
}

TEST_CASE("a_console_with_no_battery_has_no_save_path") {
	const std::string romPath = tempPath("plain") + ".nes";
	std::vector<std::uint8_t> prg;
	testrom::setResetVector(prg, 0xC000);
	writeFile(romPath, testrom::build(testrom::Options(), prg));

	Nes console;
	std::string error;
	REQUIRE_MESSAGE(console.loadRom(romPath, &error), error);
	CHECK(console.batteryRamPath().empty());
	CHECK(console.saveBatteryRam(&error));     // a no-op, and not a failure

	std::remove(romPath.c_str());
}
