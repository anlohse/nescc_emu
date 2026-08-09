/*
 * testHostServices.cpp -- the host's side of the plugin boundary.
 *
 * These go through the C struct rather than calling the C++ methods, because
 * the struct is what a plugin actually sees: a mistake in how it is filled in
 * -- a null where a function should be, a size that stops short of a field --
 * is invisible from the C++ side and fatal from the other.
 *
 * The settings service is worth this much care for one reason: it is the only
 * part of the boundary where the host writes a file on a plugin's say-so, and
 * getting the buffer contract wrong there is a buffer overrun in somebody
 * else's module.
 */

#include <doctest/doctest.h>

#include "../src/frontend/HostServices.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

using nesfe::HostServices;
using nesgui::Config;

namespace {

std::string tempPath() {
	static int counter = 0;
	return "nes_host_test_" + std::to_string(counter++) + ".tmp";
}

} // namespace

TEST_CASE("the_struct_a_plugin_receives_is_filled_in") {
	Config config = Config::defaults();
	HostServices host(&config, std::string());
	const nes_host* api = host.handle();

	REQUIRE(api != nullptr);
	CHECK_EQ(api->size, sizeof(nes_host));
	CHECK_EQ(api->context, static_cast<const void*>(&host));

	// The size has to cover the fields added after version 1, or a plugin
	// built against this header would decline to call them -- which is the
	// check working, and would look exactly like the feature not existing.
	CHECK(NES_HOST_PROVIDES(api, get_setting));
	CHECK(NES_HOST_PROVIDES(api, set_setting));
	CHECK(NES_HOST_PROVIDES(api, log));
}

TEST_CASE("a_setting_survives_the_round_trip") {
	Config config = Config::defaults();
	HostServices host(&config, std::string());
	const nes_host* api = host.handle();

	char value[32] = "untouched";
	CHECK_EQ(api->get_setting(api->context, "sdl-audio", "volume", value, sizeof(value)), 0u);
	CHECK_EQ(std::string(value), "");   // absent reads as empty, not as garbage

	api->set_setting(api->context, "sdl-audio", "volume", "70");
	CHECK_EQ(api->get_setting(api->context, "sdl-audio", "volume", value, sizeof(value)), 2u);
	CHECK_EQ(std::string(value), "70");
}

TEST_CASE("one_plugins_settings_are_not_anothers") {
	Config config = Config::defaults();
	HostServices host(&config, std::string());
	const nes_host* api = host.handle();

	api->set_setting(api->context, "sdl-audio", "device", "Speakers");
	api->set_setting(api->context, "sdl-video", "device", "Monitor");

	char value[32] = { 0 };
	api->get_setting(api->context, "sdl-audio", "device", value, sizeof(value));
	CHECK_EQ(std::string(value), "Speakers");
	api->get_setting(api->context, "sdl-video", "device", value, sizeof(value));
	CHECK_EQ(std::string(value), "Monitor");
}

TEST_CASE("a_value_too_long_for_the_buffer_is_cut_not_overrun") {
	Config config = Config::defaults();
	HostServices host(&config, std::string());
	const nes_host* api = host.handle();

	api->set_setting(api->context, "sdl-audio", "device",
			"A Very Long Sound Device Name Indeed");

	// Deliberately one byte of slack either side, checked afterwards: this is
	// the call that would corrupt a plugin's stack if the copy were wrong.
	char guarded[10];
	std::memset(guarded, '#', sizeof(guarded));
	const std::size_t length =
			api->get_setting(api->context, "sdl-audio", "device", guarded, 8);

	CHECK_EQ(length, 36u);                       // the full length, not what fit
	CHECK_EQ(std::string(guarded), "A Very ");   // seven characters and a NUL
	CHECK_EQ(guarded[7], '\0');
	CHECK_EQ(guarded[8], '#');                   // nothing written past the size
	CHECK_EQ(guarded[9], '#');
}

