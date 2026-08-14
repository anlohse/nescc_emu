#ifndef NES_FRONTEND_CRT_FILTER_H
#define NES_FRONTEND_CRT_FILTER_H

//
// What a console picture looked like on a television, in two stages.
//
// A CRT did two separate things to a picture, and doing them in the wrong order
// is what makes an imitation look like a grid of coloured squares instead.
//
//   1. It blurred it. The beam was a spot with soft edges, the signal was
//      bandwidth-limited, and the phosphor spread what light it got. Nothing
//      about a television was sharp.
//   2. It multiplied what was left by a mask. Colour came from three phosphor
//      stripes behind a grille, and the beam drew lines with gaps between them.
//
// So: stretch it soft first, then multiply by
//
//   [R  G  B ]
//   [R  G  B ]
//   [r  g  b ]      <- dimmer, where the beam was fading
//
// The important part is *where* each of those lives, and they live in different
// places. The mask is a property of the screen: the stripes were in the glass, at
// a pitch that had nothing to do with what resolution was being displayed. So it
// is built in output pixels and multiplied over the stretched picture, which is
// both more faithful and the reason this works at any window size rather than
// only at 3x.
//
// The blur and the scanlines are the other way around. They are in the *signal*
// -- the gaps are one per line the console drew, and softness is bandwidth -- so
// the scanline spacing follows 240 rather than the window, and the blur is
// measured in console pixels and does not change when the window is resized.
//
// Which is what makes this cheap. The multiply is one blended draw on the GPU and
// the stretch is what a renderer does anyway; the only per-frame arithmetic is
// three taps each way over the 256x240 frame, which is the box that turns the
// renderer's linear stretch into a quadratic one. Nothing is done per *output*
// pixel, so a maximised window costs exactly what a small one does.
//

#include <cstdint>

