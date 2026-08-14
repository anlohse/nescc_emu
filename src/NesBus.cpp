#include "nes/NesBus.h"

namespace nes {

namespace {
const std::uint16_t RAM_END      = 0x1FFF;
const std::uint16_t RAM_MASK     = 0x07FF; // 2 KB mirrored four times
const std::uint16_t PPU_END      = 0x3FFF;
const std::uint16_t PPU_MASK     = 0x0007; // 8 registers mirrored every 8 bytes
const std::uint16_t APU_BEGIN    = 0x4000;
const std::uint16_t APU_CHANNELS_END = 0x4013;
const std::uint16_t APU_STATUS   = 0x4015;
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

NesBus::NesBus(Cartridge* cartridge, Ppu* ppu, Apu* apu) :
		m_ram(), m_cartridge(cartridge), m_ppu(ppu), m_apu(apu), m_dmaStall(0),
		m_stubReads(0), m_stubWrites(0), m_openBus(0),
		m_dotNumerator(3), m_dotDenominator(1), m_dotRemainder(0),
		m_ticking(false), m_accountedCycles(0), m_overruns(0) {
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

/* ------------------------------------------------------------------------- */
/* Time                                                                       */
/* ------------------------------------------------------------------------- */

void NesBus::setRegion(Region region) {
	if (region == Region::Pal) {
		m_dotNumerator = 16;    // 3.2 dots per CPU cycle
		m_dotDenominator = 5;
	} else {
		m_dotNumerator = 3;
		m_dotDenominator = 1;
	}
	m_dotRemainder = 0;
}

void NesBus::advance(int cycles) {
	if (cycles <= 0)
		return;

	// The APU's DMC reads its samples over this same bus, like a second bus
	// master. Those reads must not charge time of their own: the fetch costs
	// the CPU a stall that is accounted separately, and letting it tick from in
	// here would recurse.
	const bool wasTicking = m_ticking;
	m_ticking = false;

	m_dotRemainder += cycles * m_dotNumerator;
	if (m_ppu)
		m_ppu->tick(m_dotRemainder / m_dotDenominator);
	m_dotRemainder %= m_dotDenominator;
	if (m_apu)
		m_apu->tick(cycles);

	m_ticking = wasTicking;
}

void NesBus::cycle() {
	if (!m_ticking)
		return;
	m_accountedCycles++;
	advance(1);
}

void NesBus::beginInstruction() {
	if (m_cartridge)
		m_cartridge->beginInstruction();
	m_accountedCycles = 0;
	m_ticking = true;
}

void NesBus::endInstruction(int cycles) {
	m_ticking = false;
	const int owed = cycles - m_accountedCycles;
	if (owed < 0) {
		m_overruns++;
		return;
	}
	advance(owed);
}

std::uint8_t NesBus::readZapper() const {
	const Controller& gun = m_controllers[1];
	// Bit 3 is inverted: the phototransistor pulls the line down when it sees
	// light, so the bit is CLEAR on a hit and set the rest of the time. A gun
	// pointed away from the television is the same as one seeing nothing, which
	// is exactly the case Duck Hunt checks before it believes a shot.
	std::uint8_t value = Controller::ZAPPER_LIGHT;
	if (m_ppu && gun.zapperOnScreen()
			&& m_ppu->lightAt(gun.zapperX(), gun.zapperY()))
		value = 0;

	// Bit 4 is inverted too, which is the part no two references agree on.
	// Determined against Duck Hunt: with the bit set while the trigger is held
	// the game never begins its shot sequence at all, and with it clear the
	// screen blanks four frames later exactly as it should. Both bits being
	// active low is also the more sensible piece of hardware -- a closed switch
	// and a conducting phototransistor each pull their line down.
	if (!gun.zapperTrigger())
		value |= Controller::ZAPPER_TRIGGER;
	return value;
}

/* ------------------------------------------------------------------------- */
/* Access                                                                     */
/* ------------------------------------------------------------------------- */

uint8 NesBus::read(uint16 address) {
	// Whatever comes back was, by definition, on the data bus -- which is what an
	// unmapped read later returns. Recorded here rather than at each decode branch
	// so there is one place it can be got wrong.
	const uint8 value = readDecoded(address);
	m_openBus = value;
	return value;
}

uint8 NesBus::readDecoded(uint16 address) {
	cycle();

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
		if (address == JOY2) {
			if (m_controllers[1].device() == Controller::DEVICE_ZAPPER)
				return readZapper() | CONTROLLER_OPEN_BUS;
			return m_controllers[1].read() | CONTROLLER_OPEN_BUS;
		}
		// Reading $4015 acknowledges the APU's frame interrupt, so this must be
		// the real read and never peek().
		if (address == APU_STATUS)
			return m_apu ? m_apu->readStatus() : 0;
		// Everything else here is write-only or unallocated, and neither drives the
		// data bus: $4000-$4013 and $4014 are the APU's and the DMA's write-only
		// registers, $4018-$401F is the test space the 2A03 never decoded. All of
		// them read back as whatever the bus last carried.
		m_stubReads++;
		return m_openBus;
	}

	// $4020-$40FF: past everything the 2A03 decodes, and past everything any board
	// implemented here decodes either -- NROM, UxROM, CNROM, AxROM, MMC1, MMC3 and
	// mapper 87 all start at $6000. So nothing drives the lines.
	//
	// Named rather than folded into the range above because it is a board-level
	// claim rather than a chip-level one: a Famicom Disk System or one of the
	// pirate boards that does decode here would need this back.
	if (address < 0x4100) {
		m_stubReads++;
		return m_openBus;
	}

	if (m_cartridge)
		return m_cartridge->cpuRead(address);
	return m_openBus;   // no cartridge answering: nothing drives the lines
}

void NesBus::write(uint16 address, uint8 val) {
	cycle();
	// A write drives the data bus just as a read does, so it is what a later
	// unmapped read finds there.
	m_openBus = val;

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
				// The copy's own reads are not charged: the whole transfer is
				// paid for by the stall below, and ticking here as well would
				// advance the PPU twice over the same 513 cycles.
				const bool wasTicking = m_ticking;
				m_ticking = false;
				const std::uint16_t base = static_cast<std::uint16_t>(val << 8);
				std::uint8_t index = m_ppu->oamAddress();
				for (int i = 0; i < 256; i++)
					m_ppu->writeOam(static_cast<std::uint8_t>(index + i),
							read(static_cast<uint16>(base + i)));
				m_ticking = wasTicking;
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
		if ((address >= APU_BEGIN && address <= APU_CHANNELS_END)
				|| address == APU_STATUS || address == JOY2) {
			if (m_apu)
				m_apu->writeRegister(address, val);
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
		if (address == APU_STATUS)
			return m_apu ? m_apu->peekStatus() : 0;
		return 0;
	}

	if (m_cartridge)
		return m_cartridge->cpuRead(address);
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Save states                                                                */
/* ------------------------------------------------------------------------- */

void NesBus::serialize(State& state) {
	state.tag("BUS ");
	state.bytes(m_ram.data(), m_ram.size());
	state.value(m_dmaStall);

	// The dot fraction matters on PAL, where a CPU cycle is 3.2 dots: dropping
	// the remainder would shift the whole picture by up to a dot per restore.
	state.value(m_dotRemainder);
	state.value(m_accountedCycles);
	state.value(m_ticking);

	for (int port = 0; port < 2; port++)
		m_controllers[port].serialize(state);

	// The counters are diagnostics rather than machine state, and they are here
	// so that a state resumed twice reports the same numbers as one run
	// straight through -- which is what the determinism test compares.
	state.value(m_stubReads);
	state.value(m_stubWrites);
	state.value(m_overruns);
}

} // namespace nes
