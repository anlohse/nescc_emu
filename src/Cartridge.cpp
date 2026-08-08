#include "nes/Cartridge.h"

#include <fstream>
#include <sstream>

namespace nes {

namespace {

const std::size_t HEADER_SIZE  = 16;
const std::size_t TRAINER_SIZE = 512;
const std::size_t PRG_UNIT     = 16 * 1024;
const std::size_t CHR_UNIT     = 8 * 1024;

// Header byte 6
const std::uint8_t F6_MIRROR_VERTICAL = 0x01;
const std::uint8_t F6_BATTERY         = 0x02;
const std::uint8_t F6_TRAINER         = 0x04;
const std::uint8_t F6_FOUR_SCREEN     = 0x08;

void fail(std::string* error, const std::string& message) {
	if (error)
		*error = message;
}

} // namespace

std::unique_ptr<Cartridge> Cartridge::fromINes(const std::vector<std::uint8_t>& image,
		std::string* error) {

	if (image.size() < HEADER_SIZE) {
		fail(error, "not an iNES image: shorter than a 16-byte header");
		return nullptr;
	}
	if (!(image[0] == 'N' && image[1] == 'E' && image[2] == 'S' && image[3] == 0x1A)) {
		fail(error, "not an iNES image: bad magic, expected \"NES\\x1A\"");
		return nullptr;
	}

	const std::uint8_t flags6 = image[6];
	const std::uint8_t flags7 = image[7];

	// NES 2.0 sets bits 3-2 of byte 7 to binary 10. We read such images as
	// iNES 1.0, which is correct for the fields we use.
	const bool isNes20 = (flags7 & 0x0C) == 0x08;

	const std::size_t prgSize = static_cast<std::size_t>(image[4]) * PRG_UNIT;
	const std::size_t chrSize = static_cast<std::size_t>(image[5]) * CHR_UNIT;

	if (prgSize == 0) {
		fail(error, "iNES header declares zero PRG ROM banks");
		return nullptr;
	}

	std::size_t offset = HEADER_SIZE;
	if (flags6 & F6_TRAINER)
		offset += TRAINER_SIZE; // 512-byte trainer sits between header and PRG

	if (image.size() < offset + prgSize + chrSize) {
		std::ostringstream os;
		os << "iNES image truncated: header declares " << (prgSize + chrSize)
		   << " bytes of ROM after offset " << offset << ", file holds "
		   << (image.size() > offset ? image.size() - offset : 0);
		fail(error, os.str());
		return nullptr;
	}

	const int mapperNumber = ((flags7 & 0xF0) | (flags6 >> 4));

	Mirroring mirroring = (flags6 & F6_MIRROR_VERTICAL)
			? Mirroring::Vertical : Mirroring::Horizontal;
	if (flags6 & F6_FOUR_SCREEN)
		mirroring = Mirroring::FourScreen;

	std::vector<std::uint8_t> prg(image.begin() + offset, image.begin() + offset + prgSize);
	std::vector<std::uint8_t> chr(image.begin() + offset + prgSize,
			image.begin() + offset + prgSize + chrSize);

	std::unique_ptr<Mapper> mapper;
	switch (mapperNumber) {
	case 0:
		mapper.reset(new NromMapper(std::move(prg), std::move(chr), mirroring));
		break;
	default: {
		std::ostringstream os;
		os << "unsupported mapper " << mapperNumber << " (only 0/NROM is implemented)";
		fail(error, os.str());
		return nullptr;
	}
	}

	std::unique_ptr<Cartridge> cart(new Cartridge());
	cart->m_mapper = std::move(mapper);
	cart->m_prgSize = prgSize;
	cart->m_chrSize = chrSize;
	cart->m_hasBattery = (flags6 & F6_BATTERY) != 0;
	cart->m_isNes20 = isNes20;
	return cart;
}

std::unique_ptr<Cartridge> Cartridge::fromFile(const std::string& path, std::string* error) {
	std::ifstream is(path.c_str(), std::ifstream::binary);
	if (!is) {
		fail(error, "cannot open " + path);
		return nullptr;
	}
	std::vector<std::uint8_t> image((std::istreambuf_iterator<char>(is)),
			std::istreambuf_iterator<char>());
	if (image.empty()) {
		fail(error, "empty file: " + path);
		return nullptr;
	}
	return fromINes(image, error);
}

} // namespace nes
