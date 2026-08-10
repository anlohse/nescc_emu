//
// The menu bar, as a real Win32 menu.
//
// Nothing here decides what is on it. MenuModel builds the tree and is tested
// without a window; this turns that tree into an HMENU, hangs it on the
// emulator's window and reports which item was picked.
//

#include "MenuBar.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

namespace nesfe {

namespace {

/*
 * Menu ids are the model's action numbers, offset past the values Windows uses
 * for itself in a message loop we do not own.
 */
const UINT ID_BASE = 0x8000;

int chosenAction = MENU_NONE;

std::string withShortcut(const MenuItem& item) {
	// A tab is what tells Windows to right-align the rest, which is how every
	// other program on the desktop shows an accelerator.
	if (item.shortcut.empty())
		return item.label;
	return item.label + "\t" + item.shortcut;
}

HMENU buildSection(const MenuSection& section) {
	HMENU menu = CreatePopupMenu();
	for (std::size_t i = 0; i < section.items.size(); i++) {
		const MenuItem& item = section.items[i];
		if (item.separator) {
			AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
			continue;
		}

		UINT flags = MF_STRING;
		if (!item.enabled)
			flags |= MF_GRAYED;
		if (item.checked)
			flags |= item.radio ? MF_CHECKED : MF_CHECKED;

		const std::string text = withShortcut(item);
		AppendMenuA(menu, flags, ID_BASE + item.action, text.c_str());

		// A radio tick draws as a bullet rather than a check, which is how a
		// "one of these" set is meant to read.
		if (item.radio && item.checked)
			CheckMenuRadioItem(menu, ID_BASE + item.action, ID_BASE + item.action,
					ID_BASE + item.action, MF_BYCOMMAND);
	}
	return menu;
}

} // namespace

bool menuBarAvailable() {
	return true;
}

bool setMenuBar(void* window, const std::vector<MenuSection>& sections, bool grow) {
	HWND hwnd = static_cast<HWND>(window);
	if (!hwnd)
		return false;

	HMENU bar = CreateMenu();
	for (std::size_t i = 0; i < sections.size(); i++)
		AppendMenuA(bar, MF_POPUP,
				reinterpret_cast<UINT_PTR>(buildSection(sections[i])),
				sections[i].label.c_str());

	HMENU previous = GetMenu(hwnd);
	if (!SetMenu(hwnd, bar)) {
		DestroyMenu(bar);
		return false;
	}
	if (previous)
		DestroyMenu(previous);   // SetMenu does not free the one it replaced

	if (grow) {
		// A menu is drawn inside the client area, so without this the picture
		// loses a strip of its height to it. Grow the window by exactly the
		// menu's height and the player keeps the size they asked for.
		RECT client;
		RECT frame;
		if (GetClientRect(hwnd, &client) && GetWindowRect(hwnd, &frame)) {
			const int menuHeight = GetSystemMetrics(SM_CYMENU);
			SetWindowPos(hwnd, nullptr, 0, 0,
					frame.right - frame.left,
					(frame.bottom - frame.top) + menuHeight,
					SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		}
	}
	DrawMenuBar(hwnd);
	return true;
}

int takeMenuAction() {
	const int action = chosenAction;
	chosenAction = MENU_NONE;
	return action;
}

bool handleMenuCommand(unsigned message, unsigned long long wParam,
		long long lParam) {
	if (message != WM_COMMAND)
		return false;
	// A menu command carries a null lParam. A control puts its own window
	// handle there, and those belong to whichever dialog is up.
	if (lParam != 0)
		return false;

	const UINT id = LOWORD(static_cast<WPARAM>(wParam));
	if (id < ID_BASE)
		return false;
	chosenAction = static_cast<int>(id - ID_BASE);
	return true;
}

void showAboutBox(void* parent, const char* text) {
	MessageBoxA(static_cast<HWND>(parent), text, "About nes",
			MB_OK | MB_ICONINFORMATION);
}

} // namespace nesfe

#endif // _WIN32