TEST_CASE("a_null_buffer_still_answers_the_length") {
	// So a plugin can ask how much room it needs before finding any.
	Config config = Config::defaults();
	HostServices host(&config, std::string());
	const nes_host* api = host.handle();

	api->set_setting(api->context, "sdl-audio", "device", "Speakers");
	CHECK_EQ(api->get_setting(api->context, "sdl-audio", "device", nullptr, 0), 8u);
}

TEST_CASE("writing_a_setting_writes_the_file") {
	const std::string path = tempPath();
	Config config = Config::defaults();
	HostServices host(&config, path);
	const nes_host* api = host.handle();

	api->set_setting(api->context, "sdl-audio", "volume", "40");
	CHECK_EQ(host.writes(), 1);

	// Persisted at once rather than at shutdown, so a setting changed in a
	// dialog survives the program being killed with the dialog still up.
	Config reloaded = Config::defaults();
	CHECK(reloaded.load(path));
	CHECK_EQ(reloaded.pluginSetting("sdl-audio", "volume"), "40");
	std::remove(path.c_str());
}

TEST_CASE("settings_of_a_plugin_that_is_not_installed_survive") {
	// The promise that makes one shared file acceptable: taking a config to a
	// machine without some plugin, running there, and taking it back must not
	// quietly strip that plugin's settings out.
	const std::string path = tempPath();
	{
		std::ofstream os(path.c_str(), std::ofstream::trunc);
		os << "[plugin.something-else]\n"
		   << "shader = crt-royale\n"
		   << "\n[video]\nscale = 2\n";
	}

	Config config = Config::defaults();
	std::string warnings;
	CHECK(config.load(path, &warnings));
	CHECK_EQ(warnings, "");           // not a key this program knows, not a complaint
	CHECK_EQ(config.pluginSetting("something-else", "shader"), "crt-royale");
	CHECK_EQ(config.scale, 2);

	CHECK(config.save(path));
	Config reloaded = Config::defaults();
	CHECK(reloaded.load(path));
	CHECK_EQ(reloaded.pluginSetting("something-else", "shader"), "crt-royale");
	std::remove(path.c_str());
}

TEST_CASE("a_setting_is_looked_up_however_it_was_capitalised") {
	// Section and key names elsewhere in the file are case-insensitive, and a
	// plugin should not be the one place where they are not.
	Config config = Config::defaults();
	config.setPluginSetting("SDL-Audio", "Volume", "55");
	CHECK_EQ(config.pluginSetting("sdl-audio", "volume"), "55");
	CHECK_EQ(config.pluginSetting("sdl-audio", "missing", "fallback"), "fallback");
}

TEST_CASE("services_with_nothing_behind_them_answer_rather_than_crash") {
	// The --settings path has no window and no console, and a plugin is
	// entitled to ask for both. Null is a real answer; a crash is not.
	Config config = Config::defaults();
	HostServices host(&config, std::string());
	const nes_host* api = host.handle();

	int width = -1;
	int height = -1;
	CHECK_EQ(api->get_frame(api->context, &width, &height), nullptr);
	CHECK_EQ(api->window_handle(api->context), nullptr);
	api->log(api->context, "a plugin said something");   // must not throw
}

TEST_CASE("a_frame_source_is_passed_through_once_it_exists") {
	Config config = Config::defaults();
	HostServices host(&config, std::string());
	const nes_host* api = host.handle();

	static std::uint8_t picture[4] = { 1, 2, 3, 4 };
	host.setFrameSource([](int* width, int* height) {
		if (width) *width = 2;
		if (height) *height = 2;
		return static_cast<const std::uint8_t*>(picture);
	});

	int width = 0;
	int height = 0;
	CHECK_EQ(api->get_frame(api->context, &width, &height), picture);
	CHECK_EQ(width, 2);
	CHECK_EQ(height, 2);
}
