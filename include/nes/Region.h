#ifndef NES_REGION_H
#define NES_REGION_H

namespace nes {

/**
 * Which television standard the cartridge was built for.
 *
 * This is not a cosmetic flag: it changes the machine underneath. A PAL console
 * runs a different crystal, so the CPU is slower (1.662607 MHz against
 * 1.789773), the PPU draws 312 scanlines instead of 262, and the two chips run
 * at a ratio of 16:5 instead of 3:1. The APU's frame sequencer, noise periods
 * and DMC rates are all retuned to match.
 *
 * The visible consequence is that a PAL game run at NTSC timing plays about 20%
 * too fast with its music pitched up -- which is exactly what European players
 * experienced in reverse, since most PAL releases were straight ports left
 * running slow.
 *
 * Japan and North America both used NTSC, so a Famicom cartridge and its NES
 * counterpart are timing-identical. Region here means PAL versus NTSC, not
 * Japan versus America.
 */
enum class Region {
	Ntsc,
	Pal
};

inline const char* toString(Region region) {
	return region == Region::Pal ? "PAL" : "NTSC";
}

} // namespace nes

#endif // NES_REGION_H
