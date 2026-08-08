//
// nes_run -- load a .nes image and run it.
//
// Headless: --screenshot writes the last rendered frame as a PPM, and --trace
// emits a per-instruction log that can be diffed against a reference.
//

#include "nes/Nes.h"

#include <6502cc/unasm.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

/** A scripted button press: hold @a buttons from @a startFrame for @a frames. */
struct ScriptedPress {
	std::uint8_t buttons;
	int port;
	unsigned long long startFrame;
	unsigned long long endFrame;
};

const int WAV_RATE = 44100;

/**
 * Decimates the APU's CPU-rate output to 44.1 kHz by averaging each window.
 *
 * No drift correction here, unlike the window front-end: there is no sound card
 * to stay in step with, so the ratio is fixed and the result is deterministic.
 * That is the point of dumping audio from the headless runner -- the same ROM
 * and the same inputs produce a byte-identical WAV, which is testable.
 */
class WavRecorder {
public:
	explicit WavRecorder(int cpuClockHz = nes::Apu::CPU_CLOCK_HZ) :
			m_cpuClockHz(cpuClockHz), m_phase(0.0), m_accumulator(0.0), m_count(0) { }

	void consume(const std::vector<float>& input) {
		const double ratio = static_cast<double>(m_cpuClockHz) / WAV_RATE;
		for (float sample : input) {
			m_accumulator += sample;
			m_count++;
			m_phase += 1.0;
			if (m_phase < ratio)
				continue;
			m_phase -= ratio;
			m_samples.push_back(static_cast<float>(m_accumulator / m_count));
			m_accumulator = 0.0;
			m_count = 0;
		}
	}

	std::size_t frames() const { return m_samples.size(); }

	/** Mono 16-bit PCM. @return false if the file could not be written. */
	bool write(const char* path) const {
		std::FILE* f = std::fopen(path, "wb");
		if (!f)
			return false;

		const std::uint32_t dataBytes = static_cast<std::uint32_t>(m_samples.size() * 2);
		const std::uint32_t byteRate = WAV_RATE * 2;
		auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
		auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };

		std::fwrite("RIFF", 1, 4, f);
		u32(36 + dataBytes);
		std::fwrite("WAVEfmt ", 1, 8, f);
		u32(16);              // PCM header size
		u16(1);               // PCM
		u16(1);               // mono
		u32(WAV_RATE);
		u32(byteRate);
		u16(2);               // block align
		u16(16);              // bits per sample
		std::fwrite("data", 1, 4, f);
		u32(dataBytes);

		for (float sample : m_samples) {
			// The mixer peaks around a quarter of unity with several channels
			// playing, so this uses most of the format's range without ever
			// reaching it. The clamp is a backstop, not the normal path -- if
			// it engages regularly the gain is wrong.
			float scaled = sample * 2.0f;
			if (scaled > 1.0f) scaled = 1.0f;
			if (scaled < -1.0f) scaled = -1.0f;
			u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(scaled * 32767.0f)));
		}
		std::fclose(f);
		return true;
	}

private:
	int m_cpuClockHz;
	double m_phase;
	double m_accumulator;
	int m_count;
	std::vector<float> m_samples;
};

void usage(const char* argv0) {
	std::printf(
		"usage: %s <rom.nes> [options]\n"
		"\n"
		"  --trace           write one line per instruction to stdout\n"
		"  --start-pc=HEX    begin at HEX instead of the reset vector\n"
		"                    (nestest's automated mode wants C000)\n"
		"  --max=N           stop after N instructions (default 100000000)\n"
		"  --stop-on-trap    stop when PC stops advancing\n"
		"  --frames=N        run N PPU frames, then stop\n"
		"  --screenshot=FILE write the final frame as a binary PPM\n"
		"  --audio=FILE      record the APU to a 44.1 kHz mono WAV\n"
		"  --dump-attr       print the attribute table as one palette digit per tile\n"
		"  --press=SPEC      hold a button for a while; repeatable\n"
		"                    SPEC is BUTTON@FRAME[:HELD][/PORT], e.g.\n"
		"                      --press=start@200        Start at frame 200 for 10\n"
		"                      --press=right@260:120    Right for 120 frames\n"
		"                      --press=a@300:4/2        A on the second pad\n"
		"                    buttons: a b select start up down left right\n"
		"\n"
		"Trace format matches the register fields of the standard nestest log:\n"
		"  C000  4C F5 C5  JMP $c5f5   A:00 X:00 Y:00 P:24 SP:FD CYC:7\n",
		argv0);
}

