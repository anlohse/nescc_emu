/*
 * testApp.cpp -- the front-end run loop, driven entirely by test doubles.
 *
 * This file is the argument for the backend interfaces existing at all. Every
 * behaviour here -- pause, fast-forward, reset, the audio queue steering, the
 * pacer's refusal to chase a deadline it has no hope of meeting -- used to be
 * unreachable without a window, a sound card and a real second of wall clock.
 * None of the four doubles below touches a device or sleeps.
 */

#include "TestRom.h"
#include "../src/frontend/App.h"

#include <doctest/doctest.h>

#include <vector>

using namespace nesfe;

namespace {

/** Records what it was shown, and never opens anything. */
class FakeVideo : public VideoSink {
public:
	FakeVideo() : presents(0), screenshots(0), lastIndex(0) { }

	bool open(const VideoOptions&, Error*) override { return true; }
	void close() override { }

	void present(const std::uint8_t* indices, int width, int height) override {
		presents++;
		lastIndex = indices[0];
		lastWidth = width;
		lastHeight = height;
	}

	void setTitle(const char* t) override { title = t; }
	bool saveScreenshot(const char*) override { screenshots++; return true; }

	int presents;
	int screenshots;
	std::uint8_t lastIndex;
	int lastWidth = 0;
	int lastHeight = 0;
	std::string title;
};

/** A queue that plays nothing and drains only when told to. */
class FakeAudio : public AudioSink {
public:
	FakeAudio() : opened(false), clears(0), queued(0), pretendQueuedSeconds(0.0) { }

	bool open(int, Error*) override { opened = true; return true; }
	void close() override { opened = false; }
	bool isOpen() const override { return opened; }

	void queue(const float*, std::size_t count) override { queued += count; }
	double queuedSeconds() const override { return pretendQueuedSeconds; }
	void clear() override { clears++; }

	bool opened;
	int clears;
	std::size_t queued;
	double pretendQueuedSeconds;
};

/** Hands out a scripted sequence, one entry per frame, then nothing. */
class ScriptedInput : public InputSource {
public:
	bool open(Error*) override { return true; }
	void close() override { }

	void poll(InputState* out) override {
		if (m_next < m_script.size())
			*out = m_script[m_next++];
		polls++;
	}

	void push(const InputState& s) { m_script.push_back(s); }

	void pushCommand(unsigned command) {
		InputState s;
		s.commands = command;
		m_script.push_back(s);
	}

	void pushIdle(int count) {
		for (int i = 0; i < count; i++)
			m_script.push_back(InputState());
	}

	int polls = 0;

private:
	std::vector<InputState> m_script;
	std::size_t m_next = 0;
};

/**
 * Time that only moves when asked.
 *
 * The reason the Clock interface exposes now() and sleep() rather than a
 * "wait for the next frame": with the deadline arithmetic in the loop instead
 * of the backend, a clock like this makes pacing exactly reproducible.
 */
class FakeClock : public Clock {
public:
	double now() const override { return m_now; }

	void sleep(double seconds) override {
		sleeps++;
		slept += seconds;
		m_now += seconds;      // a cooperative sleeper, unlike the real one
	}

	void advance(double seconds) { m_now += seconds; }

	int sleeps = 0;
	double slept = 0.0;

private:
	double m_now = 0.0;
};

/** A console running a tiny NROM program that spins. */
std::unique_ptr<nes::Cartridge> spinCart() {
	std::vector<std::uint8_t> prg;
	testrom::setResetVector(prg, 0xC000);
	prg[0x0000] = 0x4C;        // JMP $C000
	prg[0x0001] = 0x00;
	prg[0x0002] = 0xC0;
	return nes::Cartridge::fromINes(testrom::build(testrom::Options(), prg));
}

struct Rig {
	nes::Nes console;
	FakeVideo video;
	FakeAudio audio;
	ScriptedInput input;
	FakeClock clock;

	Rig() {
		auto cart = spinCart();
		REQUIRE(cart != nullptr);
		console.setCartridge(std::move(cart));
		console.reset();
	}

	App make() { return App(console, video, audio, input, clock); }
};

} // namespace

TEST_CASE("the_loop_runs_without_a_window_a_device_or_a_clock") {
	Rig rig;
	rig.input.pushIdle(3);
	rig.input.pushCommand(COMMAND_QUIT);

	App app = rig.make();
	app.run();

	CHECK_EQ(rig.video.presents, 3);
	CHECK_EQ(app.frames(), 3);
	CHECK_EQ(rig.video.lastWidth, nes::Ppu::SCREEN_WIDTH);
	CHECK_EQ(rig.video.lastHeight, nes::Ppu::SCREEN_HEIGHT);
}

TEST_CASE("pause_stops_the_console_but_not_the_picture") {
	// A paused emulator still presents: the window has to keep redrawing or it
	// goes blank the moment anything overlaps it.
	Rig rig;
	rig.input.pushCommand(COMMAND_PAUSE);
	rig.input.pushIdle(4);
	rig.input.pushCommand(COMMAND_QUIT);

	App app = rig.make();
	app.run();

	CHECK(app.paused());
	CHECK_EQ(app.frames(), 0);        // nothing was stepped
	CHECK_EQ(rig.video.presents, 5);  // but every frame was shown
}

