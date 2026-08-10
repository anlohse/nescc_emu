#include "nes/Ppu.h"

namespace nes {

namespace {
/*
 * How far past the vblank flag going up a $2002 read still costs the interrupt.
 *
 * Reading on the same dot returns the flag clear and loses the interrupt;
 * reading one or two dots later returns it set and still loses the interrupt,
 * because the /NMI line went down and came back up before the CPU sampled it.
 * Three dots on, nothing is unusual any more.
 *
 * The number is blargg's, not a guess: 06-suppression prints one row per dot
 * and asks for exactly one row where the flag is lost and three where the
 * interrupt is.
 */
const int VBLANK_RACE_DOTS = 3;

// How long PPU address line A12 must sit low before a rise counts. The MMC3
// filters that line, and the filter works out at roughly three CPU cycles --
// nine dots. It is what separates the once-a-line edge a game wants to count
// from the every-eight-dots chatter of ordinary background fetches.
const int A12_FILTER_DOTS = 9;

/** True during the four dots of an eight-dot fetch group that read pattern data. */
bool patternPhase(int offsetWithinGroup) {
	return (offsetWithinGroup & 4) != 0;
}
} // namespace

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
	setRegion(Region::Ntsc);
	reset();
}

void Ppu::setRegion(Region region) {
	m_region = region;
	m_scanlinesPerFrame = (region == Region::Pal)
			? PAL_SCANLINES_PER_FRAME : SCANLINES_PER_FRAME;
	m_preRenderScanline = (region == Region::Pal)
			? PAL_PRE_RENDER_SCANLINE : PRE_RENDER_SCANLINE;
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
	m_nmiDelay = 0;
	m_sprite0HitDot = -1;
	m_dotsSinceVblank = VBLANK_RACE_DOTS;
	m_dotsSinceVblankEnd = VBLANK_RACE_DOTS;
	m_vblankRaces = 0;
	m_a12 = false;
	// Long enough that the first rise after a reset counts, which is what a
	// board sitting idle with nothing driving the line would see.
	m_a12LowDots = A12_FILTER_DOTS;
	m_spriteFetchA12.fill(false);
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
	if (m_region == Region::Ntsc && m_scanline == m_preRenderScanline
			&& m_dot == DOTS_PER_SCANLINE - 1
			&& (m_frame & 1) && renderingEnabled()) {
		// With rendering on, odd frames drop the last dot of the pre-render
		// line. That one-dot difference keeps the NTSC colour phase aligned --
		// and PAL, whose colour carrier does not divide the same way, has no
		// such skip.
		m_dot = 0;
		m_scanline = 0;
		m_frame++;
	} else if (m_dot >= DOTS_PER_SCANLINE) {
		m_dot = 0;
		m_scanline++;
		if (m_scanline >= m_scanlinesPerFrame) {
			m_scanline = 0;
			m_frame++;
		}
	}

	const bool visible = m_scanline < SCREEN_HEIGHT;
	const bool preRender = m_scanline == m_preRenderScanline;

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
		else if (m_dot == 257) {
			copyHorizontalBits();
			// The fetches from here to dot 320 are for the *next* line, so the
			// tables they read from have to be worked out from that line's
			// sprites rather than the one just drawn.
			scanSpriteFetches(m_scanline + 1);
		} else if (preRender && m_dot >= 280 && m_dot <= 304)
			copyVerticalBits();
	}

	// After the rendering work, so a fetch and the address it puts on the bus
	// belong to the same dot.
	updateA12();

	if (m_dotsSinceVblank < VBLANK_RACE_DOTS)
		m_dotsSinceVblank++;
	if (m_dotsSinceVblankEnd < VBLANK_RACE_DOTS)
		m_dotsSinceVblankEnd++;

	if (m_dot != 1)
		return;

	if (m_scanline == VBLANK_SCANLINE) {
		m_dotsSinceVblank = 0;
		m_status |= STATUS_VBLANK;
		if (m_ctrl & CTRL_NMI_ENABLE)
			m_nmiPending = true;
	} else if (preRender) {
		m_dotsSinceVblankEnd = 0;
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
/* A12                                                                        */
/* ------------------------------------------------------------------------- */

void Ppu::scanSpriteFetches(int line) {
	// Eight fetches happen whether or not there are eight sprites: the unused
	// slots re-fetch tile $FF, which in 8x16 mode lives in the upper table.
	// That is not a detail worth arguing with -- it is why a mostly empty line
	// still toggles A12 the way a full one does.
	const bool tall = (m_ctrl & CTRL_SPRITE_SIZE_16) != 0;
	const bool table = (m_ctrl & CTRL_SPRITE_PATTERN) != 0;
	m_spriteFetchA12.fill(tall ? true : table);
	if (!tall || line < 0)
		return;

	const int height = 16;
	int found = 0;
	for (int i = 0; i < 64 && found < 8; i++) {
		const int spriteY = m_oam[i * 4 + 0];
		const int row = line - spriteY - 1;
		if (row < 0 || row >= height)
			continue;
		// In 8x16 the tile's low bit chooses the table, and the rest of the
		// index picks the pair. So which half of CHR a sprite fetch lands in is
		// per sprite rather than per frame.
		m_spriteFetchA12[found++] = (m_oam[i * 4 + 1] & 1) != 0;
	}
}

bool Ppu::a12Level() const {
	const bool visible = m_scanline < SCREEN_HEIGHT;
	const bool preRender = m_scanline == m_preRenderScanline;
	if (!renderingEnabled() || !(visible || preRender)) {
		// Nothing is fetching, so the line carries whatever address the CPU put
		// there: a $2006 write, or the auto-increment after $2007. Games drive
		// the counter this way on purpose when the screen is off.
		return (m_vramAddr & 0x1000) != 0;
	}

	const bool bgTable = (m_ctrl & CTRL_BG_PATTERN) != 0;
	if (m_dot >= 1 && m_dot <= 256)
		return bgTable && patternPhase((m_dot - 1) & 7);
	if (m_dot >= 257 && m_dot <= 320) {
		const int slot = (m_dot - 257) / 8;
		return m_spriteFetchA12[slot & 7] && patternPhase((m_dot - 257) & 7);
	}
	if (m_dot >= 321 && m_dot <= 336)   // the next line's first two tiles
		return bgTable && patternPhase((m_dot - 321) & 7);

	// Dot 0 is idle, and 337-340 are two more nametable fetches: A12 low.
	return false;
}

void Ppu::updateA12() {
	const bool level = a12Level();
	if (level) {
		if (!m_a12 && m_a12LowDots >= A12_FILTER_DOTS && m_cartridge)
			m_cartridge->ppuA12Rise();
		m_a12LowDots = 0;
	} else if (m_a12LowDots < A12_FILTER_DOTS) {
		m_a12LowDots++;
	}
	m_a12 = level;
}

/* ------------------------------------------------------------------------- */
/* Rendering                                                                  */
/* ------------------------------------------------------------------------- */

bool Ppu::evaluateSpriteOverflow(int line, int height) const {
	// $2002 bit 5, and it is not "were there more than eight sprites". The
	// hardware walks OAM with two indices -- n for the sprite, m for the byte
	// within it -- and once eight sprites are in range it stops incrementing them
	// as a pair. From there n and m both advance, so the byte it reads as a Y
	// coordinate is the next sprite's *tile number*, then an *attribute*, then an
	// *X position*, then a Y again. It is comparing the wrong bytes against the
	// scanline, and whatever they happen to contain decides the flag.
	//
	// That misalignment is the whole of the famous overflow bug, and it cuts both
	// ways: a line with nine sprites can leave the flag clear, and a line with
	// three can set it. Counting instead gets the common case right and every
	// interesting case wrong, which is exactly the shape of blargg's results --
	// 5.Emulator passed, because it tests what an emulator gets right by accident,
	// while Basics, Details, Timing and Obscure all failed.
	int n = 0;                 // which sprite
	int m = 0;                 // which byte of it, and the source of the trouble
	int found = 0;

	while (n < 64) {
		// Sprite data is delayed a scanline, so OAM holds top - 1. Same
		// convention as the renderer above, deliberately: these two have to agree
		// about what "in range" means or the flag contradicts the picture.
		const int row = line - m_oam[n * 4 + m] - 1;
		const bool inRange = (row >= 0 && row < height);

		if (found < 8) {
			// Still filling the eight slots. m is zero throughout this phase --
			// copying a sprite walks m through 1, 2, 3 and back to 0 -- so this
			// really is reading a Y coordinate.
			if (inRange)
				found++;
			n++;
			continue;
		}

		// Eight found. Now the reads go wrong.
		if (inRange)
			return true;
		n++;
		m = (m + 1) & 3;       // and no carry into n, which is the bug
	}
	return false;
}

void Ppu::renderScanline(int line, int fromX) {
	std::uint8_t* row = m_framebuffer.data() + line * SCREEN_WIDTH;
	const bool wholeLine = fromX <= 0;
	if (fromX < 0)
		fromX = 0;
	if (fromX >= SCREEN_WIDTH)
		return;
	if (wholeLine)
		m_sprite0HitDot = -1;

	// Two-bit pattern value per pixel; 0 means transparent / backdrop.
	std::uint8_t bgPattern[SCREEN_WIDTH] = { 0 };
	std::uint8_t bgAttribute[SCREEN_WIDTH] = { 0 };

	if (m_mask & MASK_SHOW_BACKGROUND) {
		const std::uint16_t patternBase = (m_ctrl & CTRL_BG_PATTERN) ? 0x1000 : 0x0000;
		const int fineY = (m_vramAddr >> 12) & 7;

		for (int x = fromX; x < SCREEN_WIDTH; x++) {
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
			for (int x = fromX; x < 8; x++)
				bgPattern[x] = 0;
	}

	/* -- see evaluateSpriteOverflow below for why the flag is not a count -- */

	// Sprites are evaluated once per line and do not move partway across it:
	// hardware picks them during the previous line and latches their patterns,
	// so a redraw of the tail reuses what was found at the start.
	std::uint8_t* sprPattern = m_sprPattern.data();
	std::uint8_t* sprAttribute = m_sprAttribute.data();
	bool* sprBehind = m_sprBehind.data();
	bool* sprIsZero = m_sprIsZero.data();

	if (wholeLine) {
		m_sprPattern.fill(0);
		m_sprAttribute.fill(0);
		m_sprBehind.fill(false);
		m_sprIsZero.fill(false);
	}

	// The overflow flag is the hardware's own evaluation and not a count, so it is
	// worked out separately -- and it happens whenever *rendering* is on, not only
	// when sprites are being shown, because the evaluation is what runs and it
	// does not consult the sprite-enable bit.
	if (wholeLine && renderingEnabled()) {
		const int height = (m_ctrl & CTRL_SPRITE_SIZE_16) ? 16 : 8;
		if (evaluateSpriteOverflow(line, height))
			m_status |= STATUS_OVERFLOW;
	}

	if (wholeLine && (m_mask & MASK_SHOW_SPRITES)) {
		const int height = (m_ctrl & CTRL_SPRITE_SIZE_16) ? 16 : 8;
		int found = 0;

		for (int i = 0; i < 64; i++) {
			const std::uint8_t spriteY = m_oam[i * 4 + 0];
			// Sprite data is delayed one scanline, so OAM holds top - 1.
			const int rowInSprite = line - spriteY - 1;
			if (rowInSprite < 0 || rowInSprite >= height)
				continue;

			// Only the first eight are drawn. The ninth and beyond are dropped
			// here; whether the *flag* is set is a different question, answered
			// above by the evaluation rather than by this count.
			if (++found > 8)
				break;

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

	for (int x = fromX; x < SCREEN_WIDTH; x++) {
		const bool bgOpaque = bgPattern[x] != 0;
		const bool sprOpaque = sprPattern[x] != 0;

		// Sprite zero overlapping opaque background is how games find the split
		// point for a status bar. It is never reported on the last pixel.
		//
		// Only looked for on the first pass. A redraw of the tail happens
		// because the game already acted on the hit, and re-reporting it from
		// the redrawn pixels would feed the game's own response back to it.
		if (wholeLine && sprOpaque && bgOpaque && sprIsZero[x] && x != 255
				&& m_sprite0HitDot < 0)
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

void Ppu::redrawRestOfLine() {
	if (m_scanline >= SCREEN_HEIGHT)
		return;
	// Pixel x is produced at dot x + 1, so at dot D the pixels from D - 1 on
	// have not been drawn yet and are the ones this write still governs.
	if (m_dot < 2 || m_dot > SCREEN_WIDTH)
		return;

	renderScanline(m_scanline, m_dot - 1);
}

bool Ppu::lightAt(int x, int y) const {
	if (x < 0 || y < 0 || x >= SCREEN_WIDTH || y >= SCREEN_HEIGHT)
		return false;

	const std::uint32_t rgb =
			nesPaletteRgb()[m_framebuffer[y * SCREEN_WIDTH + x] & 0x3F];
	const int r = (rgb >> 16) & 0xFF;
	const int g = (rgb >> 8) & 0xFF;
	const int b = rgb & 0xFF;

	// Rec. 601 luma, the weighting the composite signal itself carries, and so
	// what a sensor watching that signal responds to.
	const int luma = (r * 299 + g * 587 + b * 114) / 1000;

	// Near-white only. The threshold has to clear every bright colour a game
	// puts on screen as scenery, and those go higher than they look: Duck
	// Hunt's foliage green is luma 172 and its lightest grey is 171. A game
	// blanks the screen and asks whether the gun sees anything before it
	// believes a shot at all, so a background counted as light does not merely
	// score wrongly -- it makes every shot fail that check and be discarded.
	//
	// What a game does strobe at a target is $30 or $20, both pure white at
	// 255, which leaves plenty of room above the scenery.
	return luma >= 200;
}

bool Ppu::takeNmi() {
	if (!m_nmiPending)
		return false;
	if (m_nmiDelay > 0) {
		// Raised too late in an instruction for the CPU to have sampled the
		// line during it, so the interrupt waits for the one after. Hardware
		// samples /NMI partway through each cycle; a write that pulls it down
		// in its own final cycle misses that sample by a hair.
		m_nmiDelay--;
		return false;
	}
	m_nmiPending = false;
	return true;
}

/* ------------------------------------------------------------------------- */
/* Registers                                                                  */
/* ------------------------------------------------------------------------- */

std::uint8_t Ppu::readRegister(std::uint16_t reg) {
	switch (reg & 7) {
	case 2: {
		// Only the top three bits come from the status register; the rest are
		// whatever was last on the PPU data bus.
		std::uint8_t status = m_status;

		// The race at the top of vblank. Three dots decide it, and the CPU is
		// on the losing side of all three: whatever the flag reads back as, the
		// interrupt does not happen. Games that poll $2002 for vblank instead
		// of using the NMI are unaffected, which is why so few notice.
		//
		// Which three dots, and what each does, is measured rather than
		// reasoned: blargg's 02-vbl_set_time and 06-suppression print a row per
		// dot, and the shape they demand is one dot where the flag is lost and
		// three where the interrupt is. A read one dot *before* the flag is due
		// is entirely ordinary -- an earlier version of this cancelled that one
		// too, which cost the row those tests print first.
		// The other end of vblank has its own one-dot asymmetry, and it is a
		// readback rather than a state change: the flag is down, but a read
		// landing on the very dot it went down still comes away with it set.
		// Only what this read returns is affected -- the flag really is gone,
		// which is why enabling NMI on that dot raises nothing (07-nmi_on_timing
		// checks that, and moving the clear itself instead of the readback is
		// what broke it).
		if (m_dotsSinceVblankEnd == 0)
			status |= STATUS_VBLANK;

		if (m_dotsSinceVblank == 0) {
			// The same dot. The read and the flag collide, and the read comes
			// away with nothing -- bit 7 clear, no interrupt, and the flag is
			// gone for the frame.
			status &= static_cast<std::uint8_t>(~STATUS_VBLANK);
			m_nmiPending = false;
			m_vblankRaces++;
		} else if (m_dotsSinceVblank < VBLANK_RACE_DOTS) {
			// A dot or two late: the flag is genuinely up and reads back set,
			// but clearing it here pulls /NMI up again before the CPU sampled
			// it. The interrupt is lost and the read looks perfectly normal,
			// which is what makes this worth modelling at all.
			m_nmiPending = false;
			m_vblankRaces++;
		}

		const std::uint8_t value =
				static_cast<std::uint8_t>((status & 0xE0) | (m_openBus & 0x1F));
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
		const bool patternMoved = ((value ^ m_ctrl) & CTRL_BG_PATTERN) != 0;
		m_ctrl = value;
		// Only the background pattern-table bit reaches the line being drawn.
		// The nametable select goes to t, which the line has already read.
		if (patternMoved)
			redrawRestOfLine();
		// Enabling NMI while the vblank flag is already up fires one
		// immediately. Some games rely on this to start their frame loop.
		//
		// "Immediately" means after the instruction *following* this one. The
		// write lands in this instruction's last cycle, which is past the point
		// where the CPU samples /NMI, so the interrupt cannot be taken until
		// the end of the next one.
		if (!wasEnabled && (value & CTRL_NMI_ENABLE) && (m_status & STATUS_VBLANK)) {
			m_nmiPending = true;
			m_nmiDelay = 1;
		}
		m_tempAddr = static_cast<std::uint16_t>((m_tempAddr & 0xF3FF)
				| ((value & 0x03) << 10));   // nametable select -> t
		break;
	}
	case 1: {
		// Every bit of this register lands on the pixels being drawn right now:
		// what is shown, what is masked off at the left edge, greyscale and the
		// colour emphasis.
		const bool changed = value != m_mask;
		m_mask = value;
		if (changed)
			redrawRestOfLine();
		break;
	}
	case 3:
		m_oamAddr = value;
		break;
	case 4:
		m_oam[m_oamAddr++] = value;   // writing advances the address
		break;
	case 5:
		if (!m_writeToggle) {
			// Fine X is the one part of a $2005 write the current line sees --
			// it feeds the pixel multiplexer directly. Coarse X goes to t and
			// only reaches v at the end of the line.
			const bool fineXMoved = (value & 0x07) != m_fineX;
			m_fineX = value & 0x07;
			m_tempAddr = static_cast<std::uint16_t>((m_tempAddr & 0xFFE0) | (value >> 3));
			if (fineXMoved)
				redrawRestOfLine();
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
			// The second write drops straight into v, so the rest of the line
			// is fetched from somewhere else entirely.
			const bool moved = m_vramAddr != m_tempAddr;
			m_vramAddr = m_tempAddr;
			if (moved)
				redrawRestOfLine();
		}
		m_writeToggle = !m_writeToggle;
		break;
	case 7:
		// A trace of where writes actually land, which is how the one that looked
		// like a lost character in 4.Obscure was found to be landing at $30C0 --
		// rendering having moved v out from under the CPU. Read once, because this
		// is a hot path: a getenv per write is not free.
		//
		// NES_TRACE_VRAM=20C0 watches 32 bytes from there; =ALL watches letters.
		{
			static const char* const want = std::getenv("NES_TRACE_VRAM");
			if (want) {
				const std::uint16_t at =
						static_cast<std::uint16_t>(m_vramAddr & 0x3FFF);
				static const bool all = (want[0] == 'A');
				static const unsigned low = all ? 0u
						: static_cast<unsigned>(std::strtoul(want, nullptr, 16));
				if (all ? (value >= 0x41 && value <= 0x5A)
						: (at >= low && at < low + 0x20))
					std::fprintf(stderr, "$2007 -> %04X = %02X  line %3d dot %3d"
							"  rendering %d inc %d\n",
							at, value, m_scanline, m_dot,
							renderingEnabled() ? 1 : 0,
							(m_ctrl & CTRL_INCREMENT_32) ? 32 : 1);
			}
		}
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
	case Mirroring::SingleScreenA:
		// All four nametables land on the same physical kilobyte.
		return static_cast<std::uint16_t>(a & 0x03FF);
	case Mirroring::SingleScreenB:
		return static_cast<std::uint16_t>(0x0400 | (a & 0x03FF));
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

/* ------------------------------------------------------------------------- */
/* Save states                                                                */
/* ------------------------------------------------------------------------- */

void Ppu::serialize(State& state) {
	state.tag("PPU ");

	state.value(m_ctrl);
	state.value(m_mask);
	state.value(m_status);
	state.value(m_oamAddr);
	state.value(m_vramAddr);
	state.value(m_tempAddr);
	state.value(m_fineX);
	state.value(m_writeToggle);
	state.value(m_readBuffer);
	state.value(m_openBus);

	// Where in the frame the beam is. Without this a state restored mid-picture
	// resumes at the top and the game's own split lands somewhere else.
	state.value(m_scanline);
	state.value(m_dot);
	state.value(m_frame);

	state.value(m_nmiPending);
	state.value(m_nmiDelay);
	state.value(m_sprite0HitDot);
	state.value(m_dotsSinceVblank);
	state.value(m_dotsSinceVblankEnd);
	state.value(m_vblankRaces);

	// A12's history, or an MMC3 counter resumes against an edge that never
	// happened.
	state.value(m_a12);
	state.value(m_a12LowDots);
	state.bytes(m_spriteFetchA12.data(), m_spriteFetchA12.size());

	state.bytes(m_vram.data(), m_vram.size());
	state.bytes(m_palette.data(), m_palette.size());
	state.bytes(m_oam.data(), m_oam.size());

	// The picture itself. It is derived rather than fundamental, and it is here
	// anyway: a state restored partway down a frame has a half-drawn screen, and
	// rebuilding the top half from nothing would show a seam for one frame.
	state.bytes(m_framebuffer.data(), m_framebuffer.size());
	state.bytes(m_sprPattern.data(), m_sprPattern.size());
}

} // namespace nes
