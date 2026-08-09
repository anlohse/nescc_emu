#ifndef NES_GUI_CONFIG_H
#define NES_GUI_CONFIG_H

#include <SDL.h>

#include <string>

namespace nesgui {

/**
 * Settings for the window front-end, read from a plain text file.
 *
 * Kept beside the executable rather than in a user profile directory, so the
 * released zip stays portable: unpack it anywhere, edit the file next to the
 * program, take the folder with you. Nothing is installed and nothing is left
 * behind.
 *
 * Button names are SDL's own -- "Z", "Right Shift", "Keypad 8" for keys, "a",
 * "dpstart", "dpup" for pad buttons -- because SDL can convert both directions.
 * Inventing a naming scheme would mean maintaining a table that drifts out of
 * step with SDL's, and getting the unusual keys wrong.
 */
struct Config {
	// Indexed the same way everywhere: A, B, Select, Start, Up, Down, Left, Right.
	SDL_Scancode keys[2][8];
	SDL_GameControllerButton padButtons[2][8];

	int scale = 3;
	bool fullscreen = false;
	bool audio = true;

	// Which plugin to use for each job, by id. Empty means "whichever is first",
	// which is also what happens when the named one is not installed -- a config
	// naming a plugin that has since been removed should still start.
	std::string videoPlugin;
	std::string audioPlugin;
	std::string inputPlugin;

	/** The defaults, which are also what gets written on a first run. */
	static Config defaults();

	/**
	 * Where the config lives: beside the executable, falling back to the
	 * working directory if SDL cannot work out where that is.
	 */
	static std::string path();

	/**
	 * Read @p path over the top of these values.
	 *
	 * Unknown keys and unparseable values are reported through @p warn and then
	 * ignored, rather than rejecting the file. A typo in one binding should
	 * cost that binding, not the whole configuration.
	 *
	 * @return false only when the file exists and could not be opened.
	 */
	bool load(const std::string& path, std::string* warnings = nullptr);

	/** Write these values out, with comments explaining each section. */
	bool save(const std::string& path) const;
};

} // namespace nesgui

#endif // NES_GUI_CONFIG_H
