/*
 * testPpu.cpp -- PPU timing, register semantics and the NMI that drives a frame.
 *
 * Nothing renders yet; these cover the behaviour game code depends on before a
 * pixel matters.
 */

#include "TestRom.h"
#include "nes/Nes.h"

#include <doctest/doctest.h>

#include <memory>
#include <vector>

using namespace nes;

namespace {

/** Dots from power-on to scanline @p sl, dot @p d. */
int dotsTo(int sl, int d) {
	return sl * Ppu::DOTS_PER_SCANLINE + d;
}

std::unique_ptr<Cartridge> makeCart(bool vertical = false) {
	std::vector<std::uint8_t> prg;
	testrom::setResetVector(prg, 0xC000);
	testrom::Options o;
	o.verticalMirroring = vertical;
	o.chrBanks = 0;   // CHR RAM, so pattern-table writes stick
	return Cartridge::fromINes(testrom::build(o, prg));
}

} // namespace

TEST_CASE("vblank_starts_at_scanline_241_dot_1") {
	Ppu ppu;
	CHECK_FALSE(ppu.inVBlank());

	ppu.tick(dotsTo(Ppu::VBLANK_SCANLINE, 1) - 1);
	CHECK_FALSE(ppu.inVBlank());       // one dot early

	ppu.tick(1);
	CHECK(ppu.inVBlank());
	CHECK_EQ(ppu.scanline(), Ppu::VBLANK_SCANLINE);
}

TEST_CASE("vblank_clears_at_the_pre_render_line") {
	Ppu ppu;
	ppu.tick(dotsTo(Ppu::VBLANK_SCANLINE, 2));
	REQUIRE(ppu.inVBlank());

	ppu.tick(dotsTo(Ppu::PRE_RENDER_SCANLINE, 2) - dotsTo(Ppu::VBLANK_SCANLINE, 2));
	CHECK_FALSE(ppu.inVBlank());
	CHECK_EQ(ppu.scanline(), Ppu::PRE_RENDER_SCANLINE);
}

TEST_CASE("a_frame_is_262_scanlines_of_341_dots") {
	Ppu ppu;
	CHECK_EQ(ppu.frame(), 0);
	ppu.tick(Ppu::DOTS_PER_SCANLINE * Ppu::SCANLINES_PER_FRAME);
	CHECK_EQ(ppu.frame(), 1);
	CHECK_EQ(ppu.scanline(), 0);
	CHECK_EQ(ppu.dot(), 0);
}

TEST_CASE("nmi_fires_on_vblank_only_when_enabled") {
	SUBCASE("disabled by default") {
		Ppu ppu;
		ppu.tick(dotsTo(Ppu::VBLANK_SCANLINE, 2));
		CHECK(ppu.inVBlank());
		CHECK_FALSE(ppu.takeNmi());
	}
	SUBCASE("enabled through $2000 bit 7") {
		Ppu ppu;
		ppu.writeRegister(0, Ppu::CTRL_NMI_ENABLE);
		ppu.tick(dotsTo(Ppu::VBLANK_SCANLINE, 2));
		CHECK(ppu.takeNmi());
		CHECK_FALSE(ppu.takeNmi());    // consumed
	}
	SUBCASE("enabling during vblank fires immediately") {
		Ppu ppu;
		ppu.tick(dotsTo(Ppu::VBLANK_SCANLINE, 2));
		REQUIRE(ppu.inVBlank());
		REQUIRE_FALSE(ppu.takeNmi());
		ppu.writeRegister(0, Ppu::CTRL_NMI_ENABLE);
		CHECK(ppu.takeNmi());
	}
}

