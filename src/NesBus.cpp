#include "nes/NesBus.h"

namespace nes {

namespace {
const std::uint16_t RAM_END      = 0x1FFF;
const std::uint16_t RAM_MASK     = 0x07FF; // 2 KB mirrored four times
const std::uint16_t PPU_END      = 0x3FFF;
const std::uint16_t PPU_MASK     = 0x0007; // 8 registers mirrored every 8 bytes
const std::uint16_t OAM_DMA      = 0x4014;
const std::uint16_t JOY1         = 0x4016;
const std::uint16_t JOY2         = 0x4017;
const std::uint16_t IO_END       = 0x401F;

// $4016/$4017 return one bit of controller data in bit 0; the rest of the byte
// is whatever the CPU bus last carried. Since these reads are always part of a
// $40xx access, the high byte of the address is what lingers, and bit 6 of $40
// is the only bit that shows. Games test bit 0 alone, but some test ROMs check
// this, and it costs nothing to be right.
const std::uint8_t CONTROLLER_OPEN_BUS = 0x40;

// An OAM DMA costs 513 CPU cycles, or 514 when it starts on an odd cycle.
// Nothing tracks CPU cycle parity yet, so this always charges the shorter one.
const int OAM_DMA_CYCLES = 513;
} // namespace

NesBus::NesBus(Cartridge* cartridge, Ppu* ppu) :
		m_ram(), m_cartridge(cartridge), m_ppu(ppu), m_dmaStall(0),
		m_stubReads(0), m_stubWrites(0) {
	m_ram.fill(0);
}

void NesBus::clearRam() {
	m_ram.fill(0);
}

int NesBus::takeDmaStall() {
	const int stall = m_dmaStall;
	m_dmaStall = 0;
	return stall;
}

uint8 NesBus::read(uint16 address) {
	if (address <= RAM_END)
		return m_ram[address & RAM_MASK];

	if (address <= PPU_END) {
		// Reads of $2002 and $2007 change PPU state; this must be the real
		// register read, never peek().
		return m_ppu ? m_ppu->readRegister(address & PPU_MASK) : 0;
	}

	if (address <= IO_END) {
		// Reading a controller port clocks its shift register, so like the PPU
		// registers this must never be served from peek().
		if (address == JOY1)
			return m_controllers[0].read() | CONTROLLER_OPEN_BUS;
		if (address == JOY2)
			return m_controllers[1].read() | CONTROLLER_OPEN_BUS;
		// APU status at $4015 also has read side effects. Still stubbed.
		m_stubReads++;
		return 0;
	}

	if (m_cartridge)
		return m_cartridge->cpuRead(address);
	return 0; // open bus
}

void NesBus::write(uint16 address, uint8 val) {
	if (address <= RAM_END) {
		m_ram[address & RAM_MASK] = val;
		return;
	}

	if (address <= PPU_END) {
		if (m_ppu)
			m_ppu->writeRegister(address & PPU_MASK, val);
		return;
	}

	if (address <= IO_END) {
		if (address == OAM_DMA) {
			// Copy a whole 256-byte page into OAM. The CPU is held off the bus
			// while this happens, which the stall accounts for.
			if (m_ppu) {
				const std::uint16_t base = static_cast<std::uint16_t>(val << 8);
				std::uint8_t index = m_ppu->oamAddress();
				for (int i = 0; i < 256; i++)
					m_ppu->writeOam(static_cast<std::uint8_t>(index + i),
							read(static_cast<uint16>(base + i)));
			}
			m_dmaStall += OAM_DMA_CYCLES;
			return;
		}
		if (address == JOY1) {
			// One strobe line runs to both ports. $4017 is *not* its partner on
			// writes -- that address is the APU frame counter, which is why a
			// game latches both pads with a single write here.
			m_controllers[0].writeStrobe(val);
			m_controllers[1].writeStrobe(val);
			return;
		}
		m_stubWrites++;
		return;
	}

	if (m_cartridge)
		m_cartridge->cpuWrite(address, val);
}

std::uint8_t NesBus::peek(std::uint16_t address) const {
	if (address <= RAM_END)
		return m_ram[address & RAM_MASK];

	if (address <= PPU_END)
		return m_ppu ? m_ppu->peekRegister(address & PPU_MASK) : 0;

	if (address <= IO_END) {
		if (address == JOY1)
			return m_controllers[0].peek() | CONTROLLER_OPEN_BUS;
		if (address == JOY2)
			return m_controllers[1].peek() | CONTROLLER_OPEN_BUS;
		return 0;
	}

	if (m_cartridge)
		return m_cartridge->cpuRead(address);
	return 0;
}

} // namespace nes
