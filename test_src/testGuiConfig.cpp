/*
 * testGuiConfig.cpp -- the front-end's configuration file.
 *
 * Parsers fail quietly, which is the problem: a typo'd binding that is silently
 * dropped looks exactly like a binding that does not work. So as well as the
 * happy path, these check that bad input is reported rather than swallowed, and
 * that one bad line does not take the rest of the file with it.
 *
 * Built only when the SDL front-end is, since the names come from SDL.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/GuiConfig.h"

#include <cstdio>
#include <fstream>
#include <string>

using nesgui::Config;

namespace {

// A, B, Select, Start, Up, Down, Left, Right.
enum { A = 0, B, SELECT, START, UP, DOWN, LEFT, RIGHT };

std::string writeTemp(const std::string& contents) {
	static int counter = 0;
	const std::string path = "nes_cfg_test_" + std::to_string(counter++) + ".tmp";
	std::ofstream os(path.c_str(), std::ofstream::trunc);
	os << contents;
	return path;
}

} // namespace

TEST_CASE("defaults_are_sane") {
	const Config c = Config::defaults();
	CHECK_EQ(c.keys[0][A], SDL_SCANCODE_Z);
	CHECK_EQ(c.keys[0][START], SDL_SCANCODE_RETURN);
	CHECK_EQ(c.keys[1][UP], SDL_SCANCODE_KP_8);
	CHECK_EQ(c.padButtons[0][A], SDL_CONTROLLER_BUTTON_A);
	CHECK_EQ(c.padButtons[0][LEFT], SDL_CONTROLLER_BUTTON_DPAD_LEFT);
	CHECK_EQ(c.scale, 3);
	CHECK_FALSE(c.fullscreen);
	CHECK(c.audio);
}

TEST_CASE("a_port_starts_on_the_keyboard_and_can_be_given_a_named_gamepad") {
	// Which device drives a port is chosen, not detected, and the choice is what
	// gets saved. Both ports begin on the keyboard because that is the one device
	// certain to be attached -- and because a pad silently taking a port is how a
	// phantom controller from a twin USB adapter stole player one.
	const Config c = Config::defaults();
	CHECK_EQ(c.device[0], nesgui::PORT_KEYBOARD);
	CHECK_EQ(c.device[1], nesgui::PORT_KEYBOARD);

	Config loaded = Config::defaults();
	std::string warnings;
	const std::string path = writeTemp(
			"[controller1]\ndevice = gamepad\ngamepad = 2\n"
			"[controller2]\ndevice = keyboard\n");
	REQUIRE(loaded.load(path, &warnings));
	CHECK_EQ(warnings, "");
	CHECK_EQ(loaded.device[0], nesgui::PORT_GAMEPAD);
	// Written 1-based for a person, held 0-based for an array.
	CHECK_EQ(loaded.gamepad[0], 1);
	CHECK_EQ(loaded.device[1], nesgui::PORT_KEYBOARD);
}

TEST_CASE("a_gamepad_number_outside_the_list_is_refused_rather_than_stored") {
	// A number nothing can be read from would look like a working setting and
	// behave like a dead controller, which is the failure this whole change exists
	// to stop happening quietly.
	Config c = Config::defaults();
	std::string warnings;
	const std::string path = writeTemp("[controller1]\ndevice = gamepad\ngamepad = 9\n");
	REQUIRE(c.load(path, &warnings));
	CHECK(warnings.find("gamepad must be") != std::string::npos);
	CHECK_EQ(c.gamepad[0], 0);           // left as it was

	Config bad = Config::defaults();
	warnings.clear();
	const std::string other = writeTemp("[controller1]\ndevice = mouse\n");
	REQUIRE(bad.load(other, &warnings));
	CHECK(warnings.find("'mouse'") != std::string::npos);
	CHECK_EQ(bad.device[0], nesgui::PORT_KEYBOARD);
}

TEST_CASE("the_device_choice_survives_a_save_and_load") {
	Config c = Config::defaults();
	c.device[0] = nesgui::PORT_GAMEPAD;
	c.gamepad[0] = 3;
	c.device[1] = nesgui::PORT_GAMEPAD;
	c.gamepad[1] = 0;

	const std::string path = writeTemp("");
	REQUIRE(c.save(path));

	Config back;
	std::string warnings;
	REQUIRE(back.load(path, &warnings));
	CHECK_EQ(warnings, "");
	CHECK_EQ(back.device[0], nesgui::PORT_GAMEPAD);
	CHECK_EQ(back.gamepad[0], 3);
	CHECK_EQ(back.device[1], nesgui::PORT_GAMEPAD);
	CHECK_EQ(back.gamepad[1], 0);
}

TEST_CASE("a_missing_file_leaves_the_defaults_alone") {
	Config c = Config::defaults();
	std::string warnings = "untouched";
	// Every first run looks like this, so it must not be an error.
	CHECK(c.load("no_such_config_file.tmp", &warnings));
	CHECK_EQ(warnings, "untouched");
	CHECK_EQ(c.keys[0][A], SDL_SCANCODE_Z);
}

TEST_CASE("bindings_are_read_back") {
	const std::string path = writeTemp(
		"[keyboard1]\n"
		"a = K\n"
		"start = Space\n"
		"[keyboard2]\n"
		"b = L\n"
		"[pad1]\n"
		"a = y\n"
		"select = leftshoulder\n");

	Config c = Config::defaults();
	std::string warnings;
	REQUIRE(c.load(path, &warnings));
	CHECK_MESSAGE(warnings.empty(), warnings);

	CHECK_EQ(c.keys[0][A], SDL_SCANCODE_K);
	CHECK_EQ(c.keys[0][START], SDL_SCANCODE_SPACE);
	CHECK_EQ(c.keys[1][B], SDL_SCANCODE_L);
	CHECK_EQ(c.padButtons[0][A], SDL_CONTROLLER_BUTTON_Y);
	CHECK_EQ(c.padButtons[0][SELECT], SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
	// Anything the file did not mention keeps its default.
	CHECK_EQ(c.keys[0][B], SDL_SCANCODE_X);
	std::remove(path.c_str());
}

TEST_CASE("video_and_audio_settings_are_read") {
	const std::string path = writeTemp(
		"[video]\n"
		"scale = 5\n"
		"fullscreen = yes\n"
		"[audio]\n"
		"enabled = off\n");

	Config c = Config::defaults();
	std::string warnings;
	REQUIRE(c.load(path, &warnings));
	CHECK_MESSAGE(warnings.empty(), warnings);
	CHECK_EQ(c.scale, 5);
	CHECK(c.fullscreen);
	CHECK_FALSE(c.audio);
	std::remove(path.c_str());
}

TEST_CASE("comments_and_blank_lines_are_ignored") {
	const std::string path = writeTemp(
		"# a leading comment\n"
		"\n"
		"[video]   ; a section, with a trailing comment\n"
		"   scale = 4    # indented, and commented\n"
		"\n");

	Config c = Config::defaults();
	std::string warnings;
	REQUIRE(c.load(path, &warnings));
	CHECK_MESSAGE(warnings.empty(), warnings);
	CHECK_EQ(c.scale, 4);
	std::remove(path.c_str());
}

TEST_CASE("a_bad_binding_is_reported_and_skipped") {
	// The failure this guards against: a typo'd key name being dropped in
	// silence, which is indistinguishable from a binding that does not work.
	const std::string path = writeTemp(
		"[keyboard1]\n"
		"a = NotAKeyName\n"
		"b = Q\n");

	Config c = Config::defaults();
	std::string warnings;
	REQUIRE(c.load(path, &warnings));
	CHECK(warnings.find("NotAKeyName") != std::string::npos);
	CHECK_EQ(c.keys[0][A], SDL_SCANCODE_Z);    // left at its default
	CHECK_EQ(c.keys[0][B], SDL_SCANCODE_Q);    // and the rest still applied
	std::remove(path.c_str());
}

TEST_CASE("unknown_names_and_malformed_lines_are_reported") {
	const std::string path = writeTemp(
		"[keyboard1]\n"
		"turbo = T\n"                 // not one of the eight buttons
		"this line has no equals\n"
		"[nonsense]\n"
		"a = K\n"                     // a section nothing knows about
		"[video]\n"
		"scale = 99\n"                // out of range
		"fullscreen = maybe\n"        // not a boolean
		"colour = blue\n");           // not a setting

	Config c = Config::defaults();
	std::string warnings;
	REQUIRE(c.load(path, &warnings));

	CHECK(warnings.find("turbo") != std::string::npos);
	CHECK(warnings.find("expected key = value") != std::string::npos);
	CHECK(warnings.find("outside any known section") != std::string::npos);
	CHECK(warnings.find("scale must be") != std::string::npos);
	CHECK(warnings.find("fullscreen must be") != std::string::npos);
	CHECK(warnings.find("colour") != std::string::npos);

	// Nothing valid was lost, and nothing invalid was applied.
	CHECK_EQ(c.scale, 3);
	CHECK_FALSE(c.fullscreen);
	std::remove(path.c_str());
}

TEST_CASE("a_written_config_reads_back_identically") {
	// The round trip has to hold or editing the generated file would silently
	// change bindings the user never touched.
	Config original = Config::defaults();
	original.keys[0][A] = SDL_SCANCODE_KP_PERIOD;      // an awkward name
	original.keys[1][SELECT] = SDL_SCANCODE_LSHIFT;
	original.padButtons[1][B] = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
	original.scale = 6;
	original.fullscreen = true;
	original.audio = false;

	const std::string path = writeTemp("");
	REQUIRE(original.save(path));

	Config reloaded = Config::defaults();
	std::string warnings;
	REQUIRE(reloaded.load(path, &warnings));
	CHECK_MESSAGE(warnings.empty(), warnings);

	for (int port = 0; port < 2; port++) {
		for (int i = 0; i < 8; i++) {
			CHECK_EQ(reloaded.keys[port][i], original.keys[port][i]);
			CHECK_EQ(reloaded.padButtons[port][i], original.padButtons[port][i]);
		}
	}
	CHECK_EQ(reloaded.scale, original.scale);
	CHECK_EQ(reloaded.fullscreen, original.fullscreen);
	CHECK_EQ(reloaded.audio, original.audio);
	std::remove(path.c_str());
}

TEST_CASE("section_and_key_names_are_case_insensitive") {
	const std::string path = writeTemp(
		"[KeyBoard1]\n"
		"START = Space\n"
		"[VIDEO]\n"
		"Scale = 2\n");

	Config c = Config::defaults();
	std::string warnings;
	REQUIRE(c.load(path, &warnings));
	CHECK_MESSAGE(warnings.empty(), warnings);
	CHECK_EQ(c.keys[0][START], SDL_SCANCODE_SPACE);
	CHECK_EQ(c.scale, 2);
	std::remove(path.c_str());
}

/* ----------------------------------------------------------------------- */
/* Recent ROMs                                                              */
/* ----------------------------------------------------------------------- */

