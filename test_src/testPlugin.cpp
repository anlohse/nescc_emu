/*
 * testPlugin.cpp -- the plugin boundary, exercised by a plugin written in C.
 *
 * The fake below is deliberately not a wrapper around anything: it is a plain C
 * api struct with static functions behind it, which is what a loadable module
 * will be. If the boundary can carry this, it can carry a .dll -- everything
 * left after that is loading and symbol lookup, not interface design.
 */

#include "../src/plugin/Module.h"
#include "../src/plugin/PluginHost.h"

#include <SDL.h>

#include <doctest/doctest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#  define PATH_SEPARATOR "\\"
#else
#  define PATH_SEPARATOR "/"
#endif

using namespace nesplug;

namespace {

/* ------------------------------------------------------------------------- */
/* A fake video plugin, in the shape a real module would take                  */
/* ------------------------------------------------------------------------- */

struct FakeVideoState {
	int opens;
	int closes;
	int presents;
	int lastScale;
	int lastFullscreen;
	std::uint8_t lastFirstPixel;
	bool failOpen;
	char lastTitle[64];
};

FakeVideoState g_video;

void* fakeVideoCreate(const nes_host*) {
	std::memset(&g_video, 0, sizeof(g_video));
	return &g_video;
}

void fakeVideoDestroy(void*) { }

int fakeVideoOpen(void* self, int scale, int fullscreen, const char*,
		char* error, std::size_t errorSize) {
	FakeVideoState* s = static_cast<FakeVideoState*>(self);
	s->lastScale = scale;
	s->lastFullscreen = fullscreen;
	if (s->failOpen) {
		// Writes into the host's buffer. Nothing is allocated to cross back,
		// which is the rule that keeps two heaps from meeting.
		std::strncpy(error, "no display", errorSize - 1);
		error[errorSize - 1] = '\0';
		return 0;
	}
	s->opens++;
	return 1;
}

void fakeVideoClose(void* self) { static_cast<FakeVideoState*>(self)->closes++; }

void fakeVideoPresent(void* self, const std::uint8_t* indices, int, int) {
	FakeVideoState* s = static_cast<FakeVideoState*>(self);
	s->presents++;
	s->lastFirstPixel = indices[0];
}

void fakeVideoSetTitle(void* self, const char* title) {
	FakeVideoState* s = static_cast<FakeVideoState*>(self);
	std::strncpy(s->lastTitle, title, sizeof(s->lastTitle) - 1);
	s->lastTitle[sizeof(s->lastTitle) - 1] = '\0';
}

const nes_video_api FAKE_VIDEO_API = {
	sizeof(nes_video_api),
	fakeVideoCreate, fakeVideoDestroy, fakeVideoOpen, fakeVideoClose,
	fakeVideoPresent, fakeVideoSetTitle,
	nullptr,      // no screenshot support
	nullptr       // no dialog
};

const nes_plugin_info FAKE_VIDEO_INFO = {
	sizeof(nes_plugin_info), "fake-video", "Fake video", "0.1", NES_PLUGIN_VIDEO
};

const nes_plugin_info OTHER_VIDEO_INFO = {
	sizeof(nes_plugin_info), "other-video", "Other video", "0.1", NES_PLUGIN_VIDEO
};

const nes_plugin_info FAKE_AUDIO_INFO = {
	sizeof(nes_plugin_info), "fake-audio", "Fake audio", "0.1", NES_PLUGIN_AUDIO
};

/**
 * An api struct that stops after present(): what a module built against an
 * older header looks like to a newer host.
 */
const nes_video_api TRUNCATED_VIDEO_API = {
	offsetof(nes_video_api, set_title),   // everything from here on is absent
	fakeVideoCreate, fakeVideoDestroy, fakeVideoOpen, fakeVideoClose,
	fakeVideoPresent,
	fakeVideoSetTitle,     // present in memory, but outside the declared size
	nullptr, nullptr
};

} // namespace

