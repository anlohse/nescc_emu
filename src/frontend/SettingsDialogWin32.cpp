//
// The plugin chooser, as a Win32 window.
//
// Built in code rather than from a resource script, so the dialog has no build
// step of its own and no .rc file to keep in step with the constants below.
// There are eleven controls; a resource compiler would not have saved much.
//
// Nothing here decides anything. Which plugins exist, what a selection means
// and what gets written back is PluginSettings' job, and is tested without a
// window. This file puts that on screen and reports which button was pressed.
//

#include "SettingsDialog.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>

namespace nesfe {

namespace {

const int ID_FIRST_COMBO     = 1000;   // + kind index
const int ID_FIRST_CONFIGURE = 1010;   // + kind index
const int ID_OK              = 1;
const int ID_CANCEL          = 2;

const int MARGIN        = 14;
const int ROW_HEIGHT     = 34;
const int LABEL_WIDTH    = 76;
// Wide enough for the longest name a plugin is likely to have plus its version
// and where it came from: "SDL2 keyboard and gamepads 1.0  (built in)" already
// overflows a narrower box, and a truncated name is useless for telling two
// plugins apart, which is the entire job of this list.
const int COMBO_WIDTH    = 258;
const int BUTTON_WIDTH   = 96;
const int CLIENT_WIDTH   = MARGIN + LABEL_WIDTH + 4 + COMBO_WIDTH + 8
		+ BUTTON_WIDTH + MARGIN;

struct DialogState {
	PluginSettings* settings;
	HWND combo[PluginSettings::KIND_COUNT];
	HWND configure[PluginSettings::KIND_COUNT];
	bool accepted;
};

/** Refresh a Configure button's enabled state from the current selection. */
void updateConfigureButton(DialogState* state, int index) {
	EnableWindow(state->configure[index],
			state->settings->canConfigure(index) ? TRUE : FALSE);
}

void fillCombo(DialogState* state, int index) {
	const std::vector<PluginChoice>& choices = state->settings->choicesFor(index);
	for (std::size_t i = 0; i < choices.size(); i++) {
		// Say where it came from. With a built-in and a loaded module both
		// present, the names are identical and the file is the only thing that
		// tells them apart -- which is exactly what someone opening this
		// dialog after installing a plugin wants to check.
		std::string label = choices[i].name;
		if (!choices[i].version.empty())
			label += " " + choices[i].version;
		label += choices[i].path.empty() ? "  (built in)" : "  (plugin)";
		SendMessageA(state->combo[index], CB_ADDSTRING, 0,
				reinterpret_cast<LPARAM>(label.c_str()));
	}

	const int selected = state->settings->selectedIndex(index);
	if (selected >= 0)
		SendMessageA(state->combo[index], CB_SETCURSEL,
				static_cast<WPARAM>(selected), 0);
	else
		EnableWindow(state->combo[index], FALSE);   // nothing of this kind
	updateConfigureButton(state, index);
}

LRESULT CALLBACK dialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
	DialogState* state = reinterpret_cast<DialogState*>(
			GetWindowLongPtr(window, GWLP_USERDATA));

	switch (message) {
	case WM_COMMAND: {
		if (!state)
			break;
		const int id = LOWORD(wParam);

		if (id >= ID_FIRST_COMBO && id < ID_FIRST_COMBO + PluginSettings::KIND_COUNT) {
			if (HIWORD(wParam) == CBN_SELCHANGE) {
				const int index = id - ID_FIRST_COMBO;
				const int choice = static_cast<int>(
						SendMessage(state->combo[index], CB_GETCURSEL, 0, 0));
				state->settings->select(index, choice);
				updateConfigureButton(state, index);
			}
			return 0;
		}

		if (id >= ID_FIRST_CONFIGURE
				&& id < ID_FIRST_CONFIGURE + PluginSettings::KIND_COUNT) {
			state->settings->configure(id - ID_FIRST_CONFIGURE);
			return 0;
		}

		if (id == ID_OK) {
			state->accepted = true;
			DestroyWindow(window);
			return 0;
		}
		if (id == ID_CANCEL) {
			DestroyWindow(window);
			return 0;
		}
		break;
	}
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

bool settingsDialogAvailable() {
	return true;
}

bool showSettingsDialog(PluginSettings* settings, void* parent) {
	HINSTANCE instance = GetModuleHandle(nullptr);

	static bool registered = false;
	if (!registered) {
		WNDCLASSA cls;
		ZeroMemory(&cls, sizeof(cls));
		cls.lpfnWndProc = dialogProc;
		cls.hInstance = instance;
		cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
		cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
		cls.lpszClassName = "nesPluginSettings";
		if (!RegisterClassA(&cls))
			return false;
		registered = true;
	}

	const int rowsBottom = MARGIN + PluginSettings::KIND_COUNT * ROW_HEIGHT;
	const int noteY      = rowsBottom + 6;
	const int buttonsY   = rowsBottom + 34;
	const int clientHeight = buttonsY + 26 + MARGIN;

	// CreateWindowEx sizes the whole window, caption and borders included, so
	// asking for the client size directly would lose the bottom of the dialog
	// behind the frame -- which is exactly what it did until a screenshot
	// showed the OK button cut in half.
	const DWORD style = WS_POPUPWINDOW | WS_CAPTION;
	const DWORD exStyle = WS_EX_DLGMODALFRAME;
	RECT frame = { 0, 0, CLIENT_WIDTH, clientHeight };
	AdjustWindowRectEx(&frame, style, FALSE, exStyle);

	// A window with no minimise or maximise: this is a dialog, and giving it
	// the buttons of a document window invites people to try them.
	HWND window = CreateWindowExA(exStyle, "nesPluginSettings", "Plugins", style,
			CW_USEDEFAULT, CW_USEDEFAULT,
			frame.right - frame.left, frame.bottom - frame.top,
			static_cast<HWND>(parent), nullptr, instance, nullptr);
	if (!window)
		return false;

	DialogState state;
	state.settings = settings;
	state.accepted = false;
	SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));

