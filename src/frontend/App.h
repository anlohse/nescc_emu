#ifndef NES_FRONTEND_APP_H
#define NES_FRONTEND_APP_H

#include "Backend.h"

#include "nes/Nes.h"

#include <functional>
#include <vector>

namespace nesfe {

/**
 * Decimates the APU's CPU-rate output down to the audio device's rate.
 *
 * Two clocks nobody synchronised: the frame loop paces off the host's
 * performance counter, the sound card runs off its own crystal. Left alone they
 * drift, and the queue either empties into crackling or grows into latency. So
 * the ratio is nudged a fraction of a percent based on how much audio is
 * already waiting -- far too small to hear, and it holds the two together
 * indefinitely.
 */
class Resampler {
public:
	/**
	 * @param lowSeconds   below this much queued, emit slightly more.
	 * @param highSeconds  above it, slightly fewer.
	 */
	Resampler(int cpuClockHz, int sampleRate, double lowSeconds, double highSeconds);

	/**
	 * @param queuedSeconds  how much audio the device still has to play; the
	 *                       only feedback there is, and what stops the drift.
	 */
	void process(const std::vector<float>& input, double queuedSeconds,
			std::vector<float>* out);

	void reset();

private:
	int m_cpuClockHz;
	int m_sampleRate;
	double m_lowSeconds;
	double m_highSeconds;
	double m_phase;
	double m_accumulator;
	int m_count;
};

/**
 * The emulator as a program: one console, four backends, and the loop between.
 *
 * Contains no SDL, no file paths and no window. Everything host-shaped arrives
 * through the four interfaces, which is what lets the whole loop -- pacing,
 * pause, fast-forward, audio steering, battery saves -- run in a test with no
 * window and no wall clock.
 */
class App {
public:
	struct Options {
		int audioSampleRate = 44100;
		/** How much audio to keep queued ahead of the device. Three frames. */
		double targetQueuedSeconds = 0.050;
		double queueSlackSeconds = 0.015;
		/** Give up catching up after this many frames behind and rebase. */
		int maxFramesBehind = 4;
	};

	App(nes::Nes& console, VideoSink& video, AudioSink& audio,
			InputSource& input, Clock& clock, const Options& options = Options());

	/**
	 * What to run when the player asks for the settings dialog.
	 *
	 * A callback rather than anything concrete, because the loop must stay free
	 * of toolkits: a dialog is the single most platform-bound thing in the
	 * program, and knowing about one here would undo the reason this class can
	 * be tested at all. Unset means the command is ignored.
	 *
	 * The emulator pauses around the call. A dialog is modal and may sit open
	 * for a minute; coming back to a deadline that far in the past would look
	 * like a stall and be chased through the pacer's catch-up logic.
	 */
	void setSettingsHandler(std::function<void()> handler) {
		m_settingsHandler = handler;
	}

	/**
	 * Run one frame: poll input, act on it, step the console, present, pace.
	 * @return false once the player has asked to quit.
	 */
	bool runFrame();

	/** runFrame() until it says to stop. */
	void run();

	bool paused() const { return m_paused; }
	bool muted() const { return m_muted; }
	/** Frames per second as actually measured, or 0 before the first second. */
	double measuredFps() const { return m_measuredFps; }
	/** How many frames have been produced. */
	unsigned long frames() const { return m_frames; }
	/** How often the pacer gave up catching up and rebased its deadline. */
	unsigned long rebases() const { return m_rebases; }

private:
	void applyCommands(const InputState& state);
	void pumpAudio(bool generating);
	void pace();

	nes::Nes& m_console;
	VideoSink& m_video;
	AudioSink& m_audio;
	InputSource& m_input;
	Clock& m_clock;
	Options m_options;

	Resampler m_resampler;
	std::vector<float> m_audioOut;

	double m_frameSeconds;
	double m_deadline;
	bool m_deadlineValid;

	bool m_running;
	bool m_paused;
	bool m_muted;
	bool m_wasTurbo;
	int m_shotIndex;

	std::function<void()> m_settingsHandler;

	unsigned long m_frames;
	unsigned long m_rebases;
	int m_framesThisSecond;
	double m_fpsMark;
	double m_measuredFps;
};

} // namespace nesfe

#endif // NES_FRONTEND_APP_H
