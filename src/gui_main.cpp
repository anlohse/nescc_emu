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
#include "frontend/HostServices.h"
#include "frontend/FileDialog.h"
#include "frontend/KeysReference.h"
#include "frontend/MenuBar.h"
#include "frontend/SdlBackend.h"
#include "frontend/PluginSettings.h"
#include "frontend/SdlPlugin.h"
#include "frontend/SettingsDialog.h"
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
		"usage: %s [rom.nes] [options]\n"
		"\n"
		"With no ROM the window opens empty; load one from the Emulation menu.\n"
		"\n"
		"  --scale=N      integer window scale, default 3\n"
		"  --fullscreen   start in borderless fullscreen\n"
		"  --no-audio     run silent\n"
		"  --pads         report what SDL sees of the gamepads, then exit\n"
		"\n"
		"Player 1: arrows, Z = A, X = B, Enter = Start, Right Shift = Select\n"
		"Player 2: numpad 8456, Numpad 1 = A, Numpad 2 = B,\n"
		"          Numpad Enter = Start, Numpad + = Select\n"
		"\n"
		"Each console port reads one device, chosen in Settings > Configure\n"
		"Controller: the keyboard, or one of the gamepads by name. Nothing is\n"
		"guessed -- a USB adapter can present a controller per socket whether or\n"
		"not anything is plugged into it, so \"whichever was found first\" picks the\n"
		"wrong one. Both ports start on the keyboard.\n"
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

/**
 * Fill @p registry with everything available: loaded modules first, then the
 * built-ins, which lose any id a module already claimed.
 *
 * @p modules must outlive the registry -- the entries hold references to their
 * libraries, but the caller owning the vector keeps the intent visible.
 */
void buildRegistry(nesplug::Registry* registry,
		std::vector<std::shared_ptr<nesplug::Module> >* modules,
		const std::string& directory);

/** A `plugins` folder beside the executable, like nes.cfg is beside it. */
std::string pluginDirectory() {
	char* base = SDL_GetBasePath();
	if (!base)
		return "plugins";
	std::string result = std::string(base) + "plugins";
	SDL_free(base);
	return result;
}

void buildRegistry(nesplug::Registry* registry,
		std::vector<std::shared_ptr<nesplug::Module> >* modules,
		const std::string& directory) {
	std::string warning;

	// Loadable modules first, so one dropped into the folder beats the copy
	// compiled in. That is the point of being able to drop one in.
	*modules = nesplug::loadModules(directory, &warning);
	for (std::size_t i = 0; i < modules->size(); i++) {
		registry->add(NES_PLUGIN_ABI_VERSION, (*modules)[i]->info(),
				(*modules)[i]->api(), (*modules)[i]->path(), &warning, (*modules)[i]);
		SDL_Log("loaded %s from %s", (*modules)[i]->info()->name,
				(*modules)[i]->path().c_str());
	}

	// A built-in losing its id to a module is the expected outcome of
	// installing one, so it goes to the log rather than to stderr with the
	// real warnings.
	std::string shadowed;
	registry->add(nesfe::sdlPluginAbiVersion(), nesfe::sdlVideoInfo(),
			nesfe::sdlVideoApi(), std::string(), &shadowed);
	registry->add(nesfe::sdlPluginAbiVersion(), nesfe::sdlAudioInfo(),
			nesfe::sdlAudioApi(), std::string(), &shadowed);
	registry->add(nesfe::sdlPluginAbiVersion(), nesfe::sdlInputInfo(),
			nesfe::sdlInputApi(), std::string(), &shadowed);
	if (!shadowed.empty())
		SDL_Log("%s", shadowed.c_str());
	if (!warning.empty())
		std::fprintf(stderr, "%s\n", warning.c_str());
}

/** The part of a path a person recognises: no directory, no extension. */
std::string displayName(const std::string& path) {
	std::size_t start = path.find_last_of("/\\");
	start = (start == std::string::npos) ? 0 : start + 1;
	std::size_t end = path.find_last_of('.');
	if (end == std::string::npos || end < start)
		end = path.size();
	return path.substr(start, end - start);
}

