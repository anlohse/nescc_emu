#ifndef NES_PPU_H
#define NES_PPU_H

#include "Cartridge.h"

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
	// NTSC geometry.
	static const int DOTS_PER_SCANLINE = 341;
	static const int SCANLINES_PER_FRAME = 262;
	static const int VBLANK_SCANLINE = 241;
	static const int PRE_RENDER_SCANLINE = 261;

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

	/** 64 entries of packed 0x00RRGGBB for the NES's fixed colour palette. */
	static const std::uint32_t* nesPaletteRgb();

private:
	void tickOne();
	std::uint16_t mirrorNametable(std::uint16_t address) const;
	static std::uint16_t mirrorPalette(std::uint16_t address);

	void renderScanline(int line);
	void incrementY();
	void copyHorizontalBits();
	void copyVerticalBits();

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

	int m_scanline;
	int m_dot;
	std::uint64_t m_frame;
	bool m_nmiPending;

	std::array<std::uint8_t, SCREEN_WIDTH * SCREEN_HEIGHT> m_framebuffer;
	// Dot at which sprite 0 overlaps the background on the current scanline, or
	// -1. The flag is raised when the counter reaches it rather than when the
	// line is drawn, because games poll for it and then change scroll mid-frame.
	int m_sprite0HitDot;
};

} // namespace nes

#endif // NES_PPU_H
