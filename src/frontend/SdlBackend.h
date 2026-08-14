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
#include "CrtFilter.h"
#include "../GuiConfig.h"
#include "../plugin/nes_plugin.h"

#include <SDL.h>

#include <cstdint>
#include <string>
#include <vector>

namespace nesfe {

/** A window, a renderer and a streaming texture. */
class SdlVideo : public VideoSink {
public:
	/**
	 * @param host where this plugin's own settings live. Null means the
	 *             defaults, which is what a test or a headless run wants.
	 */
	explicit SdlVideo(const nes_host* host = nullptr);
	~SdlVideo();

	bool open(const VideoOptions& options, Error* error) override;
	void close() override;
	void present(const std::uint8_t* indices, int width, int height) override;
	void setTitle(const char* title) override;
	bool saveScreenshot(const char* path) override;

	/** The native handle -- HWND, X11 Window -- for a dialog to sit over. */
	void* nativeWindow() const;

	/**
	 * Which console pixel a window pixel falls on, or -1 outside the picture.
	 *
	 * SDL's renderer already knows: it is doing the letterboxing and the
	 * scaling, so asking it is both shorter and right at any window size.
	 */
	void windowToFrame(int windowX, int windowY, int* frameX, int* frameY) const;

	/**
	 * This plugin's own settings: how the picture is drawn, and nothing else.
	 *
	 * Window size and fullscreen are not here. The host reads those and passes
	 * them to whichever video plugin is loaded, so the host's own dialog is
	 * where they are edited; a plugin writing them too would be a second author
	 * of one setting. What is left is what only this plugin knows -- the filter
	 * it scales with, and the shape it thinks a console pixel is.
	 */
	void configure();

	/** The NES's pixel aspect on a television: 8:7, not square. */
	static const int WIDE_WIDTH = 292;   // 256 * 8 / 7

private:
	std::string setting(const char* key, const char* fallback) const;
	void putSetting(const char* key, const char* value);

	const nes_host* m_host;
	SDL_Window* m_window;
	SDL_Renderer* m_renderer;
	SDL_Texture* m_texture;
	std::uint32_t m_argbPalette[64];
	std::vector<std::uint32_t> m_pixels;
	int m_width;
	int m_height;
	/** What the renderer's logical width is: m_width, or WIDE_WIDTH. */
	int m_logicalWidth;

	bool m_crt;
	/** Which screen the mask imitates: a television, or a monitor. */
	CrtMaskKind m_maskKind;
	/**
	 * The mask, multiplied over the stretched picture.
	 *
	 * Built in output pixels and rebuilt whenever the window changes size,
	 * because the stripes are a property of the screen rather than of the
	 * signal -- which is what makes the effect work at any size instead of only
	 * at an exact 3x.
	 */
	SDL_Texture* m_mask;
	int m_maskWidth;
	int m_maskHeight;

	/** Where the picture goes in the window, letterboxed to its aspect. */
	SDL_Rect pictureRect() const;
	void ensureMask(const SDL_Rect& into);
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
	 * The mouse, as a light gun on port two.
	 *
	 * Reported whenever the emulator asks; whether the console listens is
	 * decided by whether the loaded game is a light-gun game. Position is in
	 * window pixels -- turning those into console pixels belongs to the video
	 * plugin, which is the only thing that knows the scale and the letterboxing.
	 */
	void pollZapper(ZapperState* out);

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
	/**
	 * Every pad SDL offers, indexed by *gamepad number* and not by console port.
	 *
	 * The distinction is the whole fix. These used to be indexed by port, so the
	 * first pad SDL enumerated became player one -- and a twin adapter enumerates
	 * a controller per socket whether or not a pad is in it, so the phantom could
	 * win. Which pad drives which port is now a setting, and this is just the list
	 * to choose from.
	 */
	SDL_GameController* m_pads[nesgui::MAX_GAMEPADS];
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