TEST_CASE("reading_status_clears_vblank_and_the_write_toggle") {
	Ppu ppu;
	ppu.tick(dotsTo(Ppu::VBLANK_SCANLINE, 2));
	REQUIRE(ppu.inVBlank());

	const std::uint8_t status = ppu.readRegister(2);
	CHECK((status & Ppu::STATUS_VBLANK) != 0);
	CHECK_FALSE(ppu.inVBlank());                    // read clears it
	CHECK_EQ(ppu.readRegister(2) & Ppu::STATUS_VBLANK, 0);

	// $2006 takes a high byte then a low byte; $2002 resets that sequence, so a
	// half-finished write must not be completed by the next one.
	ppu.writeRegister(6, 0x21);                     // high byte
	ppu.readRegister(2);                            // resets the toggle
	ppu.writeRegister(6, 0x24);                     // treated as a high byte again
	ppu.writeRegister(6, 0x00);                     // low byte -> $2400
	ppu.writeRegister(7, 0x5A);
	CHECK_EQ(ppu.vramRead(0x2400), 0x5A);
}

TEST_CASE("vram_address_auto_increments") {
	Ppu ppu;
	SUBCASE("by one across a row") {
		ppu.writeRegister(0, 0);
		ppu.writeRegister(6, 0x20);
		ppu.writeRegister(6, 0x00);
		ppu.writeRegister(7, 0x11);
		ppu.writeRegister(7, 0x22);
		CHECK_EQ(ppu.vramRead(0x2000), 0x11);
		CHECK_EQ(ppu.vramRead(0x2001), 0x22);
	}
	SUBCASE("by 32 down a column") {
		ppu.writeRegister(0, Ppu::CTRL_INCREMENT_32);
		ppu.writeRegister(6, 0x20);
		ppu.writeRegister(6, 0x00);
		ppu.writeRegister(7, 0x11);
		ppu.writeRegister(7, 0x22);
		CHECK_EQ(ppu.vramRead(0x2000), 0x11);
		CHECK_EQ(ppu.vramRead(0x2020), 0x22);
	}
}

TEST_CASE("data_reads_below_the_palette_are_buffered") {
	Ppu ppu;
	ppu.writeRegister(6, 0x20);
	ppu.writeRegister(6, 0x00);
	ppu.writeRegister(7, 0xAB);          // $2000 = AB
	ppu.writeRegister(7, 0xCD);          // $2001 = CD

	ppu.writeRegister(6, 0x20);
	ppu.writeRegister(6, 0x00);
	CHECK_EQ(ppu.readRegister(7), 0x00); // first read returns the stale buffer
	CHECK_EQ(ppu.readRegister(7), 0xAB); // now the byte from $2000
	CHECK_EQ(ppu.readRegister(7), 0xCD);
}

TEST_CASE("palette_reads_are_immediate") {
	Ppu ppu;
	ppu.writeRegister(6, 0x3F);
	ppu.writeRegister(6, 0x00);
	ppu.writeRegister(7, 0x21);

	ppu.writeRegister(6, 0x3F);
	ppu.writeRegister(6, 0x00);
	CHECK_EQ(ppu.readRegister(7), 0x21); // no one-access delay here
}

TEST_CASE("palette_backdrop_entries_mirror") {
	Ppu ppu;
	ppu.vramWrite(0x3F00, 0x0A);
	CHECK_EQ(ppu.vramRead(0x3F10), 0x0A);   // sprite backdrop mirrors background
	ppu.vramWrite(0x3F14, 0x0B);
	CHECK_EQ(ppu.vramRead(0x3F04), 0x0B);
	// The whole palette repeats every 32 bytes through $3FFF.
	CHECK_EQ(ppu.vramRead(0x3F20), 0x0A);
}

