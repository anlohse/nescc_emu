/*
 * testCrtFilter.cpp -- the television mask, as arithmetic.
 *
 * The mask is multiplied over a picture the renderer has already stretched with
 * a linear filter, so what is testable here is the mask itself: where its
 * stripes fall, how the scanline gap behaves, and how much light the whole thing
 * costs. None of that is visible in a screenshot -- a mask that is one pixel out
 * of step looks like a slightly odd tint until somebody magnifies it.
 *
 * The hue case is where the constants come from, and it was written after a
 * measurement contradicted the design: a multiply can only take light away, what
 * it takes has to be paid in beforehand, and a palette whose colours already sit
 * at 255 has nowhere to be paid. Paying anyway is what turned Mario's sky
 * lavender.
 */

#include "../src/frontend/CrtFilter.h"
#include "nes/Ppu.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <vector>

using namespace nesfe;

namespace {

double luma(std::uint32_t argb) {
	const double r = (argb >> 16) & 0xFF;
	const double g = (argb >> 8) & 0xFF;
	const double b = argb & 0xFF;
	return (r * 299.0 + g * 587.0 + b * 114.0) / 1000.0;
}

std::vector<std::uint32_t> mask(int width, int height, int lines,
		CrtMaskKind kind = CRT_APERTURE_GRILLE) {
	std::vector<std::uint32_t> out(static_cast<std::size_t>(width) * height, 0);
	buildCrtMask(width, height, lines, kind, out.data());
	return out;
}

int red(std::uint32_t p)   { return (p >> 16) & 0xFF; }
int green(std::uint32_t p) { return (p >> 8) & 0xFF; }
int blue(std::uint32_t p)  { return p & 0xFF; }

} // namespace

TEST_CASE("the_stripes_repeat_every_three_output_pixels") {
	// The pitch is in screen pixels and has nothing to do with the source, which
	// is the whole point of applying the mask after the stretch: on a television
	// the stripes were in the glass.
	const std::vector<std::uint32_t> m = mask(12, 3, 1);

	for (int x = 0; x < 12; x += CRT_MASK_PITCH) {
		CAPTURE(x);
		CHECK(red(m[x]) > green(m[x]));
		CHECK(red(m[x]) > blue(m[x]));
		CHECK(green(m[x + 1]) > red(m[x + 1]));
		CHECK(blue(m[x + 2]) > red(m[x + 2]));
	}
	// And it really repeats, rather than drifting.
	for (int x = 0; x + CRT_MASK_PITCH < 12; x++)
		CHECK_EQ(m[x], m[x + CRT_MASK_PITCH]);
}

TEST_CASE("a_slot_mask_staggers_the_gaps_and_not_the_colours") {
	// A television broke each colour column into slots and put the bridges between
	// them half a slot from the neighbouring column's. That is a brick wall on its
	// side: the columns stay in step and the *gaps* alternate.
	//
	// Which is the correction that matters here. Staggering the colours instead
	// gives a plausible-looking lattice that no television ever had.
	const int lines = 4;
	const int height = lines * 3;
	const int width = CRT_MASK_PITCH * 4;
	const std::vector<std::uint32_t> m =
			mask(width, height, lines, CRT_SLOT_MASK);

	for (int y = 0; y < height; y++) {
		CAPTURE(y);
		// Every triad column leads with red in its first pixel, green in its
		// second, blue in its third -- exactly as a grille does. The scanline
		// scales all three of a pixel's channels together, so whichever channel
		// leads is simply the largest whatever the column's phase.
		for (int x = 0; x < width; x += CRT_MASK_PITCH) {
			CHECK(red(m[y * width + x]) > green(m[y * width + x]));
			CHECK(green(m[y * width + x + 1]) > red(m[y * width + x + 1]));
			CHECK(blue(m[y * width + x + 2]) > red(m[y * width + x + 2]));
		}
	}

	// Neighbouring triad columns are out of step vertically, and columns two apart
	// are back in step. Comparing the same channel of the same stripe position
	// isolates the beam: only the slot phase can differ.
	bool everDiffered = false;
	for (int y = 0; y < height; y++) {
		CAPTURE(y);
		CHECK_EQ(red(m[y * width]), red(m[y * width + CRT_MASK_PITCH * 2]));
		if (red(m[y * width]) != red(m[y * width + CRT_MASK_PITCH]))
			everDiffered = true;
	}
	CHECK(everDiffered);
}