namespace nesfe {

/**
 * Which screen the mask is imitating. They were different pieces of hardware.
 *
 * A monitor -- and a Trinitron television -- used an aperture grille: continuous
 * vertical red, green and blue stripes running the whole height of the tube,
 * with nothing interrupting them. Most televisions used a slot mask, where the
 * colour columns run straight down just the same but each one is broken into
 * short slots, and the bridges between one column's slots sit half a slot away
 * from its neighbour's:
 *
 *   R  G  B    r  g  b
 *   R  G  B    R  G  B
 *   r  g  b    r  g  b
 *
 * A brick wall, but stacked sideways -- the columns stay in step and the *gaps*
 * alternate. It is the reason a television never looked quite like a monitor
 * showing the same picture.
 *
 * Each kind comes in two pitches, which is a second, independent thing: how many
 * output pixels one triad spans. Three is one pixel per phosphor, which is the
 * coarse, obvious mask. Two puts the same three phosphors in two pixels -- a
 * finer grille on the same screen, which is what a smaller dot pitch was -- and
 * every output pixel then straddles a stripe boundary rather than sitting inside
 * one. Coverage is integrated for that reason, so both pitches come out of the
 * same arithmetic and neither can favour a channel.
 */
enum CrtMaskKind {
	CRT_APERTURE_GRILLE,     /**< unbroken stripes: a monitor, three pixels to a triad */
	CRT_SLOT_MASK,           /**< staggered slots: a television, three pixels to a triad */
	CRT_APERTURE_GRILLE_2,   /**< unbroken stripes, two pixels to a triad */
	CRT_SLOT_MASK_2          /**< staggered slots, two pixels to a triad */
};

/** How many output pixels one red-green-blue triad of the coarse mask spans. */
const int CRT_MASK_PITCH = 3;

/** And of the fine one: three phosphors sharing two pixels. */
const int CRT_FINE_PITCH = 2;

/** The pitch @p kind is drawn at, in output pixels. */
int crtMaskPitch(CrtMaskKind kind);

/**
 * How far a slot mask's bridges are from its neighbouring column's, in lines.
 *
 * Half, which is what makes it a brick wall rather than a grid. In lines rather
 * than in pixels because a slot's height is a scanline's height -- the bridge is
 * where the beam was already fading -- so this shifts the beam and needs no
 * geometry of its own. The beam integral takes a position rather than an index,
 * so half a line costs exactly what a whole one does.
 */
extern const float CRT_STAGGER;

/** How much of the other two channels a stripe removes. 1.0 is pure separation. */
extern const float CRT_STRIPE;

/** How dark the gap between two scanlines is, relative to a line's middle. */
extern const float CRT_SCANLINE;

/**
 * The gamma exponent applied before the mask. Below 1.0 brightens.
 *
 * A multiply can only remove light, so what the mask costs has to be paid in
 * before it -- and a curve is the way to pay, not a multiply. Most NES colours
 * already have a channel at 255, where a multiply has nowhere to put the light
 * and clips: the channel keeps its value while the other two grow, which is not
 * a brighter colour but a different one. A curve through (0,0) and (255,255)
 * lifts the midtones, where a masked picture actually looks gloomy, and cannot
 * clip anything at all.
 */
extern const float CRT_LIFT;

/**
 * How much of a source pixel each of its two neighbours gets, per axis.
 *
 * This is what turns the renderer's linear stretch into a quadratic one, and the
 * number is derived rather than chosen. Stretching linearly reconstructs the
 * picture with a tent -- the order-1 B-spline. Stretching quadratically uses the
 * order-2 one, and B-splines are boxes convolved together, so B2 = B1 * B0: a
 * quadratic stretch *is* a linear stretch of a picture blurred by one more box.
 *
 * Doing that box here, on 256x240 samples, rather than on every pixel of a
 * window, is both far cheaper and more faithful. Softness is bandwidth -- a
 * property of the signal, like the scanlines and unlike the mask -- so it is
 * measured in console pixels and must not change when the window is resized.
 *
 * The weight follows from matching how much the two blur. A tent has variance
 * 1/6 and B2 has 1/4, so the kernel in front of it must carry 1/12; for a
 * symmetric three-tap [t, 1-2t, t] the variance is 2t, which gives t = 1/24.
 * Larger would be a blurrier picture than a quadratic, not a rounder one.
 */
extern const float CRT_SOFTEN;

/**
 * Blur one frame by a source pixel, ready for a linear stretch to finish it.
 *
 * Separable, three taps, and held at the edges rather than run off them --
 * treating what is outside the picture as black would draw a dark border the
 * console never sent.
 *
 * @param picture  width * height pixels of 0xAARRGGBB
 * @param scratch  width * height of working space, owned by the caller so that
 *                 nothing is allocated per frame
 * @param out      width * height; may not overlap @p picture or @p scratch
 */
void softenPicture(const std::uint32_t* picture, int width, int height,
		std::uint32_t* scratch, std::uint32_t* out);

/**
 * Build the mask for a picture @p width by @p height output pixels.
 *
 * @param sourceLines  how many lines the console drew, which is what sets the
 *                     scanline spacing -- 240 for the NES.
 * @param kind         which screen to imitate.
 * @param out          width * height pixels of 0xAARRGGBB, to be multiplied
 *                     over the stretched picture.
 */
void buildCrtMask(int width, int height, int sourceLines, CrtMaskKind kind,
		std::uint32_t* out);

/**
 * The console palette through a gamma curve.
 *
 * One function for two jobs, because they are the same arithmetic: the lift the
 * mask needs paying for, and whatever brightness somebody actually wants. Both
 * are an exponent on each channel, and exponents compose by multiplying --
 * (x^a)^b is x^(ab) -- so a CRT picture with the brightness turned up is one
 * pass with CRT_LIFT / gamma, not two passes and twice the rounding.
 *
 * Below 1.0 brightens. Nothing clips, because a curve through (0,0) and
 * (255,255) has room everywhere between them; that is why this is a curve and
 * not a multiply.
 *
 * @param palette   64 entries of 0xAARRGGBB
 * @param exponent  1.0 leaves the palette alone
 * @param out       64 entries, opaque
 */
void gammaPalette(const std::uint32_t* palette, float exponent,
		std::uint32_t* out);

} // namespace nesfe

#endif // NES_FRONTEND_CRT_FILTER_H
