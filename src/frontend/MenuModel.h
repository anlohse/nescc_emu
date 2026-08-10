#ifndef NES_FRONTEND_MENU_MODEL_H
#define NES_FRONTEND_MENU_MODEL_H

//
// What is in the menu bar, and what state each item is in.
//
// No window, no HMENU, no toolkit -- just the tree and the rules about it, so
// the questions worth getting right can be asked in a test: is Reset offered
// with no cartridge loaded, does Pause show a tick while paused, does an item
// nobody has implemented yet look unavailable rather than broken.
//
// A menu is also where a program admits what it cannot do. Items that are not
// built yet are listed deliberately, disabled, because a gap somebody can see
// is worth more than a gap they have to guess at -- and because this is the
// list we work down.
//

#include <string>
#include <vector>

namespace nesfe {

/**
 * Everything the menu can ask for.
 *
 * Deliberately not the plugin ABI's command bits. Some of these are console
 * actions the run loop already understands, and the rest are host business --
 * files, dialogs, save slots -- that no plugin should ever hear about. Keeping
 * them apart is what stops the ABI growing a "show the about box" command.
 */
enum MenuAction {
	MENU_NONE = 0,

	// Emulation
	MENU_LOAD_ROM,
	MENU_CLOSE_ROM,
	MENU_RESET,
	MENU_HARD_RESET,
	MENU_PAUSE,
	MENU_FRAME_ADVANCE,
	MENU_SCREENSHOT,
	MENU_MUTE,
	MENU_EXIT,

	// State
	MENU_SAVE_STATE,
	MENU_LOAD_STATE,
	MENU_NEXT_SLOT,
	MENU_PREV_SLOT,

	// Settings
	MENU_CONFIGURE_VIDEO,
	MENU_CONFIGURE_AUDIO,
	MENU_CONFIGURE_INPUT,
	MENU_CONFIGURE_PLUGINS,

	// Help
	MENU_HOTKEYS,
	MENU_ABOUT,

	MENU_ACTION_COUNT
};

/*
 * The two numbered families live past the named actions rather than among them,
 * so that "slot 3" cannot collide with whatever enumerator happened to be
 * declared next. An enumerator plus an index is a trap in any enum that is
 * still being added to.
 */
const int MENU_SLOT_FIRST   = 1000;   // + slot
const int MENU_RECENT_FIRST = 1100;   // + index

/** How many save slots the State menu offers. */
const int MENU_SLOT_COUNT = 8;
/** How many recently loaded ROMs are remembered. */
const int MENU_RECENT_COUNT = 8;

struct MenuItem {
	int action = MENU_NONE;
	std::string label;
	/** Shown right-aligned, e.g. "F12". Empty when there is no key for it. */
	std::string shortcut;
	bool enabled = true;
	/** Drawn with a tick. Only meaningful for the toggles. */
	bool checked = false;
	/** A radio-style tick, used by the save slots. */
	bool radio = false;
	/** A dividing line rather than an item; every other field is ignored. */
	bool separator = false;
	/**
	 * True for something listed but not built yet.
	 *
	 * Always disabled, and worth marking as its own thing rather than just
	 * greying out: "not written yet" and "not available right now" look
	 * identical on screen and are completely different to a reader of this
	 * code.
	 */
	bool unimplemented = false;
};

struct MenuSection {
	std::string label;
	std::vector<MenuItem> items;
};

/**
 * The state the menu reflects. Small on purpose: anything the menu needs to
 * know has to be stated here, which keeps the rules in one readable place.
 */
struct MenuState {
	bool romLoaded = false;
	bool paused = false;
	bool muted = false;
	/** Which plugin kinds offer a dialog of their own. */
	bool videoConfigurable = false;
	bool audioConfigurable = false;
	bool inputConfigurable = false;
	int saveSlot = 0;
	/** Per slot: empty, or when it was written, ready to show. */
	std::vector<std::string> slotLabels;
	/** Most recent first; display names, not paths. */
	std::vector<std::string> recentRoms;
};

/** Build the whole bar for a given state. */
std::vector<MenuSection> buildMenu(const MenuState& state);

/** The action for a numbered slot or recent entry, and back again. */
inline int slotAction(int slot) { return MENU_SLOT_FIRST + slot; }
inline int recentAction(int index) { return MENU_RECENT_FIRST + index; }
/** @return -1 when @p action is not a slot. */
int slotForAction(int action);
int recentForAction(int action);

} // namespace nesfe

#endif // NES_FRONTEND_MENU_MODEL_H