TEST_CASE("nametable_mirroring_follows_the_cartridge") {
	SUBCASE("horizontal: top two nametables share, bottom two share") {
		auto cart = makeCart(false);
		REQUIRE(cart != nullptr);
		Ppu ppu(cart.get());
		ppu.vramWrite(0x2000, 0x11);
		CHECK_EQ(ppu.vramRead(0x2400), 0x11);   // NT1 mirrors NT0
		ppu.vramWrite(0x2800, 0x22);
		CHECK_EQ(ppu.vramRead(0x2C00), 0x22);   // NT3 mirrors NT2
		CHECK_EQ(ppu.vramRead(0x2000), 0x11);   // and the halves stay distinct
	}
	SUBCASE("vertical: left two share, right two share") {
		auto cart = makeCart(true);
		REQUIRE(cart != nullptr);
		Ppu ppu(cart.get());
		ppu.vramWrite(0x2000, 0x11);
		CHECK_EQ(ppu.vramRead(0x2800), 0x11);   // NT2 mirrors NT0
		ppu.vramWrite(0x2400, 0x22);
		CHECK_EQ(ppu.vramRead(0x2C00), 0x22);   // NT3 mirrors NT1
		CHECK_EQ(ppu.vramRead(0x2000), 0x11);
	}
}

TEST_CASE("pattern_tables_route_to_the_cartridge") {
	auto cart = makeCart();
	REQUIRE(cart != nullptr);
	Ppu ppu(cart.get());

	ppu.vramWrite(0x0000, 0x3C);      // CHR RAM board, so this sticks
	CHECK_EQ(ppu.vramRead(0x0000), 0x3C);
	CHECK_EQ(cart->ppuRead(0x0000), 0x3C);
}

TEST_CASE("peek_does_not_clear_the_vblank_flag") {
	// The distinction that justifies peek() existing at all.
	Nes console;
	console.setCartridge(makeCart());
	console.reset();
	console.ppu().tick(dotsTo(Ppu::VBLANK_SCANLINE, 2));
	REQUIRE(console.ppu().inVBlank());

	console.bus().peek(0x2002);
	CHECK(console.ppu().inVBlank());        // a debugger looked; nothing changed

	console.bus().read(0x2002);
	CHECK_FALSE(console.ppu().inVBlank());  // the CPU looked; the flag cleared
}

TEST_CASE("nmi_reaches_the_cpu_through_the_console") {
	// $4014 aside, this is the whole frame loop: vblank raises NMI, the CPU
	// vectors through the cartridge, the handler returns.
	std::vector<std::uint8_t> prg;
	testrom::setResetVector(prg, 0xC000);
	prg[0x0000] = 0x4C;                   // JMP $C000 -- spin
	prg[0x0001] = 0x00;
	prg[0x0002] = 0xC0;
	prg[0x3FFA] = 0x00;                   // NMI vector -> $C100
	prg[0x3FFB] = 0xC1;
	prg[0x0100] = 0x40;                   // RTI

	testrom::Options o;
	auto cart = Cartridge::fromINes(testrom::build(o, prg));
	REQUIRE(cart != nullptr);

	Nes console;
	console.setCartridge(std::move(cart));
	console.reset();
	console.ppu().writeRegister(0, Ppu::CTRL_NMI_ENABLE);

	bool entered = false;
	for (int i = 0; i < 20000 && !entered; i++) {
		console.step();
		if (console.cpuRegisters().pc == 0xC100)
			entered = true;
	}
	CHECK_MESSAGE(entered, "the NMI handler was never reached");
}

TEST_CASE("oam_dma_copies_a_page_and_stalls_the_cpu") {
	Nes console;
	console.setCartridge(makeCart());
	console.reset();

	// Fill $0200-$02FF, the page games normally stage sprites in.
	for (int i = 0; i < 256; i++)
		console.bus().write(static_cast<uint16>(0x0200 + i), static_cast<uint8>(i ^ 0x5A));

	console.bus().write(0x4014, 0x02);
	CHECK_EQ(console.bus().pendingDmaStall(), 513);

	// The copy itself is immediate.
	console.ppu().writeRegister(3, 0x00);          // OAMADDR = 0
	for (int i = 0; i < 4; i++) {
		console.ppu().writeRegister(3, static_cast<std::uint8_t>(i));
		CHECK_EQ(console.ppu().readRegister(4), static_cast<std::uint8_t>(i ^ 0x5A));
	}

	// The next step burns the stall instead of executing an instruction, and
	// the PPU keeps advancing through it.
	const int before = console.ppu().dot() + console.ppu().scanline() * Ppu::DOTS_PER_SCANLINE;
	CHECK_EQ(console.step(), 513);
	const int after = console.ppu().dot() + console.ppu().scanline() * Ppu::DOTS_PER_SCANLINE;
	CHECK_EQ(after - before, 513 * 3);
	CHECK_EQ(console.bus().pendingDmaStall(), 0);
}

