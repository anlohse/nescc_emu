/*
 * testPluginSettings.cpp -- what the plugin dialog decides, without the dialog.
 *
 * A settings screen is the part of a program most likely to be wrong in ways
 * nobody notices for months: a choice that does not persist, a plugin missing
 * from the list, a Cancel that applies anyway, a stored name that no longer
 * matches anything installed. None of those need a window to catch, and none of
 * them are caught by looking at one.
 */

#include "../src/frontend/PluginSettings.h"

#include <doctest/doctest.h>

#include <string>

using namespace nesfe;
using nesplug::Registry;

namespace {

/* Descriptors only: nothing here is ever instantiated. */
const nes_plugin_info VIDEO_A = {
	sizeof(nes_plugin_info), "video-a", "Video A", "1.0", NES_PLUGIN_VIDEO
};
const nes_plugin_info VIDEO_B = {
	sizeof(nes_plugin_info), "video-b", "Video B", "2.1", NES_PLUGIN_VIDEO
};
const nes_plugin_info AUDIO_A = {
	sizeof(nes_plugin_info), "audio-a", "Audio A", "1.0", NES_PLUGIN_AUDIO
};
const nes_plugin_info INPUT_A = {
	sizeof(nes_plugin_info), "input-a", "Input A", "1.0", NES_PLUGIN_INPUT
};

void noop(void*) { }

/** A video api that offers a settings dialog. */
const nes_video_api VIDEO_WITH_DIALOG = {
	sizeof(nes_video_api),
	nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
	noop,          // configure
	nullptr
};

/** The same board with the dialog left unimplemented. */
const nes_video_api VIDEO_WITHOUT_DIALOG = {
	sizeof(nes_video_api),
	nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
	nullptr,       // no configure
	nullptr
};

const nes_audio_api AUDIO_PLAIN = {
	sizeof(nes_audio_api),
	nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
	nullptr
};

const nes_input_api INPUT_PLAIN = {
	sizeof(nes_input_api),
	nullptr, nullptr, nullptr, nullptr, nullptr,
	nullptr
};

/** Registry with two video plugins, one audio and one input. */
Registry populated() {
	Registry registry;
	registry.add(NES_PLUGIN_ABI_VERSION, &VIDEO_A, &VIDEO_WITHOUT_DIALOG);
	registry.add(NES_PLUGIN_ABI_VERSION, &VIDEO_B, &VIDEO_WITH_DIALOG,
			"plugins/video_b.dll");
	registry.add(NES_PLUGIN_ABI_VERSION, &AUDIO_A, &AUDIO_PLAIN);
	registry.add(NES_PLUGIN_ABI_VERSION, &INPUT_A, &INPUT_PLAIN);
	return registry;
}

} // namespace

TEST_CASE("the_list_is_what_the_emulator_would_actually_use") {
	Registry registry = populated();
	nesgui::Config config = nesgui::Config::defaults();
	PluginSettings settings(registry, config);

	REQUIRE_EQ(settings.choicesFor(0).size(), 2);
	CHECK_EQ(settings.choicesFor(0)[0].id, "video-a");
	CHECK_EQ(settings.choicesFor(0)[1].id, "video-b");
	CHECK_EQ(settings.choicesFor(0)[1].version, "2.1");
	CHECK_EQ(settings.choicesFor(1).size(), 1);
	CHECK_EQ(settings.choicesFor(2).size(), 1);

	// Where it came from, so a built-in and a loaded module of the same name
	// can be told apart -- which is the first thing anyone opening this after
	// installing a plugin wants to check.
	CHECK(settings.choicesFor(0)[0].path.empty());
	CHECK_FALSE(settings.choicesFor(0)[1].path.empty());
}

TEST_CASE("nothing_is_selected_when_nothing_of_that_kind_exists") {
	Registry registry;
	registry.add(NES_PLUGIN_ABI_VERSION, &VIDEO_A, &VIDEO_WITHOUT_DIALOG);
	nesgui::Config config = nesgui::Config::defaults();
	PluginSettings settings(registry, config);

	CHECK_EQ(settings.selectedIndex(0), 0);
	CHECK_EQ(settings.selectedIndex(1), -1);      // no audio plugin at all
	CHECK_EQ(settings.selectedId(1), "");
	CHECK_FALSE(settings.canConfigure(1));
}

