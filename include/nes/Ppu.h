#ifndef NES_PPU_H
#define NES_PPU_H

#include "Cartridge.h"
#include "State.h"

#include <array>
#include <cstdint>

namespace nes {

/**
 * The 2C02 picture processing unit -- timing and register file only.
 *
 * This does not draw anything yet. What it does provide is the part every NES
 * program depends on before a single pixel matters: a dot/scanline counter
 * running at three dots per CPU cycle, the vblank flag appearing and clearing
 * at the right moments, and the NMI that drives a game's main loop.
 *
 * NTSC frame geometry: 262 scanlines of 341 dots each.
 *
 *   0-239    visible
 *   240      post-render, idle
 *   241      vblank starts at dot 1; NMI fires here if enabled
 *   242-260  vblank continues
 *   261      pre-render; vblank, sprite-0 and overflow clear at dot 1
 *
 * The register-file behaviour is real, not stubbed: the shared write toggle
 * used by $2005/$2006, the buffered read on $2007, the VRAM auto-increment and
 * the nametable/palette mirroring all work, because game code misbehaves in
 * confusing ways when they do not.
 */
class Ppu {
public:
	// NTSC geometry, and the default. PAL keeps the same 341-dot line and the
	// same vblank scanline, but adds 50 lines of extra blanking at the bottom.
	static const int DOTS_PER_SCANLINE = 341;
	static const int SCANLINES_PER_FRAME = 262;
	static const int VBLANK_SCANLINE = 241;
	static const int PRE_RENDER_SCANLINE = 261;

	static const int PAL_SCANLINES_PER_FRAME = 312;
	static const int PAL_PRE_RENDER_SCANLINE = 311;

	static const int SCREEN_WIDTH = 256;
	static const int SCREEN_HEIGHT = 240;

	// $2000 PPUCTRL
	static const std::uint8_t CTRL_INCREMENT_32   = 0x04;
	static const std::uint8_t CTRL_SPRITE_PATTERN = 0x08;
	static const std::uint8_t CTRL_BG_PATTERN     = 0x10;
	static const std::uint8_t CTRL_SPRITE_SIZE_16 = 0x20;
	static const std::uint8_t CTRL_NMI_ENABLE     = 0x80;
	// $2001 PPUMASK
	static const std::uint8_t MASK_SHOW_BG_LEFT     = 0x02;
	static const std::uint8_t MASK_SHOW_SPRITE_LEFT = 0x04;
	static const std::uint8_t MASK_SHOW_BACKGROUND  = 0x08;
	static const std::uint8_t MASK_SHOW_SPRITES     = 0x10;
	// $2002 PPUSTATUS
	static const std::uint8_t STATUS_OVERFLOW = 0x20;
	static const std::uint8_t STATUS_SPRITE0  = 0x40;
	static const std::uint8_t STATUS_VBLANK   = 0x80;
	// Sprite attribute byte
	static const std::uint8_t SPRITE_BEHIND_BG = 0x20;
	static const std::uint8_t SPRITE_FLIP_X    = 0x40;
	static const std::uint8_t SPRITE_FLIP_Y    = 0x80;

	explicit Ppu(Cartridge* cartridge = nullptr);

	void setCartridge(Cartridge* cartridge) { m_cartridge = cartridge; }

	/** Switch the frame geometry. Takes effect at the next scanline rollover. */
	void setRegion(Region region);
	Region region() const { return m_region; }
	int scanlinesPerFrame() const { return m_scanlinesPerFrame; }
	int preRenderScanline() const { return m_preRenderScanline; }

	void reset();

	/** Advance by @p dots. The caller supplies three per CPU cycle. */
	void tick(int dots);

	/** CPU-visible register read, $2000-$2007 folded to 0-7. Has side effects. */
	std::uint8_t readRegister(std::uint16_t reg);
	void writeRegister(std::uint16_t reg, std::uint8_t value);
	/** Side-effect-free view of a register, for debuggers. */
	std::uint8_t peekRegister(std::uint16_t reg) const;

