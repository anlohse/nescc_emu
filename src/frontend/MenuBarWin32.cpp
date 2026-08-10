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

/** Closes on the button, on Escape, and on the frame's close box. */
LRESULT CALLBACK textPageProc(HWND window, UINT message, WPARAM wParam,
		LPARAM lParam) {
	switch (message) {
	case WM_COMMAND:
		// The only control that reports anything here is the Close button.
		DestroyWindow(window);
		return 0;
	case WM_CLOSE:
		DestroyWindow(window);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	default:
		break;
	}
	return DefWindowProc(window, message, wParam, lParam);
}

HFONT uiFont() {
	NONCLIENTMETRICSA metrics;
	metrics.cbSize = sizeof(metrics);
	if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
		return CreateFontIndirectA(&metrics.lfMessageFont);
	return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
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

void showTextBox(void* parent, const char* title, const std::string& text) {
	HWND owner = static_cast<HWND>(parent);

	// An edit control rather than a static: it scrolls, it selects, and a person
	// can copy a line out of it, which is a reasonable thing to want from a page
	// listing what their keys do.
	//
	// Windows wants CRLF in a multi-line edit; a lone newline shows as a box.
	std::string crlf;
	crlf.reserve(text.size() + text.size() / 16);
	for (std::size_t i = 0; i < text.size(); i++) {
		if (text[i] == '\n')
			crlf += '\r';
		crlf += text[i];
	}

	HINSTANCE instance = GetModuleHandleA(nullptr);
	static bool registered = false;
	const char* className = "nesTextPage";
	if (!registered) {
		WNDCLASSA cls;
		ZeroMemory(&cls, sizeof(cls));
		cls.lpfnWndProc = textPageProc;
		cls.hInstance = instance;
		cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
		cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
		cls.lpszClassName = className;
		if (!RegisterClassA(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
			return;
		registered = true;
	}

	const int width = 460;
	const int height = 520;
	const DWORD style = WS_POPUPWINDOW | WS_CAPTION;
	RECT frame = { 0, 0, width, height };
	AdjustWindowRectEx(&frame, style, FALSE, WS_EX_DLGMODALFRAME);

	HWND window = CreateWindowExA(WS_EX_DLGMODALFRAME, className, title, style,
			CW_USEDEFAULT, CW_USEDEFAULT,
			frame.right - frame.left, frame.bottom - frame.top,
			owner, nullptr, instance, nullptr);
	if (!window)
		return;

	HWND edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", crlf.c_str(),
			WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP
					| ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
			12, 12, width - 24, height - 60,
			window, nullptr, instance, nullptr);

	CreateWindowExA(0, "BUTTON", "Close",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
			width - 12 - 84, height - 38, 84, 26,
			window, reinterpret_cast<HMENU>(2), instance, nullptr);

	// Fixed pitch, because the two columns are aligned with spaces and a
	// proportional face would make a mess of them.
	HFONT font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
	SendMessage(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
	HFONT ui = uiFont();
	EnumChildWindows(window, [](HWND child, LPARAM param) -> BOOL {
		char cls[16] = { 0 };
		GetClassNameA(child, cls, sizeof(cls));
		if (lstrcmpiA(cls, "BUTTON") == 0)
			SendMessage(child, WM_SETFONT, static_cast<WPARAM>(param), TRUE);
		return TRUE;
	}, reinterpret_cast<LPARAM>(ui));

	if (owner)
		EnableWindow(owner, FALSE);
	ShowWindow(window, SW_SHOW);
	// Focus the button rather than the text: Enter and Escape then both close
	// the page, and no caret blinks in something nobody can edit.
	SetFocus(GetDlgItem(window, 2));

	MSG message;
	while (GetMessage(&message, nullptr, 0, 0) > 0) {
		if (IsDialogMessage(window, &message))
			continue;
		TranslateMessage(&message);
		DispatchMessage(&message);
	}

	if (owner) {
		EnableWindow(owner, TRUE);
		SetForegroundWindow(owner);
	}
	DeleteObject(font);
	DeleteObject(ui);
}

} // namespace nesfe

#endif // _WIN32
