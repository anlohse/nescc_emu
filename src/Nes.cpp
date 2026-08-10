#include "nes/Nes.h"

#include <cstring>

namespace nes {

Nes::Nes() : m_regs(), m_cartridge(), m_ppu(nullptr), m_apu(),
		m_bus(new NesBus(nullptr, &m_ppu, &m_apu)), m_clock(),
		m_cpu(new Processor(m_bus.get(), &m_regs, &m_clock)),
		m_region(Region::Ntsc) {
	// The power-on state, which reset() deliberately does not reproduce.
	powerOnRegisters();

	// The 2A03 is a 6502 with the decimal circuitry disabled: ADC and SBC
	// ignore the D flag entirely. Games set and clear D like any other bit and
	// a few leave it set, so a core doing real BCD gets different answers --
	// which is what blargg's instr_test found across seven of its ROMs.
	m_regs.decimalDisabled = true;

	// The DMC fetches its samples over the CPU bus, like a second bus master.
	m_apu.setBus(m_bus.get());
}

void Nes::powerOnRegisters() {
	// A stack pointer of 0 becomes the $FD everybody expects once the reset
	// that follows a power-on decrements it three times, which is where that
	// number comes from.
	m_regs.a = 0;
	m_regs.x = 0;
	m_regs.y = 0;
	m_regs.sp = 0;
	m_regs.sr = FLAG__;
	m_regs.pc = 0;
}

void Nes::powerOn() {
	// Everything a reset leaves alone, because this is the other switch: the
	// RAM goes, and so do the registers.
	m_bus->clearRam();
	powerOnRegisters();
	reset();
}

Nes::~Nes() = default;

bool Nes::loadRom(const std::string& path, std::string* error) {
	std::unique_ptr<Cartridge> cart = Cartridge::fromFile(path, error);
	if (!cart)
		return false;

	setCartridge(std::move(cart));

	if (m_cartridge->hasPersistentRam()) {
		m_savePath = Cartridge::batteryRamPathFor(path);
		// A missing save is the normal first run, so a failure here is only
		// ever a real read error -- worth reporting, not worth refusing to
		// start over.
		m_cartridge->loadBatteryRam(m_savePath, error);
	} else {
		m_savePath.clear();
	}
	return true;
}

bool Nes::saveBatteryRam(std::string* error) const {
	if (!m_cartridge || m_savePath.empty())
		return true;
	return m_cartridge->saveBatteryRam(m_savePath, error);
}

void Nes::setCartridge(std::unique_ptr<Cartridge> cartridge) {
	m_cartridge = std::move(cartridge);
	m_bus->setCartridge(m_cartridge.get());
	m_ppu.setCartridge(m_cartridge.get());

	// The cartridge decides the region, and the region decides the timing of
	// every other chip in the machine.
	m_region = m_cartridge ? m_cartridge->region() : Region::Ntsc;
	m_ppu.setRegion(m_region);
	m_apu.setRegion(m_region);
	m_bus->setRegion(m_region);
}

void Nes::reset() {
	// Neither the RAM nor A, X and Y are touched. Reset asserts a pin; it does
	// not clear the machine, and a game is entitled to notice what survived.
	// The stack pointer moves because the reset sequence goes through the
	// motions of pushing a return address and the status register without ever
	// writing them -- three decrements, no writes.
	m_ppu.reset();
	m_apu.reset();
	m_regs.sp -= 3;
	m_regs.sr |= FLAG_I;
	m_regs.pc = m_bus->read(0xFFFC) | (m_bus->read(0xFFFD) << 8);
	m_cpu->clearInterrupts();
	m_bus->setRegion(m_region);   // also clears the carried dot fraction
	m_clock.reset();
	m_clock.beginCycle();
	m_clock.waitCycles(7); // the reset sequence costs 7 cycles
}

int Nes::step() {
	int cycles;

	// Both DMA units steal cycles from the CPU: OAM DMA in one 513-cycle block,
	// the DMC four at a time as it fetches sample bytes. Neither stops the PPU
	// or the APU, which is the whole reason the stalls have to be modelled
	// rather than ignored.
	const int stall = m_bus->takeDmaStall() + m_apu.takeDmcStall();
	if (stall > 0) {
		// Time the CPU spends off the bus entirely, so there is nothing to
		// distribute: it all happens, and then the CPU resumes.
		m_clock.waitCycles(stall);
		cycles = stall;
		m_bus->advance(cycles);
	} else {
		// The bus advances the PPU and APU a cycle at a time as the instruction
		// makes its accesses, so a device is read at the cycle it is read at.
		// endInstruction() settles whatever the accesses did not account for.
		m_bus->beginInstruction();
		const std::uint64_t before = m_clock.cycles();
		m_cpu->step();
		cycles = static_cast<int>(m_clock.cycles() - before);
		m_bus->endInstruction(cycles);
	}

	if (m_ppu.takeNmi())
		m_cpu->nmi();
	// Two devices share the IRQ line: the APU's frame counter and DMC, and the
	// cartridge on boards with a counter of their own. Both are level-triggered
	// and stay asserted until the handler acknowledges them through their own
	// registers, so re-evaluating every step is correct -- and is why this is
	// not an edge like the NMI. The CPU sees one line, as it does in hardware.
	const bool cartIrq = m_cartridge && m_cartridge->irqAsserted();
	m_cpu->irq(m_apu.irqAsserted() || cartIrq);

	return cycles;
}

int Nes::stepFrame() {
	const std::uint64_t target = m_ppu.frame() + 1;
	int cycles = 0;
	while (m_ppu.frame() != target)
		cycles += step();
	return cycles;
}

} // namespace nes
