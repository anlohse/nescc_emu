//
// The controller bindings dialog, as a Win32 window.
//
// A group selector, a list of eight buttons and what each is bound to, and a
// capture mode: press Bind, then press the key or pad button you want. That is
// what people expect from a rebinding screen and what editing a text file
// cannot offer.
//
// Nothing here decides anything -- BindingModel does, and is tested without a
// window. This puts it on screen and turns key presses into scancodes.
//

#include "BindingsDialog.h"
#include "PadMapping.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <SDL.h>

#include <cctype>
#include <string>
#include <vector>

namespace nesfe {

namespace {

const int ID_PORT      = 1000;
const int ID_DEVICE    = 1006;
const int ID_LIST      = 1001;
const int ID_BIND      = 1002;
const int ID_CLEAR     = 1003;
const int ID_DEFAULTS  = 1004;
const int ID_STATUS    = 1005;
const int ID_OK        = 1;
const int ID_CANCEL    = 2;

const int CAPTURE_TIMER    = 1;
const int CAPTURE_INTERVAL = 30;      // ms; fast enough to feel instant

const int MARGIN       = 14;
const int CLIENT_WIDTH = 430;
const int LIST_HEIGHT  = 160;

struct DialogState {
	BindingModel* model;
	/** Which console port is being configured: the thing a player picks first. */
	HWND port;
	/** And what drives it -- the keyboard, or one of the gamepads by name. */
	HWND device;
	HWND list;
	HWND bind;
	HWND clear;
	HWND status;
	int currentPort;
	/**
	 * The binding set on show, derived from the port and its device.
	 *
	 * Kept rather than recomputed at each use because the two combos can be
	 * mid-change: the port moves first, and the device list has to be rebuilt for
	 * it before the group means anything.
	 */
	int currentGroup;
	bool capturing;
	bool accepted;
	/**
	 * Pads opened by this dialog, so a gamepad can be bound by pressing it.
	 *
	 * Indexed by gamepad number, matching the numbering in the device list and in
	 * the configuration -- not by console port.
	 */
	SDL_GameController* pads[nesgui::MAX_GAMEPADS];
	bool ownsGameController;
};

/* ------------------------------------------------------------------------- */
/* Key mapping                                                                */
/* ------------------------------------------------------------------------- */

struct NamedKey { unsigned vk; SDL_Scancode code; };

/**
 * The keys that produce no character, and so cannot be resolved through SDL's
 * own keycode table. Everything printable is handled by the layout instead.
 */
const NamedKey NAMED_KEYS[] = {
	{ VK_ESCAPE, SDL_SCANCODE_ESCAPE }, { VK_RETURN, SDL_SCANCODE_RETURN },
	{ VK_TAB, SDL_SCANCODE_TAB },       { VK_BACK, SDL_SCANCODE_BACKSPACE },
	{ VK_SPACE, SDL_SCANCODE_SPACE },   { VK_CAPITAL, SDL_SCANCODE_CAPSLOCK },
	{ VK_LSHIFT, SDL_SCANCODE_LSHIFT }, { VK_RSHIFT, SDL_SCANCODE_RSHIFT },
	{ VK_LCONTROL, SDL_SCANCODE_LCTRL },{ VK_RCONTROL, SDL_SCANCODE_RCTRL },
	{ VK_LMENU, SDL_SCANCODE_LALT },    { VK_RMENU, SDL_SCANCODE_RALT },
	{ VK_LWIN, SDL_SCANCODE_LGUI },     { VK_RWIN, SDL_SCANCODE_RGUI },
	{ VK_UP, SDL_SCANCODE_UP },         { VK_DOWN, SDL_SCANCODE_DOWN },
	{ VK_LEFT, SDL_SCANCODE_LEFT },     { VK_RIGHT, SDL_SCANCODE_RIGHT },
	{ VK_INSERT, SDL_SCANCODE_INSERT }, { VK_DELETE, SDL_SCANCODE_DELETE },
	{ VK_HOME, SDL_SCANCODE_HOME },     { VK_END, SDL_SCANCODE_END },
	{ VK_PRIOR, SDL_SCANCODE_PAGEUP },  { VK_NEXT, SDL_SCANCODE_PAGEDOWN },
	{ VK_SNAPSHOT, SDL_SCANCODE_PRINTSCREEN },
	{ VK_SCROLL, SDL_SCANCODE_SCROLLLOCK },
	{ VK_PAUSE, SDL_SCANCODE_PAUSE },
	{ VK_NUMLOCK, SDL_SCANCODE_NUMLOCKCLEAR },
	{ VK_NUMPAD0, SDL_SCANCODE_KP_0 },  { VK_NUMPAD1, SDL_SCANCODE_KP_1 },
	{ VK_NUMPAD2, SDL_SCANCODE_KP_2 },  { VK_NUMPAD3, SDL_SCANCODE_KP_3 },
	{ VK_NUMPAD4, SDL_SCANCODE_KP_4 },  { VK_NUMPAD5, SDL_SCANCODE_KP_5 },
	{ VK_NUMPAD6, SDL_SCANCODE_KP_6 },  { VK_NUMPAD7, SDL_SCANCODE_KP_7 },
	{ VK_NUMPAD8, SDL_SCANCODE_KP_8 },  { VK_NUMPAD9, SDL_SCANCODE_KP_9 },
	{ VK_ADD, SDL_SCANCODE_KP_PLUS },   { VK_SUBTRACT, SDL_SCANCODE_KP_MINUS },
	{ VK_MULTIPLY, SDL_SCANCODE_KP_MULTIPLY },
	{ VK_DIVIDE, SDL_SCANCODE_KP_DIVIDE },
	{ VK_DECIMAL, SDL_SCANCODE_KP_PERIOD },
	{ VK_F1, SDL_SCANCODE_F1 },   { VK_F2, SDL_SCANCODE_F2 },
	{ VK_F3, SDL_SCANCODE_F3 },   { VK_F4, SDL_SCANCODE_F4 },
	{ VK_F5, SDL_SCANCODE_F5 },   { VK_F6, SDL_SCANCODE_F6 },
	{ VK_F7, SDL_SCANCODE_F7 },   { VK_F8, SDL_SCANCODE_F8 },
	{ VK_F9, SDL_SCANCODE_F9 },   { VK_F10, SDL_SCANCODE_F10 },
	{ VK_F11, SDL_SCANCODE_F11 }, { VK_F12, SDL_SCANCODE_F12 }
};

} // namespace

SDL_Scancode scancodeFromVirtualKey(unsigned virtualKey, bool extended) {
	// Windows reports one code for both shifts, controls and alts unless asked
	// to distinguish them; the extended-key flag is what tells them apart, and
	// binding "the right shift" specifically is exactly what the default
	// configuration does.
	switch (virtualKey) {
	case VK_SHIFT:   virtualKey = extended ? VK_RSHIFT : VK_LSHIFT; break;
	case VK_CONTROL: virtualKey = extended ? VK_RCONTROL : VK_LCONTROL; break;
	case VK_MENU:    virtualKey = extended ? VK_RMENU : VK_LMENU; break;
	case VK_RETURN:  if (extended) return SDL_SCANCODE_KP_ENTER; break;
	default: break;
	}

	for (std::size_t i = 0; i < sizeof(NAMED_KEYS) / sizeof(NAMED_KEYS[0]); i++)
		if (NAMED_KEYS[i].vk == virtualKey)
			return NAMED_KEYS[i].code;

	// Anything printable: ask the layout what character this key makes, and ask
	// SDL which scancode makes that character. Going through the layout is what
	// makes a non-US keyboard bind the key that was actually pressed.
	const UINT character = MapVirtualKeyA(virtualKey, MAPVK_VK_TO_CHAR) & 0x7FFF;
	if (character == 0)
		return SDL_SCANCODE_UNKNOWN;
	const SDL_Keycode keycode = static_cast<SDL_Keycode>(
			std::tolower(static_cast<int>(character)));
	return SDL_GetScancodeFromKey(keycode);
}

namespace {

/* ------------------------------------------------------------------------- */
/* The window                                                                 */
/* ------------------------------------------------------------------------- */

/**
 * Rebuild the device list for the selected port, and select what it reads.
 *
 * Entry 0 is the keyboard; after that one entry per gamepad slot, named by SDL
 * when something is attached to it. Absent slots are listed too, and say so: a
 * configuration naming Gamepad 3 should still show Gamepad 3 when it is
 * unplugged, rather than silently becoming something else.
 */
void refreshDevices(DialogState* state) {
	SendMessage(state->device, CB_RESETCONTENT, 0, 0);
	SendMessageA(state->device, CB_ADDSTRING, 0,
			reinterpret_cast<LPARAM>("Keyboard"));
	for (int i = 0; i < nesgui::MAX_GAMEPADS; i++) {
		char label[160];
		const char* name = state->pads[i]
				? SDL_GameControllerName(state->pads[i]) : nullptr;
		std::snprintf(label, sizeof(label), "Gamepad %d%s%s", i + 1,
				name ? " -- " : "  (not attached)", name ? name : "");
		SendMessageA(state->device, CB_ADDSTRING, 0,
				reinterpret_cast<LPARAM>(label));
	}

	const int port = state->currentPort;
	const int row = (state->model->deviceFor(port) == nesgui::PORT_GAMEPAD)
			? (1 + state->model->gamepadFor(port)) : 0;
	SendMessage(state->device, CB_SETCURSEL, static_cast<WPARAM>(row), 0);
	state->currentGroup = state->model->groupFor(port);
}

void refreshList(DialogState* state) {
	const int selected = static_cast<int>(
			SendMessage(state->list, LB_GETCURSEL, 0, 0));
	SendMessage(state->list, LB_RESETCONTENT, 0, 0);
	for (int i = 0; i < BindingModel::BUTTON_COUNT; i++) {
		char row[128];
		std::snprintf(row, sizeof(row), "%-8s %s",
				BindingModel::buttonLabel(i),
				state->model->bindingName(state->currentGroup, i).c_str());
		SendMessageA(state->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row));
	}
	SendMessage(state->list, LB_SETCURSEL,
			static_cast<WPARAM>(selected < 0 ? 0 : selected), 0);
}

void setStatus(DialogState* state, const char* text) {
	SetWindowTextA(state->status, text);
}

void openPads(DialogState* state) {
	// The dialog may be running from a throwaway plugin instance that never
	// opened anything, so a pad has to be claimed here to be readable at all.
	if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER)) {
		if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
			return;
		state->ownsGameController = true;
	}
	// A pad with no mapping is not a game controller to SDL, so without this the
	// list below comes back empty and the dialog offers a player nothing to
	// choose -- for a pad that works.
	nesfe::mapUnknownPads();

	int found = 0;
	for (int i = 0; i < SDL_NumJoysticks() && found < nesgui::MAX_GAMEPADS; i++)
		if (SDL_IsGameController(i))
			state->pads[found++] = SDL_GameControllerOpen(i);
}

