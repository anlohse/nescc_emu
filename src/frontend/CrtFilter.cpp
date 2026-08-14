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
 * Half a line, because that is what makes a slot mask a brick wall rather than a
 * grid -- not a number with any freedom in it.
 *
 * Being in lines rather than in pixels is what makes this cheap, and it took two
 * attempts to find out. Staggering the colour columns *horizontally* by half a
 * pitch needs a pixel and a half, which no index can express and which leaves
 * offset columns at half the stripe contrast once integrated. Staggering the
 * bridges vertically by half a line asks the beam integral for a position it was
 * always able to give, changes no geometry at all, and is what the hardware
 * actually did.
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

/** One channel, curved. 0 and 255 are fixed points whatever the exponent. */
inline std::uint8_t curved(std::uint32_t channel, float exponent) {
	if (channel == 0)
		return 0;
	const float normal = static_cast<float>(channel) / 255.0f;
	return clamped(std::pow(normal, exponent) * 255.0f);
}

const float PI = 3.14159265f;

/**
 * The running total of beam brightness from the top of line zero to @p lines.
 *
 * The profile is sin(pi * t) repeated once per line: brightest down the middle,
 * dark at the seam. Its integral is what a row of pixels actually receives, and
 * having it in closed form is what lets a row be integrated instead of sampled
 * -- and, since it accepts any position rather than an index, what lets a column
 * of slots be offset by half a line for nothing.
 */
inline float beamTotal(float lines) {
	const float whole = std::floor(lines);
	const float part = lines - whole;
	// Each full line contributes the integral of a half sine, 2/pi.
	return whole * (2.0f / PI) + (1.0f - std::cos(PI * part)) / PI;
}

/** How bright the beam leaves an output row spanning [from, to) of a line. */
inline float beamLevel(float from, float to) {
	const float beam = (beamTotal(to) - beamTotal(from)) / (to - from);
	return CRT_SCANLINE + (1.0f - CRT_SCANLINE) * beam;
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
		const float aligned = beamLevel(rowTop, rowBottom);

		// And the same row again, half a line further down the beam, for the
		// columns whose slots are staggered. Free, because the integral takes a
		// position rather than an index: half a line is no harder to ask for than
		// a whole one.
		const float staggered = (kind == CRT_SLOT_MASK)
				? beamLevel(rowTop + CRT_STAGGER, rowBottom + CRT_STAGGER)
				: aligned;

		for (int x = 0; x < width; x++) {
			// The mask's own pitch, in screen pixels, with nothing to do with what
			// resolution is being shown -- because the stripes were in the glass.
			const int stripe = x % CRT_MASK_PITCH;

			// On a slot mask the colour columns run straight down the tube like a
			// grille's; what alternates is where the bridges between slots fall, so
			// neighbouring triad columns have their dark bands half a line apart.
			// That is the brick wall, on its side.
			const bool offsetColumn = ((x / CRT_MASK_PITCH) & 1) != 0;
			const float lineLevel = offsetColumn ? staggered : aligned;

			const float r = (stripe == 0 ? 1.0f : other) * lineLevel;
			const float g = (stripe == 1 ? 1.0f : other) * lineLevel;
			const float b = (stripe == 2 ? 1.0f : other) * lineLevel;

			out[y * width + x] = 0xFF000000u
					| (static_cast<std::uint32_t>(multiplier(r)) << 16)
					| (static_cast<std::uint32_t>(multiplier(g)) << 8)
					| static_cast<std::uint32_t>(multiplier(b));
		}
	}
}

void gammaPalette(const std::uint32_t* palette, float exponent,
		std::uint32_t* out) {
	// A curve rather than a multiply, and the difference is the whole point. A
	// multiply runs out of room at the top and clips, which is what turned Mario's
	// sky lavender; a curve through (0,0) and (255,255) has room everywhere in
	// between and cannot clip anything. It also puts the light where it is
	// actually missed -- the midtones -- and leaves black alone.
	for (int i = 0; i < 64; i++) {
		const std::uint32_t rgb = palette[i];
		out[i] = 0xFF000000u
				| (static_cast<std::uint32_t>(curved((rgb >> 16) & 0xFF, exponent)) << 16)
				| (static_cast<std::uint32_t>(curved((rgb >> 8) & 0xFF, exponent)) << 8)
				| static_cast<std::uint32_t>(curved(rgb & 0xFF, exponent));
	}
}

} // namespace nesfe
