//
// nes_gui -- the console in a window, running at NTSC speed with live input.
//
// This file is only wiring now: parse the command line, start SDL, construct
// one backend of each kind and hand them to App. The loop itself lives in
// frontend/App.cpp and contains no SDL, which is what lets it be tested without
// a window, a sound device or a wall clock.
//

#include "GuiConfig.h"
#include "frontend/App.h"
#include "frontend/SdlBackend.h"
#include "frontend/SdlPlugin.h"
#include "plugin/Module.h"
#include "plugin/PluginHost.h"
#include "nes/Nes.h"

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

void usage(const char* argv0) {
	std::printf(
		"usage: %s <rom.nes> [options]\n"
		"\n"
		"  --scale=N      integer window scale, default 3\n"
		"  --fullscreen   start in borderless fullscreen\n"
		"  --no-audio     run silent\n"
		"\n"
		"Player 1: arrows, Z = A, X = B, Enter = Start, Right Shift = Select\n"
		"Player 2: numpad 8456, Numpad 1 = A, Numpad 2 = B,\n"
		"          Numpad Enter = Start, Numpad + = Select\n"
		"\n"
		"Gamepads are picked up automatically, in the order they are plugged in.\n"
		"D-pad or left stick to move; A or B = NES A; X or Y = NES B;\n"
		"Start = Start, Back = Select. The keyboard keeps working alongside.\n"
		"\n"
		"All of the above can be rebound in nes.cfg, written beside this program\n"
		"on the first run. Options given here override it.\n"
		"\n"
		"  P or Space   pause\n"
		"  N            while paused, advance one frame\n"
		"  M            mute\n"
		"  Tab (held)   run unthrottled\n"
		"  R            reset\n"
		"  F12          save a screenshot beside the ROM\n"
		"  Esc          quit\n",
		argv0);
}

bool startsWith(const char* s, const char* prefix, const char** rest) {
	const std::size_t n = std::strlen(prefix);
	if (std::strncmp(s, prefix, n) != 0)
		return false;
	*rest = s + n;
	return true;
}

/** A `plugins` folder beside the executable, like nes.cfg is beside it. */
std::string pluginDirectory() {
	char* base = SDL_GetBasePath();
	if (!base)
		return "plugins";
	std::string result = std::string(base) + "plugins";
	SDL_free(base);
	return result;
}

} // namespace

