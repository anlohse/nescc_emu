/*
 * testBlargg.cpp -- the emulator against test ROMs written by someone else.
 *
 * Every other test here was written from the same understanding that produced
 * the code, which means a misunderstanding is invisible to both. These ROMs
 * were written by people with the hardware in front of them, and they disagree
 * with an emulator for reasons its author did not think of. That is the whole
 * point of them.
 *
 * blargg's suites report through memory rather than the screen: $6000 holds a
 * status byte, $6001-$6003 hold a magic number once the shell is running, and
 * $6004 begins a zero-terminated message. So a result is read rather than
 * looked at, which is what makes this automatable at all.
 *
 * The ROMs are not redistributed here. Drop them into nes/roms/blargg/ (any
 * arrangement of subdirectories) and they are picked up automatically; without
 * them every case reports as skipped.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "nes/Nes.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#if __has_include(<filesystem>)
#  include <filesystem>
#  define NES_HAVE_FILESYSTEM 1
#endif

namespace {

/* --- the shell's protocol ------------------------------------------------ */

const std::uint16_t STATUS     = 0x6000;
const std::uint16_t MAGIC      = 0x6001;   // DE B0 61 once the shell is alive
const std::uint16_t MESSAGE_AT = 0x6004;

const std::uint8_t RUNNING       = 0x80;
const std::uint8_t NEEDS_RESET   = 0x81;

// Generous: instr_test's slowest single takes a few seconds of console time,
// and oam_stress far more. A ROM that has not finished by here has hung, which
// is itself a result worth reporting.
const int FRAME_LIMIT = 2400;             // 40 seconds of console time

// A reset is requested with at least 100 ms of delay, per the shell's own
// documentation. Six frames is a hair over that.
const int RESET_DELAY_FRAMES = 6;

struct Outcome {
	enum Kind {
		PASSED,
		FAILED,
		NO_PROTOCOL,   // an older ROM that only draws its result on screen
		HUNG
	};
	Kind kind = NO_PROTOCOL;
	int status = 0;
	std::string message;
};

bool shellIsRunning(nes::Nes& console) {
	// The magic appears only once the shell has started, so a status byte read
	// before that is whatever the work RAM powered up holding.
	return console.bus().peek(MAGIC) == 0xDE
			&& console.bus().peek(MAGIC + 1) == 0xB0
			&& console.bus().peek(MAGIC + 2) == 0x61;
}

std::string readMessage(nes::Nes& console) {
	std::string text;
	for (std::uint16_t at = MESSAGE_AT; at < 0x7000; at++) {
		const std::uint8_t c = console.bus().peek(at);
		if (c == 0)
			break;
		// The messages are plain ASCII with newlines; anything else means the
		// shell is not really there and the bytes are noise.
		if (c == '\n')
			text += ' ';
		else if (c >= 0x20 && c < 0x7F)
			text += static_cast<char>(c);
	}
	// Squeeze the runs of spaces the newline substitution leaves behind.
	std::string tidy;
	for (std::size_t i = 0; i < text.size(); i++)
		if (text[i] != ' ' || (!tidy.empty() && tidy.back() != ' '))
			tidy += text[i];
	while (!tidy.empty() && tidy.back() == ' ')
		tidy.pop_back();
	return tidy;
}

/*
 * Set NES_ROM_TRACE to watch a ROM's status byte change.
 *
 * A ROM that hangs says nothing at all through the normal channel, which is
 * exactly when its progress is the only thing worth seeing -- whether it ever
 * started, whether it asked for a reset, whether it restarted from the top.
 */
bool tracing() {
	static const bool on = std::getenv("NES_ROM_TRACE") != nullptr;
	return on;
}

Outcome run(const std::string& path, std::string* error) {
	Outcome outcome;

	nes::Nes console;
	if (!console.loadRom(path, error))
		return outcome;
	console.reset();

	bool sawShell = false;
	bool justReset = false;
	int resetAt = -1;
	int lastTraced = -1;

	for (int frame = 0; frame < FRAME_LIMIT; frame++) {
		console.stepFrame();

		if (tracing()) {
			const int status = shellIsRunning(console)
					? console.bus().peek(STATUS) : -1;
			if (status != lastTraced) {
				std::printf("    frame %4d: %s\n", frame,
						status < 0 ? "shell not running"
								: (std::to_string(status)).c_str());
				lastTraced = status;
			}
		}

		if (resetAt >= 0) {
			if (frame >= resetAt) {
				// The shell asks for this, and the work RAM has to survive it:
				// the status byte it is about to write lives there.
				console.reset();
				resetAt = -1;
				// And it is still asking, because the ROM has not run since --
				// the request it wrote is exactly where it wrote it. Honouring
				// it again resets the machine before the test can take a single
				// step, forever, which is what this did until a ROM that only
				// hung said so.
				justReset = true;
			}
			continue;
		}

		if (!shellIsRunning(console))
			continue;
		sawShell = true;

		const std::uint8_t status = console.bus().peek(STATUS);
		if (justReset) {
			// Nothing counts until the ROM has said it is running again.
			if (status != RUNNING)
				continue;
			justReset = false;
		}
		if (status == RUNNING)
			continue;
		if (status == NEEDS_RESET) {
			resetAt = frame + RESET_DELAY_FRAMES;
			continue;
		}

		outcome.kind = (status == 0) ? Outcome::PASSED : Outcome::FAILED;
		outcome.status = status;
		outcome.message = readMessage(console);
		return outcome;
	}

	outcome.kind = sawShell ? Outcome::HUNG : Outcome::NO_PROTOCOL;
	return outcome;
}

