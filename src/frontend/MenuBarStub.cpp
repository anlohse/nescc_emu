//
// Everywhere without a native menu yet.
//
// Says so rather than pretending: menuBarAvailable() answers false and the
// caller keeps its hotkeys, which is exactly how the emulator worked before
// there was a menu at all.
//

#include "MenuBar.h"

#if !defined(_WIN32)

namespace nesfe {

bool menuBarAvailable() {
	return false;
}

bool setMenuBar(void*, const std::vector<MenuSection>&, bool) {
	return false;
}

int takeMenuAction() {
	return MENU_NONE;
}

bool handleMenuCommand(unsigned, unsigned long long, long long) {
	return false;
}

void showAboutBox(void*, const char*) {
}

} // namespace nesfe

#endif // !_WIN32
