#ifndef NES_FRONTEND_BINDINGS_DIALOG_H
#define NES_FRONTEND_BINDINGS_DIALOG_H

#include "BindingModel.h"

namespace nesfe {

/**
 * Show the controller bindings and let them be changed by pressing things.
 *
 * @param config  edited in place if the player accepts. Only the bindings are
 *                touched; the rest of the file is not this dialog's business.
 * @param parent  native window handle to sit over, or null.
 * @return true when accepted.
 */
bool showBindingsDialog(nesgui::Config* config, void* parent);

/** False on platforms with no implementation yet. */
bool bindingsDialogAvailable();

#if defined(_WIN32)
/**
 * Turn a Windows key press into the SDL scancode the configuration speaks.
 *
 * Printable keys go through SDL itself, via the character the current layout
 * produces, so a French or German keyboard binds the key someone actually
 * pressed rather than the one in the same place on a US board. Only the keys
 * with no character -- arrows, modifiers, function keys, the keypad -- need a
 * table, and that is small enough to read.
 *
 * Exposed for testing. @return SDL_SCANCODE_UNKNOWN for anything unmapped.
 */
SDL_Scancode scancodeFromVirtualKey(unsigned virtualKey, bool extended);
#endif

} // namespace nesfe

#endif // NES_FRONTEND_BINDINGS_DIALOG_H