TEST_CASE("a_grille_leaves_every_column_in_step") {
	// The difference from a slot mask, stated directly: an aperture grille's
	// stripes run the whole height of the tube with nothing interrupting them, so
	// every column has its dark bands in the same places.
	const int lines = 4;
	const int height = lines * 3;
	const int width = CRT_MASK_PITCH * 4;
	const std::vector<std::uint32_t> m =
			mask(width, height, lines, CRT_APERTURE_GRILLE);

	for (int y = 0; y < height; y++) {
		CAPTURE(y);
		for (int x = CRT_MASK_PITCH; x < width; x += CRT_MASK_PITCH)
			CHECK_EQ(red(m[y * width + x]), red(m[y * width]));
	}
}

TEST_CASE("staggering_the_slots_costs_no_light_at_all") {
	// The property that makes the brick pattern usable, and it comes from the beam
	// being integrated: shifting a periodic profile does not change what a whole
	// period of it integrates to, so a staggered column is exactly as bright as an
	// aligned one over any whole number of lines. If it were not, the lattice
	// would show up as vertical banding on every flat colour -- the same artefact
	// the integration was introduced to remove from the horizontal.
	const int lines = 8;
	const int height = lines * 3;
	const int width = CRT_MASK_PITCH * 2;
	const std::vector<std::uint32_t> m = mask(width, height, lines, CRT_SLOT_MASK);

	// Down a whole column of each kind, one aligned and one staggered.
	double alignedTotal = 0.0;
	double staggeredTotal = 0.0;
	for (int y = 0; y < height; y++) {
		alignedTotal += luma(m[y * width]);
		staggeredTotal += luma(m[y * width + CRT_MASK_PITCH]);
	}
	CAPTURE(alignedTotal);
	CAPTURE(staggeredTotal);
	CHECK(staggeredTotal > alignedTotal * 0.99);
	CHECK(staggeredTotal < alignedTotal * 1.01);
}

TEST_CASE("the_mask_is_a_multiplier_so_nothing_in_it_exceeds_full") {
	// Drawn with a modulate blend, where 255 means "leave this channel alone".
	// A value above that would be meaningless, and a mask of all 255 would be no
	// mask at all.
	const std::vector<std::uint32_t> m = mask(9, 9, 3);
	bool sawSomethingDark = false;
	for (std::size_t i = 0; i < m.size(); i++) {
		CHECK(red(m[i]) <= 255);
		CHECK(green(m[i]) <= 255);
		CHECK(blue(m[i]) <= 255);
		if (luma(m[i]) < 200.0)
			sawSomethingDark = true;
	}
	CHECK(sawSomethingDark);
}

TEST_CASE("every_console_line_has_a_seam_and_a_bright_middle") {
	// The scanline, and the one part of the mask that follows the *source*: the
	// gaps were in the signal, one per line the console drew, so their spacing
	// has to come from 240 rather than from the window.
	//
	// A beam was brightest down the middle of its line and darkest at the join
	// with the next, which is the shape checked here.
	//
	// The two edge rows come out exactly equal, and that is the integration
	// working rather than a coincidence: the beam profile is symmetric about the
	// middle of its line, so the top third and the bottom third receive the same
	// light. The dark seam is the bottom of one line and the top of the next
	// together, which is where a seam actually is.
	const int lines = 4;
	const int height = lines * 3;                 // three output rows per line
	const std::vector<std::uint32_t> m = mask(3, height, lines);

	for (int line = 0; line < lines; line++) {
		CAPTURE(line);
		const double top = luma(m[(line * 3 + 0) * 3]);
		const double middle = luma(m[(line * 3 + 1) * 3]);
		const double bottom = luma(m[(line * 3 + 2) * 3]);
		CHECK(middle > top);
		CHECK(middle > bottom);
		CHECK_EQ(top, bottom);
	}

	// And the pattern restarts with each line rather than fading away down the
	// screen, which is what a drifting divisor would do.
	CHECK_EQ(m[0], m[3 * 3]);
}

TEST_CASE("the_scanline_spacing_follows_the_source_not_the_window") {
	// Twice the output height for the same console lines means twice as many
	// output rows per line, and the same number of dark bands. Three rows per
	// line is where a gap first fits at all -- at two there is nowhere to put one
	// without halving the picture, and the mask correctly draws none.
	const int lines = 8;
	const std::vector<std::uint32_t> small = mask(3, lines * 3, lines);
	const std::vector<std::uint32_t> large = mask(3, lines * 6, lines);

	// Halfway between the brightest and darkest row, rather than a number chosen
	// once: the stripe strength moves both ends, and a fixed threshold would turn
	// a tuning change into a mysterious failure here.
	auto darkBands = [](const std::vector<std::uint32_t>& m, int rows) {
		double brightest = 0.0;
		double darkest = 255.0;
		for (int y = 0; y < rows; y++) {
			const double l = luma(m[y * 3]);
			brightest = (l > brightest) ? l : brightest;
			darkest = (l < darkest) ? l : darkest;
		}
		const double threshold = (brightest + darkest) / 2.0;

		int bands = 0;
		bool inBand = false;
		for (int y = 0; y < rows; y++) {
			const bool dark = luma(m[y * 3]) < threshold;
			if (dark && !inBand)
				bands++;
			inBand = dark;
		}
		return bands;
	};
	// The same count either way, which is the point. It is one more than the
	// number of lines rather than equal to it, because a seam straddles a line
	// boundary: the screen opens with the bottom half of one and closes with the
	// top half of another.
	CHECK_EQ(darkBands(small, lines * 3), darkBands(large, lines * 6));
	CHECK_EQ(darkBands(small, lines * 3), lines + 1);
}