void closePads(DialogState* state) {
	for (int i = 0; i < nesgui::MAX_GAMEPADS; i++)
		if (state->pads[i]) {
			SDL_GameControllerClose(state->pads[i]);
			state->pads[i] = nullptr;
		}
	if (state->ownsGameController) {
		SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
		state->ownsGameController = false;
	}
}

void stopCapture(DialogState* state, HWND window) {
	if (!state->capturing)
		return;
	state->capturing = false;
	KillTimer(window, CAPTURE_TIMER);
	SetWindowTextA(state->bind, "Bind...");
	setStatus(state, "");
	SetFocus(state->list);
}

void startCapture(DialogState* state, HWND window) {
	const int row = static_cast<int>(SendMessage(state->list, LB_GETCURSEL, 0, 0));
	if (row < 0)
		return;
	state->capturing = true;
	SetWindowTextA(state->bind, "Cancel");

	// Take focus off the buttons and onto the dialog itself. A key press goes
	// to whichever control has focus, so with Bind still focused the window
	// procedure below would never see one -- and every key would press the
	// button again instead of being captured.
	SetFocus(window);
	if (BindingModel::isPad(state->currentGroup)) {
		setStatus(state, "Press a button on the gamepad, or Escape to cancel.");
		// Pad state is polled rather than delivered: a gamepad does not send
		// window messages, so nothing would arrive while this dialog has focus.
		//
		// And the state it polls is only fresh because the program asks SDL to keep
		// reading joysticks while it thinks it is in the background. This is a
		// native window, so SDL believes exactly that the moment it opens -- see
		// SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS in gui_main.cpp. Without it
		// this timer runs perfectly and reads a frozen snapshot for ever.
		SetTimer(window, CAPTURE_TIMER, CAPTURE_INTERVAL, nullptr);
	} else {
		setStatus(state, "Press a key, or Escape to cancel.");
	}
}

