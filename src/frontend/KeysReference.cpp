#include "KeysReference.h"

namespace nesfe {

namespace {

// The order every binding table uses, and the names a player knows them by.
const char* BUTTON_LABELS[8] = {
	"A", "B", "Select", "Start", "Up", "Down", "Left", "Right"
};

/** Two columns, so the values line up under each other. */
std::string row(const std::string& left, const std::string& right) {
	std::string line = "  " + left;
	while (line.size() < 22)
		line += ' ';
	return line + right + "\n";
}

std::string keyName(SDL_Scancode code) {
	const char* name = SDL_GetScancodeName(code);
	// An unbound entry says so rather than showing a blank, which reads as a
	// missing line rather than a deliberate gap.
	if (code == SDL_SCANCODE_UNKNOWN || !name || !*name)
		return "(not bound)";
	return name;
}

std::string padName(SDL_GameControllerButton button) {
	const char* name = SDL_GameControllerGetStringForButton(button);
	if (button == SDL_CONTROLLER_BUTTON_INVALID || !name || !*name)
		return "(not bound)";
	return name;
}

} // namespace

std::string keysReferenceText(const nesgui::Config& config) {
	std::string text;

	for (int port = 0; port < 2; port++) {
		text += "Player " + std::to_string(port + 1) + " -- keyboard\n";
		for (int i = 0; i < 8; i++)
			text += row(BUTTON_LABELS[i], keyName(config.keys[port][i]));
		text += "\n";

		text += "Player " + std::to_string(port + 1) + " -- gamepad\n";
		for (int i = 0; i < 8; i++)
			text += row(BUTTON_LABELS[i], padName(config.padButtons[port][i]));
		text += "\n";
	}

	// Everything below is fixed rather than configurable, and saying so is more
	// use than letting somebody hunt for where to change it. Names come from
	// SDL both times, so what is printed is what will be parsed back.
	text += "Gamepads are picked up in the order they are plugged in. The other\n"
			"face-button diagonal and the left stick always work as well, so a pad\n"
			"is usable whatever is bound above.\n"
			"\n";

	text += "While playing (not rebindable yet)\n";
	text += row("P or Space", "pause");
	text += row("N", "while paused, advance one frame");
	text += row("M", "mute");
	text += row("Tab (held)", "run unthrottled");
	text += row("R", "reset");
	text += row("F12", "save a screenshot");
	text += row("Esc", "quit");
	text += "\n";

	text += "Menus\n";
	text += row("F1", "choose plugins and window size");
	text += row("Ctrl+O", "load a ROM");
	text += row("F5 / F8", "save or load the current slot");
	text += row("Ctrl+Left/Right", "previous or next slot");
	text += "\n";

	text += "All of the player bindings live in nes.cfg beside this program, and\n"
			"can be changed there or from the controller plugin's own dialog.\n";
	return text;
}

} // namespace nesfe