TEST_CASE("frame_step_advances_exactly_one_frame_while_paused") {
	Rig rig;
	rig.input.pushCommand(COMMAND_PAUSE);
	rig.input.pushCommand(COMMAND_STEP_FRAME);
	rig.input.pushIdle(3);
	rig.input.pushCommand(COMMAND_STEP_FRAME);
	rig.input.pushCommand(COMMAND_QUIT);

	App app = rig.make();
	app.run();

	CHECK(app.paused());
	CHECK_EQ(app.frames(), 2);
}

TEST_CASE("reset_saves_first_and_drops_what_is_queued") {
	Rig rig;
	rig.audio.open(44100, nullptr);
	rig.input.pushIdle(1);
	rig.input.pushCommand(COMMAND_RESET);
	rig.input.pushCommand(COMMAND_QUIT);

	App app = rig.make();
	app.run();

	// The queue holds audio from before the reset, mid-note; it belongs to the
	// run that just ended.
	CHECK_EQ(rig.audio.clears, 1);
	CHECK_EQ(rig.console.cpuRegisters().pc, 0xC000);
}

TEST_CASE("fast_forward_stops_generating_audio_and_stops_waiting") {
	Rig rig;
	rig.audio.open(44100, nullptr);

	InputState turbo;
	turbo.turbo = true;
	for (int i = 0; i < 5; i++)
		rig.input.push(turbo);
	rig.input.pushCommand(COMMAND_QUIT);

	App app = rig.make();
	app.run();

	CHECK_EQ(rig.clock.sleeps, 0);        // never paced
	CHECK_EQ(rig.audio.queued, 0);        // and never queued a sample
	CHECK_EQ(rig.audio.clears, 1);        // one discontinuity, on entering it
}

TEST_CASE("the_pacer_waits_a_frame_at_a_time") {
	Rig rig;
	rig.input.pushIdle(4);
	rig.input.pushCommand(COMMAND_QUIT);

	App app = rig.make();
	app.run();

	// The first frame only establishes the deadline; the rest each wait one
	// frame's worth, and the console is NTSC so that is 1/60.0988 s.
	//
	// Exactly, not approximately: nothing else consumes time on a fake clock,
	// so this pins the deadline arithmetic rather than merely sanity-checking
	// it. Drift would show up here as an accumulating error.
	const double frame = 1.0 / rig.console.frameRate();
	CHECK_EQ(rig.clock.sleeps, 3);
	CHECK(rig.clock.slept == doctest::Approx(frame * 3.0));
	CHECK_EQ(app.rebases(), 0);
}

TEST_CASE("the_pacer_gives_up_rather_than_chase_a_lost_deadline") {
	// Dragging a window, or a breakpoint, can stall for many frames. Running
	// twenty at once to "make up time" is worse than the lost time was.
	Rig rig;

	App app = rig.make();
	InputState idle;
	rig.input.push(idle);
	app.runFrame();                    // establishes the deadline

	rig.clock.advance(2.0);            // two seconds of nothing
	rig.input.push(idle);
	app.runFrame();

	CHECK_EQ(app.rebases(), 1);
	CHECK_EQ(rig.clock.sleeps, 0);     // and it did not sleep to get there
}

TEST_CASE("a_short_audio_queue_makes_the_resampler_emit_more") {
	// The steering that holds two free-running clocks together. Only the sign
	// matters -- the correction is a fraction of a percent either way.
	const int cpuHz = 1789773;
	std::vector<float> input(cpuHz / 60, 0.25f);

	std::vector<float> starved, flooded;
	Resampler low(cpuHz, 44100, 0.035, 0.065);
	Resampler high(cpuHz, 44100, 0.035, 0.065);
	low.process(input, 0.010, &starved);    // device nearly dry
	high.process(input, 0.200, &flooded);   // device far ahead

	CHECK(starved.size() > flooded.size());
}

TEST_CASE("audio_samples_are_dropped_rather_than_accumulated_while_muted") {
	// Muting stops them reaching the device, but the APU still produces them,
	// and nothing must be left holding onto a growing buffer.
	Rig rig;
	rig.audio.open(44100, nullptr);
	rig.input.pushCommand(COMMAND_MUTE);
	rig.input.pushIdle(4);
	rig.input.pushCommand(COMMAND_QUIT);

	App app = rig.make();
	app.run();

	CHECK(app.muted());
	CHECK_EQ(rig.audio.queued, 0);
	CHECK(rig.console.apu().samples().empty());
}

TEST_CASE("a_screenshot_is_asked_of_the_video_backend_not_taken_behind_it") {
	Rig rig;
	rig.input.pushCommand(COMMAND_SCREENSHOT);
	rig.input.pushCommand(COMMAND_QUIT);

	App app = rig.make();
	app.run();

	CHECK_EQ(rig.video.screenshots, 1);
}

TEST_CASE("the_title_reports_the_rate_actually_measured") {
	// Not the rate asked for: the measurement interval overshoots by up to a
	// frame, and rounding that away is how a correct 60.1 shows up as 61.
	Rig rig;
	App app = rig.make();

	InputState idle;
	for (int i = 0; i < 70; i++) {
		rig.input.push(idle);
		app.runFrame();
	}

	CHECK(app.measuredFps() > 55.0);
	CHECK(app.measuredFps() < 65.0);
	CHECK(rig.video.title.find("fps") != std::string::npos);
}