	/**
	 * Consume a pending NMI request.
	 * @return true once per triggering event; the caller asserts the CPU line.
	 */
	bool takeNmi();

	/** Write one byte into object attribute memory, used by OAM DMA. */
	void writeOam(std::uint8_t index, std::uint8_t value) { m_oam[index] = value; }
	std::uint8_t readOam(std::uint8_t index) const { return m_oam[index]; }
	std::uint8_t oamAddress() const { return m_oamAddr; }

	std::uint8_t control() const { return m_ctrl; }
	std::uint8_t mask() const { return m_mask; }

	int scanline() const { return m_scanline; }
	int dot() const { return m_dot; }
	std::uint64_t frame() const { return m_frame; }
	bool inVBlank() const { return (m_status & STATUS_VBLANK) != 0; }

	/**
	 * How many times a $2002 read has landed in the three dots at the top of
	 * vblank and cost the CPU that frame's interrupt.
	 *
	 * Worth being able to see. A game polling $2002 in a tight loop passes
	 * through that window regularly, so this is a count of real events, not of
	 * a theoretical edge case -- and if a game ever misbehaves, this is the
	 * first number to look at.
	 */
	unsigned long vblankRaces() const { return m_vblankRaces; }
	bool renderingEnabled() const {
		return (m_mask & (MASK_SHOW_BACKGROUND | MASK_SHOW_SPRITES)) != 0;
	}

	/** Read through the PPU address space, $0000-$3FFF. No side effects. */
	std::uint8_t vramRead(std::uint16_t address) const;
	void vramWrite(std::uint16_t address, std::uint8_t value);

	/**
	 * The completed picture: 256x240 NES colour indices, row-major.
	 *
	 * Entries are 0-63 indices into the console's fixed palette, not RGB --
	 * converting is the display layer's job. Use nesPaletteRgb() for a standard
	 * conversion table.
	 */
	const std::uint8_t* framebuffer() const { return m_framebuffer.data(); }

	/**
	 * Is the picture bright enough at (@p x, @p y) for a light gun to see it?
	 *
	 * Lives here because this is the only place that holds both the framebuffer
	 * and the palette it means anything in. A phototransistor answers to
	 * luminance, not to colour, so the palette entry is weighted the way an eye
	 * -- or a television's luma -- would weight it.
	 *
	 * The frame is sampled as it stands right now, part-drawn if the beam has
	 * not reached the bottom yet, because that is what the gun would be seeing.
	 * A game aims a white box at one target at a time and reads immediately, so
	 * an average over the finished frame would answer about the wrong thing.
	 */
	bool lightAt(int x, int y) const;

	/** 64 entries of packed 0x00RRGGBB for the NES's fixed colour palette. */
	static const std::uint32_t* nesPaletteRgb();

	/** Save or restore. @see nes/State.h */
	void serialize(State& state);

private:
	void tickOne();
	std::uint16_t mirrorNametable(std::uint16_t address) const;
	static std::uint16_t mirrorPalette(std::uint16_t address);

	/**
	 * Draw @p line from pixel @p fromX rightwards, using the state right now.
	 *
	 * Called once at the start of every visible line, and again from partway
	 * across whenever a register write changes something the rest of the line
	 * would be drawn differently for. The second call redraws only the tail, so
	 * a line nothing is written during comes out exactly as it always did.
	 *
	 * Sprites are evaluated on the first call only: hardware picks them during
	 * the previous line and latches their patterns, so they cannot change
	 * partway across.
	 */
	void renderScanline(int line, int fromX = 0);

	/** Redraw the rest of the current line, if a write just changed how it looks. */
	void redrawRestOfLine();
	void incrementY();
	void copyHorizontalBits();
	void copyVerticalBits();

	/**
	 * A12's level for the dot just reached.
	 *
	 * While rendering it comes from the fetch schedule, which is fixed: two
	 * dots of nametable, two of attribute, then four of pattern data, over and
	 * over. So A12 is high only during the pattern fetches, and only when that
	 * pattern lives in the upper table. Outside rendering nothing is fetching,
	 * and the line simply carries whatever address the CPU last left in v.
	 */
	bool a12Level() const;