/**
 * Report what SDL sees of the attached gamepads, and what the bindings do with
 * it. For when a pad is detected but nothing reaches the game.
 *
 * Both halves matter and they fail differently: SDL not seeing a button at all
 * means a driver or a missing mapping, while SDL seeing it and nothing happening
 * means the binding does not name that button.
 */
int reportPads(const nesgui::Config& config) {
	std::printf("SDL %s\n", SDL_GetRevision());
	std::printf("game controller subsystem: %s, joystick subsystem: %s\n",
			SDL_WasInit(SDL_INIT_GAMECONTROLLER) ? "up" : "DOWN",
			SDL_WasInit(SDL_INIT_JOYSTICK) ? "up" : "DOWN");
	std::printf("joystick events: %d, controller events: %d  (1 = enabled)\n",
			SDL_JoystickEventState(SDL_QUERY),
			SDL_GameControllerEventState(SDL_QUERY));
	std::printf("joysticks seen: %d\n\n", SDL_NumJoysticks());

	// A real window, because on Windows a joystick is only read while the
	// application has a foreground one. A windowless report can show a working
	// pad as pressing nothing, which is the fault it is meant to find.
	SDL_Window* window = SDL_CreateWindow("gamepad report -- press buttons",
			SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 480, 160,
			SDL_WINDOW_SHOWN);
	if (window)
		SDL_RaiseWindow(window);
	else
		std::printf("no window (%s); readings may be unreliable\n", SDL_GetError());

	SDL_GameController* pads[2] = { nullptr, nullptr };
	int open = 0;
	for (int i = 0; i < SDL_NumJoysticks(); i++) {
		std::printf("device %d: \"%s\"\n", i, SDL_JoystickNameForIndex(i));
		if (!SDL_IsGameController(i)) {
			// Without a mapping SDL will not present it as a game controller, and
			// nothing in the emulator will look at it.
			std::printf("    NOT a game controller: no mapping for this device\n");
			continue;
		}
		if (open < 2 && (pads[open] = SDL_GameControllerOpen(i)) != nullptr) {
			char* map = SDL_GameControllerMapping(pads[open]);
			std::printf("    opened as \"%s\"\n", SDL_GameControllerName(pads[open]));
			std::printf("    mapping: %s\n", map ? map : "(none)");
			if (map)
				SDL_free(map);

			// Capabilities, which say whether SDL got at the device at all. A pad
			// reporting zero buttons has not been read successfully, and no amount
			// of pressing will ever produce anything -- worth knowing without
			// needing somebody's thumb.
			SDL_Joystick* stick = SDL_GameControllerGetJoystick(pads[open]);
			if (stick)
				std::printf("    raw: %d buttons, %d axes, %d hats, driver \"%s\"\n",
						SDL_JoystickNumButtons(stick), SDL_JoystickNumAxes(stick),
						SDL_JoystickNumHats(stick),
						SDL_JoystickPathForIndex(i) ? SDL_JoystickPathForIndex(i) : "?");
			open++;
		} else {
			std::printf("    could not open: %s\n", SDL_GetError());
		}
	}
	if (open == 0) {
		std::printf("\nNothing to poll. SDL is not presenting any device as a "
				"game controller.\n");
		return 1;
	}

	std::printf("\nBindings, port 1:");
	static const char* const NAMES[8] =
			{ "a", "b", "select", "start", "up", "down", "left", "right" };
	for (int i = 0; i < 8; i++) {
		const char* to = SDL_GameControllerGetStringForButton(config.padButtons[0][i]);
		std::printf(" %s=%s", NAMES[i], to ? to : "unbound");
	}
	std::printf("\n\nPress buttons. Ten seconds, then it stops.\n");

	const Uint32 until = SDL_GetTicks() + 10000;
	std::string previous;
	while (SDL_GetTicks() < until) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			// Draining the queue is what keeps the state fresh, exactly as the
			// game loop does it -- if buttons show up here but not in a game, the
			// difference is the bindings rather than the plumbing.
		}
		SDL_GameControllerUpdate();
		SDL_JoystickUpdate();

		std::string down;
		for (int p = 0; p < open; p++) {
			for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++)
				if (SDL_GameControllerGetButton(pads[p],
						static_cast<SDL_GameControllerButton>(b))) {
					const char* name = SDL_GameControllerGetStringForButton(
							static_cast<SDL_GameControllerButton>(b));
					// Which device answered matters as much as which button: this
					// adapter presents a socket per HID collection, and SDL does not
					// promise to enumerate them in the order they are labelled.
					down += " dev";
					down += std::to_string(p);
					down += ":";
					down += (name ? name : "?");
				}

			// And the same device underneath, as a plain joystick. These two can
			// disagree, and which one sees a press says what is wrong: raw buttons
			// arriving while the controller layer stays silent means the mapping
			// names the wrong hardware, not that the pad is dead.
			SDL_Joystick* stick = SDL_GameControllerGetJoystick(pads[p]);
			if (!stick)
				continue;
			for (int b = 0; b < SDL_JoystickNumButtons(stick); b++)
				if (SDL_JoystickGetButton(stick, b)) {
					down += " dev";
					down += std::to_string(p);
					down += ":raw-b";
					down += std::to_string(b);
				}
			for (int h = 0; h < SDL_JoystickNumHats(stick); h++)
				if (SDL_JoystickGetHat(stick, h) != SDL_HAT_CENTERED) {
					down += " dev";
					down += std::to_string(p);
					down += ":raw-h";
					down += std::to_string(SDL_JoystickGetHat(stick, h));
				}
		}
		if (down != previous) {
			std::printf("pad:%s\n", down.empty() ? " (nothing)" : down.c_str());
			std::fflush(stdout);
			previous = down;
		}
		SDL_Delay(16);
	}

	for (int p = 0; p < open; p++)
		SDL_GameControllerClose(pads[p]);
	if (window)
		SDL_DestroyWindow(window);
	return 0;
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
	bool settingsOnly = false;
	bool padsOnly = false;

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
		} else if (std::strcmp(argv[i], "--settings") == 0) {
			settingsOnly = true;
		} else if (std::strcmp(argv[i], "--pads") == 0) {
			padsOnly = true;
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

	// Configuring the emulator should not require having a game to hand.
	if (settingsOnly) {
		if (SDL_Init(SDL_INIT_VIDEO) != 0) {
			std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
			return 1;
		}
		if (!nesfe::settingsDialogAvailable()) {
			std::fprintf(stderr,
					"no settings dialog on this platform yet -- edit %s\n",
					configPath.c_str());
			SDL_Quit();
			return 1;
		}

		nesplug::Registry registry;
		std::vector<std::shared_ptr<nesplug::Module> > modules;
		buildRegistry(&registry, &modules, pluginDirectory());

		// A plugin's own dialog needs somewhere to put what it decides, which
		// is the host: it owns the file. Without this, opening one from here
		// would show settings that could not be saved.
		nesfe::HostServices host(&config, configPath);
		nesfe::PluginSettings settings(registry, config, host.handle());
		const bool accepted = nesfe::showSettingsDialog(&settings, nullptr);
		if (accepted) {
			settings.apply(&config);
			if (config.save(configPath))
				SDL_Log("saved plugin selection to %s", configPath.c_str());
			else
				std::fprintf(stderr, "could not write %s\n", configPath.c_str());
		}
		SDL_Quit();
		return 0;
	}

	// Starting with no ROM is a perfectly good way to start, now that one can be
	// loaded from the menu. Naming one on the command line still works, and a
	// bad name is still a failure to start rather than a silent empty window --
	// somebody who typed a path wants to hear that it was wrong.
	nes::Nes console;
	std::string error;
	if (romPath) {
		if (!console.loadRom(romPath, &error)) {
			// A GUI launched from a file manager has nowhere to print, so say it
			// in a box as well as on stderr.
			std::fprintf(stderr, "%s\n", error.c_str());
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "nes", error.c_str(),
					nullptr);
			return 1;
		}
		// A power-up, like the menu's Load ROM does. Starting one way from the
		// command line and another way from the menu meant the same game could
		// behave differently depending on how it was opened.
		console.powerOn();
	}

	// Video is required; audio and gamepads are not. Each optional subsystem is
	// asked for separately, because one SDL_Init with three flags fails as a unit
	// -- a machine with no sound device was losing its gamepad support too, which
	// is the sort of thing nobody notices until a pad does not work.
	// Keep reading the pads even when SDL does not think it has the focus.
	//
	// SDL stops polling joysticks while the application is in the background, and
	// on Windows it only counts *its own* windows. Every dialog here is a native
	// Win32 one, so the moment the bindings dialog opens, SDL believes the
	// application is backgrounded and freezes the pad state -- which is why "press
	// a button on the gamepad" could never capture anything, however long you held
	// it. The poll ran; the state it read was stale.
	//
	// Set before the subsystem starts, because that is when SDL reads it.
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	if (wantAudio && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
		std::fprintf(stderr, "no audio: %s\n", SDL_GetError());
		wantAudio = false;
	}
	if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
		std::fprintf(stderr, "no gamepad support: %s\n", SDL_GetError());

	if (padsOnly)
		return reportPads(config);

	// Everything the front end uses arrives through the plugin boundary, even
	// the backends still compiled in. Going through the same path the loader
	// uses is what keeps that boundary honest.
	nesplug::Registry registry;
	std::vector<std::shared_ptr<nesplug::Module> > modules;
	buildRegistry(&registry, &modules, pluginDirectory());

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

	// Declared before any plugin and destroyed after all of them: a plugin is
	// entitled to call back through this pointer whenever it likes, so it must
	// not be the shorter-lived of the two.
	nesfe::HostServices host(&config, configPath);
	host.setFrameSource([&console](int* width, int* height) {
		if (width) *width = nes::Ppu::SCREEN_WIDTH;
		if (height) *height = nes::Ppu::SCREEN_HEIGHT;
		return console.ppu().framebuffer();
	});

	std::unique_ptr<nesplug::VideoPlugin> videoOwner =
			nesplug::createFrom<nesplug::VideoPlugin>(*videoEntry, host.handle());
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

	// Only now is there a window for a plugin's dialog to sit over.
	host.setWindowSource([&video]() { return video.nativeWindow(); });

	nesfe::App::Options appOptions;
	std::unique_ptr<nesplug::AudioPlugin> audioOwner;
	if (audioEntry)
		audioOwner = nesplug::createFrom<nesplug::AudioPlugin>(*audioEntry, host.handle());
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
			nesplug::createFrom<nesplug::InputPlugin>(*inputEntry, host.handle());
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

	// F1 opens the chooser. The selection is written to nes.cfg and takes
	// effect on the next launch: swapping a video or input plugin under a
	// running emulator would mean tearing down the window and the event queue
	// the dialog is itself running on.
	app.setSettingsHandler([&]() {
		if (!nesfe::settingsDialogAvailable()) {
			SDL_Log("no settings dialog on this platform yet -- edit %s",
					configPath.c_str());
			return;
		}
		nesfe::PluginSettings settings(registry, config, host.handle());
		if (!nesfe::showSettingsDialog(&settings, video.nativeWindow()))
			return;
		settings.apply(&config);
		if (config.save(configPath))
			SDL_Log("saved plugin selection to %s; it applies next launch",
					configPath.c_str());
		else
			std::fprintf(stderr, "could not write %s\n", configPath.c_str());
	});

	// The light gun. Two plugins meet here and nowhere else: one knows where
	// the mouse is, the other knows where the picture is.
	app.setZapperSource(
			[&input](nesfe::ZapperState* out) { return input.pollZapper(out); },
			[&video](int wx, int wy, int* fx, int* fy) {
				return video.windowToFrame(wx, wy, fx, fy);
			});

	/* --- The menu bar ---------------------------------------------------- */

	// Open the dialog belonging to whichever plugin is doing a given job. The
	// throwaway instance and the reasoning behind it live in PluginSettings;
	// this is only the entry point the menu needs.
	auto configureKind = [&](int kind) {
		nesfe::PluginSettings settings(registry, config, host.handle());
		app.dropDeadline();          // a modal dialog stops the emulator
		settings.configure(kind);

		// Pressing OK should change something. The dialog writes to the file, so
		// the running plugin is asked to read it again -- and the host's own copy
		// is reloaded too, because the bindings live there and the input plugin
		// holds a reference to it rather than a copy of its own.
		//
		// The reload is deliberately not a whole-config reload: recentRoms, the
		// window scale and the plugin ids can all have moved at runtime, and taking
		// the file's version of those would undo whatever just happened.
		nesgui::Config fromFile;
		std::string ignored;
		if (fromFile.load(configPath, &ignored)) {
			for (int port = 0; port < 2; port++) {
				for (int i = 0; i < 8; i++) {
					config.keys[port][i] = fromFile.keys[port][i];
					config.padButtons[port][i] = fromFile.padButtons[port][i];
				}
				config.device[port] = fromFile.device[port];
				config.gamepad[port] = fromFile.gamepad[port];
			}
			config.pluginSettings = fromFile.pluginSettings;
		}

		nesplug::ApplyResult applied = nesplug::APPLY_UNSUPPORTED;
		if (kind == 0)
			applied = video.applySettings();
		else if (kind == 1)
			applied = audio.applySettings();
		else
			applied = input.applySettings();

		// Say so when it did not take, rather than leaving somebody to guess why
		// the picture looks the same. The input plugin reports UNSUPPORTED when it
		// does not own its configuration, which is the normal case here and is
		// already handled by the reload above -- so that one is not worth a box.
		if (applied == nesplug::APPLY_PARTIAL) {
			SDL_Log("some of those settings take effect the next time nes starts");
		}
		app.dropDeadline();
	};

	// Loading is a cold boot: a cartridge going into a slot is not a reset, and
	// the RAM of the game that just left is not the new game's business.
	auto loadRom = [&](const std::string& path) {
		std::string why;
		console.saveBatteryRam();
		if (!console.loadRom(path, &why)) {
			app.dropDeadline();
			// No parent: this takes an SDL_Window, and what the video plugin
			// exports is a native handle. Unparented is the honest option
			// rather than casting one into the other.
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "nes", why.c_str(),
					nullptr);
			app.dropDeadline();
			return false;
		}
		console.powerOn();
		app.romChanged(displayName(path));

		config.noteRecentRom(path);
		if (!config.save(configPath))
			std::fprintf(stderr, "could not write %s\n", configPath.c_str());
		SDL_Log("loaded %s", path.c_str());
		return true;
	};

	/*
	 * Where a slot lives: beside the ROM, named after it.
	 *
	 * The same reasoning as the .sav file. A state belongs to one game, and
	 * keeping it next to that game means moving the folder moves the states with
	 * it -- which is what somebody who organises their ROMs expects.
	 */
	int saveSlot = 0;
	auto slotPath = [&](int slot) {
		std::string path = config.recentRoms.empty() ? std::string()
				: config.recentRoms.front();
		if (path.empty())
			return std::string();
		const std::size_t dot = path.find_last_of('.');
		const std::size_t cut = path.find_last_of("/\\");
		if (dot != std::string::npos && (cut == std::string::npos || dot > cut))
			path.erase(dot);
		return path + ".st" + std::to_string(slot + 1);
	};

	auto closeRom = [&]() {
		console.saveBatteryRam();
		console.setCartridge(nullptr);
		app.romChanged(std::string());
	};

	auto menuState = [&]() {
		nesfe::MenuState state;
		state.romLoaded = console.hasCartridge();
		state.canPickFile = nesfe::fileDialogAvailable();
		state.saveSlot = saveSlot;
		// A slot shows when it was written, taken from the file itself rather
		// than from anything this program remembers -- so a state written by an
		// earlier run, or deleted behind our back, is described correctly.
		for (int i = 0; i < nesfe::MENU_SLOT_COUNT; i++)
			state.slotLabels.push_back(nesfe::fileWrittenAt(slotPath(i)));
		for (std::size_t i = 0; i < config.recentRoms.size(); i++)
			state.recentRoms.push_back(displayName(config.recentRoms[i]));
		state.paused = app.paused();
		state.muted = app.muted();
		state.videoConfigurable = nesplug::hasConfigureDialog(*videoEntry);
		state.audioConfigurable = audioEntry
				&& nesplug::hasConfigureDialog(*audioEntry);
		state.inputConfigurable = nesplug::hasConfigureDialog(*inputEntry);
		return state;
	};

	// Attached to a window this program did not create. The menu is host
	// business -- files, save slots, dialogs -- and obliging every video plugin
	// ever written to host one would be a poor trade for it. A plugin that
	// exports no handle simply gets no menu.
	bool menuOn = nesfe::menuBarAvailable()
			&& nesfe::setMenuBar(video.nativeWindow(),
					nesfe::buildMenu(menuState()), true);
	if (nesfe::menuBarAvailable() && !menuOn)
		SDL_Log("no window handle for a menu; hotkeys still work");