/* ------------------------------------------------------------------------- */
/* The version handshake                                                      */
/* ------------------------------------------------------------------------- */

TEST_CASE("a_plugin_built_against_another_abi_is_refused_and_says_so") {
	// The single check that decides whether a plugin ecosystem stays usable.
	// Refusing quietly would be worse than not checking: the player would see a
	// feature simply missing with nothing to search for.
	Registry registry;
	std::string warning;

	CHECK_FALSE(registry.add(NES_PLUGIN_ABI_VERSION + 1, &FAKE_VIDEO_INFO,
			&FAKE_VIDEO_API, "video_old.dll", &warning));
	CHECK_EQ(registry.size(), 0);
	CHECK_EQ(registry.refused(), 1);
	CHECK(warning.find("video_old.dll") != std::string::npos);
	CHECK(warning.find("ABI version") != std::string::npos);
}

TEST_CASE("one_stale_plugin_does_not_cost_the_others") {
	Registry registry;
	std::string warning;
	registry.add(NES_PLUGIN_ABI_VERSION + 1, &FAKE_VIDEO_INFO, &FAKE_VIDEO_API,
			"stale.dll", &warning);
	registry.add(NES_PLUGIN_ABI_VERSION, &OTHER_VIDEO_INFO, &FAKE_VIDEO_API,
			"good.dll", &warning);

	CHECK_EQ(registry.size(), 1);
	CHECK(registry.find(NES_PLUGIN_VIDEO, "other-video") != nullptr);
}

TEST_CASE("a_duplicate_id_within_a_kind_is_refused") {
	// The config file stores an id. Two plugins answering to the same one would
	// make which you get depend on load order.
	Registry registry;
	std::string warning;
	CHECK(registry.add(NES_PLUGIN_ABI_VERSION, &FAKE_VIDEO_INFO, &FAKE_VIDEO_API));
	CHECK_FALSE(registry.add(NES_PLUGIN_ABI_VERSION, &FAKE_VIDEO_INFO,
			&FAKE_VIDEO_API, "second.dll", &warning));
	CHECK(warning.find("duplicate id") != std::string::npos);
}

TEST_CASE("selection_falls_back_rather_than_failing") {
	Registry registry;
	registry.add(NES_PLUGIN_ABI_VERSION, &FAKE_VIDEO_INFO, &FAKE_VIDEO_API);
	registry.add(NES_PLUGIN_ABI_VERSION, &OTHER_VIDEO_INFO, &FAKE_VIDEO_API);
	registry.add(NES_PLUGIN_ABI_VERSION, &FAKE_AUDIO_INFO, &FAKE_VIDEO_API);

	CHECK_EQ(std::string(registry.select(NES_PLUGIN_VIDEO, "other-video")->info->id),
			"other-video");
	// Named but not installed: take the first rather than refuse to start. A
	// config outliving a plugin should not be fatal.
	CHECK_EQ(std::string(registry.select(NES_PLUGIN_VIDEO, "gone")->info->id),
			"fake-video");
	CHECK_EQ(std::string(registry.select(NES_PLUGIN_VIDEO, "")->info->id),
			"fake-video");
	CHECK_EQ(registry.select(NES_PLUGIN_INPUT, ""), nullptr);
	CHECK_EQ(registry.ofKind(NES_PLUGIN_VIDEO).size(), 2);
}

/* ------------------------------------------------------------------------- */
/* Calling across the boundary                                                */
/* ------------------------------------------------------------------------- */

TEST_CASE("the_adapter_presents_a_c_plugin_as_the_interface_the_loop_speaks") {
	VideoPlugin video(&FAKE_VIDEO_API, nullptr);

	nesfe::VideoOptions options;
	options.scale = 4;
	options.fullscreen = true;
	nesfe::Error error;
	REQUIRE(video.open(options, &error));
	CHECK_EQ(g_video.lastScale, 4);
	CHECK_EQ(g_video.lastFullscreen, 1);

	std::uint8_t frame[8] = { 0x21, 0, 0, 0, 0, 0, 0, 0 };
	video.present(frame, 8, 1);
	CHECK_EQ(g_video.presents, 1);
	CHECK_EQ(g_video.lastFirstPixel, 0x21);

	video.setTitle("nes - 60.1 fps");
	CHECK_EQ(std::string(g_video.lastTitle), "nes - 60.1 fps");

	video.close();
	CHECK_EQ(g_video.closes, 1);
}

