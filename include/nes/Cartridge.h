#ifndef NES_CARTRIDGE_H
#define NES_CARTRIDGE_H

#include "Mapper.h"
#include "Region.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nes {

/**
 * A loaded cartridge image.
 *
 * Parses the iNES container, splits out the program and character ROM, and
 * constructs the mapper that board uses. All bus traffic is forwarded to the
 * mapper; Cartridge itself only owns it and reports what the header said.
 *
 * iNES 1.0 is supported. NES 2.0 images are accepted and read as iNES 1.0,
 * which is correct for their common subset -- the extra fields describe
 * submappers, larger memories and timing, none of which matter yet.
 */
class Cartridge {
public:
	/**
	 * Parse an iNES image held in memory.
	 * @return null on failure, with a description in @p error when non-null.
	 */
	static std::unique_ptr<Cartridge> fromINes(const std::vector<std::uint8_t>& image,
			std::string* error = nullptr);

	/** Read a .nes file from disk and parse it. */
	static std::unique_ptr<Cartridge> fromFile(const std::string& path,
			std::string* error = nullptr);

	std::uint8_t cpuRead(std::uint16_t address) const { return m_mapper->cpuRead(address); }
	void cpuWrite(std::uint16_t address, std::uint8_t value) { m_mapper->cpuWrite(address, value); }
	std::uint8_t ppuRead(std::uint16_t address) const { return m_mapper->ppuRead(address); }
	void ppuWrite(std::uint16_t address, std::uint8_t value) { m_mapper->ppuWrite(address, value); }

	Mirroring mirroring() const { return m_mapper->mirroring(); }
	int mapperNumber() const { return m_mapper->number(); }
	Mapper& mapper() { return *m_mapper; }

	/** Tell the board a scanline's pattern fetches happened. Drives MMC3's IRQ. */
	void ppuScanline() { m_mapper->ppuScanline(); }
	/** True while the board is holding the CPU's IRQ line down. */
	bool irqAsserted() const { return m_mapper->irqAsserted(); }

	/** Program ROM size in bytes, as declared by the header. */
	std::size_t prgSize() const { return m_prgSize; }
	/** Character ROM size in bytes; 0 means the board carries CHR RAM instead. */
	std::size_t chrSize() const { return m_chrSize; }
	bool hasBattery() const { return m_hasBattery; }
	/** True when the image declared NES 2.0 in its header. */
	bool isNes20() const { return m_isNes20; }
	/** NTSC unless the header says otherwise. Drives all system timing. */
	Region region() const { return m_region; }

private:
	Cartridge() = default;

	std::unique_ptr<Mapper> m_mapper;
	std::size_t m_prgSize = 0;
	std::size_t m_chrSize = 0;
	bool m_hasBattery = false;
	bool m_isNes20 = false;
	Region m_region = Region::Ntsc;
};

} // namespace nes

#endif // NES_CARTRIDGE_H