TEST_CASE("step_frame_advances_exactly_one_frame") {
	Nes console;
	console.setCartridge(makeCart());
	console.reset();

	const std::uint64_t start = console.ppu().frame();
	console.stepFrame();
	CHECK_EQ(console.ppu().frame(), start + 1);

	// An NTSC frame is 89342 dots, so about 29780 CPU cycles.
	const int cycles = console.stepFrame();
	CHECK(cycles > 29000);
	CHECK(cycles < 30500);
}

/* ------------------------------------------------------------------------- */
/* Rendering                                                                  */
/* ------------------------------------------------------------------------- */

namespace {

/** Fill pattern-table tile @p tile so every pixel has colour index @p value. */
void writeSolidTile(Ppu& ppu, std::uint16_t base, std::uint8_t tile, int value) {
	for (int r = 0; r < 8; r++) {
		const std::uint16_t addr = static_cast<std::uint16_t>(base + tile * 16 + r);
		ppu.vramWrite(addr, (value & 1) ? 0xFF : 0x00);
		ppu.vramWrite(static_cast<std::uint16_t>(addr + 8), (value & 2) ? 0xFF : 0x00);
	}
}

std::uint8_t pixelAt(const Ppu& ppu, int x, int y) {
	return ppu.framebuffer()[y * Ppu::SCREEN_WIDTH + x];
}

void renderOneFrame(Ppu& ppu) {
	ppu.tick(Ppu::DOTS_PER_SCANLINE * Ppu::SCANLINES_PER_FRAME);
}

/*
 * Scroll writes land in the temporary address t, and only reach the live
 * address v at dot 257 of a rendered line. Coarse scroll therefore does not
 * affect the first line of the frame it was written in -- so scroll tests need
 * a second frame before checking row 0. Fine X is the exception: it is applied
 * straight from its own register.
 */
void renderFrames(Ppu& ppu, int n) {
	for (int i = 0; i < n; i++)
		renderOneFrame(ppu);
}

} // namespace

TEST_CASE("a_disabled_ppu_renders_the_backdrop") {
	auto cart = makeCart();
	Ppu ppu(cart.get());
	ppu.vramWrite(0x3F00, 0x12);          // backdrop colour
	renderOneFrame(ppu);
	CHECK_EQ(pixelAt(ppu, 0, 0), 0x12);
	CHECK_EQ(pixelAt(ppu, 255, 239), 0x12);
}

TEST_CASE("background_tiles_render_with_their_palette") {
	auto cart = makeCart();
	Ppu ppu(cart.get());

	writeSolidTile(ppu, 0x0000, 1, 3);    // tile 1 is colour index 3 everywhere
	ppu.vramWrite(0x2000, 0x01);          // top-left of the nametable uses it
	ppu.vramWrite(0x3F00, 0x0F);          // backdrop
	ppu.vramWrite(0x3F03, 0x21);          // palette 0, colour 3
	ppu.writeRegister(1, Ppu::MASK_SHOW_BACKGROUND | Ppu::MASK_SHOW_BG_LEFT);

	renderOneFrame(ppu);

	CHECK_EQ(pixelAt(ppu, 0, 0), 0x21);   // inside the tile
	CHECK_EQ(pixelAt(ppu, 7, 7), 0x21);
	CHECK_EQ(pixelAt(ppu, 8, 0), 0x0F);   // next tile is 0 -> backdrop
	CHECK_EQ(pixelAt(ppu, 0, 8), 0x0F);   // and the row below
}

