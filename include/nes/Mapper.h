#ifndef NES_MAPPER_H
#define NES_MAPPER_H

#include <cstdint>
#include <iosfwd>
#include <vector>

/**
 * How the PPU's nametable address space is folded onto the 2 KB of VRAM on the
 * console. Fixed by the cartridge wiring on simple boards, switchable by the
 * mapper on later ones.
 */
namespace nes {

enum class Mirroring {
	Horizontal,
	Vertical,
	FourScreen
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

	/** CPU bus read, $4020-$FFFF. Returns open-bus (0) for unmapped addresses. */
	virtual std::uint8_t cpuRead(std::uint16_t address) const = 0;
	/** CPU bus write, $4020-$FFFF. Bank switching happens here on most boards. */
	virtual void cpuWrite(std::uint16_t address, std::uint8_t value) = 0;

	/** PPU bus read, $0000-$1FFF. */
	virtual std::uint8_t ppuRead(std::uint16_t address) const = 0;
	/** PPU bus write, $0000-$1FFF. Only lands when the board has CHR RAM. */
	virtual void ppuWrite(std::uint16_t address, std::uint8_t value) = 0;

	virtual Mirroring mirroring() const = 0;

	/** iNES mapper number. */
	virtual int number() const = 0;
};

/**
 * Mapper 0, NROM: no banking at all.
 *
 * Program ROM is 16 KB or 32 KB mapped at $8000. A 16 KB image appears twice,
 * so the vectors at $FFFA-$FFFF read from the top of the single bank. Character
 * memory is an 8 KB ROM, or 8 KB of RAM when the image declares no CHR banks.
 * Some boards add 8 KB of work RAM at $6000.
 */
class NromMapper : public Mapper {
public:
	NromMapper(std::vector<std::uint8_t> prg, std::vector<std::uint8_t> chr, Mirroring mirroring);

	std::uint8_t cpuRead(std::uint16_t address) const override;
	void cpuWrite(std::uint16_t address, std::uint8_t value) override;
	std::uint8_t ppuRead(std::uint16_t address) const override;
	void ppuWrite(std::uint16_t address, std::uint8_t value) override;

	Mirroring mirroring() const override { return m_mirroring; }
	int number() const override { return 0; }

	bool hasChrRam() const { return m_chrIsRam; }

private:
	std::vector<std::uint8_t> m_prg;
	std::vector<std::uint8_t> m_chr;
	std::vector<std::uint8_t> m_prgRam;
	std::uint16_t m_prgMask;
	bool m_chrIsRam;
	Mirroring m_mirroring;
};

} // namespace nes

#endif // NES_MAPPER_H
