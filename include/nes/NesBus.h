#ifndef NES_NESBUS_H
#define NES_NESBUS_H

#include "Apu.h"
#include "Cartridge.h"
#include "Controller.h"
#include "Ppu.h"
#include "Region.h"

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
 * Every region is live: PPU registers, the APU, OAM DMA at $4014 and both
 * controller ports.
 *
 * $4017 is the one address whose meaning depends on direction. Reading it is
 * controller port two; writing it is the APU's frame counter. They are
 * unrelated registers that happen to share a pin.
 *
 * This is also where time passes. Every access the CPU makes while executing an
 * instruction first advances the PPU and APU by one CPU cycle, so a device is
 * sampled at the cycle its access happens rather than at the instruction
 * boundary before it. That is the only place in the machine positioned to know
 * when an access occurs -- the CPU reports cycles after the fact, in one lump.
 */
class NesBus : public Bus {
public:
	NesBus(Cartridge* cartridge, Ppu* ppu, Apu* apu = nullptr);

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

	/** How many PPU dots a CPU cycle buys. Three on NTSC, 3.2 on PAL. */
	void setRegion(Region region);

	/**
	 * The CPU is about to execute one instruction.
	 *
	 * Turns on per-access timing until endInstruction(): each read and write
	 * advances the machine a cycle before it is served. Also tells the
	 * cartridge, since a board may care where instructions begin for reasons of
	 * its own -- MMC1 does.
	 */
	void beginInstruction();

	/**
	 * The instruction is over, having cost @p cycles.
	 *
	 * Accesses are not quite cycles: a taken branch, a page-crossing read and
	 * the internal cycles of a stack operation each cost one without touching
	 * the bus. Whatever the accesses did not account for is charged here, so
	 * the totals still come out exact and only the distribution improves.
	 *
	 * Accesses are never the larger number -- emu6502 has a test over all 256
	 * opcodes pinning that -- but if one ever were, overrunInstructions()
	 * counts it rather than letting the PPU quietly run ahead of the clock.
	 */
	void endInstruction(int cycles);

	/**
	 * Advance the PPU and APU by @p cycles CPU cycles, all at once.
	 *
	 * For time the CPU is not executing through: DMA stalls, mostly. Devices
	 * that read the bus while catching up -- the DMC fetching a sample -- do
	 * not tick again from inside here.
	 */
	void advance(int cycles);

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

	void setApu(Apu* apu) { m_apu = apu; }

	/** Count of accesses to addresses in $4000-$401F with nothing behind them. */
	unsigned long stubReads() const { return m_stubReads; }
	unsigned long stubWrites() const { return m_stubWrites; }

	/** Instructions that made more bus accesses than they had cycles. Should be 0. */
	unsigned long overrunInstructions() const { return m_overruns; }

private:
	/** One CPU cycle's worth of time, charged before an access is served. */
	void cycle();

	/**
	 * What a light gun on port two answers with.
	 *
	 * Here rather than in Controller because it needs the picture: the gun
	 * reports what it can see, and only the PPU knows what is on screen.
	 */
	std::uint8_t readZapper() const;

	std::array<std::uint8_t, 0x800> m_ram;
	Cartridge* m_cartridge;
	Ppu* m_ppu;
	Apu* m_apu;
	Controller m_controllers[2];
	int m_dmaStall;
	unsigned long m_stubReads;
	unsigned long m_stubWrites;

	// PPU dots per CPU cycle as a fraction: 3/1 on NTSC, 16/5 on PAL. PAL's
	// ratio is not a whole number, so the leftover is carried between cycles
	// rather than rounded away -- rounding would drift a whole scanline every
	// few hundred instructions.
	int m_dotNumerator;
	int m_dotDenominator;
	int m_dotRemainder;

	// True only while the CPU is executing an instruction. Everything else that
	// reaches this bus -- an OAM DMA copy, a DMC sample fetch, the reset vector
	// read -- is time already accounted for somewhere else, and must not be
	// charged twice.
	bool m_ticking;
	int m_accountedCycles;
	unsigned long m_overruns;
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
