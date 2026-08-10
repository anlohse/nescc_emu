#include "App.h"

#include <cstdio>

namespace nesfe {

/* ------------------------------------------------------------------------- */
/* Resampler                                                                  */
/* ------------------------------------------------------------------------- */

Resampler::Resampler(int cpuClockHz, int sampleRate,
		double lowSeconds, double highSeconds) :
		m_cpuClockHz(cpuClockHz), m_sampleRate(sampleRate),
		m_lowSeconds(lowSeconds), m_highSeconds(highSeconds),
		m_phase(0.0), m_accumulator(0.0), m_count(0) { }

void Resampler::process(const std::vector<float>& input, double queuedSeconds,
		std::vector<float>* out) {
	double ratio = static_cast<double>(m_cpuClockHz) / m_sampleRate;
	// Steering, not correcting: four parts in a thousand is a fifth of a
	// semitone spread across seconds, which nobody can hear, and it is enough
	// to hold two free-running clocks together indefinitely.
	if (queuedSeconds > m_highSeconds)
		ratio *= 1.004;          // running long: emit slightly fewer samples
	else if (queuedSeconds < m_lowSeconds)
		ratio *= 0.996;          // running short: emit slightly more

	for (std::size_t i = 0; i < input.size(); i++) {
		// Average across the window rather than picking one sample from it. A
		// box filter is crude, but at 40:1 it removes far more aliasing than
		// point sampling, and the APU has already rolled off at 14 kHz.
		m_accumulator += input[i];
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

void Resampler::reset() {
	m_phase = 0.0;
	m_accumulator = 0.0;
	m_count = 0;
}

/* ------------------------------------------------------------------------- */
/* App                                                                        */
/* ------------------------------------------------------------------------- */

App::App(nes::Nes& console, VideoSink& video, AudioSink& audio,
		InputSource& input, Clock& clock, const Options& options) :
		m_console(console), m_video(video), m_audio(audio), m_input(input),
		m_clock(clock), m_options(options),
		m_resampler(console.cpuClockHz(), options.audioSampleRate,
				options.targetQueuedSeconds - options.queueSlackSeconds,
				options.targetQueuedSeconds + options.queueSlackSeconds),
		m_frameSeconds(1.0 / console.frameRate()),
		m_deadline(0.0), m_deadlineValid(false),
		m_running(true), m_paused(false), m_muted(false), m_wasTurbo(false),
		m_shotIndex(0), m_frames(0), m_rebases(0), m_framesThisSecond(0),
		m_fpsMark(0.0), m_measuredFps(0.0) { }

void App::applyCommands(const InputState& state) {
	if (state.commands & COMMAND_QUIT)
		m_running = false;
	if (state.commands & COMMAND_PAUSE)
		m_paused = !m_paused;
	if (state.commands & COMMAND_MUTE) {
		m_muted = !m_muted;
		m_audio.clear();
	}
	if (state.commands & COMMAND_HARD_RESET) {
		// The switch at the back. Save first for the same reason a reset does:
		// the game is about to lose its work RAM either way.
		m_console.saveBatteryRam();
		m_console.powerOn();
		m_audio.clear();
		m_resampler.reset();
		if (m_audio.isOpen())
			m_console.apu().setSampleOutput(true);
	}
	if (state.commands & COMMAND_RESET) {
		// Save before resetting: the game is about to lose whatever was in its
		// work RAM, and a player pressing reset does not expect that to cost
		// them their file.
		m_console.saveBatteryRam();
		m_console.reset();
		// A reset silences the APU mid-note; whatever is queued belongs to the
		// run that just ended.
		m_audio.clear();
		m_resampler.reset();
		if (m_audio.isOpen())
			m_console.apu().setSampleOutput(true);
	}
	if ((state.commands & COMMAND_SETTINGS) && m_settingsHandler) {
		// Whatever the dialog does, it takes wall-clock time this loop has no
		// way to account for. Drop the deadline so the next frame starts a new
		// one rather than being measured against a moment long past, and clear
		// the queue so the device is not left playing a second of stale audio
		// underneath a modal window.
		m_audio.clear();
		m_resampler.reset();
		m_settingsHandler();
		m_deadlineValid = false;
	}
	if (state.commands & COMMAND_SCREENSHOT) {
		char name[64];
		std::snprintf(name, sizeof(name), "nes-shot-%04d.bmp", m_shotIndex);
		if (m_video.saveScreenshot(name))
			m_shotIndex++;
	}
}

void App::setZapperSource(std::function<bool(ZapperState*)> poll,
		std::function<bool(int, int, int*, int*)> toFrame) {
	m_pollZapper = poll;
	m_windowToFrame = toFrame;
	// Port two, because that is the port every light-gun game reads. The device
	// is set once rather than per frame: a console does not learn what is
	// plugged in, it simply answers the pins it has.
	m_console.controller(1).setDevice(nes::Controller::DEVICE_ZAPPER);
}

void App::updateZapper() {
	if (!m_pollZapper)
		return;

	ZapperState gun;
	if (!m_pollZapper(&gun) || !gun.connected)
		return;

	int frameX = -1;
	int frameY = -1;
	if (m_windowToFrame)
		m_windowToFrame(gun.windowX, gun.windowY, &frameX, &frameY);

	// Off the picture stays negative, and the console reads that as no light --
	// which is what pointing the gun away from the television does, and what a
	// game checks before it believes a shot.
	m_console.controller(gun.port).setZapper(frameX, frameY, gun.trigger);
}

void App::pumpAudio(bool generating) {
	if (m_audio.isOpen() && generating) {
		m_audioOut.clear();
		m_resampler.process(m_console.apu().samples(), m_audio.queuedSeconds(),
				&m_audioOut);
		if (!m_audioOut.empty())
			m_audio.queue(m_audioOut.data(), m_audioOut.size());
	}
	// Cleared either way: samples produced while muted or fast-forwarding are
	// still produced, and leaving them to pile up would leak.
	m_console.apu().clearSamples();
}

void App::pace() {
	const double now = m_clock.now();
	if (!m_deadlineValid) {
		m_deadline = now + m_frameSeconds;
		m_deadlineValid = true;
		m_fpsMark = now;
		return;
	}

	// How to wait this precisely is the backend's problem, not this loop's.
	if (now < m_deadline)
		m_clock.sleep(m_deadline - now);

	// An absolute deadline advanced by exactly one frame. Measuring the next
	// wait from "now" instead would let every frame's overshoot accumulate, and
	// the emulator would drift slower and slower.
	m_deadline += m_frameSeconds;

	// Dragging the window, or a breakpoint, can stall for many frames. Catch up
	// over a few, then give up and rebase -- running twenty frames at once to
	// "make up time" is worse than the lost time was.
	const double after = m_clock.now();
	if (after > m_deadline + m_frameSeconds * m_options.maxFramesBehind) {
		m_deadline = after + m_frameSeconds;
		m_rebases++;
	}
}

bool App::runFrame() {
	InputState state;
	m_input.poll(&state);
	// Whatever a menu asked for since the last frame arrives here, indistinct
	// from the key that means the same thing.
	state.commands |= m_posted;
	m_posted = 0;
	applyCommands(state);
	if (!m_running)
		return false;

	for (int port = 0; port < 2; port++)
		m_console.controller(port).setButtons(state.buttons[port]);

	// Before the frame runs, so the aim point is already in place when the game
	// reads the gun partway down the picture it is about to draw.
	updateZapper();

	// Fast-forward produces frames faster than the device can play them, so
	// rather than pitch everything up, stop generating and drop what is queued.
	// Entering or leaving it is a discontinuity either way.
	if (state.turbo != m_wasTurbo) {
		m_audio.clear();
		m_resampler.reset();
	}
	m_wasTurbo = state.turbo;
	const bool generating = m_audio.isOpen() && !state.turbo && !m_muted;
	m_console.apu().setSampleOutput(generating);

	const bool stepping = !m_paused || (state.commands & COMMAND_STEP_FRAME) != 0;
	if (stepping) {
		m_console.stepFrame();
		m_frames++;
	}

	pumpAudio(generating);

	m_video.present(m_console.ppu().framebuffer(),
			nes::Ppu::SCREEN_WIDTH, nes::Ppu::SCREEN_HEIGHT);

	if (state.turbo) {
		// Nothing to wait for, but the deadline must not be left in the past or
		// the first paced frame afterwards would think it was seconds behind.
		m_deadlineValid = false;
	} else {
		pace();
	}

	m_framesThisSecond++;
	const double now = m_clock.now();
	if (now - m_fpsMark >= 1.0) {
		// Divide by the interval actually measured, not by one second. The
		// interval overshoots by up to a frame, and rounding that away is how a
		// correctly paced 60.1 fps ends up displayed as 61.
		m_measuredFps = m_framesThisSecond / (now - m_fpsMark);
		m_framesThisSecond = 0;
		m_fpsMark = now;

		char title[128];
		std::snprintf(title, sizeof(title), "nes - %s%.1f fps%s%s",
				m_paused ? "paused - " : "", m_measuredFps,
				state.turbo ? " - turbo" : "",
				(m_muted || !m_audio.isOpen()) ? " - muted" : "");
		m_video.setTitle(title);
	}

	return m_running;
}

void App::run() {
	while (runFrame()) { }
}

} // namespace nesfe