	HFONT font = uiFont();
	const int comboX  = MARGIN + LABEL_WIDTH + 4;
	const int buttonX = comboX + COMBO_WIDTH + 8;

	int y = MARGIN;
	for (int index = 0; index < PluginSettings::KIND_COUNT; index++) {
		CreateWindowExA(0, "STATIC", PluginSettings::kindLabel(index),
				WS_CHILD | WS_VISIBLE, MARGIN, y + 5, LABEL_WIDTH, 20,
				window, nullptr, instance, nullptr);

		// The height given to a drop-down list is the height of the list when
		// it is open, not of the closed box, which is why this is far taller
		// than a row.
		state.combo[index] = CreateWindowExA(0, "COMBOBOX", "",
				WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
				comboX, y, COMBO_WIDTH, 240,
				window, reinterpret_cast<HMENU>(ID_FIRST_COMBO + index),
				instance, nullptr);

		state.configure[index] = CreateWindowExA(0, "BUTTON", "Settings...",
				WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
				buttonX, y, BUTTON_WIDTH, 24,
				window, reinterpret_cast<HMENU>(ID_FIRST_CONFIGURE + index),
				instance, nullptr);

		y += ROW_HEIGHT;
	}

	CreateWindowExA(0, "STATIC",
			"A change of plugin takes effect the next time the emulator starts.",
			WS_CHILD | WS_VISIBLE, MARGIN, noteY, CLIENT_WIDTH - 2 * MARGIN, 20,
			window, nullptr, instance, nullptr);

	HWND ok = CreateWindowExA(0, "BUTTON", "OK",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
			CLIENT_WIDTH - MARGIN - 2 * 84 - 8, buttonsY, 84, 26,
			window, reinterpret_cast<HMENU>(ID_OK), instance, nullptr);
	CreateWindowExA(0, "BUTTON", "Cancel",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
			CLIENT_WIDTH - MARGIN - 84, buttonsY, 84, 26,
			window, reinterpret_cast<HMENU>(ID_CANCEL), instance, nullptr);

	// The default GUI font is a 1990s bitmap face; without this the dialog
	// looks a decade older than the rest of the desktop.
	EnumChildWindows(window, [](HWND child, LPARAM param) -> BOOL {
		SendMessage(child, WM_SETFONT, static_cast<WPARAM>(param), TRUE);
		return TRUE;
	}, reinterpret_cast<LPARAM>(font));

	for (int index = 0; index < PluginSettings::KIND_COUNT; index++)
		fillCombo(&state, index);

	if (parent)
		EnableWindow(static_cast<HWND>(parent), FALSE);   // modal, by hand
	ShowWindow(window, SW_SHOW);
	SetFocus(ok);

	MSG message;
	while (GetMessage(&message, nullptr, 0, 0) > 0) {
		if (IsDialogMessage(window, &message))
			continue;          // gives Tab, arrows and Escape their usual meaning
		TranslateMessage(&message);
		DispatchMessage(&message);
	}

	if (parent) {
		EnableWindow(static_cast<HWND>(parent), TRUE);
		SetForegroundWindow(static_cast<HWND>(parent));
	}
	DeleteObject(font);
	return state.accepted;
}

} // namespace nesfe

#endif // _WIN32