TEST_CASE("a_fractional_vertical_scale_does_not_band") {
	// The case the screenshot caught. An 8:7 picture letterboxed into a 720-pixel
	// window is 631 rows for 240 console lines -- 2.63 rows each, so the scanline
	// pattern lands at a different phase on every line. With a ramp that began at
	// a fixed fraction the seams came out at 98, 109 and 116 against a 135 line:
	// wide horizontal bands, which look like a fault in the emulator rather than
	// like a television.
	//
	// A seam's own sampled depth still moves with the phase, and it has to -- rows
	// are where they are. What matters is that the light a line loses is the same
	// for every line, so a window one line tall averages to the same thing
	// wherever it is put. That is banding, stated exactly.
	const int lines = 240;
	const int height = 631;
	const std::vector<std::uint32_t> m = mask(3, height, lines);
	const int window = 3;                         // 2.63 rows per line, rounded

	double brightest = 0.0;
	double darkest = 1e9;
	for (int y = 0; y + window <= height; y++) {
		double sum = 0.0;
		for (int i = 0; i < window; i++)
			sum += luma(m[(y + i) * 3]);
		sum /= window;
		brightest = (sum > brightest) ? sum : brightest;
		darkest = (sum < darkest) ? sum : darkest;
	}
	// 6% at the time of writing, and most of that is this test's own rounding
	// rather than the mask's: a window has to be a whole number of rows and a line
	// here is 2.63 of them, so the window is 14% too tall and picks up part of a
	// neighbouring seam. Point-sampling the same profile instead of integrating it
	// gives 9%, and the ramp this replaced was far worse.
	CAPTURE(darkest);
	CAPTURE(brightest);
	CHECK(darkest > brightest * 0.93);
}

namespace {

/** One console pixel through both stages: lift the colour, average the mask. */
void throughFilter(std::uint32_t colour, double* r, double* g, double* b) {
	const std::uint32_t one[64] = { colour };
	std::uint32_t lifted[64];
	gammaPalette(one, CRT_LIFT, lifted);

	// A 3x3 patch of mask is one console pixel's worth at 3x. Averaging it is
	// what an eye does at a normal viewing distance.
	const std::vector<std::uint32_t> m = mask(CRT_MASK_PITCH, 3, 1);
	*r = 0.0;
	*g = 0.0;
	*b = 0.0;
	for (std::size_t i = 0; i < m.size(); i++) {
		*r += red(lifted[0]) * red(m[i]) / 255.0;
		*g += green(lifted[0]) * green(m[i]) / 255.0;
		*b += blue(lifted[0]) * blue(m[i]) / 255.0;
	}
	*r /= m.size();
	*g /= m.size();
	*b /= m.size();
}

} // namespace

TEST_CASE("a_saturated_colour_keeps_its_hue_through_the_filter") {
	// This is the case that changed the design, and it was found by measuring a
	// screenshot rather than by reasoning. With a linear gain the mask took 42% of
	// Mario's sky but only 16% of its red and green -- because blue was already at
	// 255 and the gain had nowhere to put it, while the other two had room to
	// grow. Blue against red went from 1.77 to 1.22 and the sky turned lavender.
	//
	// A television was dimmer than a monitor and that is fine; what is not fine is
	// being dimmer in one channel than another, because that is a different
	// colour rather than a darker one. So what is checked here is the ratio.
	const std::uint32_t sky = 0xFF9088FFu;          // 144, 136, 255
	double r = 0.0;
	double g = 0.0;
	double b = 0.0;
	throughFilter(sky, &r, &g, &b);

	const double before = 255.0 / 144.0;
	const double after = b / r;
	CAPTURE(before);
	CAPTURE(after);
	CHECK(after > before * 0.85);

	// And the channels stay in the order they started in, which is the crude
	// version of the same question.
	CHECK(b > g);
	CHECK(r > g);
}

