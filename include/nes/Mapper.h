#ifndef NES_MAPPER_H
#define NES_MAPPER_H

#include <cstdint>
#include <iosfwd>
#include <vector>

namespace nes {

/**
 * How the PPU's nametable address space is folded onto the 2 KB of VRAM on the
 * console. Fixed by the cartridge wiring on simple boards, switchable by the
 * mapper on later ones.
 *
 * The single-screen modes point all four nametables at the same physical KB.
 * Boards that scroll in only one direction use them to free the other half of
 * VRAM for something else.
 */
enum class Mirroring {
	Horizontal,
	Vertical,
	FourScreen,
	SingleScreenA,
	SingleScreenB
};

const char* toString(Mirroring m);

/** Lets test frameworks and logs print a Mirroring by name rather than as an int. */
std::ostream& operator<<(std::ostream& os, Mirroring m);

/**
 * A cartridge board: the logic between the console's buses and the ROM chips.
 *
 * The CPU side covers $4020-$FFFF, which on almost every board means optional
 * work RAM at $6000-$7FFF and program ROM at $8000-$FFFF. The PPU side covers
 * $0000-$1FFF, the pattern tables.
 *
 * Reads are const so that a debugger can inspect memory without disturbing a
 * mapper that has read-triggered side effects (MMC2 and MMC4 switch banks on
 * pattern-table fetches). Mappers that need to react to reads should do so
 * through a separate non-const hook rather than making these mutable.
 */
class Mapper {
public:
	virtual ~Mapper() = default;

	/**
	 * A new CPU instruction is about to run.
	 *
	 * Serial boards care how close together two writes land. MMC1 ignores one
	 * that arrives on the cycle straight after another, which is exactly what
	 * the second half of a read-modify-write is. Nothing here counts cycles,
	 * but it does not need to: the only way one 6502 instruction writes to the
	 * cartridge twice is that pair, so "second write of this instruction" and
	 * "write on the very next cycle" name the same event.
	 *
	 * Boards with a properly decoded register have no opinion and ignore this.
	 */
	virtual void beginInstruction() { }

	/** CPU bus read, $4020-$FFFF. Returns open-bus (0) for unmapped addresses. */
	virtual std::uint8_t cpuRead(std::uint16_t address) const = 0;
	/** CPU bus write, $4020-$FFFF. Bank switching happens here on most boards. */
	virtual void cpuWrite(std::uint16_t address, std::uint8_t value) = 0;

	/** PPU bus read, $0000-$1FFF. */
	virtual std::uint8_t ppuRead(std::uint16_t address) const = 0;
	/** PPU bus write, $0000-$1FFF. Only lands when the board has CHR RAM. */
	virtual void ppuWrite(std::uint16_t address, std::uint8_t value) = 0;

	virtual Mirroring mirroring() const = 0;

	/**
	 * One scanline's worth of pattern fetches has happened.
	 *
	 * MMC3 and its relatives count these to time an IRQ partway down the
	 * screen, which is how a game splits the display without polling. Real
	 * hardware counts rising edges on PPU address line A12 rather than
	 * scanlines; during rendering that works out to one per line, which is
	 * what this models. Boards without a counter ignore it.
	 */
	virtual void ppuA12Rise() { }

	/**
	 * True while the board is holding the CPU's IRQ line down.
	 *
	 * Level-triggered, like the APU's: it stays asserted until the game
	 * acknowledges it through the board's own register.
	 */
	virtual bool irqAsserted() const { return false; }

	/**
	 * Tell the board whether a write to $8000-$FFFF collides with its ROM.
	 *
	 * On the discrete-logic boards the bank register is not decoded away from
	 * the ROM chip, so during a write both the CPU and the ROM drive the data
	 * bus and the board sees the two values ANDed together. Boards with a
	 * properly decoded register -- MMC1, MMC3 -- and boards whose register is
	 * not in ROM space at all ignore this.
	 */
	virtual void setBusConflicts(bool /*enabled*/) { }

	/**
	 * The board's work RAM at $6000-$7FFF, or null when it has none.
	 *
	 * Whether this survives a power cycle is not the board's business: the
	 * header's battery bit decides that, and Cartridge owns the decision. All
	 * this does is say which bytes there are to keep.
	 */
	virtual std::vector<std::uint8_t>* workRam() { return nullptr; }
	virtual const std::vector<std::uint8_t>* workRam() const { return nullptr; }

	/** iNES mapper number. */
	virtual int number() const = 0;
};

/**
 * Shared plumbing for boards that map banks of ROM into fixed windows.
 *
 * Every mapper here owns the same three memories and answers the same two
 * questions: which byte of PRG does this CPU address land on, and which byte of
 * CHR does this PPU address land on. Subclasses answer those and handle their
 * register writes; the address decoding, PRG RAM, CHR RAM and bounds checking
 * live here once.
 */
class BankedMapper : public Mapper {
public:
	BankedMapper(std::vector<std::uint8_t> prg, std::vector<std::uint8_t> chr,
			Mirroring mirroring, bool fourScreen = false);

