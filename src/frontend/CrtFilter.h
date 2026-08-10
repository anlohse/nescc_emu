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
// So: stretch with a linear filter first, then multiply by
//
//   [R  G  B ]
//   [R  G  B ]
//   [r  g  b ]      <- dimmer, where the beam was fading
//
// The important part is *where* that mask lives. It is a property of the screen,
// not of the signal -- the stripes were in the glass, at a pitch that had nothing
// to do with what resolution was being displayed. So it is built in output pixels
// and multiplied over the stretched picture, which is both more faithful and the
// reason this works at any window size rather than only at 3x.
//
// Scanlines are the other way around: they *are* in the signal, one per line the
// console drew, so their spacing follows the source rather than the screen.
//
// The multiply is one blended draw on the GPU, and the stretch is what a
// renderer does anyway. Nothing here touches a pixel per frame.
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
 */
enum CrtMaskKind {
	CRT_APERTURE_GRILLE,   /**< unbroken stripes: a monitor, or a Trinitron */
	CRT_SLOT_MASK          /**< staggered slots: an ordinary television */
};

/** How many output pixels one red-green-blue triad of the mask spans. */
const int CRT_MASK_PITCH = 3;

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
 * The console palette, lifted by CRT_LIFT, for use with the mask.
 *
 * @param palette  64 entries of 0xAARRGGBB
 * @param out      64 entries, clamped
 */
void brightenForCrt(const std::uint32_t* palette, std::uint32_t* out);

} // namespace nesfe

#endif // NES_FRONTEND_CRT_FILTER_H