#if defined(_WIN32)
	// The input plugin drains SDL's event queue and drops what it does not
	// recognise, so a menu command cannot arrive that way. A message hook is
	// the one route into this process that does not run through somebody else's
	// plugin.
	SDL_SetWindowsMessageHook([](void*, void*, unsigned int message,
			Uint64 wParam, Sint64 lParam) {
		nesfe::handleMenuCommand(message, wParam, lParam);
	}, nullptr);
#endif

	// Name whatever was loaded from the command line, and remember it: a ROM
	// opened that way belongs in the recent list as much as one picked from the
	// menu.
	if (console.hasCartridge()) {
		app.romChanged(displayName(romPath));
		config.noteRecentRom(romPath);
		// Written now rather than at exit: a ROM named on the command line
		// belongs in the list as much as one picked from the menu, and a crash
		// or a kill should not be what decides whether it was remembered.
		config.save(configPath);
	}

	bool wasPaused = app.paused();
	bool wasMuted = app.muted();
	bool refreshMenu = false;

	while (app.runFrame()) {
		const int action = nesfe::takeMenuAction();
		const int recent = nesfe::recentForAction(action);
		if (recent >= 0) {
			// The list can be rewritten by the load, so take the path first.
			if (recent < static_cast<int>(config.recentRoms.size())) {
				const std::string path = config.recentRoms[recent];
				loadRom(path);
				refreshMenu = true;
			}
		}
		switch (action) {
		case nesfe::MENU_NONE:
			break;
		case nesfe::MENU_LOAD_ROM: {
			if (!nesfe::fileDialogAvailable()) {
				SDL_Log("no file picker on this platform yet -- name a ROM on "
						"the command line");
				break;
			}
			// Start where the last ROM came from, which is almost always where
			// the next one is.
			std::string startDir;
			if (!config.recentRoms.empty()) {
				const std::string& last = config.recentRoms.front();
				const std::size_t cut = last.find_last_of("/\\");
				if (cut != std::string::npos)
					startDir = last.substr(0, cut);
			}
			std::string chosen;
			app.dropDeadline();
			const bool picked = nesfe::chooseRomFile(video.nativeWindow(),
					startDir, &chosen);
			app.dropDeadline();
			if (picked && loadRom(chosen))
				refreshMenu = true;
			break;
		}
		case nesfe::MENU_CLOSE_ROM:
			closeRom();
			refreshMenu = true;
			break;
		case nesfe::MENU_SAVE_STATE: {
			std::string why;
			if (!console.saveState(slotPath(saveSlot), &why))
				SDL_Log("save state: %s", why.c_str());
			else
				SDL_Log("saved slot %d", saveSlot + 1);
			// The slot's timestamp is part of the menu, so it has to be rebuilt.
			refreshMenu = true;
			break;
		}
		case nesfe::MENU_LOAD_STATE: {
			std::string why;
			if (!console.loadState(slotPath(saveSlot), &why)) {
				// Worth a box rather than only the log: this is a thing somebody
				// asked for and did not get, and the reason matters -- a state
				// for another game reads very differently from a missing one.
				app.dropDeadline();
				SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "nes",
						why.c_str(), nullptr);
				app.dropDeadline();
			} else {
				SDL_Log("loaded slot %d", saveSlot + 1);
			}
			break;
		}
		case nesfe::MENU_NEXT_SLOT:
			saveSlot = (saveSlot + 1) % nesfe::MENU_SLOT_COUNT;
			refreshMenu = true;
			break;
		case nesfe::MENU_PREV_SLOT:
			saveSlot = (saveSlot + nesfe::MENU_SLOT_COUNT - 1)
					% nesfe::MENU_SLOT_COUNT;
			refreshMenu = true;
			break;
		case nesfe::MENU_RESET:
			app.postCommand(nesfe::COMMAND_RESET);
			break;
		case nesfe::MENU_HARD_RESET:
			app.postCommand(nesfe::COMMAND_HARD_RESET);
			break;
		case nesfe::MENU_PAUSE:
			app.postCommand(nesfe::COMMAND_PAUSE);
			break;
		case nesfe::MENU_FRAME_ADVANCE:
			app.postCommand(nesfe::COMMAND_STEP_FRAME);
			break;
		case nesfe::MENU_MUTE:
			app.postCommand(nesfe::COMMAND_MUTE);
			break;
		case nesfe::MENU_SCREENSHOT:
			app.postCommand(nesfe::COMMAND_SCREENSHOT);
			break;
		case nesfe::MENU_EXIT:
			app.postCommand(nesfe::COMMAND_QUIT);
			break;
		case nesfe::MENU_CONFIGURE_VIDEO:
			configureKind(0);
			break;
		case nesfe::MENU_CONFIGURE_AUDIO:
			configureKind(1);
			break;
		case nesfe::MENU_CONFIGURE_INPUT:
			configureKind(2);
			break;
		case nesfe::MENU_CONFIGURE_PLUGINS:
			app.postCommand(nesfe::COMMAND_SETTINGS);
			break;
		case nesfe::MENU_HOTKEYS:
			// Built from the configuration each time it is opened, so a binding
			// changed a minute ago is what it describes.
			app.dropDeadline();
			nesfe::showTextBox(video.nativeWindow(), "Keys and Buttons",
					nesfe::keysReferenceText(config));
			app.dropDeadline();
			break;
		case nesfe::MENU_ABOUT:
			app.dropDeadline();
			nesfe::showAboutBox(video.nativeWindow(),
					"nes -- a NES emulator in C++17, on the emu6502 core.\n\n"
					"Video is built in; audio and controllers are plugins.\n"
					"Press F1 for the plugin chooser.");
			app.dropDeadline();
			break;
		default: {
			const int slot = nesfe::slotForAction(action);
			if (slot >= 0) {
				saveSlot = slot;
				refreshMenu = true;
				break;
			}
			// Something listed but not built yet. Recent ROMs were handled
			// above. The menu already shows those disabled, so
			// arriving here means a new item was added without a case, which is
			// worth hearing about.
			if (recent < 0)
				SDL_Log("menu action %d is not wired up yet", action);
			break;
		}
		}

		// The ticks beside Pause and Mute have to follow the state, and the
		// state can change from a key as easily as from the menu. Loading or
		// closing a ROM changes more than a tick: half the bar depends on
		// whether there is a cartridge.
		if (menuOn && (refreshMenu || app.paused() != wasPaused
				|| app.muted() != wasMuted)) {
			wasPaused = app.paused();
			wasMuted = app.muted();
			refreshMenu = false;
			nesfe::setMenuBar(video.nativeWindow(), nesfe::buildMenu(menuState()),
					false);
		}
	}

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
