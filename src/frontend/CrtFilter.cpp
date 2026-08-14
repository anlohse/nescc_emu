#include "CrtFilter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

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

/*
 * 1/24, and the derivation is in the header. It is a small number on purpose:
 * a linear stretch is already most of the blur at these magnifications, and a
 * quadratic one is only a little softer than it. Anything larger would be a
 * blurrier picture rather than a rounder one, which is a different setting from
 * the one being asked for here.
 */
const float CRT_SOFTEN = 1.0f / 24.0f;

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

/**
 * How much of one channel survives in output pixel @p col of a triad.
 *
 * The stripes are continuous and the pixels are not, so this integrates the one
 * over the other instead of handing each pixel a stripe. At a pitch of three the
 * two come to the same thing -- one pixel, one stripe -- which is why it was
 * never needed while three was the only pitch. At a pitch of two it is the whole
 * difference: three stripes have to share two pixels, so every pixel straddles a
 * boundary. Written out by hand that came to red and blue getting a stripe each
 * and green getting half of one twice, which is a fifth less green than red, and
 * a mask that is not achromatic in total does not dim a picture -- it tints it.
 * That is where the violet came from.
 *
 * Integrating cannot get that wrong, because every stripe is the same width: the
 * three channels sum to the same thing over a triad at any pitch, and to the same
 * *mean* too, so changing the pitch changes how fine the mask is and not how
 * bright or what colour.
 */
inline float stripeLevel(int channel, int col, int pitch) {
	// Measured in stripe widths, of which a triad has three and spans pitch pixels.
	const float from = static_cast<float>(col) * 3.0f / pitch;
	const float to = static_cast<float>(col + 1) * 3.0f / pitch;
	const float mineFrom = std::max(from, static_cast<float>(channel));
	const float mineTo = std::min(to, static_cast<float>(channel + 1));
	const float mine = (mineTo > mineFrom) ? mineTo - mineFrom : 0.0f;
	const float elsewhere = (to - from) - mine;
	return (mine + elsewhere * (1.0f - CRT_STRIPE)) / (to - from);
}

} // namespace

int crtMaskPitch(CrtMaskKind kind) {
	return (kind == CRT_APERTURE_GRILLE_2 || kind == CRT_SLOT_MASK_2)
			? CRT_FINE_PITCH : CRT_MASK_PITCH;
}

void buildCrtMask(int width, int height, int sourceLines, CrtMaskKind kind,
		std::uint32_t* out) {
	if (width <= 0 || height <= 0)
		return;

	const int pitch = crtMaskPitch(kind);
	const bool staggered = (kind == CRT_SLOT_MASK || kind == CRT_SLOT_MASK_2);

	// The two axes come from different places, and keeping them apart is what makes
	// this work at any window size. The pitch is horizontal and is measured in
	// screen pixels, because the stripes were in the glass. The beam is vertical
	// and is measured in console lines, because the gaps were in the signal -- so
	// it is divided by however many rows the window gives each of the 240 lines,
	// whether that is a whole number or 2.63.
	const float linesPerRow =
			static_cast<float>(sourceLines) / static_cast<float>(height);

	for (int y = 0; y < height; y++) {
		const float rowTop = y * linesPerRow;
		const float rowBottom = rowTop + linesPerRow;
		const float aligned = beamLevel(rowTop, rowBottom);
		// Half a line down for every other triad column, which is the slot mask's
		// brick bond. It costs nothing: the beam integral takes a position rather
		// than a row index, so an offset one is the same call.
		const float offset = staggered
				? beamLevel(rowTop + CRT_STAGGER, rowBottom + CRT_STAGGER)
				: aligned;

		for (int x = 0; x < width; x++) {
			const int col = x % pitch;
			const float lineLevel = ((x / pitch) % 2 == 1) ? offset : aligned;

			const float r = stripeLevel(0, col, pitch) * lineLevel;
			const float g = stripeLevel(1, col, pitch) * lineLevel;
			const float b = stripeLevel(2, col, pitch) * lineLevel;

			out[static_cast<std::size_t>(y) * width + x] = 0xFF000000u
					| (static_cast<std::uint32_t>(multiplier(r)) << 16)
					| (static_cast<std::uint32_t>(multiplier(g)) << 8)
					| static_cast<std::uint32_t>(multiplier(b));
		}
	}
}

namespace {

/** Fixed point, so the same picture gives the same bytes on every machine. */
const int SOFTEN_ONE = 4096;
const int SOFTEN_SIDE = static_cast<int>(CRT_SOFTEN * SOFTEN_ONE + 0.5f);
const int SOFTEN_MIDDLE = SOFTEN_ONE - 2 * SOFTEN_SIDE;

/** Three samples through the kernel, a channel at a time. */
inline std::uint32_t softened(std::uint32_t before, std::uint32_t here,
		std::uint32_t after) {
	std::uint32_t result = 0xFF000000u;
	for (int shift = 0; shift <= 16; shift += 8) {
		const int value = (static_cast<int>((before >> shift) & 0xFF) * SOFTEN_SIDE
				+ static_cast<int>((here >> shift) & 0xFF) * SOFTEN_MIDDLE
				+ static_cast<int>((after >> shift) & 0xFF) * SOFTEN_SIDE
				+ SOFTEN_ONE / 2) / SOFTEN_ONE;
		result |= static_cast<std::uint32_t>(value) << shift;
	}
	return result;
}

} // namespace

void softenPicture(const std::uint32_t* picture, int width, int height,
		std::uint32_t* scratch, std::uint32_t* out) {
	if (width <= 0 || height <= 0)
		return;

	// Separable, so it is three taps across and three down rather than nine of
	// them -- and both passes run over 256x240 samples, not over a window.
	for (int y = 0; y < height; y++) {
		const std::uint32_t* row = picture + static_cast<std::size_t>(y) * width;
		std::uint32_t* to = scratch + static_cast<std::size_t>(y) * width;
		for (int x = 0; x < width; x++)
			to[x] = softened(row[x > 0 ? x - 1 : 0], row[x],
					row[x + 1 < width ? x + 1 : width - 1]);
	}

	for (int y = 0; y < height; y++) {
		const std::size_t above = static_cast<std::size_t>(y > 0 ? y - 1 : 0) * width;
		const std::size_t here = static_cast<std::size_t>(y) * width;
		const std::size_t below =
				static_cast<std::size_t>(y + 1 < height ? y + 1 : height - 1) * width;
		std::uint32_t* to = out + here;
		for (int x = 0; x < width; x++)
			to[x] = softened(scratch[above + x], scratch[here + x],
					scratch[below + x]);
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
