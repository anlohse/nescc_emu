#include "CrtFilter.h"

#include <cmath>

namespace nesfe {

/*
 * Measured, not chosen. See testCrtFilter.cpp.
 *
 * The stripe is weak on purpose, and the reason is a measurement. At 0.55 the
 * mask took 42% of Mario's sky -- but only 16% of its red and green, because
 * blue was already at 255 and a linear gain had nowhere to put it. The sky went
 * from blue to lavender. Anything a mask takes evenly is just a dimmer
 * television, which is right; what is wrong is taking it unevenly, and clipping
 * is what makes it uneven. Weak stripes clip less, and are closer to a real
 * mask anyway: phosphor stripes bleed into each other and the glass mixes them.
 *
 * The scanline can be much stronger than the stripe because it is achromatic --
 * it dims all three channels together, so it cannot shift a hue however deep it
 * goes. It carries most of the effect for that reason.
 */
const float CRT_STRIPE = 0.28f;
const float CRT_SCANLINE = 0.55f;
const float CRT_LIFT = 0.80f;

/*
 * Half a pitch, because that is what a hexagonal lattice is -- not a number with
 * any freedom in it.
 *
 * A third was tried and rejected, and the reason is worth recording because it
 * is tempting. A third of three pixels is a whole pixel, so an offset row keeps
 * full stripe contrast and the brick pattern is far more obvious; half of three
 * is a pixel and a half, so a staggered pixel straddles two stripes, gets a
 * share of each, and comes out at half the contrast. The obvious-looking one is
 * the wrong lattice, and choosing between them is a matter of taste rather than
 * of correctness -- this is the softer and more faithful of the two.
 */
const float CRT_STAGGER = 0.5f;

namespace {

inline std::uint8_t clamped(float value) {
	if (value <= 0.0f)
		return 0;
	return static_cast<std::uint8_t>(value > 255.0f ? 255.0f : value);
}

inline std::uint8_t multiplier(float fraction) {
	return clamped(fraction * 255.0f);
}

/** One channel, curved by CRT_LIFT. 0 and 255 are fixed points. */
inline std::uint8_t lifted(std::uint32_t channel) {
	if (channel == 0)
		return 0;
	const float normal = static_cast<float>(channel) / 255.0f;
	return clamped(std::pow(normal, CRT_LIFT) * 255.0f);
}

const float PI = 3.14159265f;

/**
 * The running total of one phosphor stripe's width from zero to @p at.
 *
 * The stripe for a channel is a band one pixel wide repeating every pitch.
 * Accumulating it in closed form is what lets a stripe be integrated across an
 * output pixel rather than sampled once in it -- which is in turn what lets a
 * shadow mask be offset by half a pitch, since half of three pixels is not a
 * whole number of pixels and no shift of an index could express it.
 */
inline float stripeTotal(int channel, float at) {
	const float pitch = static_cast<float>(CRT_MASK_PITCH);
	const float whole = std::floor(at / pitch);
	const float within = at - whole * pitch;              // 0 .. pitch
	const float part = within - static_cast<float>(channel);
	return whole + ((part < 0.0f) ? 0.0f : ((part > 1.0f) ? 1.0f : part));
}

/** How much of the output pixel spanning [from, to) one stripe covers. */
inline float stripeCoverage(int channel, float from, float to) {
	return stripeTotal(channel, to) - stripeTotal(channel, from);
}

/**
 * The running total of beam brightness from the top of line zero to @p lines.
 *
 * The profile is sin(pi * t) repeated once per line: brightest down the middle,
 * dark at the seam. Its integral is what a row of pixels actually receives, and
 * having it in closed form is what lets a row be integrated instead of sampled.
 */
inline float beamTotal(float lines) {
	const float whole = std::floor(lines);
	const float part = lines - whole;
	// Each full line contributes the integral of a half sine, 2/pi.
	return whole * (2.0f / PI) + (1.0f - std::cos(PI * part)) / PI;
}

} // namespace

void buildCrtMask(int width, int height, int sourceLines, CrtMaskKind kind,
		std::uint32_t* out) {
	const float other = 1.0f - CRT_STRIPE;

	// How many output rows one console line covers -- three at 3x, and something
	// awkward like 2.63 once an 8:7 picture has been letterboxed into a window.
	const float rowsPerLine = (sourceLines > 0)
			? static_cast<float>(height) / static_cast<float>(sourceLines)
			: 1.0f;
	const float linesPerRow = 1.0f / rowsPerLine;

	for (int y = 0; y < height; y++) {
		// The beam, brightest down the middle of its line and darkest at the seam
		// with the next one -- and *integrated* over the row rather than sampled
		// once in it, because a row of pixels covers a span of the line and
		// receives all of it.
		//
		// That distinction is the whole of this function's difficulty. At a
		// fractional scale every line meets the rows at a different phase, so a
		// single sample per row lands somewhere different each time and the error
		// shows up as wide horizontal bands. Measured on a screenshot: seams at
		// 98, 109 and 116 against a 135 line, which looks like a fault in the
		// emulator rather than like a television. An integral over the row has no
		// phase to be wrong about -- every line loses the same light whatever the
		// scale -- and the closed form costs two cosines.
		const float rowTop = static_cast<float>(y) * linesPerRow;
		const float rowBottom = rowTop + linesPerRow;
		const float beam =
				(beamTotal(rowBottom) - beamTotal(rowTop)) / linesPerRow;
		const float lineLevel = CRT_SCANLINE + (1.0f - CRT_SCANLINE) * beam;

		// Which row of triads this output row falls in, and how far across that
		// row is from the one above it. Every other row on a shadow mask, none of
		// them on a grille.
		const int triadRow = y / CRT_MASK_ROW;
		const float shift = (kind == CRT_SHADOW_MASK && (triadRow & 1) != 0)
				? CRT_STAGGER * static_cast<float>(CRT_MASK_PITCH)
				: 0.0f;

		for (int x = 0; x < width; x++) {
			// The stripes, integrated across the pixel rather than sampled in it.
			// With no shift that comes out exactly as it would from an index --
			// one channel full and two at `other` -- and with half a pitch of
			// shift it comes out as two halves, because that is what the pixel is
			// actually looking at.
			//
			// The three coverages always sum to one whatever the shift, so a
			// staggered row costs no more light than an aligned one. That is what
			// stops the brick pattern reading as horizontal banding.
			const float from = static_cast<float>(x) + shift;
			const float to = from + 1.0f;
			const float r = (other + CRT_STRIPE * stripeCoverage(0, from, to))
					* lineLevel;
			const float g = (other + CRT_STRIPE * stripeCoverage(1, from, to))
					* lineLevel;
			const float b = (other + CRT_STRIPE * stripeCoverage(2, from, to))
					* lineLevel;

			out[y * width + x] = 0xFF000000u
					| (static_cast<std::uint32_t>(multiplier(r)) << 16)
					| (static_cast<std::uint32_t>(multiplier(g)) << 8)
					| static_cast<std::uint32_t>(multiplier(b));
		}
	}
}

void brightenForCrt(const std::uint32_t* palette, std::uint32_t* out) {
	// A gamma lift rather than a multiply, and the difference is the whole point.
	// A multiply runs out of room at the top and clips, which is what turned the
	// sky lavender; a curve through (0,0) and (255,255) has room everywhere in
	// between and cannot clip anything. It also puts the light where it is
	// actually missed -- the midtones, which is where a masked picture looks
	// gloomy -- and leaves black alone.
	for (int i = 0; i < 64; i++) {
		const std::uint32_t rgb = palette[i];
		out[i] = 0xFF000000u
				| (static_cast<std::uint32_t>(lifted((rgb >> 16) & 0xFF)) << 16)
				| (static_cast<std::uint32_t>(lifted((rgb >> 8) & 0xFF)) << 8)
				| static_cast<std::uint32_t>(lifted(rgb & 0xFF));
	}
}

} // namespace nesfe