TEST_CASE("the_newest_rom_goes_to_the_front") {
	Config c = Config::defaults();
	c.noteRecentRom("a.nes");
	c.noteRecentRom("b.nes");
	REQUIRE_EQ(c.recentRoms.size(), 2u);
	CHECK_EQ(c.recentRoms[0], "b.nes");
	CHECK_EQ(c.recentRoms[1], "a.nes");
}

TEST_CASE("loading_the_same_rom_again_moves_it_rather_than_repeating_it") {
	Config c = Config::defaults();
	c.noteRecentRom("a.nes");
	c.noteRecentRom("b.nes");
	c.noteRecentRom("a.nes");
	REQUIRE_EQ(c.recentRoms.size(), 2u);
	CHECK_EQ(c.recentRoms[0], "a.nes");
	CHECK_EQ(c.recentRoms[1], "b.nes");
}

TEST_CASE("the_same_path_spelled_differently_is_the_same_rom") {
	// Windows hands back whatever spelling was typed, and the same game twice
	// in a list of recent games is exactly what nobody wants.
	Config c = Config::defaults();
	c.noteRecentRom("C:/Games/Zelda.nes");
	c.noteRecentRom("c:/games/zelda.NES");
	CHECK_EQ(c.recentRoms.size(), 1u);
	CHECK_EQ(c.recentRoms[0], "c:/games/zelda.NES");   // the newest spelling wins
}