/** Poll the pads while capturing. @return true once something was bound. */
bool pollPadCapture(DialogState* state, HWND window) {
	const int row = static_cast<int>(SendMessage(state->list, LB_GETCURSEL, 0, 0));
	if (row < 0)
		return false;

	SDL_GameControllerUpdate();
	for (int pad = 0; pad < 2; pad++) {
		if (!state->pads[pad])
			continue;
		for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; button++) {
			if (!SDL_GameControllerGetButton(state->pads[pad],
					static_cast<SDL_GameControllerButton>(button)))
				continue;
			SDL_Log("Bindings dialog: player %d gamepad button %d pressed\n", pad + 1, button);
			state->model->bindPad(state->currentGroup, row,
					static_cast<SDL_GameControllerButton>(button));
			stopCapture(state, window);
			refreshList(state);
			return true;
		}
	}
	return false;
}

LRESULT CALLBACK bindingsProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
	DialogState* state = reinterpret_cast<DialogState*>(
			GetWindowLongPtr(window, GWLP_USERDATA));

	switch (message) {
	case WM_TIMER:
		if (state && state->capturing && wParam == CAPTURE_TIMER)
			pollPadCapture(state, window);
		return 0;

	// SYSKEYDOWN as well: Alt and F10 arrive as system keys, and refusing to
	// bind them because of which message Windows chose would be arbitrary.
	case WM_SYSKEYDOWN:
	case WM_KEYDOWN: {
		if (!state || !state->capturing)
			break;
		if (wParam == VK_ESCAPE) {
			stopCapture(state, window);
			return 0;
		}
		if (BindingModel::isPad(state->currentGroup))
			return 0;          // waiting for a pad, not a key

		const bool extended = (lParam & (1 << 24)) != 0;
		const SDL_Scancode code = scancodeFromVirtualKey(
				static_cast<unsigned>(wParam), extended);
		const int row = static_cast<int>(SendMessage(state->list, LB_GETCURSEL, 0, 0));
		if (code == SDL_SCANCODE_UNKNOWN) {
			// Better than binding the wrong key silently, which is what a
			// guess at an unmapped key would amount to.
			setStatus(state, "That key is not one SDL can name. Try another.");
			return 0;
		}
		if (row >= 0) {
			state->model->bindKey(state->currentGroup, row, code);
			stopCapture(state, window);
			refreshList(state);
		}
		return 0;
	}

	case WM_COMMAND: {
		if (!state)
			break;
		const int id = LOWORD(wParam);

		if (id == ID_PORT && HIWORD(wParam) == CBN_SELCHANGE) {
			stopCapture(state, window);
			state->currentPort = static_cast<int>(
					SendMessage(state->port, CB_GETCURSEL, 0, 0));
			// The device list belongs to the port, so it is rebuilt before the
			// bindings are: which set to show depends on what this port reads.
			refreshDevices(state);
			refreshList(state);
			return 0;
		}
		if (id == ID_DEVICE && HIWORD(wParam) == CBN_SELCHANGE) {
			stopCapture(state, window);
			const int row = static_cast<int>(
					SendMessage(state->device, CB_GETCURSEL, 0, 0));
			// Choosing here is the whole feature: it says what this port reads,
			// and it is saved. Nothing is inferred from what happens to be plugged
			// in, so an unattached slot stays chosen and simply reads nothing.
			if (row <= 0)
				state->model->selectKeyboard(state->currentPort);
			else
				state->model->selectGamepad(state->currentPort, row - 1);
			state->currentGroup = state->model->groupFor(state->currentPort);
			refreshList(state);
			return 0;
		}
		if (id == ID_LIST && HIWORD(wParam) == LBN_DBLCLK) {
			startCapture(state, window);
			return 0;
		}
		if (id == ID_BIND) {
			if (state->capturing)
				stopCapture(state, window);
			else
				startCapture(state, window);
			return 0;
		}
		if (id == ID_CLEAR) {
			const int row = static_cast<int>(
					SendMessage(state->list, LB_GETCURSEL, 0, 0));
			if (row >= 0) {
				state->model->clear(state->currentGroup, row);
				refreshList(state);
			}
			return 0;
		}
		if (id == ID_DEFAULTS) {
			state->model->restoreDefaults();
			refreshList(state);
			return 0;
		}
		if (id == ID_OK) {
			state->accepted = true;
			DestroyWindow(window);
			return 0;
		}
		if (id == ID_CANCEL) {
			// Escape reaches here through IsDialogMessage. While capturing it
			// means "stop waiting", not "throw the whole dialog away".
			if (state->capturing) {
				stopCapture(state, window);
				return 0;
			}
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

HFONT listFont() {
	// Fixed pitch so the button column lines up with the binding column.
	return CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
}

} // namespace

bool bindingsDialogAvailable() {
	return true;
}

bool showBindingsDialog(nesgui::Config* config, void* parent) {
	HINSTANCE instance = GetModuleHandle(nullptr);

	static bool registered = false;
	if (!registered) {
		WNDCLASSA cls;
		ZeroMemory(&cls, sizeof(cls));
		cls.lpfnWndProc = bindingsProc;
		cls.hInstance = instance;
		cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
		cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
		cls.lpszClassName = "nesBindings";
		if (!RegisterClassA(&cls))
			return false;
		registered = true;
	}

	BindingModel model(*config);

	const int listY     = MARGIN + 34;
	const int buttonsY  = listY + LIST_HEIGHT + 10;
	const int statusY   = buttonsY + 34;
	const int acceptY   = statusY + 26;
	const int clientHeight = acceptY + 26 + MARGIN;

	const DWORD style = WS_POPUPWINDOW | WS_CAPTION;
	const DWORD exStyle = WS_EX_DLGMODALFRAME;
	RECT frame = { 0, 0, CLIENT_WIDTH, clientHeight };
	AdjustWindowRectEx(&frame, style, FALSE, exStyle);

	HWND window = CreateWindowExA(exStyle, "nesBindings", "Controller bindings",
			style, CW_USEDEFAULT, CW_USEDEFAULT,
			frame.right - frame.left, frame.bottom - frame.top,
			static_cast<HWND>(parent), nullptr, instance, nullptr);
	if (!window)
		return false;

	DialogState state;
	state.model = &model;
	state.currentPort = 0;
	state.currentGroup = 0;
	state.capturing = false;
	state.accepted = false;
	for (int i = 0; i < nesgui::MAX_GAMEPADS; i++)
		state.pads[i] = nullptr;
	state.ownsGameController = false;
	SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));

	// Two questions, in the order a person asks them: which player, then what
	// they are holding. The port picker is narrow because it only ever says
	// "Player 1" or "Player 2"; the device list carries controller names.
	const int portWidth = 130;
	state.port = CreateWindowExA(0, "COMBOBOX", "",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
			MARGIN, MARGIN, portWidth, 200,
			window, reinterpret_cast<HMENU>(ID_PORT), instance, nullptr);
	SendMessageA(state.port, CB_ADDSTRING, 0,
			reinterpret_cast<LPARAM>("Player 1"));
	SendMessageA(state.port, CB_ADDSTRING, 0,
			reinterpret_cast<LPARAM>("Player 2"));
	SendMessage(state.port, CB_SETCURSEL, 0, 0);

	state.device = CreateWindowExA(0, "COMBOBOX", "",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
			MARGIN + portWidth + 8, MARGIN,
			CLIENT_WIDTH - 2 * MARGIN - portWidth - 8, 200,
			window, reinterpret_cast<HMENU>(ID_DEVICE), instance, nullptr);

	state.list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL
					| LBS_NOTIFY | LBS_HASSTRINGS,
			MARGIN, listY, CLIENT_WIDTH - 2 * MARGIN, LIST_HEIGHT,
			window, reinterpret_cast<HMENU>(ID_LIST), instance, nullptr);

	state.bind = CreateWindowExA(0, "BUTTON", "Bind...",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
			MARGIN, buttonsY, 92, 26,
			window, reinterpret_cast<HMENU>(ID_BIND), instance, nullptr);
	state.clear = CreateWindowExA(0, "BUTTON", "Clear",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
			MARGIN + 100, buttonsY, 92, 26,
			window, reinterpret_cast<HMENU>(ID_CLEAR), instance, nullptr);
	CreateWindowExA(0, "BUTTON", "Defaults",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
			MARGIN + 200, buttonsY, 92, 26,
			window, reinterpret_cast<HMENU>(ID_DEFAULTS), instance, nullptr);

	state.status = CreateWindowExA(0, "STATIC", "",
			WS_CHILD | WS_VISIBLE, MARGIN, statusY,
			CLIENT_WIDTH - 2 * MARGIN, 20,
			window, reinterpret_cast<HMENU>(ID_STATUS), instance, nullptr);

	HWND ok = CreateWindowExA(0, "BUTTON", "OK",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
			CLIENT_WIDTH - MARGIN - 2 * 84 - 8, acceptY, 84, 26,
			window, reinterpret_cast<HMENU>(ID_OK), instance, nullptr);
	CreateWindowExA(0, "BUTTON", "Cancel",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
			CLIENT_WIDTH - MARGIN - 84, acceptY, 84, 26,
			window, reinterpret_cast<HMENU>(ID_CANCEL), instance, nullptr);

	HFONT font = uiFont();
	EnumChildWindows(window, [](HWND child, LPARAM param) -> BOOL {
		SendMessage(child, WM_SETFONT, static_cast<WPARAM>(param), TRUE);
		return TRUE;
	}, reinterpret_cast<LPARAM>(font));
	HFONT mono = listFont();
	SendMessage(state.list, WM_SETFONT, reinterpret_cast<WPARAM>(mono), TRUE);

	// Pads first: the device list names them, so it cannot be built until they
	// are open.
	openPads(&state);
	refreshDevices(&state);
	refreshList(&state);

	if (parent)
		EnableWindow(static_cast<HWND>(parent), FALSE);
	ShowWindow(window, SW_SHOW);
	SetFocus(ok);

	MSG message;
	while (GetMessage(&message, nullptr, 0, 0) > 0) {
		// While capturing, key presses belong to the binding rather than to the
		// dialog's navigation: without this, binding Tab or Enter would move
		// the focus or press OK instead.
		if (!state.capturing && IsDialogMessage(window, &message))
			continue;
		TranslateMessage(&message);
		DispatchMessage(&message);
	}

	closePads(&state);
	if (parent) {
		EnableWindow(static_cast<HWND>(parent), TRUE);
		SetForegroundWindow(static_cast<HWND>(parent));
	}
	DeleteObject(mono);
	DeleteObject(font);

	if (state.accepted)
		model.apply(config);
	return state.accepted;
}

} // namespace nesfe

#endif // _WIN32
