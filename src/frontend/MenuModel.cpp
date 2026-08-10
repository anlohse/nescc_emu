#include "MenuModel.h"

#include <cstdio>

namespace nesfe {

namespace {

MenuItem separator() {
	MenuItem item;
	item.separator = true;
	return item;
}

MenuItem entry(int action, const char* label, const char* shortcut = "",
		bool enabled = true) {
	MenuItem item;
	item.action = action;
	item.label = label;
	item.shortcut = shortcut;
	item.enabled = enabled;
	return item;
}

/** Listed so it can be seen, disabled because it does not exist yet. */
MenuItem todo(int action, const char* label, const char* shortcut = "") {
	MenuItem item = entry(action, label, shortcut, false);
	item.unimplemented = true;
	return item;
}

MenuItem toggle(int action, const char* label, const char* shortcut,
		bool checked, bool enabled = true) {
	MenuItem item = entry(action, label, shortcut, enabled);
	item.checked = checked;
	return item;
}

} // namespace

std::vector<MenuSection> buildMenu(const MenuState& state) {
	std::vector<MenuSection> bar;

	/* --- Emulation ------------------------------------------------------- */
	{
		MenuSection s;
		s.label = "&Emulation";
		s.items.push_back(todo(MENU_LOAD_ROM, "&Load ROM...", "Ctrl+O"));
		s.items.push_back(todo(MENU_CLOSE_ROM, "&Close ROM"));

		// Recent ROMs earns its place only once loading exists; until then the
		// list is empty and says so rather than pretending to be a menu.
		if (state.recentRoms.empty()) {
			s.items.push_back(todo(MENU_RECENT_FIRST, "Recent ROMs"));
		} else {
			for (std::size_t i = 0; i < state.recentRoms.size(); i++)
				s.items.push_back(entry(recentAction(static_cast<int>(i)),
						state.recentRoms[i].c_str()));
		}

		s.items.push_back(separator());
		// Reset is the button on the front of the console: RAM survives it, and
		// so do A, X and Y. A hard reset is the switch at the back, and the
		// difference is real rather than cosmetic.
		s.items.push_back(entry(MENU_RESET, "&Reset", "R", state.romLoaded));
		s.items.push_back(entry(MENU_HARD_RESET, "&Hard Reset", "", state.romLoaded));
		s.items.push_back(separator());
		s.items.push_back(toggle(MENU_PAUSE, "&Pause", "P", state.paused,
				state.romLoaded));
		s.items.push_back(entry(MENU_FRAME_ADVANCE, "Frame &Advance", "N",
				state.romLoaded && state.paused));
		s.items.push_back(separator());
		s.items.push_back(toggle(MENU_MUTE, "&Mute", "M", state.muted));
		s.items.push_back(entry(MENU_SCREENSHOT, "&Screenshot", "F12",
				state.romLoaded));
		s.items.push_back(separator());
		s.items.push_back(entry(MENU_EXIT, "E&xit", "Alt+F4"));
		bar.push_back(s);
	}

	/* --- State ----------------------------------------------------------- */
	{
		MenuSection s;
		s.label = "&State";
		s.items.push_back(todo(MENU_SAVE_STATE, "&Save State", "F5"));
		s.items.push_back(todo(MENU_LOAD_STATE, "&Load State", "F8"));
		s.items.push_back(separator());

		// The slots are listed even though nothing writes them yet: the shape of
		// this menu is the specification for what save states have to provide,
		// and an empty slot is a real state a finished version still shows.
		for (int i = 0; i < MENU_SLOT_COUNT; i++) {
			char label[64];
			const std::string when = i < static_cast<int>(state.slotLabels.size())
					? state.slotLabels[i] : std::string();
			std::snprintf(label, sizeof(label), "Slot &%d  %s", i + 1,
					when.empty() ? "(empty)" : when.c_str());
			MenuItem item = todo(slotAction(i), label);
			item.radio = true;
			item.checked = (i == state.saveSlot);
			s.items.push_back(item);
		}

		s.items.push_back(separator());
		s.items.push_back(todo(MENU_NEXT_SLOT, "&Next Slot", "Ctrl+Right"));
		s.items.push_back(todo(MENU_PREV_SLOT, "&Previous Slot", "Ctrl+Left"));
		bar.push_back(s);
	}

	/* --- Settings -------------------------------------------------------- */
	{
		MenuSection s;
		s.label = "&Settings";
		// Each of these opens the dialog belonging to the plugin currently
		// chosen for that job, which is why they can be unavailable: a plugin
		// is not obliged to have one.
		s.items.push_back(entry(MENU_CONFIGURE_VIDEO, "Configure &Video Plugin...",
				"", state.videoConfigurable));
		s.items.push_back(entry(MENU_CONFIGURE_AUDIO, "Configure &Audio Plugin...",
				"", state.audioConfigurable));
		s.items.push_back(entry(MENU_CONFIGURE_INPUT, "Configure &Controller Plugin...",
				"", state.inputConfigurable));
		s.items.push_back(separator());
		s.items.push_back(entry(MENU_CONFIGURE_PLUGINS, "Configure &Plugins...", "F1"));
		bar.push_back(s);
	}

	/* --- Help ------------------------------------------------------------ */
	{
		MenuSection s;
		s.label = "&Help";
		s.items.push_back(todo(MENU_HOTKEYS, "&Keys and Buttons"));
		s.items.push_back(separator());
		s.items.push_back(entry(MENU_ABOUT, "&About"));
		bar.push_back(s);
	}

	return bar;
}

int slotForAction(int action) {
	const int slot = action - MENU_SLOT_FIRST;
	return (slot >= 0 && slot < MENU_SLOT_COUNT) ? slot : -1;
}

int recentForAction(int action) {
	const int index = action - MENU_RECENT_FIRST;
	return (index >= 0 && index < MENU_RECENT_COUNT) ? index : -1;
}

} // namespace nesfe
