#ifndef NES_FRONTEND_CRT_FILTER_H
#define NES_FRONTEND_CRT_FILTER_H

//
// What a console picture looked like on a television.
//
// A CRT never showed a grid of flat squares. Colour came from three phosphor
// stripes behind a mask, so a white pixel was a red stripe beside a green one
// beside a blue one, and the eye did the mixing. And the beam drew lines with
// gaps between them, so brightness fell away at the bottom of each one.
//
// Both of those are reproducible without a shader, which matters because
// SDL_Renderer does not portably offer one: each console pixel becomes a 3x3
// cell, the three columns carry red, green and blue, and the last row is dimmer.
//
//   [R  G  B ]
//   [R  G  B ]
//   [r  g  b ]      <- dimmer, where the beam was fading
//
// Each column keeps its own channel whole and dims the other two rather than
// dropping them entirely. That is a measurement rather than a preference: pure
// separation costs two thirds of the light and gain cannot buy it back, because
// almost every NES colour already has a channel near full and multiplying clips
// it. Strong stripes keep the pattern and three quarters of the brightness. The
// numbers, and the measurement, are in CrtFilter.cpp and its test.
//

#include <cstdint>

namespace nesfe {

/** Every console pixel becomes this many across and down. */
const int CRT_SCALE = 3;

/**
 * How much of the other two channels a column loses. 1.0 is pure separation.
 */
extern const float CRT_STRIPE;

/** A modest lift, to pay back part of what the mask takes. */
extern const float CRT_GAIN;

/** How bright the last row of each cell is, relative to the first two. */
extern const float CRT_SCANLINE;

/**
 * Expand @p indices into @p out, which must hold (width * CRT_SCALE) by
 * (height * CRT_SCALE) pixels.
 *
 * @param indices  console pixels, 0-63 palette entries
 * @param palette  64 entries of 0xAARRGGBB
 * @param out      destination, row-major, same pixel format as the palette
 */
void crtExpand(const std::uint8_t* indices, int width, int height,
		const std::uint32_t* palette, std::uint32_t* out);

} // namespace nesfe

#endif // NES_FRONTEND_CRT_FILTER_H
