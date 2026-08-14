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
#include "../../plugin/FieldsDialog.h"

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {

/** This plugin's id, and so the name its settings are filed under. */
const char* const PLUGIN_ID = "sdl-audio";

/** The buffer sizes offered, in samples. Powers of two, as a device wants. */
const int BUFFER_SIZES[] = { 256, 512, 1024, 2048, 4096 };
const int BUFFER_COUNT = 5;

class SdlAudioDevice {
public:
	explicit SdlAudioDevice(const nes_host* host) :
			m_host(host), m_device(0), m_sampleRate(0), m_volume(100),
			m_ownsSubsystem(false) { }

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

		// The settings this plugin owns, asked for at the moment they are
		// needed rather than in the constructor: a dialog may have changed them
		// since the program started.
		m_volume = clampVolume(std::atoi(setting("volume", "100").c_str()));
		const int buffer = validBuffer(std::atoi(setting("buffer", "1024").c_str()));
		const std::string device = setting("device", "");

		SDL_AudioSpec want;
		SDL_zero(want);
		want.freq = sampleRate;
		want.format = AUDIO_F32SYS;
		want.channels = 1;         // the NES mixes to one signal
		want.samples = static_cast<Uint16>(buffer);
		want.callback = nullptr;   // queued, so no callback thread to lock against

		SDL_AudioSpec got;
		// An empty name means whatever the system calls default. So does a name
		// that is no longer there: a pair of headphones being unplugged between
		// two runs should cost the setting, not the sound.
		m_device = SDL_OpenAudioDevice(device.empty() ? nullptr : device.c_str(),
				0, &want, &got, 0);
		if (m_device == 0 && !device.empty()) {
			log("the chosen audio device is not available; using the default");
			m_device = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
		}
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
		if (!m_device)
			return;
		if (m_volume >= 100) {
			SDL_QueueAudio(m_device, samples,
					static_cast<Uint32>(count * sizeof(float)));
			return;
		}
		// Scaled into a buffer of this plugin's own: the samples belong to the
		// host and are not ours to write over. Linear rather than logarithmic
		// deliberately -- this is a trim against the rest of the desktop, and
		// the system volume control is still the loudness knob people reach
		// for.
		const float gain = m_volume / 100.0f;
		m_scaled.resize(count);
		for (std::size_t i = 0; i < count; i++)
			m_scaled[i] = samples[i] * gain;
		SDL_QueueAudio(m_device, m_scaled.data(),
				static_cast<Uint32>(count * sizeof(float)));
	}

	/**
	 * Re-read the settings and act on them now.
	 *
	 * Volume is free: it is applied to each sample as it goes out, so reading the
	 * new value is the whole job. The device and the buffer size are properties of
	 * an open SDL device and can only change by closing and opening one -- which is
	 * done here, at the same sample rate, because a person who has just chosen a
	 * different output expects to hear it from the different output.
	 *
	 * @return true when everything was applied.
	 */
	bool applySettings() {
		m_volume = clampVolume(std::atoi(setting("volume", "100").c_str()));

		if (!m_device)
			return true;    // nothing open: the next open() reads it all anyway

		// Reopening drops whatever was queued, which is a short silence rather
		// than a click: the queue holds sound that belonged to the old device.
		const int rate = m_sampleRate;
		close();
		char error[256];
		error[0] = '\0';
		if (!open(rate, error, sizeof error)) {
			log(error[0] ? error : "could not reopen the audio device");
			return false;
		}
		return true;
	}

	/** This plugin's own settings: the device, how loud, and how much latency. */
	void configure() {
		if (!nesdlg::fieldsDialogAvailable()) {
			log("no audio settings dialog on this platform yet");
			return;
		}

		// Device names come from SDL, which needs its audio subsystem up. A
		// dialog is usually opened on a throwaway instance that has opened
		// nothing, so bring it up for as long as this takes and put it back.
		const bool startedHere = !SDL_WasInit(SDL_INIT_AUDIO)
				&& SDL_InitSubSystem(SDL_INIT_AUDIO) == 0;

		std::vector<nesdlg::Field> fields(3);
		std::vector<std::string> deviceNames;

		fields[0].label = "Output";
		fields[0].options.push_back("System default");
		deviceNames.push_back("");
		const std::string current = setting("device", "");
		const int devices = SDL_GetNumAudioDevices(0);
		for (int i = 0; i < devices; i++) {
			const char* name = SDL_GetAudioDeviceName(i, 0);
			if (!name)
				continue;
			deviceNames.push_back(name);
			fields[0].options.push_back(name);
			if (current == name)
				fields[0].selected = static_cast<int>(deviceNames.size()) - 1;
		}

		fields[1].label = "Volume";
		const int volume = clampVolume(std::atoi(setting("volume", "100").c_str()));
		for (int percent = 100; percent >= 0; percent -= 10) {
			char label[32];
			std::snprintf(label, sizeof(label), "%d%%", percent);
			fields[1].options.push_back(label);
			if (percent == roundToTen(volume))
				fields[1].selected = (100 - percent) / 10;
		}

		fields[2].label = "Buffer";
		const int buffer = validBuffer(std::atoi(setting("buffer", "1024").c_str()));
		for (int i = 0; i < BUFFER_COUNT; i++) {
			// Milliseconds are the number that means something to a player; the
			// sample count is only how a device has to be asked.
			char label[64];
			std::snprintf(label, sizeof(label), "%d samples  (about %d ms)",
					BUFFER_SIZES[i], BUFFER_SIZES[i] * 1000 / 44100);
			fields[2].options.push_back(label);
			if (BUFFER_SIZES[i] == buffer)
				fields[2].selected = i;
		}

		void* parent = NULL;
		if (NES_HOST_PROVIDES(m_host, window_handle))
			parent = m_host->window_handle(m_host->context);

		const bool accepted = nesdlg::showFieldsDialog("SDL2 audio", parent,
				&fields, "Applied as soon as you press OK; the device is reopened.");

		if (startedHere)
			SDL_QuitSubSystem(SDL_INIT_AUDIO);
		if (!accepted)
			return;

		putSetting("device", deviceNames[fields[0].selected].c_str());
		char value[16];
		std::snprintf(value, sizeof(value), "%d", 100 - fields[1].selected * 10);
		putSetting("volume", value);
		std::snprintf(value, sizeof(value), "%d", BUFFER_SIZES[fields[2].selected]);
		putSetting("buffer", value);
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

	static int clampVolume(int volume) {
		if (volume < 0) return 0;
		if (volume > 100) return 100;
		return volume;
	}

	static int roundToTen(int volume) {
		return ((volume + 5) / 10) * 10;
	}

	/** The nearest size offered, so a hand-edited file cannot ask for three. */
	static int validBuffer(int samples) {
		for (int i = 0; i < BUFFER_COUNT; i++)
			if (BUFFER_SIZES[i] == samples)
				return samples;
		return 1024;
	}

	/*
	 * Settings live with the host. This plugin has nowhere of its own to keep
	 * them, and should not invent one: a program with four configuration files
	 * in three formats is what that leads to.
	 */
	std::string setting(const char* key, const char* fallback) const {
		if (!NES_HOST_PROVIDES(m_host, get_setting))
			return fallback;
		char value[256] = { 0 };
		const size_t length = m_host->get_setting(m_host->context, PLUGIN_ID,
				key, value, sizeof(value));
		if (length == 0 || length >= sizeof(value))
			return fallback;
		return value;
	}

	void putSetting(const char* key, const char* value) {
		if (NES_HOST_PROVIDES(m_host, set_setting))
			m_host->set_setting(m_host->context, PLUGIN_ID, key, value);
	}

	void log(const char* message) {
		if (NES_HOST_PROVIDES(m_host, log))
			m_host->log(m_host->context, message);
		else
			SDL_Log("%s", message);
	}

	/** Only quit what we started. The host may still be using its own. */
	void releaseSubsystem() {
		if (m_ownsSubsystem) {
			SDL_QuitSubSystem(SDL_INIT_AUDIO);
			m_ownsSubsystem = false;
		}
	}

	const nes_host* m_host;
	SDL_AudioDeviceID m_device;
	int m_sampleRate;
	int m_volume;
	std::vector<float> m_scaled;
	bool m_ownsSubsystem;
};

/* ------------------------------------------------------------------------- */
/* The C surface                                                              */
/* ------------------------------------------------------------------------- */

void* audioCreate(const nes_host* host) {
	// nothrow: an exception escaping into the host's C frame is undefined, and
	// there is nothing useful to do with a bad_alloc here anyway.
	return new (std::nothrow) SdlAudioDevice(host);
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

void audioConfigure(void* self) {
	// Nothing may be thrown back across the boundary, and there is nothing to
	// report either: a settings dialog that failed changed no settings.
	try {
		static_cast<SdlAudioDevice*>(self)->configure();
	} catch (...) { }
}

int audioApplySettings(void* self) {
	try {
		return static_cast<SdlAudioDevice*>(self)->applySettings() ? 1 : 0;
	} catch (...) {
		return 0;
	}
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
	audioConfigure,
	audioApplySettings
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
