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

	/** Load a .nes file. @return false on failure, with a reason in @p error. */
	bool loadRom(const std::string& path, std::string* error = nullptr);
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
	 * a pending interrupt), then ticks the PPU by three dots per CPU cycle and
	 * the APU by one, and forwards the NMI or IRQ they raised. That ordering is
	 * what makes the console a system rather than a CPU with peripherals bolted
	 * on.
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

private:
	Registers m_regs;
	std::unique_ptr<Cartridge> m_cartridge;
	Ppu m_ppu;
	Apu m_apu;
	std::unique_ptr<NesBus> m_bus;
	default_clock m_clock;
	std::unique_ptr<Processor> m_cpu;
};

} // namespace nes

#endif // NES_NES_H
