#include "BindingModel.h"

namespace nesfe {

namespace {

const char* GROUP_LABELS[BindingModel::GROUP_COUNT] = {
	"Player 1 keyboard",
	"Player 2 keyboard",
	"Player 1 gamepad",
	"Player 2 gamepad"
};

const char* BUTTON_LABELS[BindingModel::BUTTON_COUNT] = {
	"A", "B", "Select", "Start", "Up", "Down", "Left", "Right"
};

} // namespace

BindingModel::BindingModel(const nesgui::Config& config) :
		m_config(config), m_changed(false) { }

const char* BindingModel::groupLabel(int group) {
	return GROUP_LABELS[group];
}

const char* BindingModel::buttonLabel(int button) {
	return BUTTON_LABELS[button];
}

bool BindingModel::isBound(int group, int button) const {
	if (isPad(group))
		return m_config.padButtons[portOf(group)][button] != SDL_CONTROLLER_BUTTON_INVALID;
	return m_config.keys[portOf(group)][button] != SDL_SCANCODE_UNKNOWN;
}

std::string BindingModel::bindingName(int group, int button) const {
	if (!isBound(group, button))
		return "(none)";

	if (isPad(group)) {
		const char* name = SDL_GameControllerGetStringForButton(
				m_config.padButtons[portOf(group)][button]);
		return name ? name : "(none)";
	}
	const char* name = SDL_GetScancodeName(m_config.keys[portOf(group)][button]);
	return (name && *name) ? name : "(none)";
}

void BindingModel::bindKey(int group, int button, SDL_Scancode code) {
	if (isPad(group) || code == SDL_SCANCODE_UNKNOWN)
		return;
	const int port = portOf(group);

	// Take it from whatever else in this group already had it. Only within the
	// group: the two ports are separate controllers, and a player who wants
	// both on one keyboard is entitled to overlap them if they insist.
	for (int i = 0; i < BUTTON_COUNT; i++)
		if (i != button && m_config.keys[port][i] == code)
			m_config.keys[port][i] = SDL_SCANCODE_UNKNOWN;

	if (m_config.keys[port][button] != code) {
		m_config.keys[port][button] = code;
		m_changed = true;
	}
}

void BindingModel::bindPad(int group, int button, SDL_GameControllerButton padButton) {
	if (!isPad(group) || padButton == SDL_CONTROLLER_BUTTON_INVALID)
		return;
	const int port = portOf(group);

	for (int i = 0; i < BUTTON_COUNT; i++)
		if (i != button && m_config.padButtons[port][i] == padButton)
			m_config.padButtons[port][i] = SDL_CONTROLLER_BUTTON_INVALID;

	if (m_config.padButtons[port][button] != padButton) {
		m_config.padButtons[port][button] = padButton;
		m_changed = true;
	}
}

void BindingModel::clear(int group, int button) {
	if (!isBound(group, button))
		return;
	if (isPad(group))
		m_config.padButtons[portOf(group)][button] = SDL_CONTROLLER_BUTTON_INVALID;
	else
		m_config.keys[portOf(group)][button] = SDL_SCANCODE_UNKNOWN;
	m_changed = true;
}

void BindingModel::restoreDefaults() {
	const nesgui::Config defaults = nesgui::Config::defaults();
	for (int port = 0; port < 2; port++) {
		for (int i = 0; i < BUTTON_COUNT; i++) {
			if (m_config.keys[port][i] != defaults.keys[port][i]
					|| m_config.padButtons[port][i] != defaults.padButtons[port][i])
				m_changed = true;
			m_config.keys[port][i] = defaults.keys[port][i];
			m_config.padButtons[port][i] = defaults.padButtons[port][i];
		}
		// Which device a port reads is part of the defaults too: both on the
		// keyboard, because that is the one thing certain to be attached.
		if (m_config.device[port] != defaults.device[port]
				|| m_config.gamepad[port] != defaults.gamepad[port])
			m_changed = true;
		m_config.device[port] = defaults.device[port];
		m_config.gamepad[port] = defaults.gamepad[port];
	}
}

void BindingModel::selectKeyboard(int port) {
	if (m_config.device[port] == nesgui::PORT_KEYBOARD)
		return;
	m_config.device[port] = nesgui::PORT_KEYBOARD;
	m_changed = true;
}

void BindingModel::selectGamepad(int port, int gamepad) {
	if (gamepad < 0 || gamepad >= nesgui::MAX_GAMEPADS)
		return;
	if (m_config.device[port] == nesgui::PORT_GAMEPAD
			&& m_config.gamepad[port] == gamepad)
		return;
	m_config.device[port] = nesgui::PORT_GAMEPAD;
	m_config.gamepad[port] = gamepad;
	m_changed = true;
}

void BindingModel::apply(nesgui::Config* config) const {
	// Bindings and the device each port reads. Window scale, the plugin ids and
	// everything else in the file belong to somebody else and are not this
	// dialog's to rewrite.
	for (int port = 0; port < 2; port++) {
		for (int i = 0; i < BUTTON_COUNT; i++) {
			config->keys[port][i] = m_config.keys[port][i];
			config->padButtons[port][i] = m_config.padButtons[port][i];
		}
		config->device[port] = m_config.device[port];
		config->gamepad[port] = m_config.gamepad[port];
	}
}

} // namespace nesfe