TEST_CASE("a_failure_reason_crosses_back_without_allocating") {
	VideoPlugin video(&FAKE_VIDEO_API, nullptr);
	g_video.failOpen = true;   // create() zeroed it; set it before opening

	nesfe::VideoOptions options;
	nesfe::Error error;
	CHECK_FALSE(video.open(options, &error));
	CHECK_EQ(error, "no display");
}

TEST_CASE("an_optional_entry_point_a_plugin_does_not_have_is_not_called") {
	// The fake declares no screenshot support. Calling through a null pointer
	// would be the obvious bug here, and the obvious crash.
	VideoPlugin video(&FAKE_VIDEO_API, nullptr);
	CHECK_FALSE(video.saveScreenshot("shot.bmp"));
	video.configure();          // must be a no-op, not a jump to nowhere
}

TEST_CASE("a_plugin_built_against_an_older_header_is_read_only_as_far_as_it_goes") {
	// This is what the size field buys: the host can add entry points without
	// invalidating modules that predate them. The truncated api has a valid
	// set_title pointer sitting in memory, but declares a size that stops
	// before it -- so the host must not call it, even though it would work.
	VideoPlugin video(&TRUNCATED_VIDEO_API, nullptr);

	nesfe::VideoOptions options;
	nesfe::Error error;
	REQUIRE(video.open(options, &error));

	std::uint8_t frame[8] = { 0x0F, 0, 0, 0, 0, 0, 0, 0 };
	video.present(frame, 8, 1);
	CHECK_EQ(g_video.presents, 1);      // what it does declare still works

	CHECK_FALSE(video.saveScreenshot("shot.bmp"));
	video.configure();                  // beyond the declared size: not called
}

/* ------------------------------------------------------------------------ */
/* Loading a real library                                                    */
/* ------------------------------------------------------------------------ */

/*
 * These load audio_sdl.dll -- the genuine article, built by this project into
 * plugins/ beside the test binary. Everything above proves the interface
 * design; this proves the loading, and the two failure modes that only appear
 * once a file on disk is involved: a library that is not a plugin, and an
 * instance outliving the reference that kept its library mapped.
 */

namespace {

std::string pluginPath(const char* leaf) {
	char* base = SDL_GetBasePath();
	const std::string dir = base ? std::string(base) : std::string();
	if (base)
		SDL_free(base);
	return dir + "plugins" + PATH_SEPARATOR + leaf + nesplug::moduleSuffix();
}

} // namespace

TEST_CASE("a_missing_file_fails_without_fuss") {
	std::string error;
	CHECK_FALSE(static_cast<bool>(nesplug::Module::load(pluginPath("no_such_plugin"), &error)));
	CHECK_FALSE(error.empty());
}

TEST_CASE("a_library_that_is_not_a_plugin_is_recognised_as_such") {
	// Whatever else is beside the test binary -- SDL2 itself, a runtime -- will
	// load as a library and then not answer. Saying so plainly matters, because
	// this is the common case for anything that ends up in the folder.
	char* base = SDL_GetBasePath();
	REQUIRE(base != nullptr);
	const std::string self = std::string(base) + "nes_gui_test.exe";
	SDL_free(base);

	std::string error;
	std::shared_ptr<nesplug::Module> module = nesplug::Module::load(self, &error);
	CHECK_FALSE(static_cast<bool>(module));
	CHECK_FALSE(error.empty());
}

