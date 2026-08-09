#ifndef NES_PLUGIN_FIELDS_DIALOG_H
#define NES_PLUGIN_FIELDS_DIALOG_H

//
// A settings dialog that is a list of choices, for anyone who has to put one up.
//
// Header-only on purpose. A loadable plugin links against nothing of the host's
// -- that is the whole point of the boundary -- so anything it can share with
// the host has to be shareable without a library. This is small enough to be:
// rows of "label: one of these", an OK and a Cancel.
//
// It is not a widget toolkit and should not grow into one. The moment a plugin
// needs a control this cannot express, it writes its own dialog; that is why
// configure() is a plugin's own function rather than a description the host
// renders. This just saves the third and fourth copy of the same eighty lines
// of window creation.
//

#include <string>
#include <vector>

namespace nesdlg {

/** One row: a label, the choices, and which is current. */
struct Field {
	std::string label;
	std::vector<std::string> options;
	int selected = 0;
};

/**
 * Show the fields and wait.
 *
 * @param parent native window handle to sit over, or null
 * @param note   a line under the fields, or null
 * @return true when OK was pressed, with each field's selected index updated;
 *         false on Cancel, on close, and on every platform without an
 *         implementation -- a caller cannot tell those apart and does not need
 *         to, because all three mean "change nothing".
 */
bool showFieldsDialog(const char* title, void* parent,
		std::vector<Field>* fields, const char* note = nullptr);

/** Whether this build can show one at all. */
bool fieldsDialogAvailable();

} // namespace nesdlg

