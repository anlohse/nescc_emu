#ifndef NES_TEST_ROM_H
#define NES_TEST_ROM_H

// Builds synthetic iNES images so the loader and bus can be tested without
// shipping anyone else's ROM.

#include <cstdint>
#include <vector>

namespace testrom {

struct Options {
	int prgBanks = 1;              // 16 KB units
	int chrBanks = 1;              // 8 KB units; 0 means the board has CHR RAM
	int mapper = 0;
	bool verticalMirroring = false;
	bool fourScreen = false;
	bool battery = false;
	bool trainer = false;
	bool nes20 = false;
	bool truncate = false;         // declare more ROM than the file actually holds
};

/**
 * @param prg  contents placed at the start of program ROM; the rest is zeroed.
 *             Program ROM is prgBanks * 16 KB, so index $3FFC of a one-bank
 *             image is the low byte of the reset vector.
 */
inline std::vector<std::uint8_t> build(const Options& o,
		const std::vector<std::uint8_t>& prg = std::vector<std::uint8_t>(),
		const std::vector<std::uint8_t>& chr = std::vector<std::uint8_t>()) {

	std::vector<std::uint8_t> image;
	image.reserve(16 + 512 + o.prgBanks * 16384 + o.chrBanks * 8192);

	std::uint8_t flags6 = static_cast<std::uint8_t>((o.mapper & 0x0F) << 4);
	if (o.verticalMirroring) flags6 |= 0x01;
	if (o.battery)           flags6 |= 0x02;
	if (o.trainer)           flags6 |= 0x04;
	if (o.fourScreen)        flags6 |= 0x08;

	std::uint8_t flags7 = static_cast<std::uint8_t>(o.mapper & 0xF0);
	if (o.nes20) flags7 |= 0x08;

	image.push_back('N');
	image.push_back('E');
	image.push_back('S');
	image.push_back(0x1A);
	image.push_back(static_cast<std::uint8_t>(o.prgBanks));
	image.push_back(static_cast<std::uint8_t>(o.chrBanks));
	image.push_back(flags6);
	image.push_back(flags7);
	for (int i = 8; i < 16; i++)
		image.push_back(0);

	if (o.trainer)
		image.insert(image.end(), 512, 0xAA);

	if (o.truncate)
		return image;   // header promises ROM that is not there

	std::vector<std::uint8_t> prgData(o.prgBanks * 16384, 0);
	for (std::size_t i = 0; i < prg.size() && i < prgData.size(); i++)
		prgData[i] = prg[i];
	image.insert(image.end(), prgData.begin(), prgData.end());

	std::vector<std::uint8_t> chrData(o.chrBanks * 8192, 0);
	for (std::size_t i = 0; i < chr.size() && i < chrData.size(); i++)
		chrData[i] = chr[i];
	image.insert(image.end(), chrData.begin(), chrData.end());

	return image;
}

/** Place a reset vector into the last two bytes of a one-bank PRG buffer. */
inline void setResetVector(std::vector<std::uint8_t>& prg16k, std::uint16_t pc) {
	prg16k.resize(16384, 0);
	prg16k[0x3FFC] = static_cast<std::uint8_t>(pc & 0xFF);
	prg16k[0x3FFD] = static_cast<std::uint8_t>(pc >> 8);
}

} // namespace testrom

#endif // NES_TEST_ROM_H
