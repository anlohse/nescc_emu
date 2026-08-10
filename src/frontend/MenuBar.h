#ifndef NES_FRONTEND_MENU_BAR_H
#define NES_FRONTEND_MENU_BAR_H

//
// A real menu bar on the emulator's window.
//
// The window belongs to the video plugin; the menu belongs to the host, because
// what is on it -- files, save slots, dialogs -- is host business that no video
// plugin should have to know about. So the host attaches a menu to a window it
// did not create, using the handle the plugin already exports for parenting
// dialogs.
//
// That is a deliberate exception to the rule that a plugin owns its window, and
// the alternative was worse: an ABI call obliging every video plugin ever
// written to host a menu, for the benefit of a host feature. A plugin that
// exports no handle simply gets no menu, on Windows or anywhere else.
//
// Two consequences of a native menu, both handled here:
//
//   - It eats into the client area. The window is grown by exactly the menu's
//     height so the picture keeps the size the player asked for.
//   - While a menu is open the platform runs its own message loop, and the
//     emulator stops. That is normal for a native menu and is why the caller
//     is told to treat the time as lost rather than as a stall to catch up on.
//

#include "MenuModel.h"

#include <string>

namespace nesfe {

/** Whether this build can put a menu on a window at all. */
bool menuBarAvailable();

/**
 * Attach a menu built from @p sections to @p window.
 *
 * Replaces any menu already there, so this doubles as "refresh": rebuild from
 * the model and hand it over whenever the state behind it changes.
 *
 * @param window native handle, or null to do nothing
 * @param grow   true the first time, to enlarge the window by the menu's
 *               height. Subsequent refreshes must not grow it again.
 * @return false when there is no menu to be had.
 */
bool setMenuBar(void* window, const std::vector<MenuSection>& sections, bool grow);

/**
 * The action chosen since the last call, or MENU_NONE.
 *
 * A queue of one: menus are driven by a person, and a person cannot outrun a
 * frame. Reading it clears it.
 */
int takeMenuAction();

/**
 * Offer a platform message to the menu.
 *
 * The host does not own the window procedure *or* the event pump -- the input
 * plugin drains SDL's queue, and an event it does not recognise is dropped. So
 * a menu command reaches here through a platform message hook instead, which is
 * the one route that does not run through somebody else's plugin.
 *
 * @return true when the message was a menu command and has been consumed.
 */
bool handleMenuCommand(unsigned message, unsigned long long wParam,
		long long lParam);

/** Put up the About box. Here because it is the same platform problem. */
void showAboutBox(void* parent, const char* text);

/**
 * Show a page of read-only text, scrollable, with nothing to fill in.
 *
 * A message box would do for three lines and not for thirty: it cannot scroll,
 * and it centres what it shows, which ruins two columns meant to line up.
 */
void showTextBox(void* parent, const char* title, const std::string& text);

} // namespace nesfe

#endif // NES_FRONTEND_MENU_BAR_H