/* ------------------------------------------------------------------------- */

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace nesdlg {

namespace fieldsdialog {

const int ID_FIRST_COMBO = 1000;
const int ID_OK          = 1;
const int ID_CANCEL      = 2;

const int MARGIN       = 14;
const int ROW_HEIGHT   = 34;
const int LABEL_WIDTH  = 108;
const int COMBO_WIDTH  = 236;
const int CLIENT_WIDTH = MARGIN + LABEL_WIDTH + 6 + COMBO_WIDTH + MARGIN;

struct State {
	std::vector<Field>* fields;
	std::vector<HWND> combos;
	bool accepted;
};

inline LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
	State* state = reinterpret_cast<State*>(GetWindowLongPtr(window, GWLP_USERDATA));

	switch (message) {
	case WM_COMMAND: {
		if (!state)
			break;
		const int id = LOWORD(wParam);
		if (id == ID_OK) {
			// Read the selections here rather than as they change: Cancel then
			// means the caller's fields were never touched, which is the only
			// definition of Cancel anybody trusts.
			for (std::size_t i = 0; i < state->combos.size(); i++) {
				const LRESULT choice =
						SendMessage(state->combos[i], CB_GETCURSEL, 0, 0);
				if (choice != CB_ERR)
					(*state->fields)[i].selected = static_cast<int>(choice);
			}
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

/**
 * The module this code was compiled into, which is not always the program.
 *
 * A plugin is a separate module with its own copy of this header, and a window
 * class belongs to the module that registered it. Taking the handle from the
 * address of this function -- rather than GetModuleHandle(NULL), which always
 * answers the executable -- is what keeps a plugin's dialog from being handed
 * the host's window procedure.
 */
inline HINSTANCE thisModule() {
	HMODULE module = nullptr;
	GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
					| GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&thisModule), &module);
	return module ? module : GetModuleHandleA(nullptr);
}

inline HFONT uiFont() {
	NONCLIENTMETRICSA metrics;
	metrics.cbSize = sizeof(metrics);
	if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
		return CreateFontIndirectA(&metrics.lfMessageFont);
	return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

} // namespace fieldsdialog

inline bool fieldsDialogAvailable() {
	return true;
}

inline bool showFieldsDialog(const char* title, void* parent,
		std::vector<Field>* fields, const char* note) {
	using namespace fieldsdialog;

	if (!fields || fields->empty())
		return false;

	HINSTANCE instance = thisModule();

	// One class per module, named for it, so two modules using this header do
	// not fight over one registration.
	char className[64];
	wsprintfA(className, "nesFieldsDialog%p", static_cast<void*>(instance));

	WNDCLASSA cls;
	ZeroMemory(&cls, sizeof(cls));
	cls.lpfnWndProc = proc;
	cls.hInstance = instance;
	cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
	cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
	cls.lpszClassName = className;
	if (!RegisterClassA(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		return false;

	const int rows = static_cast<int>(fields->size());
	const int rowsBottom   = MARGIN + rows * ROW_HEIGHT;
	const int noteY        = rowsBottom + 2;
	const int buttonsY     = rowsBottom + (note ? 30 : 8);
	const int clientHeight = buttonsY + 26 + MARGIN;

	// CreateWindowEx sizes the frame, not the client area, so the height asked
	// for has to be grown by whatever the caption and borders take.
	const DWORD style = WS_POPUPWINDOW | WS_CAPTION;
	const DWORD exStyle = WS_EX_DLGMODALFRAME;
	RECT frame = { 0, 0, CLIENT_WIDTH, clientHeight };
	AdjustWindowRectEx(&frame, style, FALSE, exStyle);

	HWND window = CreateWindowExA(exStyle, className, title, style,
			CW_USEDEFAULT, CW_USEDEFAULT,
			frame.right - frame.left, frame.bottom - frame.top,
			static_cast<HWND>(parent), nullptr, instance, nullptr);
	if (!window)
		return false;

	State state;
	state.fields = fields;
	state.accepted = false;
	SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));

	HFONT font = uiFont();
	const int comboX = MARGIN + LABEL_WIDTH + 6;

	int y = MARGIN;
	for (int i = 0; i < rows; i++) {
		const Field& field = (*fields)[i];
		CreateWindowExA(0, "STATIC", field.label.c_str(),
				WS_CHILD | WS_VISIBLE, MARGIN, y + 5, LABEL_WIDTH, 20,
				window, nullptr, instance, nullptr);

		// The height is the open list's height, not the closed box's.
		HWND combo = CreateWindowExA(0, "COMBOBOX", "",
				WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
				comboX, y, COMBO_WIDTH, 260,
				window, reinterpret_cast<HMENU>(ID_FIRST_COMBO + i),
				instance, nullptr);
		for (std::size_t option = 0; option < field.options.size(); option++)
			SendMessageA(combo, CB_ADDSTRING, 0,
					reinterpret_cast<LPARAM>(field.options[option].c_str()));
		SendMessage(combo, CB_SETCURSEL, static_cast<WPARAM>(field.selected), 0);
		if (field.options.empty())
			EnableWindow(combo, FALSE);
		state.combos.push_back(combo);
		y += ROW_HEIGHT;
	}

	if (note)
		CreateWindowExA(0, "STATIC", note, WS_CHILD | WS_VISIBLE,
				MARGIN, noteY, CLIENT_WIDTH - 2 * MARGIN, 20,
				window, nullptr, instance, nullptr);

	HWND ok = CreateWindowExA(0, "BUTTON", "OK",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
			CLIENT_WIDTH - MARGIN - 2 * 84 - 8, buttonsY, 84, 26,
			window, reinterpret_cast<HMENU>(ID_OK), instance, nullptr);
	CreateWindowExA(0, "BUTTON", "Cancel",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
			CLIENT_WIDTH - MARGIN - 84, buttonsY, 84, 26,
			window, reinterpret_cast<HMENU>(ID_CANCEL), instance, nullptr);

	EnumChildWindows(window, [](HWND child, LPARAM param) -> BOOL {
		SendMessage(child, WM_SETFONT, static_cast<WPARAM>(param), TRUE);
		return TRUE;
	}, reinterpret_cast<LPARAM>(font));

	if (parent)
		EnableWindow(static_cast<HWND>(parent), FALSE);   // modal, by hand
	ShowWindow(window, SW_SHOW);
	SetFocus(ok);

	MSG message;
	while (GetMessage(&message, nullptr, 0, 0) > 0) {
		if (IsDialogMessage(window, &message))
			continue;              // Tab, arrows and Escape keep their meaning
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

} // namespace nesdlg

#else // !_WIN32

namespace nesdlg {

inline bool fieldsDialogAvailable() {
	return false;
}

inline bool showFieldsDialog(const char*, void*, std::vector<Field>*, const char*) {
	return false;
}

} // namespace nesdlg

#endif // _WIN32

#endif // NES_PLUGIN_FIELDS_DIALOG_H