	std::uint8_t cpuRead(std::uint16_t address) const override;
	void cpuWrite(std::uint16_t address, std::uint8_t value) override;
	std::uint8_t ppuRead(std::uint16_t address) const override;
	void ppuWrite(std::uint16_t address, std::uint8_t value) override;

	Mirroring mirroring() const override {
		// Four-screen boards carry their own extra VRAM, so the wiring wins
		// over anything the mapper's mirroring register says.
		return m_fourScreen ? Mirroring::FourScreen : m_mirroring;
	}

	/** True when the board has no CHR ROM and the PPU is writing to RAM. */
	bool hasChrRam() const { return m_chrIsRam; }

	std::vector<std::uint8_t>* workRam() override { return &m_prgRam; }
	const std::vector<std::uint8_t>* workRam() const override { return &m_prgRam; }

	void setBusConflicts(bool enabled) override { m_busConflicts = enabled; }
	bool hasBusConflicts() const { return m_busConflicts; }

protected:
	/** Byte offset into PRG for a CPU address in $8000-$FFFF. */
	virtual std::size_t prgOffset(std::uint16_t address) const = 0;
	/** Byte offset into CHR for a PPU address in $0000-$1FFF. */
	virtual std::size_t chrOffset(std::uint16_t address) const = 0;
	/** A write to $8000-$FFFF. Boards with no registers leave this alone. */
	virtual void writeRegister(std::uint16_t /*address*/, std::uint8_t /*value*/) { }

	/**
	 * A write to $6000-$7FFF, which is work RAM on most boards.
	 *
	 * A few put a bank register there instead. That is only safe because those
	 * boards carry no RAM for it to collide with -- there is nothing at that
	 * address to overwrite.
	 */
	virtual void writeWorkRam(std::uint16_t address, std::uint8_t value);

	std::vector<std::uint8_t> m_prg;
	std::vector<std::uint8_t> m_chr;
	std::vector<std::uint8_t> m_prgRam;
	bool m_chrIsRam;
	Mirroring m_mirroring;
	bool m_fourScreen;
	bool m_busConflicts;
};

/**
 * Mapper 0, NROM: no banking at all.
 *
 * Program ROM is 16 KB or 32 KB mapped at $8000. A 16 KB image appears twice,
 * so the vectors at $FFFA-$FFFF read from the top of the single bank. Character
 * memory is an 8 KB ROM, or 8 KB of RAM when the image declares no CHR banks.
 * Some boards add 8 KB of work RAM at $6000.
 */
class NromMapper : public BankedMapper {
public:
	NromMapper(std::vector<std::uint8_t> prg, std::vector<std::uint8_t> chr,
			Mirroring mirroring, bool fourScreen = false);

	int number() const override { return 0; }

protected:
	std::size_t prgOffset(std::uint16_t address) const override;
	std::size_t chrOffset(std::uint16_t address) const override;

private:
	std::uint16_t m_prgMask;
};

/**
 * Mapper 2, UxROM: one switchable 16 KB PRG bank, one fixed.
 *
 * $8000-$BFFF selects any bank; $C000-$FFFF is permanently the last one. That
 * fixed half is not a convenience -- it holds the reset and interrupt vectors,
 * so it has to stay reachable no matter which bank the game has switched in.
 * The board carries CHR RAM rather than CHR ROM.
 */
class UxRomMapper : public BankedMapper {
public:
	UxRomMapper(std::vector<std::uint8_t> prg, std::vector<std::uint8_t> chr,
			Mirroring mirroring, bool fourScreen = false);

	int number() const override { return 2; }

protected:
	std::size_t prgOffset(std::uint16_t address) const override;
	std::size_t chrOffset(std::uint16_t address) const override;
	void writeRegister(std::uint16_t address, std::uint8_t value) override;

private:
	int m_bank;
};

/**
 * Mapper 3, CNROM: fixed program ROM, switchable 8 KB character bank.
 *
 * The inverse of UxROM. Games with more graphics than code use it to page whole
 * tile sets in and out.
 */
class CnRomMapper : public BankedMapper {
public:
	CnRomMapper(std::vector<std::uint8_t> prg, std::vector<std::uint8_t> chr,
			Mirroring mirroring, bool fourScreen = false);

	int number() const override { return 3; }

protected:
	std::size_t prgOffset(std::uint16_t address) const override;
	std::size_t chrOffset(std::uint16_t address) const override;
	void writeRegister(std::uint16_t address, std::uint8_t value) override;

private:
	std::uint16_t m_prgMask;
	int m_bank;
};

/**
 * Mapper 7, AxROM: a 32 KB PRG bank and a single-screen mirroring select.
 *
 * The whole address space switches at once, so there is no fixed bank holding
 * the vectors -- every bank has to carry its own copy. Bit 4 of the register
 * picks which nametable the single screen points at, which is how these games
 * flip between two full screens instead of scrolling.
 */
class AxRomMapper : public BankedMapper {
public:
	AxRomMapper(std::vector<std::uint8_t> prg, std::vector<std::uint8_t> chr,
			Mirroring mirroring, bool fourScreen = false);

