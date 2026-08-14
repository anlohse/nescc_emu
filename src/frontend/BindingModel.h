#ifndef NES_FRONTEND_BINDING_MODEL_H
#define NES_FRONTEND_BINDING_MODEL_H

//
// The controller bindings as a table, with no dialog around them.
//
// Four groups of eight: a keyboard layout and a gamepad layout for each of the
// two ports. Everything a rebinding screen has to get right -- what a binding
// currently reads as, what happens when a key is already used by something
// else, what Cancel means -- is here and is tested. The window is elsewhere and
// decides nothing.
//

#include "../GuiConfig.h"

#include <string>

namespace nesfe {

class BindingModel {
public:
	enum Group {
		KEYBOARD_1 = 0,
		KEYBOARD_2 = 1,
		PAD_1      = 2,
		PAD_2      = 3,
		GROUP_COUNT = 4
	};

	static const int BUTTON_COUNT = 8;

	explicit BindingModel(const nesgui::Config& config);

	static const char* groupLabel(int group);
	/** A, B, Select, Start, Up, Down, Left, Right -- the order used everywhere. */
	static const char* buttonLabel(int button);
	static bool isPad(int group) { return group >= PAD_1; }

	/**
	 * What this binding reads as, in SDL's own names.
	 *
	 * SDL converts both directions, so a name shown here is exactly the name
	 * that will be written to the file and read back from it. Inventing a
	 * second naming scheme would mean a table that drifts out of step with
	 * SDL's and gets the unusual keys wrong.
	 */
	std::string bindingName(int group, int button) const;

	/** True when nothing is bound -- shown as "(none)" rather than blank. */
	bool isBound(int group, int button) const;

	/**
	 * Bind a key, taking it from whatever else in the same group had it.
	 *
	 * One key driving two NES buttons is never what someone meant, and finding
	 * out by playing is worse than watching the old binding disappear.
	 */
	void bindKey(int group, int button, SDL_Scancode code);
	void bindPad(int group, int button, SDL_GameControllerButton padButton);

	void clear(int group, int button);
	void restoreDefaults();

	/**
	 * Which device a console port reads, and which gamepad if it reads one.
	 *
	 * Chosen here rather than detected, so that a port with no pad attached to its
	 * chosen slot simply reads nothing. That is the honest outcome and it is
	 * visible in the dialog, where an absent gamepad is still listed and marked as
	 * absent -- guessing instead is what made a phantom controller from a twin
	 * adapter steal player one.
	 */
	nesgui::InputDevice deviceFor(int port) const { return m_config.device[port]; }
	int gamepadFor(int port) const { return m_config.gamepad[port]; }
	void selectKeyboard(int port);
	void selectGamepad(int port, int gamepad);

	/** The group holding the bindings a port would use as it is configured. */
	int groupFor(int port) const {
		return (m_config.device[port] == nesgui::PORT_GAMEPAD)
				? (PAD_1 + port) : port;
	}

	bool changed() const { return m_changed; }

	/** Write the bindings into @p config, leaving the rest of it alone. */
	void apply(nesgui::Config* config) const;

private:
	int portOf(int group) const { return (group & 1); }

	nesgui::Config m_config;
	bool m_changed;
};

} // namespace nesfe

#endif // NES_FRONTEND_BINDING_MODEL_H
