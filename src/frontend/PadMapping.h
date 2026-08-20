#ifndef NES_FRONTEND_PAD_MAPPING_H
#define NES_FRONTEND_PAD_MAPPING_H

//
// Making SDL admit that a gamepad exists.
//
// SDL has two ways to read a pad. The joystick API reports whatever the device
// says -- button 7, axis 1, hat 0 -- and the game controller API reports named
// controls, "a" and "start" and "dpleft", which is what bindings are written
// against and what every part of this emulator uses.
//
// The catch is that the second one only works for devices SDL has a *mapping*
// for. Mappings live in a table compiled into SDL, keyed by the device's GUID,
// and that table covers the pads somebody thought to add. A generic USB pad --
// the cheap kind, often shaped like a SNES controller, reporting itself as
// nothing more descriptive than "USB gamepad" -- is usually not in it. For such
// a device SDL_IsGameController returns false, SDL_GameControllerOpen refuses,
// and every list of pads in the program is empty. The pad works perfectly; the
// program simply never asks it anything.
//
// That is a bad failure to leave in place, because it is invisible from the
// outside: the pad lights up, Windows tests it, other games use it, and this
// emulator shows nothing. So rather than requiring the world's mappings to
// contain a device before it can be used, a mapping is written for it here, from
// what the device itself reports.
//
// A guessed mapping puts the *names* in doubt, not the pad. Whichever button
// SDL ends up calling "x" may be labelled something else on the plastic. That
// costs nothing here, because nothing in this emulator assumes a layout: the
// bindings dialog captures whatever button is actually pressed and stores that.
// Getting the device visible is the whole problem; which name it arrives under
// is the player's to decide.
//

#include <string>

namespace nesfe {

/**
 * An SDL mapping string for a pad, from nothing but what it reports.
 *
 * Separate from the SDL plumbing, and pure, so the guess can be argued with in a
 * test instead of only in somebody's hands. Two rules shape it:
 *
 * Select and Start are the last two buttons on essentially every pad of this
 * kind, so they are taken from the top -- but only when there are enough buttons
 * left underneath for the face controls. On a small pad the face buttons win,
 * because a pad with four buttons and no Start is usable and a pad whose Start
 * and A are the same button is not.
 *
 * The directions come from a hat when there is one and from the first two axes
 * when there is not, because a pad with no hat puts its d-pad there. In that
 * case the axes are *not* also reported as a stick: it would be one control
 * answering to two names, and since a stick is already accepted as a direction,
 * every press would arrive twice.
 *
 * @param guid     the device's GUID, as SDL prints it
 * @param name     the device's reported name; commas are replaced, since they
 *                 would split the mapping's own fields
 * @param buttons  how many buttons, axes and hats the device reports
 */
std::string guessPadMapping(const char* guid, const char* name,
		int buttons, int axes, int hats);

/**
 * Give every attached joystick SDL has no mapping for a plausible one.
 *
 * Safe to call as often as convenient -- a device that SDL already presents as
 * a game controller is left exactly as it is, so this never overrides a real
 * mapping with a guess, and never disturbs a pad that already worked.
 *
 * @return how many devices were given a mapping.
 */
int mapUnknownPads();

} // namespace nesfe

#endif // NES_FRONTEND_PAD_MAPPING_H
