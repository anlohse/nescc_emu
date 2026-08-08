/*
 * testNestest.cpp -- CPU conformance against the published nestest trace.
 *
 * nestest.nes has an automated mode entered by starting at $C000 instead of the
 * reset vector. It exercises every documented opcode and most undocumented ones
 * and needs no PPU, which makes it the standard way to validate a 6502 core
 * against real NES code.
 *
 * The ROM and its reference log are not redistributed here; drop them into
 * nes/roms/ (see nes/CMakeLists.txt). Without them this reports as skipped.
 *
 * Only the architectural state is compared -- PC, A, X, Y, P, SP and the
 * cumulative cycle count. The disassembly text in the log is not, because its
 * operand annotations ("= 00", "@ 80 = 12") describe resolved addresses that
 * emu6502's disassembler does not produce. Those columns are for humans; the
 * register columns are what actually validate the CPU.
 */

#include "nes/Nes.h"

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

const std::uint16_t NESTEST_ENTRY = 0xC000;

struct LogLine {
	std::uint16_t pc;
	std::uint8_t a, x, y, p, sp;
	unsigned long long cycles;
	bool hasCycles;
	std::string raw;
};

/** Read a hex field introduced by @p key, e.g. " A:" -> 0x1F. */
bool hexField(const std::string& line, const char* key, unsigned long& out) {
	const std::size_t at = line.find(key);
	if (at == std::string::npos)
		return false;
	out = std::strtoul(line.c_str() + at + std::strlen(key), nullptr, 16);
	return true;
}

bool parseLine(const std::string& line, LogLine& out, std::string* why) {
	if (line.size() < 4) {
		*why = "line too short";
		return false;
	}
	out.raw = line;
	out.pc = static_cast<std::uint16_t>(std::strtoul(line.substr(0, 4).c_str(), nullptr, 16));

	unsigned long v = 0;
	// " P:" rather than "P:" so it does not match the "SP:" that follows.
	if (!hexField(line, " A:", v))  { *why = "no A: field";  return false; }
	out.a = static_cast<std::uint8_t>(v);
	if (!hexField(line, " X:", v))  { *why = "no X: field";  return false; }
	out.x = static_cast<std::uint8_t>(v);
	if (!hexField(line, " Y:", v))  { *why = "no Y: field";  return false; }
	out.y = static_cast<std::uint8_t>(v);
	if (!hexField(line, " P:", v))  { *why = "no P: field";  return false; }
	out.p = static_cast<std::uint8_t>(v);
	if (!hexField(line, "SP:", v))  { *why = "no SP: field"; return false; }
	out.sp = static_cast<std::uint8_t>(v);

	// CYC: is decimal, and only means CPU cycles in logs that also carry a PPU:
	// column. Older logs used CYC: for the PPU dot within a scanline.
	const std::size_t cyc = line.find("CYC:");
	out.hasCycles = false;
	out.cycles = 0;
	if (cyc != std::string::npos && line.find("PPU:") != std::string::npos) {
		out.cycles = std::strtoull(line.c_str() + cyc + 4, nullptr, 10);
		out.hasCycles = true;
	}
	return true;
}

std::string describe(const char* what, unsigned expected, unsigned actual) {
	char buf[96];
	std::snprintf(buf, sizeof(buf), "%s: log says %02X, emulator has %02X", what, expected, actual);
	return buf;
}

} // namespace

TEST_CASE("nestest_cpu_trace") {
#ifndef NES_TEST_ROM_DIR
	MESSAGE("nestest.nes not configured -- skipping "
			"(drop nestest.nes and nestest.log into nes/roms/, or set NES_TEST_ROM_DIR)");
#else
	const std::string dir = NES_TEST_ROM_DIR;

	nes::Nes console;
	std::string error;
	if (!console.loadRom(dir + "/nestest.nes", &error)) {
		MESSAGE("skipping: ", error);
		return;
	}

	std::ifstream log((dir + "/nestest.log").c_str());
	if (!log) {
		MESSAGE("skipping: nestest.log not found in ", dir,
				" -- the ROM alone cannot be checked without its reference trace");
		return;
	}

	console.reset();
	console.cpuRegisters().pc = NESTEST_ENTRY;   // automated mode

	std::string text;
	long long lineNumber = 0;
	bool comparedCycles = false;

	while (std::getline(log, text)) {
		if (!text.empty() && text.back() == '\r')
			text.pop_back();
		if (text.empty())
			continue;
		lineNumber++;

		LogLine expected;
		std::string why;
		REQUIRE_MESSAGE(parseLine(text, expected, &why),
				"cannot parse nestest.log line ", lineNumber, ": ", why);

		const Registers& r = console.cpuRegisters();

		// Report the first divergence and stop: everything after it is noise.
		char where[64];
		std::snprintf(where, sizeof(where), "nestest.log line %lld", lineNumber);
		INFO(where);
		INFO("log: ", expected.raw);

		REQUIRE_MESSAGE(r.pc == expected.pc,
				describe("PC", expected.pc, r.pc));
		REQUIRE_MESSAGE(r.a == expected.a,   describe("A",  expected.a,  r.a));
		REQUIRE_MESSAGE(r.x == expected.x,   describe("X",  expected.x,  r.x));
		REQUIRE_MESSAGE(r.y == expected.y,   describe("Y",  expected.y,  r.y));
		REQUIRE_MESSAGE(r.sr == expected.p,  describe("P",  expected.p,  r.sr));
		REQUIRE_MESSAGE(r.sp == expected.sp, describe("SP", expected.sp, r.sp));

		if (expected.hasCycles) {
			comparedCycles = true;
			REQUIRE_MESSAGE(console.cycles() == expected.cycles,
					"cycles: log says ", expected.cycles,
					", emulator has ", console.cycles());
		}

		console.step();
	}

	REQUIRE_MESSAGE(lineNumber > 0, "nestest.log was empty");
	MESSAGE("matched ", lineNumber, " trace lines",
			comparedCycles ? " including cycle counts" : " (log has no CPU cycle column)");

	// nestest reports failures as non-zero codes in these two bytes.
	CHECK_MESSAGE(console.bus().peek(0x0002) == 0,
			"nestest error code at $02: ", (int) console.bus().peek(0x0002));
	CHECK_MESSAGE(console.bus().peek(0x0003) == 0,
			"nestest error code at $03: ", (int) console.bus().peek(0x0003));
#endif
}
