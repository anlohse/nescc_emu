#include "nes/Mapper.h"

#include <ostream>

namespace nes {

const char* toString(Mirroring m) {
	switch (m) {
	case Mirroring::Horizontal: return "horizontal";
	case Mirroring::Vertical:   return "vertical";
	case Mirroring::FourScreen: return "four-screen";
	}
	return "unknown";
}

std::ostream& operator<<(std::ostream& os, Mirroring m) {
	return os << toString(m);
}

namespace {
const std::uint16_t PRG_BASE   = 0x8000;
const std::uint16_t PRG_RAM_LO = 0x6000;
const std::uint16_t PRG_RAM_HI = 0x7FFF;
const std::size_t   PRG_RAM_SIZE = 0x2000;
const std::size_t   CHR_RAM_SIZE = 0x2000;
const std::uint16_t CHR_MASK   = 0x1FFF;
} // namespace

NromMapper::NromMapper(std::vector<std::uint8_t> prg, std::vector<std::uint8_t> chr,
		Mirroring mirroring) :
		m_prg(std::move(prg)),
		m_chr(std::move(chr)),
		m_prgRam(PRG_RAM_SIZE, 0),
		m_prgMask(0),
		m_chrIsRam(false),
		m_mirroring(mirroring) {

	// A 16 KB image is mirrored into both halves of $8000-$FFFF, so the vectors
	// at the top of the space read from the top of the single bank. Masking with
	// size-1 does that for free, because both legal sizes are powers of two.
	if (m_prg.empty())
		m_prg.resize(0x4000, 0);
	m_prgMask = static_cast<std::uint16_t>(m_prg.size() - 1);

	// No CHR banks in the header means the board carries 8 KB of CHR RAM.
	if (m_chr.empty()) {
		m_chr.assign(CHR_RAM_SIZE, 0);
		m_chrIsRam = true;
	}
}

std::uint8_t NromMapper::cpuRead(std::uint16_t address) const {
	if (address >= PRG_BASE)
		return m_prg[(address - PRG_BASE) & m_prgMask];
	if (address >= PRG_RAM_LO && address <= PRG_RAM_HI)
		return m_prgRam[address - PRG_RAM_LO];
	return 0; // open bus
}

void NromMapper::cpuWrite(std::uint16_t address, std::uint8_t value) {
	if (address >= PRG_RAM_LO && address <= PRG_RAM_HI) {
		m_prgRam[address - PRG_RAM_LO] = value;
		return;
	}
	// Writes to $8000-$FFFF hit ROM and are discarded. NROM has no registers,
	// so there is nothing to bank-switch.
}

std::uint8_t NromMapper::ppuRead(std::uint16_t address) const {
	return m_chr[address & CHR_MASK];
}

void NromMapper::ppuWrite(std::uint16_t address, std::uint8_t value) {
	if (m_chrIsRam)
		m_chr[address & CHR_MASK] = value;
}

} // namespace nes
