#include "nes/Ppu.h"

namespace nes {

/*
 * The console's fixed 64-colour palette, as 0x00RRGGBB.
 *
 * The NES generates colour as an analogue NTSC signal, so there is no single
 * correct RGB table -- this is one of the widely used approximations.
 */
const std::uint32_t* Ppu::nesPaletteRgb() {
	static const std::uint32_t table[64] = {
		0x626262, 0x001FB2, 0x2404C8, 0x5200B2, 0x730076, 0x800024, 0x730B00, 0x522800,
		0x244400, 0x005700, 0x005C00, 0x005324, 0x003C76, 0x000000, 0x000000, 0x000000,
		0xABABAB, 0x0D57FF, 0x4B30FF, 0x8A13FF, 0xBC08D6, 0xD21269, 0xC72E00, 0x9D5400,
		0x607B00, 0x209800, 0x00A300, 0x009942, 0x007DB4, 0x000000, 0x000000, 0x000000,
		0xFFFFFF, 0x53AEFF, 0x9085FF, 0xD365FF, 0xFF57FF, 0xFF5DCF, 0xFF7757, 0xFA9E00,
		0xBDC700, 0x7AE700, 0x43F611, 0x26EF7E, 0x2CD5F6, 0x4E4E4E, 0x000000, 0x000000,
		0xFFFFFF, 0xB6E1FF, 0xCED1FF, 0xE9C3FF, 0xFFBCFF, 0xFFBDF4, 0xFFC6C3, 0xFFD59A,
		0xE9E681, 0xCEF481, 0xB6FB9A, 0xA9FAC3, 0xA9F0F4, 0xB8B8B8, 0x000000, 0x000000
	};
	return table;
}

Ppu::Ppu(Cartridge* cartridge) : m_cartridge(cartridge) {
	m_vram.fill(0);
	m_palette.fill(0);
	m_oam.fill(0);
	m_framebuffer.fill(0);
	reset();
}

void Ppu::reset() {
	m_ctrl = 0;
	m_mask = 0;
	m_status = 0;
	m_oamAddr = 0;
	m_vramAddr = 0;
	m_tempAddr = 0;
	m_fineX = 0;
	m_writeToggle = false;
	m_readBuffer = 0;
	m_openBus = 0;
	m_scanline = 0;
	m_dot = 0;
	m_frame = 0;
	m_nmiPending = false;
	m_sprite0HitDot = -1;
}

/* ------------------------------------------------------------------------- */
/* Timing                                                                     */
/* ------------------------------------------------------------------------- */

void Ppu::tick(int dots) {
	for (int i = 0; i < dots; i++)
		tickOne();
}

void Ppu::tickOne() {
	// Advance first, then do the work belonging to the dot just reached. After
	// tick(n) the PPU is at dot n and everything scheduled for it has happened.
	m_dot++;
	if (m_scanline == PRE_RENDER_SCANLINE && m_dot == DOTS_PER_SCANLINE - 1
			&& (m_frame & 1) && renderingEnabled()) {
		// With rendering on, odd frames drop the last dot of the pre-render
		// line. That one-dot difference keeps the NTSC colour phase aligned.
		m_dot = 0;
		m_scanline = 0;
		m_frame++;
	} else if (m_dot >= DOTS_PER_SCANLINE) {
		m_dot = 0;
		m_scanline++;
		if (m_scanline >= SCANLINES_PER_FRAME) {
			m_scanline = 0;
			m_frame++;
		}
	}

	const bool visible = m_scanline < SCREEN_HEIGHT;
	const bool preRender = m_scanline == PRE_RENDER_SCANLINE;

	if (m_dot == 1 && visible) {
		// Draw the whole line from the scroll state in effect at its start.
		// Games change scroll during horizontal blank, between lines, so this
		// picks up per-line splits -- which is what raster effects need.
		//
		// Dot 1 rather than dot 0 because dot 0 is idle on hardware, and because
		// the counter starts there: rendering on dot 0 would skip the very first
		// scanline after a reset.
		renderScanline(m_scanline);
	}

	// Sprite-zero hit is reported partway through the line, at the dot where
	// the overlap actually occurs, because games poll for it and then rewrite
	// the scroll registers for the remainder of the frame.
	if (visible && m_sprite0HitDot >= 0 && m_dot >= m_sprite0HitDot + 1) {
		m_status |= STATUS_SPRITE0;
		m_sprite0HitDot = -1;
	}

	if (renderingEnabled() && (visible || preRender)) {
		if (m_dot == 256)
			incrementY();
		else if (m_dot == 257)
			copyHorizontalBits();
		else if (preRender && m_dot >= 280 && m_dot <= 304)
			copyVerticalBits();
	}

	if (m_dot != 1)
		return;

	if (m_scanline == VBLANK_SCANLINE) {
		m_status |= STATUS_VBLANK;
		if (m_ctrl & CTRL_NMI_ENABLE)
			m_nmiPending = true;
	} else if (preRender) {
		m_status &= static_cast<std::uint8_t>(
				~(STATUS_VBLANK | STATUS_SPRITE0 | STATUS_OVERFLOW));
	}
}

/* ------------------------------------------------------------------------- */
/* Scroll register plumbing                                                   */
/* ------------------------------------------------------------------------- */

void Ppu::incrementY() {
	if ((m_vramAddr & 0x7000) != 0x7000) {
		m_vramAddr = static_cast<std::uint16_t>(m_vramAddr + 0x1000);   // fine Y
		return;
	}
	m_vramAddr &= static_cast<std::uint16_t>(~0x7000);
	int coarseY = (m_vramAddr & 0x03E0) >> 5;
	if (coarseY == 29) {
		coarseY = 0;
		m_vramAddr ^= 0x0800;        // past the last row: flip to the next nametable
	} else if (coarseY == 31) {
		coarseY = 0;                 // in attribute space; wrap without switching
	} else {
		coarseY++;
	}
	m_vramAddr = static_cast<std::uint16_t>((m_vramAddr & ~0x03E0) | (coarseY << 5));
}

void Ppu::copyHorizontalBits() {
	// coarse X and the horizontal nametable bit come back from t every line.
	m_vramAddr = static_cast<std::uint16_t>((m_vramAddr & ~0x041F) | (m_tempAddr & 0x041F));
}

void Ppu::copyVerticalBits() {
	m_vramAddr = static_cast<std::uint16_t>((m_vramAddr & ~0x7BE0) | (m_tempAddr & 0x7BE0));
}

/* ------------------------------------------------------------------------- */
/* Rendering                                                                  */
/* ------------------------------------------------------------------------- */

void Ppu::renderScanline(int line) {
	std::uint8_t* row = m_framebuffer.data() + line * SCREEN_WIDTH;
	m_sprite0HitDot = -1;

	// Two-bit pattern value per pixel; 0 means transparent / backdrop.
	std::uint8_t bgPattern[SCREEN_WIDTH] = { 0 };
	std::uint8_t bgAttribute[SCREEN_WIDTH] = { 0 };

	if (m_mask & MASK_SHOW_BACKGROUND) {
		const std::uint16_t patternBase = (m_ctrl & CTRL_BG_PATTERN) ? 0x1000 : 0x0000;
		const int fineY = (m_vramAddr >> 12) & 7;

		for (int x = 0; x < SCREEN_WIDTH; x++) {
			const int bit = m_fineX + x;
			// Which tile along the row, and which of its 8 columns.
			int coarseX = (m_vramAddr & 0x1F) + (bit >> 3);
			std::uint16_t nametable = m_vramAddr & 0x0C00;
			if (coarseX >= 32) {
				coarseX -= 32;
				nametable ^= 0x0400;   // ran off the edge into the next nametable
			}

			const std::uint16_t ntAddr = static_cast<std::uint16_t>(
					0x2000 | nametable | (m_vramAddr & 0x03E0) | coarseX);
			const std::uint8_t tile = vramRead(ntAddr);

			// One attribute byte covers a 32x32 pixel block, two bits per 16x16 quadrant.
			const std::uint16_t atAddr = static_cast<std::uint16_t>(
					0x23C0 | nametable | ((m_vramAddr >> 4) & 0x38) | (coarseX >> 2));
			const int shift = ((m_vramAddr >> 4) & 4) | (coarseX & 2);
			const std::uint8_t attribute = (vramRead(atAddr) >> shift) & 3;

			const std::uint16_t patAddr = static_cast<std::uint16_t>(
					patternBase + tile * 16 + fineY);
			const std::uint8_t lo = vramRead(patAddr);
			const std::uint8_t hi = vramRead(static_cast<std::uint16_t>(patAddr + 8));
			const int b = 7 - (bit & 7);

			bgPattern[x] = static_cast<std::uint8_t>(
					((lo >> b) & 1) | (((hi >> b) & 1) << 1));
			bgAttribute[x] = attribute;
		}

		if (!(m_mask & MASK_SHOW_BG_LEFT))
			for (int x = 0; x < 8; x++)
				bgPattern[x] = 0;
	}

	// Sprites: hardware evaluates at most 8 per line, in OAM order.
	std::uint8_t sprPattern[SCREEN_WIDTH] = { 0 };
	std::uint8_t sprAttribute[SCREEN_WIDTH] = { 0 };
	bool sprBehind[SCREEN_WIDTH] = { false };
	bool sprIsZero[SCREEN_WIDTH] = { false };

	if (m_mask & MASK_SHOW_SPRITES) {
		const int height = (m_ctrl & CTRL_SPRITE_SIZE_16) ? 16 : 8;
		int found = 0;

		for (int i = 0; i < 64; i++) {
			const std::uint8_t spriteY = m_oam[i * 4 + 0];
			// Sprite data is delayed one scanline, so OAM holds top - 1.
			const int rowInSprite = line - spriteY - 1;
			if (rowInSprite < 0 || rowInSprite >= height)
				continue;

			if (++found > 8) {
				m_status |= STATUS_OVERFLOW;
				break;
			}

			const std::uint8_t tile = m_oam[i * 4 + 1];
			const std::uint8_t attr = m_oam[i * 4 + 2];
			const std::uint8_t spriteX = m_oam[i * 4 + 3];

			int fineRow = (attr & SPRITE_FLIP_Y) ? (height - 1 - rowInSprite) : rowInSprite;

			std::uint16_t patAddr;
			if (height == 16) {
				// 8x16 sprites pick their pattern table from the tile's low bit
				// and use the next tile for the bottom half.
				const std::uint16_t base = (tile & 1) ? 0x1000 : 0x0000;
				std::uint16_t index = static_cast<std::uint16_t>(tile & 0xFE);
				if (fineRow >= 8) {
					index++;
					fineRow -= 8;
				}
				patAddr = static_cast<std::uint16_t>(base + index * 16 + fineRow);
			} else {
				const std::uint16_t base = (m_ctrl & CTRL_SPRITE_PATTERN) ? 0x1000 : 0x0000;
				patAddr = static_cast<std::uint16_t>(base + tile * 16 + fineRow);
			}

			const std::uint8_t lo = vramRead(patAddr);
			const std::uint8_t hi = vramRead(static_cast<std::uint16_t>(patAddr + 8));

			for (int px = 0; px < 8; px++) {
				const int x = spriteX + px;
				if (x >= SCREEN_WIDTH)
					break;
				if (sprPattern[x])
					continue;   // an earlier sprite already owns this pixel

				const int b = (attr & SPRITE_FLIP_X) ? px : (7 - px);
				const std::uint8_t value = static_cast<std::uint8_t>(
						((lo >> b) & 1) | (((hi >> b) & 1) << 1));
				if (!value)
					continue;

				sprPattern[x] = value;
				sprAttribute[x] = attr & 3;
				sprBehind[x] = (attr & SPRITE_BEHIND_BG) != 0;
				sprIsZero[x] = (i == 0);
			}
		}

		if (!(m_mask & MASK_SHOW_SPRITE_LEFT))
			for (int x = 0; x < 8; x++)
				sprPattern[x] = 0;
	}

	for (int x = 0; x < SCREEN_WIDTH; x++) {
		const bool bgOpaque = bgPattern[x] != 0;
		const bool sprOpaque = sprPattern[x] != 0;

		// Sprite zero overlapping opaque background is how games find the split
		// point for a status bar. It is never reported on the last pixel.
		if (sprOpaque && bgOpaque && sprIsZero[x] && x != 255 && m_sprite0HitDot < 0)
			m_sprite0HitDot = x;

		std::uint8_t colour;
		if (sprOpaque && (!bgOpaque || !sprBehind[x]))
			colour = m_palette[mirrorPalette(
					static_cast<std::uint16_t>(0x10 + sprAttribute[x] * 4 + sprPattern[x]))];
		else if (bgOpaque)
			colour = m_palette[mirrorPalette(
					static_cast<std::uint16_t>(bgAttribute[x] * 4 + bgPattern[x]))];
		else
			colour = m_palette[0];   // backdrop

		row[x] = static_cast<std::uint8_t>(colour & 0x3F);
	}
}

bool Ppu::takeNmi() {
	const bool pending = m_nmiPending;
	m_nmiPending = false;
	return pending;
}

/* ------------------------------------------------------------------------- */
/* Registers                                                                  */
/* ------------------------------------------------------------------------- */

std::uint8_t Ppu::readRegister(std::uint16_t reg) {
	switch (reg & 7) {
	case 2: {
		// Only the top three bits come from the status register; the rest are
		// whatever was last on the PPU data bus.
		const std::uint8_t value =
				static_cast<std::uint8_t>((m_status & 0xE0) | (m_openBus & 0x1F));
		m_status &= static_cast<std::uint8_t>(~STATUS_VBLANK);
		m_writeToggle = false;   // $2002 resets the $2005/$2006 sequence
		m_openBus = value;
		return value;
	}
	case 4:
		m_openBus = m_oam[m_oamAddr];
		return m_openBus;
	case 7: {
		const std::uint16_t addr = m_vramAddr & 0x3FFF;
		std::uint8_t value;
		if (addr >= 0x3F00) {
			// Palette reads return immediately, but the buffer still picks up
			// the nametable byte mirrored underneath the palette.
			value = m_palette[mirrorPalette(addr)];
			m_readBuffer = m_vram[mirrorNametable(addr)];
		} else {
			value = m_readBuffer;   // one access behind
			m_readBuffer = vramRead(addr);
		}
		m_vramAddr = static_cast<std::uint16_t>(
				m_vramAddr + ((m_ctrl & CTRL_INCREMENT_32) ? 32 : 1));
		m_openBus = value;
		return value;
	}
	default:
		// $2000, $2001, $2003, $2005 and $2006 are write-only; reading them
		// returns whatever is still on the data bus.
		return m_openBus;
	}
}

void Ppu::writeRegister(std::uint16_t reg, std::uint8_t value) {
	m_openBus = value;
	switch (reg & 7) {
	case 0: {
		const bool wasEnabled = (m_ctrl & CTRL_NMI_ENABLE) != 0;
		m_ctrl = value;
		// Enabling NMI while the vblank flag is already up fires one
		// immediately. Some games rely on this to start their frame loop.
		if (!wasEnabled && (value & CTRL_NMI_ENABLE) && (m_status & STATUS_VBLANK))
			m_nmiPending = true;
		m_tempAddr = static_cast<std::uint16_t>((m_tempAddr & 0xF3FF)
				| ((value & 0x03) << 10));   // nametable select -> t
		break;
	}
	case 1:
		m_mask = value;
		break;
	case 3:
		m_oamAddr = value;
		break;
	case 4:
		m_oam[m_oamAddr++] = value;   // writing advances the address
		break;
	case 5:
		if (!m_writeToggle) {
			m_fineX = value & 0x07;
			m_tempAddr = static_cast<std::uint16_t>((m_tempAddr & 0xFFE0) | (value >> 3));
		} else {
			m_tempAddr = static_cast<std::uint16_t>((m_tempAddr & 0x8FFF)
					| ((value & 0x07) << 12));
			m_tempAddr = static_cast<std::uint16_t>((m_tempAddr & 0xFC1F)
					| ((value & 0xF8) << 2));
		}
		m_writeToggle = !m_writeToggle;
		break;
	case 6:
		if (!m_writeToggle) {
			m_tempAddr = static_cast<std::uint16_t>((m_tempAddr & 0x00FF)
					| ((value & 0x3F) << 8));   // high byte first, 14 bits total
		} else {
			m_tempAddr = static_cast<std::uint16_t>((m_tempAddr & 0xFF00) | value);
			m_vramAddr = m_tempAddr;
		}
		m_writeToggle = !m_writeToggle;
		break;
	case 7:
		vramWrite(static_cast<std::uint16_t>(m_vramAddr & 0x3FFF), value);
		m_vramAddr = static_cast<std::uint16_t>(
				m_vramAddr + ((m_ctrl & CTRL_INCREMENT_32) ? 32 : 1));
		break;
	default:
		break;   // $2002 is read-only
	}
}

std::uint8_t Ppu::peekRegister(std::uint16_t reg) const {
	switch (reg & 7) {
	case 0: return m_ctrl;
	case 1: return m_mask;
	case 2: return m_status;
	case 3: return m_oamAddr;
	case 4: return m_oam[m_oamAddr];
	case 7: return m_readBuffer;
	default: return m_openBus;
	}
}

/* ------------------------------------------------------------------------- */
/* PPU address space                                                          */
/* ------------------------------------------------------------------------- */

std::uint16_t Ppu::mirrorNametable(std::uint16_t address) const {
	const std::uint16_t a = address & 0x0FFF;
	const Mirroring m = m_cartridge ? m_cartridge->mirroring() : Mirroring::Horizontal;
	switch (m) {
	case Mirroring::Vertical:
		// NT0 NT1 NT0 NT1
		return static_cast<std::uint16_t>(a & 0x07FF);
	case Mirroring::Horizontal:
		// NT0 NT0 NT1 NT1
		return static_cast<std::uint16_t>(((a & 0x0800) >> 1) | (a & 0x03FF));
	case Mirroring::FourScreen:
		return a;
	}
	return a;
}

std::uint16_t Ppu::mirrorPalette(std::uint16_t address) {
	std::uint16_t index = address & 0x1F;
	// The sprite backdrop entries are mirrors of the background ones.
	if (index == 0x10 || index == 0x14 || index == 0x18 || index == 0x1C)
		index = static_cast<std::uint16_t>(index - 0x10);
	return index;
}

std::uint8_t Ppu::vramRead(std::uint16_t address) const {
	const std::uint16_t addr = address & 0x3FFF;
	if (addr < 0x2000)
		return m_cartridge ? m_cartridge->ppuRead(addr) : 0;   // pattern tables
	if (addr < 0x3F00)
		return m_vram[mirrorNametable(addr)];
	return m_palette[mirrorPalette(addr)];
}

void Ppu::vramWrite(std::uint16_t address, std::uint8_t value) {
	const std::uint16_t addr = address & 0x3FFF;
	if (addr < 0x2000) {
		if (m_cartridge)
			m_cartridge->ppuWrite(addr, value);   // only lands on CHR RAM boards
		return;
	}
	if (addr < 0x3F00) {
		m_vram[mirrorNametable(addr)] = value;
		return;
	}
	m_palette[mirrorPalette(addr)] = value & 0x3F;
}

} // namespace nes
