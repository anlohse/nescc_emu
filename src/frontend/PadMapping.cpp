#include "PadMapping.h"

#include <SDL.h>

#include <algorithm>

namespace nesfe {

namespace {

/** Append "name:bN," if that button exists and is not already spoken for. */
void bindButton(std::string* to, const char* name, int button, int limit) {
	if (button < 0 || button >= limit)
		return;
	*to += name;
	*to += ":b";
	*to += std::to_string(button);
	*to += ",";
}

/**
 * How many buttons a pad needs before Select and Start are worth reserving.
 *
 * Six: four face buttons and the two above them. Below that the face buttons
 * keep the low indices and Select and Start go unbound, which is the right way
 * round -- a four button pad with no Start can still be bound to something
 * playable, whereas one whose Start and A are the same physical button gives a
 * player a control that does two things at once and no way to tell.
 */
const int ENOUGH_FOR_SELECT_AND_START = 6;

} // namespace

std::string guessPadMapping(const char* guid, const char* name,
		int buttons, int axes, int hats) {
	std::string label = (name && *name) ? name : "Generic gamepad";
	std::replace(label.begin(), label.end(), ',', ' ');

	std::string mapping = guid ? guid : "";
	mapping += ",";
	mapping += label;
	mapping += ",platform:";
	mapping += SDL_GetPlatform();
	mapping += ",";

	int faces = buttons;
	if (buttons >= ENOUGH_FOR_SELECT_AND_START) {
		faces = buttons - 2;
		bindButton(&mapping, "back", buttons - 2, buttons);
		bindButton(&mapping, "start", buttons - 1, buttons);
	}

	bindButton(&mapping, "a", 0, faces);
	bindButton(&mapping, "b", 1, faces);
	bindButton(&mapping, "x", 2, faces);
	bindButton(&mapping, "y", 3, faces);
	bindButton(&mapping, "leftshoulder", 4, faces);
	bindButton(&mapping, "rightshoulder", 5, faces);
	// As buttons rather than as axes, because a pad of this kind has the clicky
	// sort. A binding that called them axes would read as half-pressed for ever.
	bindButton(&mapping, "lefttrigger", 6, faces);
	bindButton(&mapping, "righttrigger", 7, faces);

	if (hats > 0) {
		mapping += "dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,";
		if (axes >= 2)
			mapping += "leftx:a0,lefty:a1,";
		if (axes >= 4)
			mapping += "rightx:a2,righty:a3,";
	} else if (axes >= 2) {
		mapping += "dpup:-a1,dpdown:+a1,dpleft:-a0,dpright:+a0,";
		if (axes >= 4)
			mapping += "rightx:a2,righty:a3,";
	}
	return mapping;
}

int mapUnknownPads() {
	int added = 0;
	for (int i = 0; i < SDL_NumJoysticks(); i++) {
		// A device SDL already knows is left alone. Its own mapping came from
		// somebody who had the pad in their hands, and a guess must never
		// displace that.
		if (SDL_IsGameController(i))
			continue;

		SDL_Joystick* stick = SDL_JoystickOpen(i);
		if (!stick)
			continue;
		char guid[64] = { 0 };
		SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(stick), guid, sizeof guid);
		const int buttons = SDL_JoystickNumButtons(stick);
		const std::string mapping = guessPadMapping(guid,
				SDL_JoystickNameForIndex(i), buttons,
				SDL_JoystickNumAxes(stick), SDL_JoystickNumHats(stick));
		SDL_JoystickClose(stick);

		// A device with almost nothing on it is not a gamepad -- pedals, dongles
		// and odd HID collections turn up in this list too, and inventing
		// controls for them would put entries in the dialog that can never be
		// pressed.
		if (buttons < 2)
			continue;

		if (SDL_GameControllerAddMapping(mapping.c_str()) >= 0) {
			added++;
			SDL_Log("gamepad \"%s\" had no mapping; guessed one from its %d "
					"buttons", SDL_JoystickNameForIndex(i), buttons);
		} else {
			SDL_Log("gamepad \"%s\" has no mapping and one could not be added: %s",
					SDL_JoystickNameForIndex(i), SDL_GetError());
		}
	}
	return added;
}

} // namespace nesfe