TEST_CASE("the_list_is_capped") {
	Config c = Config::defaults();
	for (int i = 0; i < 20; i++)
		c.noteRecentRom(std::to_string(i) + ".nes", 8);
	CHECK_EQ(c.recentRoms.size(), 8u);
	CHECK_EQ(c.recentRoms[0], "19.nes");
	CHECK_EQ(c.recentRoms[7], "12.nes");
}

TEST_CASE("an_empty_path_is_not_remembered") {
	Config c = Config::defaults();
	c.noteRecentRom("");
	CHECK(c.recentRoms.empty());
}

TEST_CASE("recent_roms_survive_a_round_trip_in_order") {
	const std::string path = writeTemp("");
	Config c = Config::defaults();
	c.noteRecentRom("one.nes");
	c.noteRecentRom("two with spaces.nes");
	REQUIRE(c.save(path));

	Config back = Config::defaults();
	std::string warnings;
	REQUIRE(back.load(path, &warnings));
	CHECK_EQ(warnings, "");
	REQUIRE_EQ(back.recentRoms.size(), 2u);
	CHECK_EQ(back.recentRoms[0], "two with spaces.nes");
	CHECK_EQ(back.recentRoms[1], "one.nes");
	std::remove(path.c_str());
}

TEST_CASE("a_path_with_an_equals_sign_survives") {
	// The ordinal is the key and the path is the value, so a path may contain
	// anything at all -- which a naive "key = value" split on a path would not
	// have managed.
	const std::string path = writeTemp("");
	Config c = Config::defaults();
	c.noteRecentRom("D:/roms/weird = name.nes");
	REQUIRE(c.save(path));

	Config back = Config::defaults();
	REQUIRE(back.load(path));
	REQUIRE_EQ(back.recentRoms.size(), 1u);
	CHECK_EQ(back.recentRoms[0], "D:/roms/weird = name.nes");
	std::remove(path.c_str());
}