	int number() const override { return 7; }

protected:
	std::size_t prgOffset(std::uint16_t address) const override;
	std::size_t chrOffset(std::uint16_t address) const override;
	void writeRegister(std::uint16_t address, std::uint8_t value) override;

private:
	int m_bank;
};

/**
 * Mapper 87: a CHR bank register living in the work-RAM window.
 *
 * Discrete logic rather than a custom chip, used by a handful of Japanese
 * releases. Program ROM is fixed like NROM; writes to $6000-$7FFF select one of
 * four 8 KB character banks.
 *
 * The two bank bits arrive swapped -- bit 0 of the value drives the high bit of
 * the bank and bit 1 drives the low one. That is not a specification quirk so
 * much as how the wires happened to be soldered, and it is the only thing that
 * distinguishes this board from a plain CNROM.
 */
class Mapper87 : public BankedMapper {
public:
	Mapper87(std::vector<std::uint8_t> prg, std::vector<std::uint8_t> chr,
			Mirroring mirroring, bool fourScreen = false);

	int number() const override { return 87; }

protected:
	std::size_t prgOffset(std::uint16_t address) const override;
	std::size_t chrOffset(std::uint16_t address) const override;
	void writeWorkRam(std::uint16_t address, std::uint8_t value) override;

private:
	std::uint16_t m_prgMask;
	int m_bank;
};

/**
 * Mapper 1, MMC1: four registers loaded one bit at a time.
 *
 * The board has five pins to spare, not thirteen, so the CPU writes a register
 * five times and the board shifts one bit in on each write. The fifth write
 * commits, and which of the four registers it commits to is decided by the
 * address of that last write. A write with bit 7 set resets the sequence -- the
 * standard way out of a partial write, and what a game does on startup.
 *
 * The board defends that protocol against the 6502's own quirk. A
 * read-modify-write instruction writes twice, the unmodified byte and then the
 * result, on consecutive cycles; MMC1 takes the first and ignores the second.
 * So `DEC $8000` shifts in a bit of the value that was *already there*, not of
 * the decremented one -- and a handful of games, Bill & Ted's among them, are
 * written expecting precisely that.
 */
class Mmc1Mapper : public BankedMapper {
public:
	Mmc1Mapper(std::vector<std::uint8_t> prg, std::vector<std::uint8_t> chr,
			Mirroring mirroring, bool fourScreen = false);

	int number() const override { return 1; }

	void beginInstruction() override {
		m_instructionsKnown = true;
		m_wroteThisInstruction = false;
	}

protected:
	std::size_t prgOffset(std::uint16_t address) const override;
	std::size_t chrOffset(std::uint16_t address) const override;
	void writeRegister(std::uint16_t address, std::uint8_t value) override;

private:
	void commit(std::uint16_t address, std::uint8_t value);
	void applyMirroring();

	std::uint8_t m_shift;     // holds a walking 1 that marks the fifth write

	// The rule is really about cycles, and the mapper cannot see cycles. It can
	// see instruction boundaries, if whoever drives it reports them -- and one
	// instruction writing twice is the only way two writes end up adjacent.
	// Until somebody reports a boundary there is nothing to reason from, so
	// every write is taken at face value rather than guessed at.
	bool m_instructionsKnown;
	bool m_wroteThisInstruction;

	std::uint8_t m_control;
	std::uint8_t m_chrBank0;
	std::uint8_t m_chrBank1;
	std::uint8_t m_prgBank;
};

/**
 * Mapper 4, MMC3: eight bank registers and a scanline counter.
 *
 * Banking is finer than anything before it -- 8 KB of PRG and 1 KB of CHR at a
 * time -- but the counter is what made the board matter. It decrements once per
 * scanline and raises an IRQ when it hits zero, so a game can split the screen
 * at an exact line without burning the CPU polling sprite-zero. Status bars,
 * parallax layers and windowed views all come from it.
 */
class Mmc3Mapper : public BankedMapper {
public:
	Mmc3Mapper(std::vector<std::uint8_t> prg, std::vector<std::uint8_t> chr,
			Mirroring mirroring, bool fourScreen = false);

	int number() const override { return 4; }

	void ppuA12Rise() override;
	bool irqAsserted() const override { return m_irqPending; }

protected:
	std::size_t prgOffset(std::uint16_t address) const override;
	std::size_t chrOffset(std::uint16_t address) const override;
	void writeRegister(std::uint16_t address, std::uint8_t value) override;

private:
	std::uint8_t m_bankSelect;
	std::uint8_t m_banks[8];
	bool m_prgMode;
	bool m_chrInvert;

	std::uint8_t m_irqLatch;
	std::uint8_t m_irqCounter;
	bool m_irqReload;
	bool m_irqEnabled;
	bool m_irqPending;
};

} // namespace nes

#endif // NES_MAPPER_H