TEST_CASE("the_left_column_can_be_masked") {
	auto cart = makeCart();
	Ppu ppu(cart.get());

	writeSolidTile(ppu, 0x0000, 1, 3);
	for (int i = 0; i < 4; i++)
		ppu.vramWrite(static_cast<std::uint16_t>(0x2000 + i), 0x01);
	ppu.vramWrite(0x3F00, 0x0F);
	ppu.vramWrite(0x3F03, 0x21);
	ppu.writeRegister(1, Ppu::MASK_SHOW_BACKGROUND);   // no MASK_SHOW_BG_LEFT

	renderOneFrame(ppu);

	CHECK_EQ(pixelAt(ppu, 0, 0), 0x0F);   // leftmost 8 pixels suppressed
	CHECK_EQ(pixelAt(ppu, 7, 0), 0x0F);
	CHECK_EQ(pixelAt(ppu, 8, 0), 0x21);
}

TEST_CASE("sprites_render_over_the_background") {
	auto cart = makeCart();
	Ppu ppu(cart.get());

	writeSolidTile(ppu, 0x0000, 1, 1);    // background tile
	writeSolidTile(ppu, 0x0000, 2, 1);    // sprite tile
	ppu.vramWrite(0x2000, 0x01);          // covers y = 0..7
	ppu.vramWrite(0x2020, 0x01);          // and the tile row below, y = 8..15
	ppu.vramWrite(0x3F00, 0x0F);
	ppu.vramWrite(0x3F01, 0x11);          // background colour
	ppu.vramWrite(0x3F11, 0x28);          // sprite palette 0, colour 1

	// Sprite 1 (not zero) at x=0, covering scanline 4.
	ppu.writeRegister(3, 4);              // OAMADDR -> sprite 1's Y byte
	ppu.writeRegister(4, 3);              // Y: appears on scanlines 4..11
	ppu.writeRegister(4, 2);              // tile
	ppu.writeRegister(4, 0);              // attributes: palette 0, in front
	ppu.writeRegister(4, 0);              // X

	ppu.writeRegister(1, Ppu::MASK_SHOW_BACKGROUND | Ppu::MASK_SHOW_BG_LEFT
			| Ppu::MASK_SHOW_SPRITES | Ppu::MASK_SHOW_SPRITE_LEFT);

	renderOneFrame(ppu);

	CHECK_EQ(pixelAt(ppu, 0, 0), 0x11);   // above the sprite: background
	CHECK_EQ(pixelAt(ppu, 0, 4), 0x28);   // sprite covers the background
	CHECK_EQ(pixelAt(ppu, 0, 11), 0x28);
	CHECK_EQ(pixelAt(ppu, 0, 12), 0x11);  // past its 8 rows
}

TEST_CASE("sprites_behind_the_background_stay_hidden") {
	auto cart = makeCart();
	Ppu ppu(cart.get());

	writeSolidTile(ppu, 0x0000, 1, 1);
	writeSolidTile(ppu, 0x0000, 2, 1);
	ppu.vramWrite(0x2000, 0x01);
	ppu.vramWrite(0x3F01, 0x11);
	ppu.vramWrite(0x3F11, 0x28);

	ppu.writeRegister(3, 4);
	ppu.writeRegister(4, 3);
	ppu.writeRegister(4, 2);
	ppu.writeRegister(4, Ppu::SPRITE_BEHIND_BG);
	ppu.writeRegister(4, 0);

	ppu.writeRegister(1, Ppu::MASK_SHOW_BACKGROUND | Ppu::MASK_SHOW_BG_LEFT
			| Ppu::MASK_SHOW_SPRITES | Ppu::MASK_SHOW_SPRITE_LEFT);

	renderOneFrame(ppu);
	CHECK_EQ(pixelAt(ppu, 0, 4), 0x11);   // opaque background wins
}