bool startsWith(const char* s, const char* prefix, const char** rest) {
	const std::size_t n = std::strlen(prefix);
	if (std::strncmp(s, prefix, n) != 0)
		return false;
	*rest = s + n;
	return true;
}

/**
 * Parse BUTTON@FRAME[:HELD][/PORT].
 *
 * A press has to last several frames to register: a game samples the pad once
 * per frame, and menus commonly want to see the button held across two or three
 * before acting. Ten is a comfortable default -- about a sixth of a second.
 */
bool parsePress(const char* spec, ScriptedPress* out) {
	const char* at = std::strchr(spec, '@');
	if (!at || at == spec)
		return false;

	const std::string name(spec, at - spec);
	out->buttons = nes::Controller::buttonFromName(name.c_str());
	if (out->buttons == 0)
		return false;

	char* end = nullptr;
	const unsigned long long start = std::strtoull(at + 1, &end, 10);
	if (end == at + 1)
		return false;

	unsigned long long held = 10;
	out->port = 0;
	while (*end == ':' || *end == '/') {
		const char sep = *end;
		char* next = nullptr;
		const unsigned long long value = std::strtoull(end + 1, &next, 10);
		if (next == end + 1)
			return false;
		if (sep == ':')
			held = value;
		else
			out->port = (value >= 2) ? 1 : 0;   // ports named 1 and 2, indexed 0 and 1
		end = next;
	}
	if (*end != '\0' || held == 0)
		return false;

	out->startFrame = start;
	out->endFrame = start + held;
	return true;
}

/**
 * Print which palette each tile of a nametable selects, one digit per tile.
 *
 * The attribute table packs four tiles' palette numbers into two bits each and
 * covers a 4x4 block per byte, which makes it painful to read by hand and easy
 * to decode wrongly. Seeing the decoded grid next to the rendered picture is
 * how you tell "the game asked for that" from "we picked the wrong palette".
 */
void dumpAttributes(const nes::Ppu& ppu, std::uint16_t nametable) {
	std::fprintf(stderr, "attributes for nametable $%04X:\n", nametable);
	for (int row = 0; row < 30; row++) {
		std::fprintf(stderr, "  ");
		for (int col = 0; col < 32; col++) {
			const std::uint16_t address = static_cast<std::uint16_t>(
					nametable + 0x3C0 + (row / 4) * 8 + (col / 4));
			const std::uint8_t attribute = ppu.vramRead(address);
			// Two bits per 2x2 quadrant of the 4x4 block.
			const int shift = ((row & 2) << 1) | (col & 2);
			std::fprintf(stderr, "%d", (attribute >> shift) & 3);
		}
		std::fprintf(stderr, "\n");
	}
}

/** Dump the framebuffer as a binary PPM -- viewable anywhere, no dependencies. */
bool writePpm(const char* path, const nes::Ppu& ppu) {
	std::FILE* f = std::fopen(path, "wb");
	if (!f)
		return false;
	std::fprintf(f, "P6\n%d %d\n255\n", nes::Ppu::SCREEN_WIDTH, nes::Ppu::SCREEN_HEIGHT);
	const std::uint8_t* fb = ppu.framebuffer();
	const std::uint32_t* palette = nes::Ppu::nesPaletteRgb();
	for (int i = 0; i < nes::Ppu::SCREEN_WIDTH * nes::Ppu::SCREEN_HEIGHT; i++) {
		const std::uint32_t rgb = palette[fb[i] & 0x3F];
		const unsigned char pixel[3] = {
			static_cast<unsigned char>((rgb >> 16) & 0xFF),
			static_cast<unsigned char>((rgb >> 8) & 0xFF),
			static_cast<unsigned char>(rgb & 0xFF)
		};
		std::fwrite(pixel, 1, 3, f);
	}
	std::fclose(f);
	return true;
}

} // namespace

