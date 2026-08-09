#ifndef NES_FRONTEND_BACKEND_H
#define NES_FRONTEND_BACKEND_H

//
// The seams between the emulator and the machine it is running on.
//
// Four things the console needs from its host and cannot provide itself: a
// surface to draw on, somewhere to send samples, someone pressing buttons, and
// a clock to pace against. Each is an abstract class here and an SDL
// implementation elsewhere, so the run loop in App.cpp contains no SDL at all
// and can be driven by test doubles with no window, no sound device and no
// wall clock.
//
// The signatures deliberately avoid anything that would be awkward to carry
// across a C boundary -- no containers in arguments, no ownership passed either
// way, errors reported through an out-parameter rather than thrown. That costs
// nothing today and keeps the door open if these ever become loadable modules;
// see ROADMAP.md, which argues for staying on this side of that door.
//

#include "../plugin/nes_plugin.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace nesfe {

/** Where a backend puts the reason it could not start. */
typedef std::string Error;

/* ------------------------------------------------------------------------- */
/* Video                                                                      */
/* ------------------------------------------------------------------------- */

struct VideoOptions {
	int scale = 3;
	bool fullscreen = false;
	const char* title = "nes";
};

/**
 * Somewhere to put a finished frame.
 *
 * Frames arrive as one byte per pixel of NES palette index, which is what the
 * PPU actually produces -- converting to real colour is the backend's business,
 * because only it knows what format its surface wants. A backend that writes
 * PNGs and one that drives a texture want different things from the same 61440
 * bytes.
 */
class VideoSink {
public:
	virtual ~VideoSink() { }

	/** @return false and fill @p error if there is no surface to be had. */
	virtual bool open(const VideoOptions& options, Error* error) = 0;
	virtual void close() = 0;

	/**
	 * Show one frame.
	 * @param indices  width * height palette indices, 0-63 in the low six bits.
	 */
	virtual void present(const std::uint8_t* indices, int width, int height) = 0;

	/** Status text for a title bar or overlay. Backends without one ignore it. */
	virtual void setTitle(const char* title) { (void) title; }

	/** Write the last presented frame somewhere. @return false if unsupported. */
	virtual bool saveScreenshot(const char* path) { (void) path; return false; }
};

/* ------------------------------------------------------------------------- */
/* Audio                                                                      */
/* ------------------------------------------------------------------------- */

/**
 * Somewhere to put samples, and a way to ask how far behind it is.
 *
 * queuedSeconds() is the part that matters and the part a naive interface
 * leaves out. The emulator and the sound device run off different clocks, so
 * the resampling ratio has to be nudged by how much audio is already waiting --
 * without that number there is nothing to steer by, and the queue drifts into
 * either crackling or latency.
 */
class AudioSink {
public:
	virtual ~AudioSink() { }

	/** @return false and fill @p error if no device could be opened. */
	virtual bool open(int sampleRate, Error* error) = 0;
	virtual void close() = 0;

	/** True once open() has succeeded and not yet been closed. */
	virtual bool isOpen() const = 0;

	/** Hand over @p count mono samples in -1..1. */
	virtual void queue(const float* samples, std::size_t count) = 0;

	/** How much audio is waiting to be played. */
	virtual double queuedSeconds() const = 0;

	/** Throw away what is queued -- after a reset, or entering fast-forward. */
	virtual void clear() = 0;
};

/* ------------------------------------------------------------------------- */
/* Input                                                                      */
/* ------------------------------------------------------------------------- */

/**
 * Things the player can ask of the emulator, as opposed to of the game.
 *
 * The values come from the plugin ABI rather than being written twice. Two
 * lists of bit flags that must agree is a bug waiting for someone to add a
 * command to one of them.
 */
enum Command {
	COMMAND_NONE       = NES_COMMAND_NONE,
	COMMAND_QUIT       = NES_COMMAND_QUIT,
	COMMAND_PAUSE      = NES_COMMAND_PAUSE,          // toggle
	COMMAND_STEP_FRAME = NES_COMMAND_STEP_FRAME,     // advance one frame while paused
	COMMAND_MUTE       = NES_COMMAND_MUTE,           // toggle
	COMMAND_RESET      = NES_COMMAND_RESET,
	COMMAND_SCREENSHOT = NES_COMMAND_SCREENSHOT
};

/**
 * One frame's worth of input.
 *
 * @a buttons is the state of each port for the frame about to run. @a commands
 * are edges -- things that happened once since the last poll -- while @a turbo
 * is a level, because it is held rather than pressed.
 */
struct InputState {
	std::uint8_t buttons[2];
	unsigned commands;
	bool turbo;

	InputState() : commands(COMMAND_NONE), turbo(false) {
		buttons[0] = 0;
		buttons[1] = 0;
	}
};

/**
 * Whoever is pressing the buttons.
 *
 * One call per frame produces both the pad state and any commands, because on
 * a real input system they arrive together in the same event queue and
 * separating them would mean draining it twice.
 */
class InputSource {
public:
	virtual ~InputSource() { }

	virtual bool open(Error* error) = 0;
	virtual void close() = 0;

	/** Drain whatever has happened since the last call. */
	virtual void poll(InputState* out) = 0;
};

/* ------------------------------------------------------------------------- */
/* Time                                                                       */
/* ------------------------------------------------------------------------- */

/**
 * A monotonic clock and an accurate wait.
 *
 * Not "wait for the next frame": the deadline arithmetic that keeps the
 * emulator from drifting is the interesting part and belongs in the run loop
 * where it can be read, changed and tested. A fake clock then makes pacing
 * deterministic, which a real one never is.
 *
 * But sleeping accurately *is* the host's problem. Every operating system's
 * sleep is coarse -- a scheduler tick is most of a frame -- so a real backend
 * sleeps the bulk and spins out the remainder against its own clock. The run
 * loop must not attempt that itself: a loop spinning on now() cannot terminate
 * against a clock that only moves when something asks it to, which is precisely
 * what a test needs.
 */
class Clock {
public:
	virtual ~Clock() { }

	/** Seconds since some fixed point. Only differences are meaningful. */
	virtual double now() const = 0;

	/**
	 * Wait @p seconds as precisely as this host can manage. On return, now()
	 * must have advanced by at least that much.
	 */
	virtual void sleep(double seconds) = 0;
};

} // namespace nesfe

#endif // NES_FRONTEND_BACKEND_H
