#include "GuiConfig.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace nesgui {

namespace {

// The order every binding array uses, and the names they go by in the file.
const char* BUTTON_NAMES[8] = {
	"a", "b", "select", "start", "up", "down", "left", "right"
};

std::string trim(const std::string& s) {
	std::size_t begin = 0;
	std::size_t end = s.size();
	while (begin < end && std::isspace(static_cast<unsigned char>(s[begin])))
		begin++;
	while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])))
		end--;
	return s.substr(begin, end - begin);
}

std::string lower(const std::string& s) {
	std::string out = s;
	for (char& c : out)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return out;
}

/** Which of the eight buttons a name refers to, or -1. */
int buttonIndex(const std::string& name) {
	for (int i = 0; i < 8; i++)
		if (name == BUTTON_NAMES[i])
			return i;
	return -1;
}

bool parseBool(const std::string& value, bool* out) {
	const std::string v = lower(value);
	if (v == "true" || v == "yes" || v == "on" || v == "1") { *out = true; return true; }
	if (v == "false" || v == "no" || v == "off" || v == "0") { *out = false; return true; }
	return false;
}

void warn(std::string* warnings, const std::string& message) {
	if (!warnings)
		return;
	if (!warnings->empty())
		*warnings += "\n";
	*warnings += message;
}

} // namespace

Config Config::defaults() {
	Config c;

	const SDL_Scancode player1[8] = {
		SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RETURN,
		SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT
	};
	const SDL_Scancode player2[8] = {
		SDL_SCANCODE_KP_1, SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_PLUS, SDL_SCANCODE_KP_ENTER,
		SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_5, SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_6
	};
	const SDL_GameControllerButton pad[8] = {
		SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_X,
		SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START,
		SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
		SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT
	};

	for (int i = 0; i < 8; i++) {
		c.keys[0][i] = player1[i];
		c.keys[1][i] = player2[i];
		c.padButtons[0][i] = pad[i];
		c.padButtons[1][i] = pad[i];
	}
	return c;
}

std::string Config::path() {
	char* base = SDL_GetBasePath();
	if (!base)
		return "nes.cfg";
	std::string result = std::string(base) + "nes.cfg";
	SDL_free(base);
	return result;
}

bool Config::load(const std::string& file, std::string* warnings) {
	std::ifstream is(file.c_str());
	if (!is)
		return true;   // no file yet; the defaults stand

	std::string section;
	std::string line;
	int lineNumber = 0;

	while (std::getline(is, line)) {
		lineNumber++;
		const std::size_t comment = line.find_first_of("#;");
		if (comment != std::string::npos)
			line = line.substr(0, comment);
		line = trim(line);
		if (line.empty())
			continue;

		if (line.front() == '[' && line.back() == ']') {
			section = lower(trim(line.substr(1, line.size() - 2)));
			continue;
		}

		const std::size_t equals = line.find('=');
		if (equals == std::string::npos) {
			warn(warnings, file + ":" + std::to_string(lineNumber) + ": expected key = value");
			continue;
		}
		const std::string key = lower(trim(line.substr(0, equals)));
		const std::string value = trim(line.substr(equals + 1));
		const std::string where = file + ":" + std::to_string(lineNumber) + ": ";

		if (section == "video") {
			if (key == "scale") {
				const int scaleValue = std::atoi(value.c_str());
				if (scaleValue >= 1 && scaleValue <= 8)
					scale = scaleValue;
				else
					warn(warnings, where + "scale must be 1..8");
			} else if (key == "fullscreen") {
				if (!parseBool(value, &fullscreen))
					warn(warnings, where + "fullscreen must be true or false");
			} else if (key == "plugin") {
				videoPlugin = value;
			} else {
				warn(warnings, where + "unknown setting '" + key + "'");
			}
			continue;
		}

		if (section == "audio") {
			if (key == "enabled") {
				if (!parseBool(value, &audio))
					warn(warnings, where + "enabled must be true or false");
			} else if (key == "plugin") {
				audioPlugin = value;
			} else {
				warn(warnings, where + "unknown setting '" + key + "'");
			}
			continue;
		}

		if (section == "input") {
			if (key == "plugin")
				inputPlugin = value;
			else
				warn(warnings, where + "unknown setting '" + key + "'");
			continue;
		}

		const bool isKeyboard = (section == "keyboard1" || section == "keyboard2");
		const bool isPad = (section == "pad1" || section == "pad2");
		if (!isKeyboard && !isPad) {
			warn(warnings, where + "setting outside any known section");
			continue;
		}

		const int port = (section.back() == '2') ? 1 : 0;
		const int index = buttonIndex(key);
		if (index < 0) {
			warn(warnings, where + "'" + key + "' is not one of a, b, select, start, "
					"up, down, left, right");
			continue;
		}

		if (isKeyboard) {
			// SDL knows every key by name, including the awkward ones, and the
			// same names it prints are the ones it parses.
			const SDL_Scancode code = SDL_GetScancodeFromName(value.c_str());
			if (code == SDL_SCANCODE_UNKNOWN)
				warn(warnings, where + "'" + value + "' is not a key name SDL knows");
			else
				keys[port][index] = code;
		} else {
			const SDL_GameControllerButton button =
					SDL_GameControllerGetButtonFromString(value.c_str());
			if (button == SDL_CONTROLLER_BUTTON_INVALID)
				warn(warnings, where + "'" + value + "' is not a gamepad button SDL knows");
			else
				padButtons[port][index] = button;
		}
	}
	return true;
}