TEST_CASE("the_filter_dims_the_picture_without_gutting_it") {
	// What the whole thing costs across the palette. Landing at 1.0 is not the
	// goal -- a mask multiplies, so some light has to go, and a television really
	// was dimmer than this monitor. A third would be gloom, and this is what
	// guards against drifting there.
	const std::uint32_t* palette = nes::Ppu::nesPaletteRgb();
	double plain = 0.0;
	double filtered = 0.0;
	for (int entry = 0; entry < 64; entry++) {
		plain += luma(palette[entry]);
		double r = 0.0;
		double g = 0.0;
		double b = 0.0;
		throughFilter(palette[entry], &r, &g, &b);
		filtered += (r * 299.0 + g * 587.0 + b * 114.0) / 1000.0;
	}
	plain /= 64;
	filtered /= 64;

	CHECK(filtered > plain * 0.60);
	CHECK(filtered < plain * 0.95);
}

TEST_CASE("a_gamma_of_one_changes_nothing_and_the_directions_are_the_right_way_round") {
	// The identity matters because it is the default: somebody who has not touched
	// the brightness control must get exactly the palette the console has.
	std::uint32_t out[64];
	const std::uint32_t* base = nes::Ppu::nesPaletteRgb();
	gammaPalette(base, 1.0f, out);
	for (int i = 0; i < 64; i++) {
		CAPTURE(i);
		CHECK_EQ(out[i], 0xFF000000u | base[i]);
	}

	// And which way is up. An exponent below 1.0 brightens -- the dialog shows the
	// reciprocal, because "gamma 2.0" meaning "darker" would surprise everybody.
	std::uint32_t brighter[64];
	std::uint32_t darker[64];
	gammaPalette(base, 0.5f, brighter);
	gammaPalette(base, 2.0f, darker);
	int brighterWins = 0;
	int darkerWins = 0;
	for (int i = 0; i < 64; i++) {
		if (luma(brighter[i]) > luma(base[i] | 0xFF000000u))
			brighterWins++;
		if (luma(darker[i]) < luma(base[i] | 0xFF000000u))
			darkerWins++;
	}
	// Not all 64: black and white are fixed points under any exponent.
	CHECK(brighterWins > 50);
	CHECK(darkerWins > 50);
}

TEST_CASE("composing_two_gammas_is_one_gamma_of_their_product") {
	// The reason the CRT lift and the brightness control are a single pass. Applying
	// one curve and then another is the same as applying one curve whose exponent is
	// the product, so the emulator multiplies CRT_LIFT by 1/gamma and curves once --
	// half the work and half the rounding.
	//
	// Checked to within a step, because each pass rounds to a byte and two passes
	// round twice; that difference is exactly what doing it once avoids.
	const std::uint32_t* base = nes::Ppu::nesPaletteRgb();
	std::uint32_t once[64];
	std::uint32_t twice[64];
	std::uint32_t middle[64];

	gammaPalette(base, 0.8f * 0.5f, once);
	gammaPalette(base, 0.8f, middle);
	gammaPalette(middle, 0.5f, twice);

	for (int i = 0; i < 64; i++) {
		CAPTURE(i);
		CHECK(std::abs(red(once[i]) - red(twice[i])) <= 2);
		CHECK(std::abs(green(once[i]) - green(twice[i])) <= 2);
		CHECK(std::abs(blue(once[i]) - blue(twice[i])) <= 2);
	}
}

TEST_CASE("the_lift_cannot_clip_and_leaves_the_ends_alone") {
	// The reason it is a curve and not a multiply. White is already at full: a
	// multiply would have to clip it, and clipping is what breaks a hue. A curve
	// through both ends has nothing to clip.
	const std::uint32_t white[64] = { 0xFFFFFFFFu };
	std::uint32_t out[64];
	gammaPalette(white, CRT_LIFT, out);
	CHECK_EQ(red(out[0]), 255);
	CHECK_EQ(green(out[0]), 255);
	CHECK_EQ(blue(out[0]), 255);

	// Black stays black, or every dark scene turns grey.
	const std::uint32_t black[64] = { 0xFF000000u };
	gammaPalette(black, CRT_LIFT, out);
	CHECK_EQ(out[0] & 0x00FFFFFFu, 0u);

	// In between it brightens, monotonically. A curve that crossed itself would
	// turn a gradient into banding.
	std::uint32_t ramp[64];
	for (int i = 0; i < 64; i++) {
		const std::uint32_t v = static_cast<std::uint32_t>(i * 4);
		ramp[i] = 0xFF000000u | (v << 16) | (v << 8) | v;
	}
	gammaPalette(ramp, CRT_LIFT, out);
	for (int i = 1; i < 64; i++) {
		CAPTURE(i);
		CHECK(red(out[i]) >= red(out[i - 1]));
		CHECK(red(out[i]) >= i * 4);
	}
}
