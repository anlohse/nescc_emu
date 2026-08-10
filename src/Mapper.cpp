#include "nes/Mapper.h"

#include <ostream>

namespace nes {

const char* toString(Mirroring m) {
	switch (m) {
	case Mirroring::Horizontal:    return "horizontal";
	case Mirroring::Vertical:      return "vertical";
	case Mirroring::FourScreen:    return "four-screen";
	case Mirroring::SingleScreenA: return "single-screen A";
	case Mirroring::SingleScreenB: return "single-screen B";
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

const std::size_t BANK_1K  = 0x0400;
const std::size_t BANK_4K  = 0x1000;
const std::size_t BANK_8K  = 0x2000;
const std::size_t BANK_16K = 0x4000;
const std::size_t BANK_32K = 0x8000;

/**
 * Byte offset of a bank, wrapping the index into what the ROM actually holds.
 *
 * Negative indices count back from the end, so -1 is the last bank and -2 the
 * one before it. Most boards fix their vectors in the last bank, and naming it
 * that way avoids every mapper having to work out how many banks there are.
 */
std::size_t bankOffset(const std::vector<std::uint8_t>& memory, int bank, std::size_t bankSize) {
	const std::size_t count = memory.size() / bankSize;
	if (count == 0)
		return 0;
	int index = bank % static_cast<int>(count);
	if (index < 0)
		index += static_cast<int>(count);
	return static_cast<std::size_t>(index) * bankSize;
}

} // namespace

/* ------------------------------------------------------------------------- */
/* BankedMapper                                                               */
/* ------------------------------------------------------------------------- */

BankedMapper::BankedMapper(std::vector<std::uint8_t> prg, std::vector<std::uint8_t> chr,
		Mirroring mirroring, bool fourScreen) :
		m_prg(std::move(prg)),
		m_chr(std::move(chr)),
		m_prgRam(PRG_RAM_SIZE, 0),
		m_chrIsRam(false),
		m_mirroring(mirroring),
		m_fourScreen(fourScreen),
		m_busConflicts(false) {

	if (m_prg.empty())
		m_prg.resize(BANK_16K, 0);

	// No CHR banks in the header means the board carries 8 KB of CHR RAM.
	if (m_chr.empty()) {
		m_chr.assign(CHR_RAM_SIZE, 0);
		m_chrIsRam = true;
	}
}

std::uint8_t BankedMapper::cpuRead(std::uint16_t address) const {
	if (address >= PRG_BASE) {
		const std::size_t offset = prgOffset(address);
		return offset < m_prg.size() ? m_prg[offset] : 0;
	}
	if (address >= PRG_RAM_LO && address <= PRG_RAM_HI)
		return m_prgRam[address - PRG_RAM_LO];
	return 0; // open bus
}

void BankedMapper::cpuWrite(std::uint16_t address, std::uint8_t value) {
	if (address >= PRG_BASE) {
		// The write cannot land in ROM, but on a banked board the address and
		// data still reach the mapper's registers.
		std::uint8_t effective = value;
		if (m_busConflicts) {
			// Both the CPU and the ROM are driving the data bus, and a bus that
			// is pulled low by either side reads low -- so the board sees the
			// AND. Games written for these boards avoid the problem by writing
			// the bank number to an address that already contains it, which
			// makes the AND a no-op.
			const std::size_t offset = prgOffset(address);
			if (offset < m_prg.size())
				effective = static_cast<std::uint8_t>(value & m_prg[offset]);
		}
		writeRegister(address, effective);
		return;
	}
	if (address >= PRG_RAM_LO && address <= PRG_RAM_HI)
		writeWorkRam(address, value);
}

void BankedMapper::writeWorkRam(std::uint16_t address, std::uint8_t value) {
	m_prgRam[address - PRG_RAM_LO] = value;
}

std::uint8_t BankedMapper::ppuRead(std::uint16_t address) const {
	const std::size_t offset = chrOffset(address);
	return offset < m_chr.size() ? m_chr[offset] : 0;
}

void BankedMapper::ppuWrite(std::uint16_t address, std::uint8_t value) {
	if (!m_chrIsRam)
		return;
	const std::size_t offset = chrOffset(address);
	if (offset < m_chr.size())
		m_chr[offset] = value;
}

/* ------------------------------------------------------------------------- */
/* Mapper 0: NROM                                                             */
/* ------------------------------------------------------------------------- */

NromMapper::NromMapper(std::vector<std::uint8_t> prg, std::vector<std::uint8_t> chr,
		Mirroring mirroring, bool fourScreen) :
		BankedMapper(std::move(prg), std::move(chr), mirroring, fourScreen),
		m_prgMask(0) {
	// A 16 KB image is mirrored into both halves of $8000-$FFFF, so the vectors
	// at the top of the space read from the top of the single bank. Masking with
	// size-1 does that for free, because both legal sizes are powers of two.
	m_prgMask = static_cast<std::uint16_t>(m_prg.size() - 1);
}

std::size_t NromMapper::prgOffset(std::uint16_t address) const {
	return static_cast<std::size_t>((address - PRG_BASE) & m_prgMask);
}

std::size_t NromMapper::chrOffset(std::uint16_t address) const {
	return static_cast<std::size_t>(address & CHR_MASK);
}

/* ------------------------------------------------------------------------- */
/* Mapper 2: UxROM                                                            */
/* ------------------------------------------------------------------------- */

UxRomMapper::UxRomMapper(std::vector<std::uint8_t> prg, std::vector<std::uint8_t> chr,
		Mirroring mirroring, bool fourScreen) :
		BankedMapper(std::move(prg), std::move(chr), mirroring, fourScreen),
		m_bank(0) { }

std::size_t UxRomMapper::prgOffset(std::uint16_t address) const {
	const std::size_t within = address & (BANK_16K - 1);
	if (address < 0xC000)
		return bankOffset(m_prg, m_bank, BANK_16K) + within;
	// The high half never moves: it holds the vectors.
	return bankOffset(m_prg, -1, BANK_16K) + within;
}

std::size_t UxRomMapper::chrOffset(std::uint16_t address) const {
	return static_cast<std::size_t>(address & CHR_MASK);
}

void UxRomMapper::writeRegister(std::uint16_t /*address*/, std::uint8_t value) {
	// Any address in $8000-$FFFF selects; only the data matters.
	m_bank = value & 0x0F;
}

/* ------------------------------------------------------------------------- */
/* Mapper 3: CNROM                                                            */
/* ------------------------------------------------------------------------- */

CnRomMapper::CnRomMapper(std::vector<std::uint8_t> prg, std::vector<std::uint8_t> chr,
		Mirroring mirroring, bool fourScreen) :
		BankedMapper(std::move(prg), std::move(chr), mirroring, fourScreen),
		m_prgMask(0), m_bank(0) {
	m_prgMask = static_cast<std::uint16_t>(m_prg.size() - 1);
}

std::size_t CnRomMapper::prgOffset(std::uint16_t address) const {
	return static_cast<std::size_t>((address - PRG_BASE) & m_prgMask);
}

std::size_t CnRomMapper::chrOffset(std::uint16_t address) const {
	return bankOffset(m_chr, m_bank, BANK_8K) + (address & CHR_MASK);
}

void CnRomMapper::writeRegister(std::uint16_t /*address*/, std::uint8_t value) {
	m_bank = value & 0x03;
}

/* ------------------------------------------------------------------------- */
/* Mapper 7: AxROM                                                            */
/* ------------------------------------------------------------------------- */

AxRomMapper::AxRomMapper(std::vector<std::uint8_t> prg, std::vector<std::uint8_t> chr,
		Mirroring mirroring, bool fourScreen) :
		BankedMapper(std::move(prg), std::move(chr), mirroring, fourScreen),
		m_bank(0) {
	// The board has no fixed bank and no wired mirroring; it powers up on one
	// screen and the game picks which.
	m_mirroring = Mirroring::SingleScreenA;
}

std::size_t AxRomMapper::prgOffset(std::uint16_t address) const {
	return bankOffset(m_prg, m_bank, BANK_32K) + (address & (BANK_32K - 1));
}

std::size_t AxRomMapper::chrOffset(std::uint16_t address) const {
	return static_cast<std::size_t>(address & CHR_MASK);
}

void AxRomMapper::writeRegister(std::uint16_t /*address*/, std::uint8_t value) {
	m_bank = value & 0x07;
	m_mirroring = (value & 0x10) ? Mirroring::SingleScreenB : Mirroring::SingleScreenA;
}

/* ------------------------------------------------------------------------- */
/* Mapper 87: CHR bank register in the work-RAM window                        */
/* ------------------------------------------------------------------------- */

Mapper87::Mapper87(std::vector<std::uint8_t> prg, std::vector<std::uint8_t> chr,
		Mirroring mirroring, bool fourScreen) :
		BankedMapper(std::move(prg), std::move(chr), mirroring, fourScreen),
		m_prgMask(0), m_bank(0) {
	m_prgMask = static_cast<std::uint16_t>(m_prg.size() - 1);
}

std::size_t Mapper87::prgOffset(std::uint16_t address) const {
	return static_cast<std::size_t>((address - PRG_BASE) & m_prgMask);
}

std::size_t Mapper87::chrOffset(std::uint16_t address) const {
	return bankOffset(m_chr, m_bank, BANK_8K) + (address & CHR_MASK);
}

void Mapper87::writeWorkRam(std::uint16_t /*address*/, std::uint8_t value) {
	// The bank bits are crossed on the board: value bit 0 becomes bank bit 1
	// and vice versa. Writing 1 selects bank 2, not bank 1.
	m_bank = ((value & 0x01) << 1) | ((value & 0x02) >> 1);
}

/* ------------------------------------------------------------------------- */
/* Mapper 1: MMC1                                                             */
/* ------------------------------------------------------------------------- */

namespace {
// The shift register starts with a 1 in the top bit. Four writes push it down
// to bit 0; seeing it there is how the board knows the next write is the fifth.
const std::uint8_t MMC1_SHIFT_RESET = 0x10;

const std::uint8_t MMC1_CTRL_MIRROR_MASK = 0x03;
const std::uint8_t MMC1_CTRL_PRG_MODE    = 0x0C;
const std::uint8_t MMC1_CTRL_CHR_4K      = 0x10;
} // namespace

Mmc1Mapper::Mmc1Mapper(std::vector<std::uint8_t> prg, std::vector<std::uint8_t> chr,
		Mirroring mirroring, bool fourScreen) :
		BankedMapper(std::move(prg), std::move(chr), mirroring, fourScreen),
		m_shift(MMC1_SHIFT_RESET),
		m_instructionsKnown(false), m_wroteThisInstruction(false),
		// PRG mode 3 at power-on: the last bank is fixed at $C000, which is the
		// only arrangement where the reset vector is guaranteed to be readable
		// before the game has configured anything.
		m_control(0x0C),
		m_chrBank0(0), m_chrBank1(0), m_prgBank(0) {
	applyMirroring();
}

void Mmc1Mapper::writeRegister(std::uint16_t address, std::uint8_t value) {
	// The second write of a read-modify-write lands one cycle after the first,
	// and the board is still busy with it. Note that this drops the write the
	// programmer meant and keeps the one the CPU made on its way past -- which
	// is the whole point, since the two carry different bits.
	if (m_instructionsKnown) {
		if (m_wroteThisInstruction)
			return;
		m_wroteThisInstruction = true;
	}

	if (value & 0x80) {
		// Reset: abandon a partial sequence and force the PRG mode back to
		// "last bank fixed high", so the vectors are reachable again.
		m_shift = MMC1_SHIFT_RESET;
		m_control |= MMC1_CTRL_PRG_MODE;
		applyMirroring();
		return;
	}

	const bool fifthWrite = (m_shift & 1) != 0;
	m_shift = static_cast<std::uint8_t>(((m_shift >> 1) | ((value & 1) << 4)) & 0x1F);
	if (!fifthWrite)
		return;

	// Only the address of the *last* write picks the destination register.
	commit(address, m_shift);
	m_shift = MMC1_SHIFT_RESET;
}

void Mmc1Mapper::commit(std::uint16_t address, std::uint8_t value) {
	switch ((address >> 13) & 3) {
	case 0:   // $8000-$9FFF
		m_control = value;
		applyMirroring();
		break;
	case 1:   // $A000-$BFFF
		m_chrBank0 = value;
		break;
	case 2:   // $C000-$DFFF
		m_chrBank1 = value;
		break;
	default:  // $E000-$FFFF; bit 4 disables PRG RAM, which nothing here needs
		m_prgBank = static_cast<std::uint8_t>(value & 0x0F);
		break;
	}
}

void Mmc1Mapper::applyMirroring() {
	switch (m_control & MMC1_CTRL_MIRROR_MASK) {
	case 0: m_mirroring = Mirroring::SingleScreenA; break;
	case 1: m_mirroring = Mirroring::SingleScreenB; break;
	case 2: m_mirroring = Mirroring::Vertical; break;
	default: m_mirroring = Mirroring::Horizontal; break;
	}
}

std::size_t Mmc1Mapper::prgOffset(std::uint16_t address) const {
	const std::size_t within16 = address & (BANK_16K - 1);
	switch ((m_control & MMC1_CTRL_PRG_MODE) >> 2) {
	case 0:
	case 1:
		// One 32 KB bank; the low bit of the bank number is ignored.
		return bankOffset(m_prg, m_prgBank >> 1, BANK_32K) + (address & (BANK_32K - 1));
	case 2:
		// First bank fixed low, switchable high.
		if (address < 0xC000)
			return within16;
		return bankOffset(m_prg, m_prgBank, BANK_16K) + within16;
	default:
		// Switchable low, last bank fixed high. The common arrangement.
		if (address < 0xC000)
			return bankOffset(m_prg, m_prgBank, BANK_16K) + within16;
		return bankOffset(m_prg, -1, BANK_16K) + within16;
	}
}

std::size_t Mmc1Mapper::chrOffset(std::uint16_t address) const {
	const std::uint16_t addr = address & CHR_MASK;
	if (m_control & MMC1_CTRL_CHR_4K) {
		const int bank = (addr < 0x1000) ? m_chrBank0 : m_chrBank1;
		return bankOffset(m_chr, bank, BANK_4K) + (addr & (BANK_4K - 1));
	}
	// One 8 KB bank, low bit ignored -- the same trick as the 32 KB PRG mode.
	return bankOffset(m_chr, m_chrBank0 >> 1, BANK_8K) + addr;
}

/* ------------------------------------------------------------------------- */
/* Mapper 4: MMC3                                                             */
/* ------------------------------------------------------------------------- */

Mmc3Mapper::Mmc3Mapper(std::vector<std::uint8_t> prg, std::vector<std::uint8_t> chr,
		Mirroring mirroring, bool fourScreen) :
		BankedMapper(std::move(prg), std::move(chr), mirroring, fourScreen),
		m_bankSelect(0), m_banks(), m_prgMode(false), m_chrInvert(false),
		m_irqLatch(0), m_irqCounter(0), m_irqReload(false),
		m_irqEnabled(false), m_irqPending(false) {
	for (int i = 0; i < 8; i++)
		m_banks[i] = 0;
}

void Mmc3Mapper::writeRegister(std::uint16_t address, std::uint8_t value) {
	// Registers come in pairs: the even address of each range selects, the odd
	// one acts. Only bits 13-14 of the address and bit 0 matter.
	switch (address & 0xE001) {
	case 0x8000:
		m_bankSelect = static_cast<std::uint8_t>(value & 0x07);
		m_prgMode = (value & 0x40) != 0;
		m_chrInvert = (value & 0x80) != 0;
		break;
	case 0x8001:
		m_banks[m_bankSelect] = value;
		break;
	case 0xA000:
		// Ignored on four-screen boards, which mirroring() handles.
		m_mirroring = (value & 1) ? Mirroring::Horizontal : Mirroring::Vertical;
		break;
	case 0xA001:
		break;   // PRG RAM protect; nothing here enforces it
	case 0xC000:
		m_irqLatch = value;
		break;
	case 0xC001:
		// Does not load the counter directly -- it forces a reload at the next
		// scanline, which is why the latch can be changed mid-frame safely.
		m_irqCounter = 0;
		m_irqReload = true;
		break;
	case 0xE000:
		m_irqEnabled = false;
		m_irqPending = false;   // disabling also acknowledges
		break;
	case 0xE001:
		m_irqEnabled = true;
		break;
	default:
		break;
	}
}

void Mmc3Mapper::ppuA12Rise() {
	if (m_irqCounter == 0 || m_irqReload) {
		m_irqCounter = m_irqLatch;
		m_irqReload = false;
	} else {
		m_irqCounter--;
	}
	if (m_irqCounter == 0 && m_irqEnabled)
		m_irqPending = true;
}

std::size_t Mmc3Mapper::prgOffset(std::uint16_t address) const {
	const std::size_t within = address & (BANK_8K - 1);
	const int slot = (address - PRG_BASE) / static_cast<int>(BANK_8K);   // 0..3

	int bank;
	switch (slot) {
	case 0:
		// Mode swaps which of these two is switchable and which is fixed to the
		// second-to-last bank.
		bank = m_prgMode ? -2 : (m_banks[6] & 0x3F);
		break;
	case 1:
		bank = m_banks[7] & 0x3F;
		break;
	case 2:
		bank = m_prgMode ? (m_banks[6] & 0x3F) : -2;
		break;
	default:
		bank = -1;   // the last bank is always fixed at $E000: the vectors
		break;
	}
	return bankOffset(m_prg, bank, BANK_8K) + within;
}

std::size_t Mmc3Mapper::chrOffset(std::uint16_t address) const {
	// The inversion bit swaps the two halves of the pattern space wholesale,
	// which lets a game exchange its background and sprite tile sets with one
	// write instead of six.
	std::uint16_t addr = address & CHR_MASK;
	if (m_chrInvert)
		addr ^= 0x1000;

	const int slot = addr / static_cast<int>(BANK_1K);   // 0..7
	int bank;
	if (slot < 2)
		bank = (m_banks[0] & 0xFE) + slot;          // R0 spans 2 KB
	else if (slot < 4)
		bank = (m_banks[1] & 0xFE) + (slot - 2);    // R1 spans 2 KB
	else
		bank = m_banks[2 + (slot - 4)];             // R2-R5 are 1 KB each

	return bankOffset(m_chr, bank, BANK_1K) + (addr & (BANK_1K - 1));
}

/* ------------------------------------------------------------------------- */
/* Save states                                                                */
/* ------------------------------------------------------------------------- */

void BankedMapper::serialize(State& state) {
	state.tag("MAPR");
	// Work RAM is the part a player would notice losing: it is where a game
	// with a battery keeps its file, and where every game keeps its variables
	// if the board has any.
	state.blob(m_prgRam);
	state.value(m_mirroring);
	state.value(m_fourScreen);

	// CHR only when the board has RAM there rather than ROM. Writable CHR is
	// state; a ROM is a copy of the file and has no business in a save.
	std::uint8_t chrIsRam = m_chr.empty() ? 0 : (m_chrIsRam ? 1 : 0);
	state.value(chrIsRam);
	if (chrIsRam)
		state.blob(m_chr);
}

void UxRomMapper::serialize(State& state) {
	BankedMapper::serialize(state);
	state.tag("UXRM");
	state.value(m_bank);
}

void CnRomMapper::serialize(State& state) {
	BankedMapper::serialize(state);
	state.tag("CNRM");
	state.value(m_bank);
}

void AxRomMapper::serialize(State& state) {
	BankedMapper::serialize(state);
	state.tag("AXRM");
	state.value(m_bank);
}

void Mapper87::serialize(State& state) {
	BankedMapper::serialize(state);
	state.tag("M087");
	state.value(m_bank);
}

void Mmc1Mapper::serialize(State& state) {
	BankedMapper::serialize(state);
	state.tag("MMC1");
	state.value(m_shift);
	state.value(m_control);
	state.value(m_chrBank0);
	state.value(m_chrBank1);
	state.value(m_prgBank);
	// The serial port is mid-word as often as not, so both halves of its state
	// have to travel: what has been shifted in, and how far.
	state.value(m_instructionsKnown);
	state.value(m_wroteThisInstruction);
}

void Mmc3Mapper::serialize(State& state) {
	BankedMapper::serialize(state);
	state.tag("MMC3");
	state.value(m_bankSelect);
	state.bytes(m_banks, sizeof(m_banks));
	state.value(m_prgMode);
	state.value(m_chrInvert);
	// The IRQ counter is the whole reason a state has to include this board:
	// restoring mid-frame with a stale counter puts the split on the wrong line.
	state.value(m_irqLatch);
	state.value(m_irqCounter);
	state.value(m_irqReload);
	state.value(m_irqEnabled);
	state.value(m_irqPending);
}

} // namespace nes
