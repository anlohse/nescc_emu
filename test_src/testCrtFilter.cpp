/*
 * testCrtFilter.cpp -- the television look, as arithmetic.
 *
 * Two things are worth pinning here, and neither is visible in a screenshot.
 *
 * The pattern: each console pixel becomes three stripes and a dimmer last row,
 * and each stripe carries exactly one channel. Getting a channel into the wrong
 * column tints the whole picture, which looks like a palette bug rather than a
 * filter bug.
 *
 * The brightness: a mask throws away two thirds of the light, so the filter has
 * to give it back. How much is a measurement, and this is where it is made --
 * the gain is not a number somebody liked the look of.
 */

#include "../src/frontend/CrtFilter.h"
#include "nes/Ppu.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

using namespace nesfe;

namespace {

/** Rec. 601 luma, the same weighting the light gun uses. */
double luma(std::uint32_t argb) {
	const double r = (argb >> 16) & 0xFF;
	const double g = (argb >> 8) & 0xFF;
	const double b = argb & 0xFF;
	return (r * 299.0 + g * 587.0 + b * 114.0) / 1000.0;
}

std::vector<std::uint32_t> expand(const std::vector<std::uint8_t>& indices,
		int width, int height) {
	std::vector<std::uint32_t> out(
			static_cast<std::size_t>(width) * CRT_SCALE * height * CRT_SCALE, 0);
	crtExpand(indices.data(), width, height, nes::Ppu::nesPaletteRgb(), out.data());
	return out;
}

/** Palette entry $30 is white, $0F is black. */
const std::uint8_t WHITE = 0x30;
const std::uint8_t BLACK = 0x0F;

} // namespace

TEST_CASE("each_column_leads_with_its_own_channel") {
	const std::vector<std::uint8_t> one(1, WHITE);
	const std::vector<std::uint32_t> out = expand(one, 1, 1);
	REQUIRE_EQ(out.size(), 9u);

	for (int row = 0; row < CRT_SCALE; row++) {
		CAPTURE(row);
		const std::uint32_t red = out[row * CRT_SCALE + 0];
		const std::uint32_t green = out[row * CRT_SCALE + 1];
		const std::uint32_t blue = out[row * CRT_SCALE + 2];

		// The stripe is strong rather than absolute: within a column its own
		// channel dominates the other two, and across columns it is the
		// strongest of the three. Getting a channel into the wrong column tints
		// the whole picture, and looks like a palette bug rather than this.
		const int redR = (red >> 16) & 0xFF, redG = (red >> 8) & 0xFF;
		const int greenG = (green >> 8) & 0xFF, greenR = (green >> 16) & 0xFF;
		const int blueB = blue & 0xFF, blueR = (blue >> 16) & 0xFF;
		CHECK(redR > redG);
		CHECK(greenG > greenR);
		CHECK(blueB > blueR);
		CHECK(redR > greenR);                  // reddest column is column 0
		CHECK(greenG > ((red >> 8) & 0xFF));   // greenest is column 1
		CHECK(blueB > (red & 0xFF));           // bluest is column 2

		CHECK((red & 0xFF000000) == 0xFF000000);   // opaque throughout
	}
}

TEST_CASE("the_last_row_of_a_cell_is_the_dim_one") {
	const std::vector<std::uint8_t> one(1, WHITE);
	const std::vector<std::uint32_t> out = expand(one, 1, 1);

	// The first two rows match each other, and the third is darker than both --
	// which is the scanline, and the thing that makes it look like a beam.
	for (int column = 0; column < CRT_SCALE; column++) {
		CAPTURE(column);
		CHECK_EQ(out[column], out[CRT_SCALE + column]);
		CHECK(luma(out[2 * CRT_SCALE + column]) < luma(out[column]));
	}
	// The ratio is exactly CRT_SCANLINE only where the gain has not clipped, so
	// this is measured on a dim colour rather than on white. On white the bright
	// row's red clamps at 255 while the dim row's 244 does not, and the ratio
	// comes out at 0.79 -- which is the clamp showing through, not a bug, and is
	// worth knowing before somebody "fixes" it.
	const std::vector<std::uint8_t> dark(1, 0x00);      // $00 is a mid grey
	const std::vector<std::uint32_t> dim = expand(dark, 1, 1);
	CHECK(luma(dim[2 * CRT_SCALE]) == doctest::Approx(
			luma(dim[0]) * CRT_SCANLINE).epsilon(0.02));
}

TEST_CASE("black_stays_black") {
	// Gain multiplies; nothing times anything is still nothing. Worth stating,
	// because a filter that lifts the blacks makes every dark scene grey.
	const std::vector<std::uint8_t> one(1, BLACK);
	const std::vector<std::uint32_t> out = expand(one, 1, 1);
	for (std::size_t i = 0; i < out.size(); i++)
		CHECK_EQ(out[i] & 0x00FFFFFF, 0u);
}

TEST_CASE("the_gain_lands_the_average_brightness_near_the_real_picture") {
	// The measurement the gain comes from. A picture of every colour the console
	// can make, through the filter, against the same picture unfiltered: if the
	// mask is not paid back the result is about a third as bright, and if it is
	// overpaid every highlight clips and the picture goes flat.
	const int width = 8;
	const int height = 8;
	std::vector<std::uint8_t> frame(width * height);
	for (int i = 0; i < width * height; i++)
		frame[i] = static_cast<std::uint8_t>(i);       // 64 palette entries

	const std::uint32_t* palette = nes::Ppu::nesPaletteRgb();
	double plain = 0.0;
	for (int i = 0; i < width * height; i++)
		plain += luma(palette[frame[i] & 0x3F]);
	plain /= width * height;

	const std::vector<std::uint32_t> out = expand(frame, width, height);
	double filtered = 0.0;
	for (std::size_t i = 0; i < out.size(); i++)
		filtered += luma(out[i]);
	filtered /= out.size();

	// About three quarters, which is where the measurement landed and is
	// deliberately not 1.0: a mask really does cost light, a television really
	// was dimmer than this monitor, and the alternative -- enough gain to match
	// -- clips most of the palette flat instead. What this guards against is
	// drift: a third would be gloom, and parity would mean the stripes had
	// stopped doing anything.
	CHECK(filtered > plain * 0.60);
	CHECK(filtered < plain * 0.85);
}

TEST_CASE("neighbouring_pixels_get_their_own_cells") {
	// The failure this catches is an off-by-one in the row stride, which turns
	// the picture into diagonal smears -- obvious once seen and easy to write.
	std::vector<std::uint8_t> frame(4, BLACK);
	frame[1] = WHITE;                   // second pixel of a 2x2
	const std::vector<std::uint32_t> out = expand(frame, 2, 2);
	const int outWidth = 2 * CRT_SCALE;

	// The lit cell is columns 3-5 of rows 0-2, and nothing else.
	for (int y = 0; y < 2 * CRT_SCALE; y++)
		for (int x = 0; x < outWidth; x++) {
			const bool inLitCell = (x >= CRT_SCALE && y < CRT_SCALE);
			CAPTURE(x);
			CAPTURE(y);
			if (inLitCell)
				CHECK((out[y * outWidth + x] & 0x00FFFFFF) != 0u);
			else
				CHECK_EQ(out[y * outWidth + x] & 0x00FFFFFF, 0u);
		}
}