/* --- finding the ROMs ---------------------------------------------------- */

std::vector<std::string> findRoms(const std::string& root) {
	std::vector<std::string> paths;
#if defined(NES_HAVE_FILESYSTEM)
	std::error_code ec;
	if (!std::filesystem::is_directory(root, ec))
		return paths;
	for (std::filesystem::recursive_directory_iterator it(root, ec), end;
			it != end; it.increment(ec)) {
		if (ec)
			break;
		if (!it->is_regular_file(ec))
			continue;
		std::string path = it->path().string();
		if (path.size() > 4 && path.compare(path.size() - 4, 4, ".nes") == 0)
			paths.push_back(path);
	}
#else
	(void)root;
#endif
	std::sort(paths.begin(), paths.end());
	return paths;
}

/** "ppu_vbl_nmi/rom_singles/05-nmi_timing.nes" -> "ppu_vbl_nmi/05-nmi_timing". */
std::string labelFor(const std::string& root, const std::string& path) {
	std::string rel = path.size() > root.size() ? path.substr(root.size() + 1) : path;
	for (std::size_t i = 0; i < rel.size(); i++)
		if (rel[i] == '\\')
			rel[i] = '/';
	if (rel.size() > 4)
		rel.erase(rel.size() - 4);
	const std::size_t singles = rel.find("/rom_singles/");
	if (singles != std::string::npos)
		rel = rel.substr(0, singles + 1) + rel.substr(singles + 13);
	return rel;
}

/*
 * What is known to fail, and why.
 *
 * A gate that is red for reasons already understood stops being read, so a
 * known failure is recorded here with its reason rather than left to shout
 * every run. Anything failing that is *not* in this list is a regression, and
 * anything here that starts passing is reported too -- a stale exception is
 * how a suite quietly stops testing something.
 */
struct Known {
	const char* label;
	const char* why;
};

const Known KNOWN_FAILURES[] = {
	// Four of these six pass now that A12 is modelled. What is left is the
	// exact dot of the edge, and a board that is not implemented at all.
	{ "mmc3_test/4-scanline_timing", "the A12 rise is a few dots off the real fetch" },
	{ "mmc3_test/6-MMC6",            "MMC6, and the MMC3 revisions, are not implemented" },

	// Vblank, the NMI it raises and the flag it sets are placed to the dot
	// here, but what happens in the two or three dots around the edge is not:
	// suppression, the exact dot NMI is taken, and the odd-frame skip's
	// interaction with enabling rendering. The $2002 race was built from
	// reasoning about this and is only partly right, which is precisely what
	// these ROMs are for.
	//
	// Three of the six are fixed, from blargg's own expected tables rather than
	// by reasoning: the flag's set and clear each have a one-dot window where a
	// read wins, and the interrupt is lost for three dots rather than two.
	// What is left is the NMI's own timing, which is about which CPU cycle the
	// interrupt is taken on rather than which dot the flag moves.
	{ "ppu_vbl_nmi/05-nmi_timing",     "NMI timing at the vblank edge" },
	{ "ppu_vbl_nmi/08-nmi_off_timing", "NMI disable timing at the vblank edge" },
	{ "ppu_vbl_nmi/10-even_odd_timing","odd-frame dot skip vs enabling rendering" },

	// Interrupt latency inside the CPU core. CLI's own delay is fixed; what is
	// left all needs the CPU to poll for interrupts *within* an instruction
	// rather than between them -- an NMI arriving partway through a BRK, a
	// taken branch moving the poll, an IRQ landing inside a DMA. The core
	// charges an instruction's cycles in one lump at the end, so there is no
	// "partway through" to poll at yet. All emu6502's, not this side's.
	{ "cpu_interrupts_v2/2-nmi_and_brk",       "NMI does not hijack BRK" },
	{ "cpu_interrupts_v2/3-nmi_and_irq",       "NMI and IRQ arriving together" },
	{ "cpu_interrupts_v2/4-irq_and_dma",       "IRQ timing across an OAM DMA" },
	{ "cpu_interrupts_v2/5-branch_delays_irq", "a taken branch does not delay the IRQ" },

	// A 6502 reads before it writes, and reads twice where an index crosses a
	// page. Those extra reads are invisible in RAM and very visible on $2007
	// or $4015, which is what these check.
	{ "instr_misc/03-dummy_reads",     "no dummy read on indexed addressing" },
	{ "instr_misc/04-dummy_reads_apu", "no dummy read on indexed addressing" },
	{ "ppu_read_buffer/test_ppu_read_buffer", "no dummy read on indexed addressing" },
	{ "cpu_exec_space/test_cpu_exec_space_apu",   "unmapped $4018-$40FF should read as open bus" },
	{ "cpu_exec_space/test_cpu_exec_space_ppuio", "RTS does not do its dummy fetch" },

	// SYA and SXA (also called SHY/SHX) are unstable on real hardware: what
	// they store depends on whether the address crossed a page, and on the
	// analogue behaviour of a bus nobody designed to do this.
	{ "instr_test-v5/07-abs_xy", "SYA and SXA do not model the page-cross case" },

	// The APU at power and at reset. The frame interrupt deliberately powers up
	// inhibited here because a game needed it -- these ROMs are the evidence
	// that was the wrong fix for whatever the real problem was.
	{ "apu_reset/4017_written",     "$4017 is not written with $00 at power" },
	{ "apu_reset/4017_timing",      "frame IRQ flag is set too late after reset" },
	{ "apu_reset/len_ctrs_enabled", "length counters are not enabled at reset" },
	{ "apu_test/8-dmc_rates",       "DMC rate 0's period is wrong" },

	// Two smaller ones, each a piece of hardware that decays or corrupts in a
	// way nothing here models yet.
	{ "ppu_open_bus/ppu_open_bus", "PPU open bus does not decay" },
	{ "oam_stress/oam_stress",     "OAM corruption during rendering is not modelled" },

	{ nullptr, nullptr }
};

