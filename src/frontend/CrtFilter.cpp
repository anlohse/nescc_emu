#include "CrtFilter.h"

namespace nesfe {

/*
 * These three numbers were measured, not chosen, and the measurement changed the
 * design. See testCrtFilter.cpp, which reproduces it.
 *
 * The first attempt gave each column one channel and nothing else -- a pure
 * aperture grille. It is the honest model of a mask and it costs two thirds of
 * the light, so the obvious repair is gain. Gain does not work: almost every
 * NES colour already has a channel near full, so multiplying clips it instantly.
 * Measured across the whole palette, raising the gain from 2 to 5 bought the
 * picture 6% more brightness and clipped 120 of its 192 channels flat.
 *
 * So the stripes are strong rather than absolute: each column keeps its own
 * channel whole and dims the other two. That is closer to a real mask anyway --
 * stripes bleed into each other and the glass in front mixes them, which is why
 * a television never showed pure primaries at pixel scale. At 0.6 the pattern is
 * plainly visible and the picture keeps about three quarters of its light with a
 * ninth of the clipping.
 */
const float CRT_STRIPE = 0.6f;
const float CRT_GAIN = 1.45f;

// Two thirds rather than a half. A beam does not stop dead between lines, and at
// a half the picture reads as venetian blinds rather than as a television.
const float CRT_SCANLINE = 0.66f;

namespace {

inline std::uint8_t clamped(float value) {
	if (value <= 0.0f)
		return 0;
	return static_cast<std::uint8_t>(value > 255.0f ? 255.0f : value);
}

} // namespace

void crtExpand(const std::uint8_t* indices, int width, int height,
		const std::uint32_t* palette, std::uint32_t* out) {
	const int outWidth = width * CRT_SCALE;
	const float other = 1.0f - CRT_STRIPE;

	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			const std::uint32_t rgb = palette[indices[y * width + x] & 0x3F];
			const float channel[3] = {
				static_cast<float>((rgb >> 16) & 0xFF) * CRT_GAIN,
				static_cast<float>((rgb >> 8) & 0xFF) * CRT_GAIN,
				static_cast<float>(rgb & 0xFF) * CRT_GAIN
			};

			std::uint32_t bright[CRT_SCALE];
			std::uint32_t dim[CRT_SCALE];
			for (int column = 0; column < CRT_SCALE; column++) {
				// This column's own channel at full strength, the other two
				// dimmed -- which is what makes three stripes read as one
				// colour from any normal distance.
				const float r = channel[0] * (column == 0 ? 1.0f : other);
				const float g = channel[1] * (column == 1 ? 1.0f : other);
				const float b = channel[2] * (column == 2 ? 1.0f : other);

				bright[column] = 0xFF000000u
						| (static_cast<std::uint32_t>(clamped(r)) << 16)
						| (static_cast<std::uint32_t>(clamped(g)) << 8)
						| static_cast<std::uint32_t>(clamped(b));
				dim[column] = 0xFF000000u
						| (static_cast<std::uint32_t>(clamped(r * CRT_SCANLINE)) << 16)
						| (static_cast<std::uint32_t>(clamped(g * CRT_SCANLINE)) << 8)
						| static_cast<std::uint32_t>(clamped(b * CRT_SCANLINE));
			}

			std::uint32_t* cell = out + (y * CRT_SCALE) * outWidth + x * CRT_SCALE;
			for (int column = 0; column < CRT_SCALE; column++) {
				cell[column] = bright[column];
				cell[outWidth + column] = bright[column];
				cell[2 * outWidth + column] = dim[column];
			}
		}
	}
}

} // namespace nesfe
