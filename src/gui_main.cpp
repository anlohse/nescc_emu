//
// nes_gui -- the console in a window, running at NTSC speed with live input.
//
// This is where the emulator stops being a batch job. nes_run stays headless
// for tracing and CI; this one owns a frame clock, a keyboard and a texture.
//

#include "nes/Nes.h"

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// NTSC is 60.0988 Hz, not 60. The difference is a frame every twenty seconds --
// small enough to ignore for one session, large enough to hear once there is
// audio, since the APU's sample rate is derived from the same clock.
const double FRAME_SECONDS = 1.0 / 60.0988;

const int NES_W = nes::Ppu::SCREEN_WIDTH;
const int NES_H = nes::Ppu::SCREEN_HEIGHT;

const int AUDIO_RATE = 44100;
// How much audio to keep queued ahead of the device. Too little and any hitch
// starves it into a crackle; too much and input feels late. Three frames.
const double TARGET_QUEUED_MS = 50.0;
const double QUEUE_SLACK_MS = 15.0;

/**
 * Decimates the APU's CPU-rate output down to the audio device's rate.
 *
 * Two clocks that nobody synchronised: the frame loop paces off the system
 * performance counter, the sound card runs off its own crystal. Left alone they
 * drift, and the queue either empties into crackling or grows into latency. So
 * the ratio is nudged a fraction of a percent based on how much audio is
 * already queued -- far too small to hear, and it holds the two together
 * indefinitely.
 */
class Resampler {
public:
	Resampler() : m_phase(0.0), m_accumulator(0.0), m_count(0) { }

	void process(const std::vector<float>& input, double queuedMs, std::vector<float>* out) {
		double ratio = static_cast<double>(nes::Apu::CPU_CLOCK_HZ) / AUDIO_RATE;
		if (queuedMs > TARGET_QUEUED_MS + QUEUE_SLACK_MS)
			ratio *= 1.004;        // running long: emit slightly fewer samples
		else if (queuedMs < TARGET_QUEUED_MS - QUEUE_SLACK_MS)
			ratio *= 0.996;        // running short: emit slightly more

		for (float sample : input) {
			// Average across the window rather than picking one sample from it.
			// A box filter is crude, but at 40:1 it removes far more aliasing
			// than point sampling, and the APU has already rolled off at 14 kHz.
			m_accumulator += sample;
			m_count++;
			m_phase += 1.0;
			if (m_phase < ratio)
				continue;
			m_phase -= ratio;
			out->push_back(static_cast<float>(m_accumulator / m_count));
			m_accumulator = 0.0;
			m_count = 0;
		}
	}

	void reset() {
		m_phase = 0.0;
		m_accumulator = 0.0;
		m_count = 0;
	}

private:
	double m_phase;
	double m_accumulator;
	int m_count;
};

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

/** Button order matching both the key maps below and the shift register. */
const std::uint8_t BUTTON_BITS[8] = {
	nes::Controller::BUTTON_A,      nes::Controller::BUTTON_B,
	nes::Controller::BUTTON_SELECT, nes::Controller::BUTTON_START,
	nes::Controller::BUTTON_UP,     nes::Controller::BUTTON_DOWN,
	nes::Controller::BUTTON_LEFT,   nes::Controller::BUTTON_RIGHT
};

/**
 * Read a controller's buttons from the keyboard state.
 *
 * Sampled once per frame rather than accumulated from events, which is close to
 * the hardware: a game latches the pad once per frame and sees exactly what was
 * held at that instant. Key repeat and event ordering are things the pad does
 * not have, and polling avoids inventing them.
 */
std::uint8_t readKeys(const Uint8* keys, const SDL_Scancode (&map)[8]) {
	std::uint8_t buttons = 0;
	for (int i = 0; i < 8; i++)
		if (keys[map[i]])
			buttons |= BUTTON_BITS[i];
	return buttons;
}

/** Which button, if any, a key is mapped to. */
std::uint8_t buttonForKey(SDL_Scancode code, const SDL_Scancode (&map)[8]) {
	for (int i = 0; i < 8; i++)
		if (map[i] == code)
			return BUTTON_BITS[i];
	return 0;
}

const SDL_Scancode PLAYER1_KEYS[8] = {
	SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RETURN,
	SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT
};

const SDL_Scancode PLAYER2_KEYS[8] = {
	SDL_SCANCODE_KP_1, SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_PLUS, SDL_SCANCODE_KP_ENTER,
	SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_5, SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_6
};

/** Expand a frame of NES colour indices into ARGB8888 for the texture. */
void blitFrame(const nes::Ppu& ppu, const std::uint32_t* argb, std::uint32_t* out) {
	const std::uint8_t* fb = ppu.framebuffer();
	for (int i = 0; i < NES_W * NES_H; i++)
		out[i] = argb[fb[i] & 0x3F];
}