const char* knownReason(const std::string& label) {
	for (int i = 0; KNOWN_FAILURES[i].label; i++)
		if (label == KNOWN_FAILURES[i].label)
			return KNOWN_FAILURES[i].why;
	return nullptr;
}

} // namespace

TEST_CASE("blargg_test_roms") {
#ifndef NES_TEST_ROM_DIR
	MESSAGE("no ROM directory configured -- skipping");
#else
	const std::string root = std::string(NES_TEST_ROM_DIR) + "/blargg";
	const std::vector<std::string> roms = findRoms(root);
	if (roms.empty()) {
		MESSAGE("no test ROMs in ", root, " -- skipping "
				"(drop blargg's suites there; they are not redistributed here)");
		return;
	}

	int passed = 0;
	int failed = 0;
	int expected = 0;
	int noProtocol = 0;
	int hung = 0;
	std::vector<std::string> unexpectedPasses;

	// One ROM at a time, for when one of them is the whole problem.
	const char* only = std::getenv("NES_ROM_FILTER");

	for (std::size_t i = 0; i < roms.size(); i++) {
		const std::string label = labelFor(root, roms[i]);
		if (only && label.find(only) == std::string::npos)
			continue;
		const char* known = knownReason(label);

		std::string error;
		const Outcome outcome = run(roms[i], &error);

		INFO(label);
		if (!error.empty()) {
			FAIL_CHECK("could not load: ", error);
			failed++;
			continue;
		}

		switch (outcome.kind) {
		case Outcome::PASSED:
			passed++;
			if (known) {
				// Not a failure, but it must not pass unnoticed: the reason
				// recorded against it is now wrong.
				unexpectedPasses.push_back(label);
				MESSAGE("now passing, but listed as known-failing (",
						std::string(known), "): ", label);
			}
			break;
		case Outcome::FAILED:
			if (known) {
				expected++;
				MESSAGE("known failure (", std::string(known), "): ", label,
						" -- ", outcome.message);
			} else {
				failed++;
				FAIL_CHECK("status ", outcome.status, ": ", outcome.message);
			}
			break;
		case Outcome::NO_PROTOCOL:
			// An older ROM that reports on screen only. Running it still proves
			// it does not crash the emulator, which is worth something, but
			// there is no result to read.
			noProtocol++;
			break;
		case Outcome::HUNG:
			hung++;
			FAIL_CHECK("did not finish within ", FRAME_LIMIT, " frames");
			break;
		}
	}

	MESSAGE("blargg: ", passed, " passed, ", failed, " failed, ",
			expected, " known failures, ", noProtocol,
			" without a readable result, ", hung, " hung, of ",
			roms.size(), " ROMs");

	for (std::size_t i = 0; i < unexpectedPasses.size(); i++)
		FAIL_CHECK("remove the known-failure entry for ", unexpectedPasses[i]);
#endif
}