int main(int argc, char** argv) {
	const char* romPath = nullptr;
	bool trace = false;
	bool stopOnTrap = false;
	bool dumpAttr = false;
	const char* screenshot = nullptr;
	const char* audioPath = nullptr;
	long long frames = -1;
	long startPc = -1;
	long long maxInstructions = 100000000LL;
	std::vector<ScriptedPress> presses;

	for (int i = 1; i < argc; i++) {
		const char* rest = nullptr;
		if (std::strcmp(argv[i], "--trace") == 0) {
			trace = true;
		} else if (std::strcmp(argv[i], "--stop-on-trap") == 0) {
			stopOnTrap = true;
		} else if (std::strcmp(argv[i], "--dump-attr") == 0) {
			dumpAttr = true;
		} else if (startsWith(argv[i], "--start-pc=", &rest)) {
			startPc = std::strtol(rest, nullptr, 16);
		} else if (startsWith(argv[i], "--max=", &rest)) {
			maxInstructions = std::strtoll(rest, nullptr, 10);
		} else if (startsWith(argv[i], "--frames=", &rest)) {
			frames = std::strtoll(rest, nullptr, 10);
		} else if (startsWith(argv[i], "--screenshot=", &rest)) {
			screenshot = rest;
		} else if (startsWith(argv[i], "--audio=", &rest)) {
			audioPath = rest;
		} else if (startsWith(argv[i], "--press=", &rest)) {
			ScriptedPress press;
			if (!parsePress(rest, &press)) {
				std::fprintf(stderr, "bad --press spec: %s\n", rest);
				return 2;
			}
			presses.push_back(press);
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
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}

	const nes::Cartridge* cart = console.cartridge();
	std::fprintf(stderr, "%s: mapper %d, PRG %zu KB, CHR %zu KB, %s mirroring, %s%s%s\n",
			romPath, cart->mapperNumber(), cart->prgSize() / 1024, cart->chrSize() / 1024,
			nes::toString(cart->mirroring()), nes::toString(cart->region()),
			cart->isNes20() ? ", NES 2.0" : "", cart->hasBattery() ? ", battery" : "");

	console.reset();
	if (startPc >= 0)
		console.cpuRegisters().pc = static_cast<uint16>(startPc);

	// The APU only mixes when someone is listening; the rest of the time the
	// channels still run but no samples are produced.
	WavRecorder recorder(console.cpuClockHz());
	if (audioPath)
		console.apu().setSampleOutput(true);

	// Disassemble through a peek view so tracing never disturbs device state.
	nes::PeekBus peekBus(&console.bus());
	UnAsm unasm;

	const std::uint64_t frameTarget = frames >= 0
			? console.ppu().frame() + static_cast<std::uint64_t>(frames)
			: 0;

	long long executed = 0;
	std::uint64_t lastInputFrame = ~std::uint64_t(0);
	while (executed < maxInstructions) {
		if (frames >= 0 && console.ppu().frame() >= frameTarget)
			break;

		// Re-evaluate the script once per frame. A game latches the pad in its
		// NMI handler, so frame granularity is as fine as scripted input can
		// usefully be.
		const std::uint64_t frame = console.ppu().frame();
		if (!presses.empty() && frame != lastInputFrame) {
			lastInputFrame = frame;
			std::uint8_t held[2] = { 0, 0 };
			for (const ScriptedPress& press : presses)
				if (frame >= press.startFrame && frame < press.endFrame)
					held[press.port] |= press.buttons;
			console.controller(0).setButtons(held[0]);
			console.controller(1).setButtons(held[1]);
		}

		const Registers& r = console.cpuRegisters();
		const uint16 pc = r.pc;

		if (trace) {
			Registers probe = { };
			probe.pc = pc;
			std::string text = unasm.unasm_line(&peekBus, &probe);
			const int length = static_cast<int>(probe.pc - pc);

			char bytes[16] = { 0 };
			int at = 0;
			for (int b = 0; b < length && at < 12; b++)
				at += std::snprintf(bytes + at, sizeof(bytes) - at, "%02X ",
						peekBus.read(static_cast<uint16>(pc + b)));

			std::printf("%04X  %-9s %-14s A:%02X X:%02X Y:%02X P:%02X SP:%02X CYC:%llu\n",
					pc, bytes, text.c_str(), r.a, r.x, r.y, r.sr, r.sp,
					static_cast<unsigned long long>(console.cycles()));
		}

		console.step();
		executed++;

		if (audioPath) {
			// Drain every step rather than every frame: at one sample per CPU
			// cycle a long run would otherwise buffer hundreds of megabytes.
			recorder.consume(console.apu().samples());
			console.apu().clearSamples();
		}

		if (stopOnTrap && console.cpuRegisters().pc == pc) {
			std::fprintf(stderr, "trapped at $%04X after %lld instructions\n", pc, executed);
			break;
		}
	}

	const Registers& r = console.cpuRegisters();
	std::fprintf(stderr,
			"stopped: PC=%04X A=%02X X=%02X Y=%02X P=%02X SP=%02X after %lld instructions, "
			"%llu cycles\n",
			r.pc, r.a, r.x, r.y, r.sr, r.sp, executed,
			static_cast<unsigned long long>(console.cycles()));
	// nestest reports its results here.
	std::fprintf(stderr, "result bytes: $02=%02X $03=%02X\n",
			console.bus().peek(0x0002), console.bus().peek(0x0003));

	// Until there is a renderer, this is how you tell whether a game is
	// actually building a screen: count what it has put in video memory.
	const nes::Ppu& ppu = console.ppu();
	int nametableBytes = 0;
	for (int a = 0x2000; a < 0x3000; a++)
		if (ppu.vramRead(static_cast<std::uint16_t>(a)) != 0)
			nametableBytes++;
	int oamBytes = 0;
	for (int i = 0; i < 256; i++)
		if (ppu.readOam(static_cast<std::uint8_t>(i)) != 0)
			oamBytes++;

	std::fprintf(stderr,
			"PPU: frame %llu, scanline %d dot %d, ctrl=%02X mask=%02X, rendering %s\n",
			static_cast<unsigned long long>(ppu.frame()), ppu.scanline(), ppu.dot(),
			ppu.control(), ppu.mask(), ppu.renderingEnabled() ? "on" : "off");
	std::fprintf(stderr, "     %d/4096 nametable bytes written, %d/256 OAM bytes set\n",
			nametableBytes, oamBytes);
	// Both halves: the background palettes at $3F00 and the sprite palettes at
	// $3F10. Printing only the first half hides half the reasons a screen can
	// come out the wrong colour.
	std::fprintf(stderr, "     bg palette:    ");
	for (int i = 0; i < 16; i++)
		std::fprintf(stderr, " %02X", ppu.vramRead(static_cast<std::uint16_t>(0x3F00 + i)));
	std::fprintf(stderr, "\n     sprite palette:");
	for (int i = 0; i < 16; i++)
		std::fprintf(stderr, " %02X", ppu.vramRead(static_cast<std::uint16_t>(0x3F10 + i)));
	std::fprintf(stderr, "\n");

	if (dumpAttr)
		dumpAttributes(ppu, 0x2000);

	if (screenshot) {
		if (writePpm(screenshot, ppu))
			std::fprintf(stderr, "wrote %s\n", screenshot);
		else
			std::fprintf(stderr, "could not write %s\n", screenshot);
	}

	std::string saveError;
	if (!console.saveBatteryRam(&saveError))
		std::fprintf(stderr, "%s\n", saveError.c_str());
	else if (!console.batteryRamPath().empty())
		std::fprintf(stderr, "wrote %s\n", console.batteryRamPath().c_str());

	if (audioPath) {
		if (recorder.write(audioPath))
			std::fprintf(stderr, "wrote %s (%.2f seconds)\n", audioPath,
					static_cast<double>(recorder.frames()) / WAV_RATE);
		else
			std::fprintf(stderr, "could not write %s\n", audioPath);
	}

	if (console.bus().stubReads() || console.bus().stubWrites())
		std::fprintf(stderr, "note: %lu reads and %lu writes hit unimplemented APU/input registers\n",
				console.bus().stubReads(), console.bus().stubWrites());
	return 0;
}