	/**
	 * Watch A12 for a rising edge, and tell the cartridge about it.
	 *
	 * The edge only counts if the line has been low for a while first. That
	 * filter is the whole reason an MMC3 can count scanlines at all: with
	 * backgrounds in the upper table A12 rises every eight dots, far too often
	 * to mean anything, and the filter throws all of those away. What survives
	 * is the once-per-line transition between background and sprite fetches.
	 */
	void updateA12();

	/** Which table each of the next line's sprite fetches will read from. */
	void scanSpriteFetches(int line);

	Cartridge* m_cartridge;

	// 4 KB covers four-screen boards; the usual two-screen layouts fold into
	// the low half via mirrorNametable().
	std::array<std::uint8_t, 0x1000> m_vram;
	std::array<std::uint8_t, 32> m_palette;
	std::array<std::uint8_t, 256> m_oam;

	std::uint8_t m_ctrl;
	std::uint8_t m_mask;
	std::uint8_t m_status;
	std::uint8_t m_oamAddr;

	// $2005 and $2006 share one write toggle, which $2002 reads reset.
	std::uint16_t m_vramAddr;
	std::uint16_t m_tempAddr;
	std::uint8_t m_fineX;
	bool m_writeToggle;

	// $2007 reads of anything below the palette are delayed by one access.
	std::uint8_t m_readBuffer;
	// The PPU's data bus retains the last value placed on it; the low five bits
	// of a $2002 read come from here rather than from the status register.
	std::uint8_t m_openBus;

	Region m_region;
	int m_scanlinesPerFrame;
	int m_preRenderScanline;

	int m_scanline;
	int m_dot;
	std::uint64_t m_frame;
	bool m_nmiPending;

	// The race at the top of vblank. Reading $2002 as the flag goes up loses the
	// CPU that frame's interrupt, because the /NMI line is pulled down and let
	// go again before the CPU ever samples it. Three dots decide it, so both
	// sides have to be dated: how long ago the flag went up, and whether a read
	// landed on the dot just before it was due to.
	// How many instruction boundaries a raised NMI must wait through. Nonzero
	// only when the line was pulled down too late in an instruction for the CPU
	// to sample it there.
	int m_nmiDelay;

	int m_dotsSinceVblank;      // capped; large means "not near the boundary"
	int m_dotsSinceVblankEnd;   // the same, for the dot the flag goes back down
	unsigned long m_vblankRaces;

	// PPU address line A12, which is a real wire and the only thing an MMC3
	// counts. It goes high whenever the address being fetched is in $1000-$1FFF
	// -- pattern fetches from the upper table while rendering, or whatever the
	// CPU last left in v when it is not.
	bool m_a12;
	int m_a12LowDots;           // how long it has been low, for the filter
	// Which pattern table each of the next line's eight sprite fetches lands
	// in. Only interesting for 8x16 sprites, where it comes from each tile's
	// low bit rather than from $2000.
	std::array<bool, 8> m_spriteFetchA12;

	std::array<std::uint8_t, SCREEN_WIDTH * SCREEN_HEIGHT> m_framebuffer;

	// Sprite output for the line being drawn, kept so the tail of a line can be
	// redrawn without evaluating sprites again -- which would be wrong as well
	// as wasteful, since hardware latches them before the line starts.
	std::array<std::uint8_t, SCREEN_WIDTH> m_sprPattern;
	std::array<std::uint8_t, SCREEN_WIDTH> m_sprAttribute;
	std::array<bool, SCREEN_WIDTH> m_sprBehind;
	std::array<bool, SCREEN_WIDTH> m_sprIsZero;
	// Dot at which sprite 0 overlaps the background on the current scanline, or
	// -1. The flag is raised when the counter reaches it rather than when the
	// line is drawn, because games poll for it and then change scroll mid-frame.
	int m_sprite0HitDot;
};

} // namespace nes

#endif // NES_PPU_H