TEST_CASE("the_configured_plugin_starts_selected") {
	Registry registry = populated();
	nesgui::Config config = nesgui::Config::defaults();
	config.videoPlugin = "video-b";
	PluginSettings settings(registry, config);

	CHECK_EQ(settings.selectedIndex(0), 1);
	CHECK_EQ(settings.selectedId(0), "video-b");
	CHECK_FALSE(settings.changed());
}

TEST_CASE("a_configured_plugin_that_is_gone_shows_the_fallback") {
	// The config outlives the plugin it names -- someone deleted the .dll. The
	// emulator falls back to the first, so the dialog has to show the fallback
	// rather than a name that is not in the list. Showing the stale name would
	// mean the dialog disagreed with the running program about what is loaded.
	Registry registry = populated();
	nesgui::Config config = nesgui::Config::defaults();
	config.videoPlugin = "video-deleted";
	PluginSettings settings(registry, config);

	CHECK_EQ(settings.selectedIndex(0), 0);
	CHECK_EQ(settings.selectedId(0), "video-a");
}

TEST_CASE("only_a_plugin_offering_a_dialog_can_be_configured") {
	Registry registry = populated();
	nesgui::Config config = nesgui::Config::defaults();
	PluginSettings settings(registry, config);

	REQUIRE_EQ(settings.selectedId(0), "video-a");
	CHECK_FALSE(settings.canConfigure(0));        // no configure entry point
	CHECK_FALSE(settings.configure(0));           // and asking does nothing

	settings.select(0, 1);
	CHECK_EQ(settings.selectedId(0), "video-b");
	CHECK(settings.canConfigure(0));

	// Audio's api declares its full size but leaves configure null.
	CHECK_FALSE(settings.canConfigure(1));
}

TEST_CASE("selecting_out_of_range_changes_nothing") {
	Registry registry = populated();
	nesgui::Config config = nesgui::Config::defaults();
	PluginSettings settings(registry, config);

	CHECK_FALSE(settings.select(0, 2));
	CHECK_FALSE(settings.select(0, -1));
	CHECK_EQ(settings.selectedIndex(0), 0);
	CHECK_FALSE(settings.changed());
}

TEST_CASE("reselecting_the_same_plugin_is_not_a_change") {
	Registry registry = populated();
	nesgui::Config config = nesgui::Config::defaults();
	PluginSettings settings(registry, config);

	CHECK(settings.select(0, 0));
	CHECK_FALSE(settings.changed());
	CHECK(settings.select(0, 1));
	CHECK(settings.changed());
}

TEST_CASE("applying_writes_the_ids_and_leaves_everything_else_alone") {
	Registry registry = populated();
	nesgui::Config config = nesgui::Config::defaults();
	config.scale = 5;
	config.fullscreen = true;
	config.keys[0][0] = SDL_SCANCODE_Q;

	PluginSettings settings(registry, config);
	settings.select(0, 1);
	settings.apply(&config);

	CHECK_EQ(config.videoPlugin, "video-b");
	CHECK_EQ(config.audioPlugin, "audio-a");
	CHECK_EQ(config.inputPlugin, "input-a");

	// The rest of the configuration belongs to somebody else.
	CHECK_EQ(config.scale, 5);
	CHECK(config.fullscreen);
	CHECK_EQ(config.keys[0][0], SDL_SCANCODE_Q);
}

TEST_CASE("a_kind_with_nothing_installed_applies_as_empty_not_as_stale") {
	// Empty means "whichever is first", which is what should be recorded when
	// there is nothing to record. Leaving the previous name in the file would
	// resurrect a choice the player can no longer see or change.
	Registry registry;
	registry.add(NES_PLUGIN_ABI_VERSION, &VIDEO_A, &VIDEO_WITHOUT_DIALOG);
	nesgui::Config config = nesgui::Config::defaults();
	config.audioPlugin = "audio-that-was-uninstalled";

	PluginSettings settings(registry, config);
	settings.apply(&config);
	CHECK_EQ(config.audioPlugin, "");
}

TEST_CASE("the_labels_are_the_three_jobs_in_a_fixed_order") {
	CHECK_EQ(PluginSettings::kindAt(0), NES_PLUGIN_VIDEO);
	CHECK_EQ(PluginSettings::kindAt(1), NES_PLUGIN_AUDIO);
	CHECK_EQ(PluginSettings::kindAt(2), NES_PLUGIN_INPUT);
	CHECK_EQ(std::string(PluginSettings::kindLabel(2)), "Controller");
}
