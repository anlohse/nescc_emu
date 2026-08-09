//
// The SDL backends, wrapped as plugins.
//
// Every one of these is a C shim over the C++ class next door: allocate it in
// create(), free it in destroy(), forward the rest. The shims exist so the
// built-in backends reach the run loop through exactly the same boundary a
// loadable module would, which is what stops the ABI from quietly rotting while
// nothing is using it.
//
// Stage two turns each of these into its own shared library. Nothing in this
// file has to change for that: the three exported functions at the bottom are
// already the module contract, they simply have not been given a .dll to live
// in yet.
//

#include "SdlBackend.h"
#include "../plugin/nes_plugin.h"

#include <cstring>
#include <new>

namespace {

/** Copy a reason into the host's buffer. Nothing is allocated to cross back. */
void setError(char* error, std::size_t size, const char* text) {
	if (!error || size == 0)
		return;
	std::strncpy(error, text ? text : "", size - 1);
	error[size - 1] = '\0';
}

/* ------------------------------------------------------------------------- */
/* Video                                                                      */
/* ------------------------------------------------------------------------- */

void* videoCreate(const nes_host* /*host*/) {
	return new (std::nothrow) nesfe::SdlVideo();
}

void videoDestroy(void* self) {
	delete static_cast<nesfe::SdlVideo*>(self);
}

int videoOpen(void* self, int scale, int fullscreen, const char* title,
		char* error, std::size_t errorSize) {
	nesfe::VideoOptions options;
	options.scale = scale;
	options.fullscreen = fullscreen != 0;
	options.title = title ? title : "nes";

	nesfe::Error reason;
	// The C++ side may throw; a throw crossing a C frame is undefined, so it
	// stops here and becomes a return code like everything else at this border.
	try {
		if (static_cast<nesfe::SdlVideo*>(self)->open(options, &reason))
			return 1;
	} catch (const std::exception& e) {
		reason = e.what();
	} catch (...) {
		reason = "video plugin threw";
	}
	setError(error, errorSize, reason.c_str());
	return 0;
}

void videoClose(void* self) {
	static_cast<nesfe::SdlVideo*>(self)->close();
}

void videoPresent(void* self, const std::uint8_t* indices, int width, int height) {
	static_cast<nesfe::SdlVideo*>(self)->present(indices, width, height);
}

void videoSetTitle(void* self, const char* title) {
	static_cast<nesfe::SdlVideo*>(self)->setTitle(title);
}

int videoSaveScreenshot(void* self, const char* path) {
	return static_cast<nesfe::SdlVideo*>(self)->saveScreenshot(path) ? 1 : 0;
}

void* videoNativeWindow(void* self) {
	return static_cast<nesfe::SdlVideo*>(self)->nativeWindow();
}

const nes_video_api VIDEO_API = {
	sizeof(nes_video_api),
	videoCreate,
	videoDestroy,
	videoOpen,
	videoClose,
	videoPresent,
	videoSetTitle,
	videoSaveScreenshot,
	nullptr,          // no settings dialog yet; the host checks before calling
	videoNativeWindow
};

const nes_plugin_info VIDEO_INFO = {
	sizeof(nes_plugin_info),
	"sdl-video",
	"SDL2 video",
	"1.0",
	NES_PLUGIN_VIDEO
};

/* ------------------------------------------------------------------------- */
/* Audio                                                                      */
/* ------------------------------------------------------------------------- */

void* audioCreate(const nes_host* /*host*/) {
	return new (std::nothrow) nesfe::SdlAudio();
}

void audioDestroy(void* self) {
	delete static_cast<nesfe::SdlAudio*>(self);
}

int audioOpen(void* self, int sampleRate, char* error, std::size_t errorSize) {
	nesfe::Error reason;
	try {
		if (static_cast<nesfe::SdlAudio*>(self)->open(sampleRate, &reason))
			return 1;
	} catch (const std::exception& e) {
		reason = e.what();
	} catch (...) {
		reason = "audio plugin threw";
	}
	setError(error, errorSize, reason.c_str());
	return 0;
}

void audioClose(void* self) {
	static_cast<nesfe::SdlAudio*>(self)->close();
}

int audioIsOpen(void* self) {
	return static_cast<nesfe::SdlAudio*>(self)->isOpen() ? 1 : 0;
}

void audioQueue(void* self, const float* samples, std::size_t count) {
	static_cast<nesfe::SdlAudio*>(self)->queue(samples, count);
}

double audioQueuedSeconds(void* self) {
	return static_cast<nesfe::SdlAudio*>(self)->queuedSeconds();
}

void audioClear(void* self) {
	static_cast<nesfe::SdlAudio*>(self)->clear();
}

const nes_audio_api AUDIO_API = {
	sizeof(nes_audio_api),
	audioCreate,
	audioDestroy,
	audioOpen,
	audioClose,
	audioIsOpen,
	audioQueue,
	audioQueuedSeconds,
	audioClear,
	nullptr
};

const nes_plugin_info AUDIO_INFO = {
	sizeof(nes_plugin_info),
	"sdl-audio",
	"SDL2 audio",
	"1.0",
	NES_PLUGIN_AUDIO
};

/* ------------------------------------------------------------------------- */
/* Input                                                                      */
/* ------------------------------------------------------------------------- */

void* inputCreate(const nes_host* /*host*/) {
	// The default constructor: this plugin loads its own bindings, because it
	// is the one that will own the dialog for editing them.
	return new (std::nothrow) nesfe::SdlInput();
}

void inputDestroy(void* self) {
	delete static_cast<nesfe::SdlInput*>(self);
}

int inputOpen(void* self, char* error, std::size_t errorSize) {
	nesfe::Error reason;
	try {
		if (static_cast<nesfe::SdlInput*>(self)->open(&reason))
			return 1;
	} catch (const std::exception& e) {
		reason = e.what();
	} catch (...) {
		reason = "input plugin threw";
	}
	setError(error, errorSize, reason.c_str());
	return 0;
}

void inputClose(void* self) {
	static_cast<nesfe::SdlInput*>(self)->close();
}

void inputPoll(void* self, nes_input_state* out) {
	nesfe::InputState state;
	static_cast<nesfe::SdlInput*>(self)->poll(&state);
	out->buttons[0] = state.buttons[0];
	out->buttons[1] = state.buttons[1];
	out->commands = state.commands;
	out->turbo = state.turbo ? 1 : 0;
}

const nes_input_api INPUT_API = {
	sizeof(nes_input_api),
	inputCreate,
	inputDestroy,
	inputOpen,
	inputClose,
	inputPoll,
	nullptr
};

const nes_plugin_info INPUT_INFO = {
	sizeof(nes_plugin_info),
	"sdl-input",
	"SDL2 keyboard and gamepads",
	"1.0",
	NES_PLUGIN_INPUT
};

} // namespace

namespace nesfe {

const nes_plugin_info* sdlInputInfo() { return &INPUT_INFO; }
const void* sdlInputApi() { return &INPUT_API; }

const nes_plugin_info* sdlVideoInfo() { return &VIDEO_INFO; }
const void* sdlVideoApi() { return &VIDEO_API; }
const nes_plugin_info* sdlAudioInfo() { return &AUDIO_INFO; }
const void* sdlAudioApi() { return &AUDIO_API; }

std::uint32_t sdlPluginAbiVersion() { return NES_PLUGIN_ABI_VERSION; }

} // namespace nesfe