TEST_CASE("the_audio_plugin_loads_from_disk_and_says_what_it_is") {
	std::string error;
	std::shared_ptr<nesplug::Module> module =
			nesplug::Module::load(pluginPath("audio_sdl"), &error);
	REQUIRE_MESSAGE(static_cast<bool>(module), error);

	CHECK_EQ(module->kind(), NES_PLUGIN_AUDIO);
	CHECK_EQ(std::string(module->info()->id), "sdl-audio");
	CHECK(module->api() != nullptr);
}

TEST_CASE("asking_a_module_for_the_wrong_kind_returns_nothing") {
	// The check the type system cannot make on its own. A static_cast from
	// void* would reinterpret this audio api as a video one and crash on the
	// third call; the descriptor's kind is the only thing that can say no.
	std::string error;
	std::shared_ptr<nesplug::Module> module =
			nesplug::Module::load(pluginPath("audio_sdl"), &error);
	REQUIRE(static_cast<bool>(module));

	CHECK(static_cast<bool>(module->create<AudioPlugin>(nullptr)));
	CHECK_FALSE(static_cast<bool>(module->create<VideoPlugin>(nullptr)));
	CHECK_FALSE(static_cast<bool>(module->create<InputPlugin>(nullptr)));
}

TEST_CASE("an_instance_keeps_its_library_mapped") {
	// The failure this design exists to prevent. Every function the instance
	// calls lives inside the library, so if the last shared_ptr to the module
	// went away here, the calls below would jump into unmapped memory -- and
	// would do it at shutdown, on someone else's machine, in a stack trace
	// naming nothing.
	std::unique_ptr<AudioPlugin> audio;
	{
		std::string error;
		std::shared_ptr<nesplug::Module> module =
				nesplug::Module::load(pluginPath("audio_sdl"), &error);
		REQUIRE(static_cast<bool>(module));
		audio = module->create<AudioPlugin>(nullptr);
		REQUIRE(static_cast<bool>(audio));
	}   // the only other reference to the module dies here

	// Still callable. Not opening a device: a test host has no sound card and
	// does not need one to prove the library is still there.
	CHECK_FALSE(audio->isOpen());
	audio->queue(nullptr, 0);
	CHECK_EQ(audio->queuedSeconds(), 0.0);
	audio->clear();
}

TEST_CASE("a_registry_entry_from_a_module_carries_it_too") {
	Registry registry;
	std::unique_ptr<AudioPlugin> audio;
	{
		std::string error;
		std::shared_ptr<nesplug::Module> module =
				nesplug::Module::load(pluginPath("audio_sdl"), &error);
		REQUIRE(static_cast<bool>(module));
		REQUIRE(registry.add(NES_PLUGIN_ABI_VERSION, module->info(), module->api(),
				module->path(), nullptr, module));
	}

	const Entry* entry = registry.select(NES_PLUGIN_AUDIO, "");
	REQUIRE(entry != NULL);
	audio = nesplug::createFrom<AudioPlugin>(*entry, nullptr);
	REQUIRE(static_cast<bool>(audio));
	CHECK_FALSE(audio->isOpen());
}

TEST_CASE("scanning_a_directory_finds_the_plugins_and_skips_the_rest") {
	char* base = SDL_GetBasePath();
	REQUIRE(base != nullptr);
	const std::string dir = std::string(base) + "plugins";
	SDL_free(base);

	std::string warnings;
	const std::vector<std::shared_ptr<nesplug::Module> > modules =
			nesplug::loadModules(dir, &warnings);
	REQUIRE(modules.size() >= 1);

	bool foundAudio = false;
	for (std::size_t i = 0; i < modules.size(); i++)
		if (std::string(modules[i]->info()->id) == "sdl-audio")
			foundAudio = true;
	CHECK(foundAudio);
}

TEST_CASE("a_directory_that_is_not_there_is_not_an_error") {
	// A build with no plugins folder beside it still has its built-ins.
	std::string warnings;
	const std::vector<std::shared_ptr<nesplug::Module> > modules =
			nesplug::loadModules("definitely-not-a-directory", &warnings);
	CHECK(modules.empty());
	CHECK(warnings.empty());
}
