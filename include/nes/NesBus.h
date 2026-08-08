#ifndef NES_NESBUS_H
#define NES_NESBUS_H

#include "Cartridge.h"
#include "Controller.h"
#include "Ppu.h"

#include <6502cc/emu_bus.h>

#include <array>
#include <cstdint>

namespace nes {

/**
 * The CPU address bus of an NES.
 *
 * Overrides emu6502's Bus to decode the 16-bit space into the console's four
 * regions. The base class's Memory pointer is unused -- every access is routed
 * here instead.
 *
 *   $0000-$1FFF  2 KB internal RAM, mirrored every $0800
 *   $2000-$3FFF  8 PPU registers, mirrored every 8 bytes
 *   $4000-$401F  APU and I/O registers
 *   $4020-$FFFF  cartridge, via the mapper
 *
 * PPU registers, OAM DMA at $4014 and the two controller ports at $4016/$4017
 * are live. The APU is still stubbed: reads return 0 and writes are counted but
 * inert.
 */
class NesBus : public Bus {
public:
	NesBus(Cartridge* cartridge, Ppu* ppu);

	uint8 read(uint16 address) override;
	void write(uint16 address, uint8 val) override;

	/**
	 * Side-effect-free read, for debuggers and disassembly.
	 *
	 * Not the same thing as read(). Several PPU registers change state when the
	 * CPU reads them -- $2002 clears the vblank flag, $2007 advances the VRAM
	 * address -- so a memory viewer that used read() would corrupt the very
	 * state it is displaying. Anything inspecting memory must come through here.
	 */
	std::uint8_t peek(std::uint16_t address) const;

	void setCartridge(Cartridge* cartridge) { m_cartridge = cartridge; }
	Cartridge* cartridge() const { return m_cartridge; }

	/** Zero the internal RAM. Real hardware powers up indeterminate. */
	void clearRam();

	const std::array<std::uint8_t, 0x800>& ram() const { return m_ram; }

	/**
	 * Cycles the CPU owes for an OAM DMA started by a write to $4014.
	 *
	 * The copy itself happens immediately; this is the stall the CPU has to
	 * absorb afterwards. Nes::step() drains it before fetching the next opcode.
	 */
	int takeDmaStall();
	int pendingDmaStall() const { return m_dmaStall; }

	/** Controller port 0 or 1, read by the CPU at $4016 and $4017. */
	Controller& controller(int port) { return m_controllers[port & 1]; }
	const Controller& controller(int port) const { return m_controllers[port & 1]; }

	/** Count of accesses to still-unimplemented APU registers. */
	unsigned long stubReads() const { return m_stubReads; }
	unsigned long stubWrites() const { return m_stubWrites; }

private:
	std::array<std::uint8_t, 0x800> m_ram;
	Cartridge* m_cartridge;
	Ppu* m_ppu;
	Controller m_controllers[2];
	int m_dmaStall;
	unsigned long m_stubReads;
	unsigned long m_stubWrites;
};

/**
 * A read-only view of a NesBus, for disassemblers and memory viewers.
 *
 * emu6502's UnAsm and any memory dump take a Bus and call read() on it, which
 * would trip the PPU's read side effects. Wrapping the real bus in one of these
 * routes those reads through peek() instead. Writes are discarded.
 */
class PeekBus : public Bus {
public:
	explicit PeekBus(const NesBus* bus) : m_bus(bus) { }

	uint8 read(uint16 address) override { return m_bus->peek(address); }
	void write(uint16 /*address*/, uint8 /*val*/) override { }

private:
	const NesBus* m_bus;
};

} // namespace nes

#endif // NES_NESBUS_H