TEST_CASE("sprite_zero_hit_is_reported_when_it_overlaps_the_background") {
	auto cart = makeCart();
	Ppu ppu(cart.get());

	writeSolidTile(ppu, 0x0000, 1, 1);
	ppu.vramWrite(0x2000, 0x01);          // opaque background at the top-left
	ppu.vramWrite(0x3F01, 0x11);
	ppu.vramWrite(0x3F11, 0x28);

	// Sprite 0, opaque, over that same tile.
	ppu.writeRegister(3, 0);
	ppu.writeRegister(4, 3);              // Y -> scanlines 4..11
	ppu.writeRegister(4, 1);              // tile
	ppu.writeRegister(4, 0);
	ppu.writeRegister(4, 0);              // X = 0

	ppu.writeRegister(1, Ppu::MASK_SHOW_BACKGROUND | Ppu::MASK_SHOW_BG_LEFT
			| Ppu::MASK_SHOW_SPRITES | Ppu::MASK_SHOW_SPRITE_LEFT);

	CHECK_EQ(ppu.peekRegister(2) & Ppu::STATUS_SPRITE0, 0);

	// The flag is cleared again at the pre-render line, so it has to be observed
	// mid-frame -- which is exactly how a game uses it.
	ppu.tick(dotsTo(12, 0));
	CHECK((ppu.peekRegister(2) & Ppu::STATUS_SPRITE0) != 0);

	ppu.tick(dotsTo(Ppu::SCANLINES_PER_FRAME, 0) - dotsTo(12, 0));
	CHECK_EQ(ppu.peekRegister(2) & Ppu::STATUS_SPRITE0, 0);   // cleared for the next frame
}

TEST_CASE("sprite_zero_hit_needs_both_layers_opaque") {
	auto cart = makeCart();
	Ppu ppu(cart.get());

	writeSolidTile(ppu, 0x0000, 1, 1);
	// Nametable left at tile 0, which is transparent everywhere.
	ppu.writeRegister(3, 0);
	ppu.writeRegister(4, 3);
	ppu.writeRegister(4, 1);
	ppu.writeRegister(4, 0);
	ppu.writeRegister(4, 0);
	ppu.writeRegister(1, Ppu::MASK_SHOW_BACKGROUND | Ppu::MASK_SHOW_BG_LEFT
			| Ppu::MASK_SHOW_SPRITES | Ppu::MASK_SHOW_SPRITE_LEFT);

	ppu.tick(dotsTo(12, 0));
	CHECK_EQ(ppu.peekRegister(2) & Ppu::STATUS_SPRITE0, 0);   // no opaque background
}

TEST_CASE("horizontal_scroll_shifts_the_background") {
	auto cart = makeCart();
	Ppu ppu(cart.get());

	writeSolidTile(ppu, 0x0000, 1, 3);
	ppu.vramWrite(0x2001, 0x01);          // second tile of the row, x = 8..15
	ppu.vramWrite(0x3F00, 0x0F);
	ppu.vramWrite(0x3F03, 0x21);
	ppu.writeRegister(1, Ppu::MASK_SHOW_BACKGROUND | Ppu::MASK_SHOW_BG_LEFT);

	SUBCASE("no scroll") {
		renderFrames(ppu, 2);
		CHECK_EQ(pixelAt(ppu, 8, 0), 0x21);
		CHECK_EQ(pixelAt(ppu, 0, 0), 0x0F);
	}
	SUBCASE("scrolled left by 8") {
		ppu.writeRegister(5, 8);          // X scroll
		ppu.writeRegister(5, 0);          // Y scroll
		renderFrames(ppu, 2);
		CHECK_EQ(pixelAt(ppu, 0, 0), 0x21);   // the tile moved to the left edge
		CHECK_EQ(pixelAt(ppu, 8, 0), 0x0F);
	}
	SUBCASE("fine scroll by 3") {
		ppu.writeRegister(5, 3);
		ppu.writeRegister(5, 0);
		renderFrames(ppu, 2);
		CHECK_EQ(pixelAt(ppu, 4, 0), 0x0F);   // tile now spans x = 5..12
		CHECK_EQ(pixelAt(ppu, 5, 0), 0x21);
		CHECK_EQ(pixelAt(ppu, 12, 0), 0x21);
		CHECK_EQ(pixelAt(ppu, 13, 0), 0x0F);
	}
}
