//
// audio_sdl -- the SDL2 audio backend, as a loadable module.
//
// The first plugin to leave the executable, chosen because audio has the
// narrowest interface and no window coupling: nothing here needs the host's
// event queue or its window handle, so loading, versioning and lifetime can be
// got right without also arguing about who owns the message pump.
//
// Self-contained on purpose. It does not share the host's SDL state and does
// not assume the host called SDL_Init at all -- a plugin that only works when
// the program that loaded it happens to have initialised the right subsystem is
// a plugin that will break the first time somebody writes a different host.
//

#include "../../plugin/nes_plugin.h"

#include <SDL.h>

#include <cstring>
#include <new>

namespace {

class SdlAudioDevice {
public:
	SdlAudioDevice() : m_device(0), m_sampleRate(0), m_ownsSubsystem(false) { }

	~SdlAudioDevice() { close(); }

	bool open(int sampleRate, char* error, std::size_t errorSize) {
		// Bring up the subsystem if nobody has. SDL reference-counts this, so
		// doing it here is safe whether or not the host also did -- and it is
		// what lets this module work under a host that knows nothing of SDL.
		if (!SDL_WasInit(SDL_INIT_AUDIO)) {
			if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
				setError(error, errorSize, SDL_GetError());
				return false;
			}
			m_ownsSubsystem = true;
		}

		SDL_AudioSpec want;
		SDL_zero(want);
		want.freq = sampleRate;
		want.format = AUDIO_F32SYS;
		want.channels = 1;         // the NES mixes to one signal
		want.samples = 1024;
		want.callback = nullptr;   // queued, so no callback thread to lock against

		SDL_AudioSpec got;
		m_device = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
		if (m_device == 0) {
			setError(error, errorSize, SDL_GetError());
			releaseSubsystem();
			return false;
		}
		m_sampleRate = got.freq ? got.freq : sampleRate;
		SDL_PauseAudioDevice(m_device, 0);
		return true;
	}

	void close() {
		if (m_device) {
			SDL_CloseAudioDevice(m_device);
			m_device = 0;
		}
		releaseSubsystem();
	}

	bool isOpen() const { return m_device != 0; }

	void queue(const float* samples, std::size_t count) {
		if (m_device)
			SDL_QueueAudio(m_device, samples,
					static_cast<Uint32>(count * sizeof(float)));
	}

	double queuedSeconds() const {
		if (!m_device || m_sampleRate <= 0)
			return 0.0;
		return static_cast<double>(SDL_GetQueuedAudioSize(m_device))
				/ sizeof(float) / m_sampleRate;
	}

	void clear() {
		if (m_device)
			SDL_ClearQueuedAudio(m_device);
	}

private:
	static void setError(char* error, std::size_t size, const char* text) {
		if (!error || size == 0)
			return;
		std::strncpy(error, text ? text : "audio device unavailable", size - 1);
		error[size - 1] = '\0';
	}

	/** Only quit what we started. The host may still be using its own. */
	void releaseSubsystem() {
		if (m_ownsSubsystem) {
			SDL_QuitSubSystem(SDL_INIT_AUDIO);
			m_ownsSubsystem = false;
		}
	}

	SDL_AudioDeviceID m_device;
	int m_sampleRate;
	bool m_ownsSubsystem;
};

/* ------------------------------------------------------------------------- */
/* The C surface                                                              */
/* ------------------------------------------------------------------------- */

void* audioCreate(const nes_host* /*host*/) {
	// nothrow: an exception escaping into the host's C frame is undefined, and
	// there is nothing useful to do with a bad_alloc here anyway.
	return new (std::nothrow) SdlAudioDevice();
}

void audioDestroy(void* self) {
	delete static_cast<SdlAudioDevice*>(self);
}

int audioOpen(void* self, int sampleRate, char* error, size_t errorSize) {
	return static_cast<SdlAudioDevice*>(self)->open(sampleRate, error, errorSize) ? 1 : 0;
}

void audioClose(void* self) {
	static_cast<SdlAudioDevice*>(self)->close();
}

int audioIsOpen(void* self) {
	return static_cast<SdlAudioDevice*>(self)->isOpen() ? 1 : 0;
}

void audioQueue(void* self, const float* samples, size_t count) {
	static_cast<SdlAudioDevice*>(self)->queue(samples, count);
}

double audioQueuedSeconds(void* self) {
	return static_cast<SdlAudioDevice*>(self)->queuedSeconds();
}

void audioClear(void* self) {
	static_cast<SdlAudioDevice*>(self)->clear();
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
	nullptr        // no settings dialog yet
};

const nes_plugin_info AUDIO_INFO = {
	sizeof(nes_plugin_info),
	"sdl-audio",
	"SDL2 audio",
	"1.0",
	NES_PLUGIN_AUDIO
};

} // namespace

/* ------------------------------------------------------------------------- */
/* Exports                                                                    */
/* ------------------------------------------------------------------------- */

#if defined(_WIN32)
#  define NES_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define NES_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

NES_PLUGIN_EXPORT uint32_t nes_plugin_abi_version(void) {
	return NES_PLUGIN_ABI_VERSION;
}

NES_PLUGIN_EXPORT const nes_plugin_info* nes_plugin_describe(void) {
	return &AUDIO_INFO;
}

NES_PLUGIN_EXPORT const void* nes_plugin_api(void) {
	return &AUDIO_API;
}
