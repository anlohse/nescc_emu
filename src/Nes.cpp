#include "nes/Nes.h"

#include <cstring>

namespace nes {

namespace {
const int DOTS_PER_CPU_CYCLE = 3;   // NTSC
} // namespace

Nes::Nes() : m_regs(), m_cartridge(), m_ppu(nullptr),
		m_bus(new NesBus(nullptr, &m_ppu)), m_clock(),
		m_cpu(new Processor(m_bus.get(), &m_regs, &m_clock)) {
	std::memset(&m_regs, 0, sizeof(m_regs));
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

	const int stall = m_bus->takeDmaStall();
	if (stall > 0) {
		// The CPU is off the bus for the duration of an OAM DMA, but the PPU
		// keeps running -- which is the whole reason the stall has to be
		// modelled rather than ignored.
		m_clock.waitCycles(stall);
		cycles = stall;
	} else {
		const std::uint64_t before = m_clock.cycles();
		m_cpu->step();
		cycles = static_cast<int>(m_clock.cycles() - before);
	}

	m_ppu.tick(cycles * DOTS_PER_CPU_CYCLE);
	if (m_ppu.takeNmi())
		m_cpu->nmi();

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
