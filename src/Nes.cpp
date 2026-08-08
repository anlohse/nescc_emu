#include "nes/Nes.h"

#include <cstring>

namespace nes {

namespace {
const int DOTS_PER_CPU_CYCLE = 3;   // NTSC
} // namespace

Nes::Nes() : m_regs(), m_cartridge(), m_ppu(nullptr), m_apu(),
		m_bus(new NesBus(nullptr, &m_ppu, &m_apu)), m_clock(),
		m_cpu(new Processor(m_bus.get(), &m_regs, &m_clock)) {
	std::memset(&m_regs, 0, sizeof(m_regs));
	// The DMC fetches its samples over the CPU bus, like a second bus master.
	m_apu.setBus(m_bus.get());
}

Nes::~Nes() = default;

bool Nes::loadRom(const std::string& path, std::string* error) {
	std::unique_ptr<Cartridge> cart = Cartridge::fromFile(path, error);
	if (!cart)
		return false;
	setCartridge(std::move(cart));
	return true;
}

void Nes::setCartridge(std::unique_ptr<Cartridge> cartridge) {
	m_cartridge = std::move(cartridge);
	m_bus->setCartridge(m_cartridge.get());
	m_ppu.setCartridge(m_cartridge.get());
}

void Nes::reset() {
	m_bus->clearRam();
	m_ppu.reset();
	m_apu.reset();
	std::memset(&m_regs, 0, sizeof(m_regs));
	m_regs.sp = 0xFD;
	m_regs.sr = FLAG__ | FLAG_I;
	m_regs.pc = m_bus->read(0xFFFC) | (m_bus->read(0xFFFD) << 8);
	m_cpu->clearInterrupts();
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
		m_clock.waitCycles(stall);
		cycles = stall;
	} else {
		const std::uint64_t before = m_clock.cycles();
		m_cpu->step();
		cycles = static_cast<int>(m_clock.cycles() - before);
	}

	m_ppu.tick(cycles * DOTS_PER_CPU_CYCLE);
	m_apu.tick(cycles);

	if (m_ppu.takeNmi())
		m_cpu->nmi();
	// The APU's IRQ is level-triggered: it stays asserted until the handler
	// acknowledges it by reading $4015 or writing $4010/$4017. Re-asserting
	// every step is correct, and is why this is not an edge like the NMI.
	m_cpu->irq(m_apu.irqAsserted());

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