int main(int argc, char* argv[]) {
	const char* romPath = nullptr;

	// The file first, then the command line over the top of it: a flag typed
	// now should beat a preference saved earlier.
	nesgui::Config config = nesgui::Config::defaults();
	const std::string configPath = nesgui::Config::path();
	std::string configWarnings;
	config.load(configPath, &configWarnings);
	if (!configWarnings.empty())
		std::fprintf(stderr, "%s\n", configWarnings.c_str());

	int scale = config.scale;
	bool fullscreen = config.fullscreen;
	bool wantAudio = config.audio;

	for (int i = 1; i < argc; i++) {
		const char* rest = nullptr;
		if (std::strcmp(argv[i], "--no-audio") == 0) {
			wantAudio = false;
		} else if (startsWith(argv[i], "--scale=", &rest)) {
			scale = std::atoi(rest);
			if (scale < 1 || scale > 8) {
				std::fprintf(stderr, "--scale must be 1..8\n");
				return 2;
			}
		} else if (std::strcmp(argv[i], "--fullscreen") == 0) {
			fullscreen = true;
		} else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
			usage(argv[0]);
			return 0;
		} else if (argv[i][0] == '-') {
			std::fprintf(stderr, "unknown option: %s\n", argv[i]);
			return 2;
		} else if (!romPath) {
			romPath = argv[i];
		} else {
			std::fprintf(stderr, "unexpected argument: %s\n", argv[i]);
			return 2;
		}
	}

	if (!romPath) {
		usage(argv[0]);
		return 2;
	}

	nes::Nes console;
	std::string error;
	if (!console.loadRom(romPath, &error)) {
		// A GUI launched from a file manager has nowhere to print, so say it in
		// a box as well as on stderr.
		std::fprintf(stderr, "%s\n", error.c_str());
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "nes", error.c_str(), nullptr);
		return 1;
	}
	console.reset();

	// Audio and gamepads are both requested but not required: a machine with no
	// sound device and no pad still gets a working emulator with a keyboard.
	const Uint32 optional = (wantAudio ? SDL_INIT_AUDIO : 0) | SDL_INIT_GAMECONTROLLER;
	if (SDL_Init(SDL_INIT_VIDEO | optional) != 0) {
		if (SDL_Init(SDL_INIT_VIDEO) != 0) {
			std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
			return 1;
		}
		wantAudio = false;
	}

	// Everything the front end uses arrives through the plugin boundary, even
	// though all three are compiled in today. Going through the same path the
	// loader will use is what keeps that boundary honest: a mistake in its shape
	// breaks the build now rather than a stranger's .dll later.
	nesplug::Registry registry;
	std::string warning;

	// Loadable modules first, so one dropped into the folder beats the copy
	// compiled in. That is the point of being able to drop one in.
	const std::vector<std::shared_ptr<nesplug::Module> > modules =
			nesplug::loadModules(pluginDirectory(), &warning);
	for (std::size_t i = 0; i < modules.size(); i++) {
		registry.add(NES_PLUGIN_ABI_VERSION, modules[i]->info(), modules[i]->api(),
				modules[i]->path(), &warning, modules[i]);
		SDL_Log("loaded %s from %s", modules[i]->info()->name,
				modules[i]->path().c_str());
	}

	// Then the built-ins, which lose any id a module already claimed. That is
	// the expected outcome of dropping a plugin in, not a problem, so it goes
	// to the log rather than to stderr with the real warnings.
	std::string shadowed;
	registry.add(nesfe::sdlPluginAbiVersion(), nesfe::sdlVideoInfo(),
			nesfe::sdlVideoApi(), std::string(), &shadowed);
	registry.add(nesfe::sdlPluginAbiVersion(), nesfe::sdlAudioInfo(),
			nesfe::sdlAudioApi(), std::string(), &shadowed);
	registry.add(nesfe::sdlPluginAbiVersion(), nesfe::sdlInputInfo(),
			nesfe::sdlInputApi(), std::string(), &shadowed);
	if (!shadowed.empty())
		SDL_Log("%s", shadowed.c_str());
	if (!warning.empty())
		std::fprintf(stderr, "%s\n", warning.c_str());

	const nesplug::Entry* videoEntry =
			registry.select(NES_PLUGIN_VIDEO, config.videoPlugin);
	const nesplug::Entry* audioEntry =
			registry.select(NES_PLUGIN_AUDIO, config.audioPlugin);
	const nesplug::Entry* inputEntry =
			registry.select(NES_PLUGIN_INPUT, config.inputPlugin);
	if (!videoEntry || !inputEntry) {
		std::fprintf(stderr, "no video or input plugin available\n");
		SDL_Quit();
		return 1;
	}

	std::unique_ptr<nesplug::VideoPlugin> videoOwner =
			nesplug::createFrom<nesplug::VideoPlugin>(*videoEntry, nullptr);
	if (!videoOwner) {
		std::fprintf(stderr, "video plugin could not be created\n");
		SDL_Quit();
		return 1;
	}
	nesplug::VideoPlugin& video = *videoOwner;
	nesfe::VideoOptions videoOptions;
	videoOptions.scale = scale;
	videoOptions.fullscreen = fullscreen;
	if (!video.open(videoOptions, &error)) {
		std::fprintf(stderr, "%s\n", error.c_str());
		SDL_Quit();
		return 1;
	}
	SDL_Log("video plugin: %s", videoEntry->info->name);

	nesfe::App::Options appOptions;
	std::unique_ptr<nesplug::AudioPlugin> audioOwner;
	if (audioEntry)
		audioOwner = nesplug::createFrom<nesplug::AudioPlugin>(*audioEntry, nullptr);
	if (!audioOwner) {
		// No audio plugin at all is a quiet emulator, not a broken one. The
		// adapter tolerates a null api, so the run loop needs no special case.
		audioOwner.reset(new nesplug::AudioPlugin(nullptr, nullptr));
	}
	nesplug::AudioPlugin& audio = *audioOwner;
	if (wantAudio && audioEntry && !audio.open(appOptions.audioSampleRate, &error))
		std::fprintf(stderr, "%s\n", error.c_str());   // silent, not fatal
	if (audioEntry)
		SDL_Log("audio plugin: %s", audioEntry->info->name);

	std::unique_ptr<nesplug::InputPlugin> inputOwner =
			nesplug::createFrom<nesplug::InputPlugin>(*inputEntry, nullptr);
	if (!inputOwner) {
		std::fprintf(stderr, "input plugin could not be created\n");
		SDL_Quit();
		return 1;
	}
	nesplug::InputPlugin& input = *inputOwner;
	input.open(nullptr);
	SDL_Log("input plugin: %s", inputEntry->info->name);

	// Write the file out once SDL is up, so the names in it come from SDL
	// rather than from a table here that could disagree with it. Doing this
	// after a successful start also means a config is only ever left behind by
	// a run that worked.
	{
		std::ifstream probe(configPath.c_str());
		if (!probe && config.save(configPath))
			SDL_Log("wrote a default configuration to %s", configPath.c_str());
	}

	nesfe::SdlClock clock;
	nesfe::App app(console, video, audio, input, clock, appOptions);
	app.run();

	// Before tearing anything down: closing the window must not cost a save.
	std::string saveError;
	if (!console.saveBatteryRam(&saveError))
		std::fprintf(stderr, "%s\n", saveError.c_str());
	else if (!console.batteryRamPath().empty())
		SDL_Log("wrote %s", console.batteryRamPath().c_str());

	audio.close();
	input.close();
	video.close();
	SDL_Quit();
	return 0;
}
