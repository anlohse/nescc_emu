#ifndef NES_FRONTEND_SDL_BACKEND_H
#define NES_FRONTEND_SDL_BACKEND_H

//
// The SDL implementation of every backend.
//
// All three live in one header because on SDL they are not really independent:
// SDL_PollEvent drains one queue that carries window, keyboard and gamepad
// events alike, so the input backend cannot run without the video backend
// having initialised the video subsystem. That coupling is genuine and worth
// naming rather than hiding -- it is exactly the sort of thing that makes a
// plugin ABI harder than it looks, because two separately loaded modules would
// have to agree about who owns the event queue.
//

#include "Backend.h"
#include "../GuiConfig.h"

#include <SDL.h>

#include <cstdint>
#include <vector>

namespace nesfe {

/** A window, a renderer and a streaming texture. */
class SdlVideo : public VideoSink {
public:
	SdlVideo();
	~SdlVideo();

	bool open(const VideoOptions& options, Error* error) override;
	void close() override;
	void present(const std::uint8_t* indices, int width, int height) override;
	void setTitle(const char* title) override;
	bool saveScreenshot(const char* path) override;

	/** The native handle -- HWND, X11 Window -- for a dialog to sit over. */
	void* nativeWindow() const;

private:
	SDL_Window* m_window;
	SDL_Renderer* m_renderer;
	SDL_Texture* m_texture;
	std::uint32_t m_argbPalette[64];
	std::vector<std::uint32_t> m_pixels;
	int m_width;
	int m_height;
};

/** Queued audio: no callback thread, so no locking against one. */
class SdlAudio : public AudioSink {
public:
	SdlAudio();
	~SdlAudio();

	bool open(int sampleRate, Error* error) override;
	void close() override;
	bool isOpen() const override { return m_device != 0; }
	void queue(const float* samples, std::size_t count) override;
	double queuedSeconds() const override;
	void clear() override;

private:
	SDL_AudioDeviceID m_device;
	int m_sampleRate;
};

/**
 * Keyboard, gamepads and the window's own events, from one queue.
 *
 * Owns its bindings by default, loading them in open(). That is the shape the
 * plugin split wants -- the controller plugin is the thing that will carry the
 * remapping dialog, so it should be the thing that owns what is being remapped.
 * The constructor taking a reference is for a host that already has a
 * configuration in hand, and for tests that want to supply one directly.
 */
class SdlInput : public InputSource {
public:
	SdlInput();
	explicit SdlInput(const nesgui::Config& config);
	~SdlInput();

	bool open(Error* error) override;
	void close() override;
	void poll(InputState* out) override;

	/**
	 * This plugin's own settings: the bindings dialog.
	 *
	 * Reads the configuration from disk and writes it back rather than editing
	 * whatever this instance happens to hold, because the instance that shows
	 * the dialog is usually a throwaway made for the purpose -- and because the
	 * bindings on disk are what the next launch will use either way.
	 */
	void configure();

	/** How many pads are currently claimed by the two ports. */
	int padCount() const;

private:
	void addPad(int deviceIndex);
	void removePad(SDL_JoystickID id);
	int portOf(SDL_JoystickID id) const;
	std::uint8_t readPad(int port) const;
	std::uint8_t padButtonFor(Uint8 sdlButton, int port) const;

	nesgui::Config m_owned;
	const nesgui::Config& m_config;
	bool m_loadOwn;
	SDL_GameController* m_pads[2];
};

/** SDL's performance counter, and SDL_Delay. */
class SdlClock : public Clock {
public:
	SdlClock();
	double now() const override;
	void sleep(double seconds) override;

private:
	double m_period;   // seconds per performance-counter tick
};

} // namespace nesfe

#endif // NES_FRONTEND_SDL_BACKEND_H
