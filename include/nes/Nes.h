#ifndef NES_NES_H
#define NES_NES_H

#include "Apu.h"
#include "Cartridge.h"
#include "NesBus.h"
#include "Ppu.h"

#include <6502cc/emu_base.h>
#include <6502cc/emu_clock.h>
#include <6502cc/emu_processor.h>

#include <cstdint>
#include <memory>
#include <string>

namespace nes {

/**
 * The console: CPU, bus and cartridge wired together.
 *
 * Deliberately does not use emu6502's I6502Emulator. That class resets through
 * a Memory object, which an NES does not have -- the reset vector comes from
 * the cartridge via the bus. Driving Processor directly is both simpler and the
 * shape a multi-chip system wants, since the master clock has to advance the
 * PPU and APU alongside the CPU rather than letting the CPU pace itself.
 *
 * The clock is a free-running default_clock: the CPU runs flat out and reports
 * cycles, and pacing (once there is video to pace against) belongs one level up.
 */
class Nes {
public:
	Nes();
	~Nes();

	Nes(const Nes&) = delete;
	Nes& operator=(const Nes&) = delete;

	/**
	 * Load a .nes file, and its save RAM if the cartridge has a battery.
	 * @return false on failure, with a reason in @p error.
	 */
	bool loadRom(const std::string& path, std::string* error = nullptr);

	/**
	 * Write save RAM back beside the ROM.
	 *
	 * Does nothing, successfully, for a cartridge with no battery or one that
	 * was not loaded from a file. Front-ends should call this on exit and after
	 * a reset -- a player who closes the window expects to keep their game.
	 */
	bool saveBatteryRam(std::string* error = nullptr) const;

	/** Where saveBatteryRam() writes, or empty when there is nothing to write. */
	const std::string& batteryRamPath() const { return m_savePath; }
	void setCartridge(std::unique_ptr<Cartridge> cartridge);
	bool hasCartridge() const { return m_cartridge != nullptr; }

	/**
	 * Power-on / reset: clear RAM, set SP to $FD and the I flag, and load PC
	 * from the reset vector at $FFFC. Charges the 7 cycles the sequence takes.
	 */
	void reset();

	/**
	 * Advance the system by one CPU instruction.
	 *
	 * Drains any OAM DMA stall first, then executes an instruction (or services
	 * a pending interrupt), then forwards the NMI or IRQ that raised. The PPU
	 * and APU are advanced by NesBus as the instruction makes its accesses,
	 * rather than in one lump afterwards, so a device is sampled at the cycle
	 * its access happens. That ordering is what makes the console a system
	 * rather than a CPU with peripherals bolted on.
	 *
	 * @return the number of CPU cycles consumed.
	 */
	int step();

	/** Run until the PPU completes a frame. @return CPU cycles consumed. */
	int stepFrame();

	Registers& cpuRegisters() { return m_regs; }
	const Registers& cpuRegisters() const { return m_regs; }
	Processor& cpu() { return *m_cpu; }
	NesBus& bus() { return *m_bus; }
	Ppu& ppu() { return m_ppu; }
	const Ppu& ppu() const { return m_ppu; }
	Apu& apu() { return m_apu; }
	const Apu& apu() const { return m_apu; }
	/** Controller port 0 or 1. Set buttons on it and the running game sees them. */
	Controller& controller(int port) { return m_bus->controller(port); }
	Cartridge* cartridge() const { return m_cartridge.get(); }

	/** Total CPU cycles since the last reset. */
	std::uint64_t cycles() const { return m_clock.cycles(); }

	/** Taken from the cartridge header; NTSC with no cartridge loaded. */
	Region region() const { return m_region; }
	/** CPU cycles per second for this region -- what audio resampling needs. */
	int cpuClockHz() const { return Apu::cpuClockHz(m_region); }
	/** Frames per second this region's PPU produces. */
	double frameRate() const { return m_region == Region::Pal ? 50.0070 : 60.0988; }

private:
	Registers m_regs;
	std::unique_ptr<Cartridge> m_cartridge;
	Ppu m_ppu;
	Apu m_apu;
	std::unique_ptr<NesBus> m_bus;
	default_clock m_clock;
	std::unique_ptr<Processor> m_cpu;

	// Empty unless the cartridge came from a file and has a battery.
	std::string m_savePath;

	Region m_region;
};

} // namespace nes

#endif // NES_NES_H
