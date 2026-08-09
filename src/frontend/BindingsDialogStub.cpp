//
// No bindings dialog on this platform yet.
//
// The model it would drive is in BindingModel and is already tested, so a GTK
// or Cocoa version is a window and a key-to-scancode mapping and nothing else.
//

#include "BindingsDialog.h"

#if !defined(_WIN32)

namespace nesfe {

bool bindingsDialogAvailable() {
	return false;
}

bool showBindingsDialog(nesgui::Config* /*config*/, void* /*parent*/) {
	return false;
}

} // namespace nesfe

#endif // !_WIN32