bool saveScreenshot(const std::uint32_t* pixels, int index) {
	char name[64];
	std::snprintf(name, sizeof(name), "nes-shot-%04d.bmp", index);

	SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(
			const_cast<std::uint32_t*>(pixels), NES_W, NES_H, 32,
			NES_W * static_cast<int>(sizeof(std::uint32_t)), SDL_PIXELFORMAT_ARGB8888);
	if (!surface)
		return false;
	const bool ok = SDL_SaveBMP(surface, name) == 0;
	SDL_FreeSurface(surface);
	if (ok)
		SDL_Log("wrote %s", name);
	return ok;
}

} // namespace

int main(int argc, char* argv[]) {
	const char* romPath = nullptr;
	int scale = 3;
	bool fullscreen = false;
	bool wantAudio = true;

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

	// Audio is requested but not required: a machine with no sound device still
	// gets a working emulator.
	if (SDL_Init(SDL_INIT_VIDEO | (wantAudio ? SDL_INIT_AUDIO : 0)) != 0) {
		if (SDL_Init(SDL_INIT_VIDEO) != 0) {
			std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
			return 1;
		}
		wantAudio = false;
	}

	SDL_Window* window = SDL_CreateWindow("nes",
			SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
			NES_W * scale, NES_H * scale,
			SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
	if (!window) {
		std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
		SDL_Quit();
		return 1;
	}

	// No PRESENTVSYNC: the frame clock below is the pacing authority, and a
	// 144 Hz display would otherwise run the console two and a half times too
	// fast. Letting both throttle produces beat-frequency stutter.
	SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
	if (!renderer)
		renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
	if (!renderer) {
		std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
		SDL_DestroyWindow(window);
		SDL_Quit();
		return 1;
	}

	// Letterbox to the NES's aspect at whatever size the window is dragged to,
	// and keep the scaling crisp -- these are 8x8 tiles, not photographs.
	SDL_RenderSetLogicalSize(renderer, NES_W, NES_H);
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

	SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
			SDL_TEXTUREACCESS_STREAMING, NES_W, NES_H);
	if (!texture) {
		std::fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
		SDL_DestroyRenderer(renderer);
		SDL_DestroyWindow(window);
		SDL_Quit();
		return 1;
	}

	// Queued audio rather than a callback: the emulator produces samples in
	// frame-sized bursts on this thread, and SDL_QueueAudio takes them without
	// any locking against a callback thread. The device's own buffering is what
	// smooths the bursts out.
	SDL_AudioDeviceID audioDevice = 0;
	if (wantAudio) {
		SDL_AudioSpec want;
		SDL_zero(want);
		want.freq = AUDIO_RATE;
		want.format = AUDIO_F32SYS;
		want.channels = 1;         // the NES mixes to one signal
		want.samples = 1024;
		want.callback = nullptr;

		SDL_AudioSpec got;
		audioDevice = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
		if (audioDevice == 0) {
			std::fprintf(stderr, "no audio: %s\n", SDL_GetError());
		} else {
			console.apu().setSampleOutput(true);
			SDL_PauseAudioDevice(audioDevice, 0);
		}
	}
	Resampler resampler;
	std::vector<float> audioOut;

	// The console's fixed palette, pre-expanded with an opaque alpha.
	std::uint32_t argbPalette[64];
	const std::uint32_t* rgb = nes::Ppu::nesPaletteRgb();
	for (int i = 0; i < 64; i++)
		argbPalette[i] = 0xFF000000u | rgb[i];

	static std::uint32_t pixels[NES_W * NES_H];

	const std::uint64_t perfFreq = SDL_GetPerformanceFrequency();
	const std::uint64_t frameTicks =
			static_cast<std::uint64_t>(FRAME_SECONDS * static_cast<double>(perfFreq));

	// An absolute deadline, advanced by exactly one frame each time. Measuring
	// the sleep from "now" instead would let every frame's overshoot accumulate,
	// and the emulator would drift slower and slower.
	std::uint64_t deadline = SDL_GetPerformanceCounter() + frameTicks;

	bool running = true;
	bool paused = false;
	bool muted = false;
	bool wasTurbo = false;
	int shotIndex = 0;
	int framesThisSecond = 0;
	std::uint64_t fpsMark = SDL_GetPerformanceCounter();

	while (running) {
		bool stepOneFrame = false;
		// Buttons whose key went down during this frame. Polling alone would
		// miss a press and release that both arrive between two frames -- rare
		// from a human, routine from anything automating the window -- so a tap
		// is held for the frame it landed in and no shorter.
		std::uint8_t tapped[2] = { 0, 0 };

		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_QUIT) {
				running = false;
			} else if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
				tapped[0] |= buttonForKey(event.key.keysym.scancode, PLAYER1_KEYS);
				tapped[1] |= buttonForKey(event.key.keysym.scancode, PLAYER2_KEYS);
				switch (event.key.keysym.scancode) {
				case SDL_SCANCODE_ESCAPE:
					running = false;
					break;
				case SDL_SCANCODE_P:
				case SDL_SCANCODE_SPACE:
					paused = !paused;
					break;
				case SDL_SCANCODE_N:
					stepOneFrame = paused;
					break;
				case SDL_SCANCODE_M:
					muted = !muted;
					if (audioDevice)
						SDL_ClearQueuedAudio(audioDevice);
					break;
				case SDL_SCANCODE_R:
					console.reset();
					if (audioDevice) {
						// A reset silences the APU mid-note; whatever is queued
						// belongs to the old run.
						SDL_ClearQueuedAudio(audioDevice);
						console.apu().setSampleOutput(true);
						resampler.reset();
					}
					break;
				case SDL_SCANCODE_F12:
					saveScreenshot(pixels, shotIndex++);
					break;
				default:
					break;
				}
			}
		}

		const Uint8* keys = SDL_GetKeyboardState(nullptr);
		console.controller(0).setButtons(readKeys(keys, PLAYER1_KEYS) | tapped[0]);
		console.controller(1).setButtons(readKeys(keys, PLAYER2_KEYS) | tapped[1]);

		// Held Tab runs flat out. The CPU is not the bottleneck -- the frame
		// clock is -- so this just stops waiting.
		const bool turbo = keys[SDL_SCANCODE_TAB] != 0;

		if (audioDevice) {
			// Turbo produces frames faster than the device can play them, so
			// rather than pitch everything up, stop generating and drop what is
			// queued. Entering or leaving it is a discontinuity either way.
			if (turbo != wasTurbo) {
				SDL_ClearQueuedAudio(audioDevice);
				resampler.reset();
			}
			console.apu().setSampleOutput(!turbo && !muted);
		}
		wasTurbo = turbo;

		if (!paused || stepOneFrame) {
			console.stepFrame();
			blitFrame(console.ppu(), argbPalette, pixels);
		}

		if (audioDevice && !turbo && !muted) {
			const double queuedMs = static_cast<double>(SDL_GetQueuedAudioSize(audioDevice))
					/ static_cast<double>(sizeof(float)) * 1000.0 / AUDIO_RATE;
			audioOut.clear();
			resampler.process(console.apu().samples(), queuedMs, &audioOut);
			if (!audioOut.empty())
				SDL_QueueAudio(audioDevice, audioOut.data(),
						static_cast<Uint32>(audioOut.size() * sizeof(float)));
		}
		console.apu().clearSamples();

		SDL_UpdateTexture(texture, nullptr, pixels,
				NES_W * static_cast<int>(sizeof(std::uint32_t)));
		SDL_RenderClear(renderer);
		SDL_RenderCopy(renderer, texture, nullptr, nullptr);
		SDL_RenderPresent(renderer);

		if (!turbo) {
			const std::uint64_t now = SDL_GetPerformanceCounter();
			if (now < deadline) {
				// Sleep the bulk, spin the last millisecond. SDL_Delay's
				// resolution is a whole tick, which is most of a frame.
				const double remainingMs =
						static_cast<double>(deadline - now) * 1000.0 / static_cast<double>(perfFreq);
				if (remainingMs > 2.0)
					SDL_Delay(static_cast<Uint32>(remainingMs - 1.0));
				while (SDL_GetPerformanceCounter() < deadline) { }
			}
		}

		deadline += frameTicks;
		// Dragging the window, or a breakpoint, can stall for many frames. Catch
		// up over a few, then give up and rebase -- running twenty frames at
		// once to "make up time" is worse than the lost time was.
		const std::uint64_t now = SDL_GetPerformanceCounter();
		if (now > deadline + frameTicks * 4)
			deadline = now + frameTicks;

		framesThisSecond++;
		const std::uint64_t elapsed = now - fpsMark;
		if (elapsed >= perfFreq) {
			// Divide by the interval actually measured, not by one second. The
			// interval overshoots by up to a frame, and rounding that away is
			// how a correctly paced 60.1 fps ends up displayed as 61.
			const double fps = static_cast<double>(framesThisSecond)
					* static_cast<double>(perfFreq) / static_cast<double>(elapsed);
			char title[128];
			std::snprintf(title, sizeof(title), "nes - %s%.1f fps%s%s",
					paused ? "paused - " : "", fps, turbo ? " - turbo" : "",
					(muted || !audioDevice) ? " - muted" : "");
			SDL_SetWindowTitle(window, title);
			framesThisSecond = 0;
			fpsMark = now;
		}
	}

	if (audioDevice)
		SDL_CloseAudioDevice(audioDevice);
	SDL_DestroyTexture(texture);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return 0;
}