bool Config::save(const std::string& file) const {
	std::ofstream os(file.c_str(), std::ofstream::trunc);
	if (!os)
		return false;

	os << "# nes configuration.\n"
	   << "#\n"
	   << "# Written automatically the first time the emulator runs. Edit freely;\n"
	   << "# delete the file to get these defaults back. Command-line options win\n"
	   << "# over anything set here.\n"
	   << "#\n"
	   << "# Key names are SDL's own: \"Z\", \"Right Shift\", \"Keypad 8\", \"Return\".\n"
	   << "# Pad button names are SDL's too: a, b, x, y, back, start, dpup, dpdown,\n"
	   << "# dpleft, dpright, leftshoulder, rightshoulder.\n"
	   << "\n";

	os << "# Each job is done by a plugin. Leaving one blank takes whichever is\n"
	   << "# first, which is also what happens if the named one is not installed.\n"
	   << "\n";

	os << "[video]\n"
	   << "plugin = " << videoPlugin << "\n"
	   << "scale = " << scale << "\n"
	   << "fullscreen = " << (fullscreen ? "true" : "false") << "\n"
	   << "\n";

	os << "[audio]\n"
	   << "plugin = " << audioPlugin << "\n"
	   << "enabled = " << (audio ? "true" : "false") << "\n"
	   << "\n";

	os << "[input]\n"
	   << "plugin = " << inputPlugin << "\n"
	   << "\n";

	for (int port = 0; port < 2; port++) {
		os << "[keyboard" << (port + 1) << "]\n";
		for (int i = 0; i < 8; i++) {
			const char* name = SDL_GetScancodeName(keys[port][i]);
			os << BUTTON_NAMES[i] << " = " << (name && *name ? name : "") << "\n";
		}
		os << "\n";
	}

	os << "# Gamepads are picked up in the order they are plugged in. The other\n"
	   << "# face-button diagonal and the left stick are always accepted as well,\n"
	   << "# so a pad still works sensibly whatever is bound here.\n";
	for (int port = 0; port < 2; port++) {
		os << "[pad" << (port + 1) << "]\n";
		for (int i = 0; i < 8; i++) {
			const char* name = SDL_GameControllerGetStringForButton(padButtons[port][i]);
			os << BUTTON_NAMES[i] << " = " << (name ? name : "") << "\n";
		}
		os << "\n";
	}

	return static_cast<bool>(os);
}

} // namespace nesgui
