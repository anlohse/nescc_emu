//
// No plugin chooser on this platform yet.
//
// Reporting that honestly rather than opening nothing: a key that appears to do
// nothing is worse than one that says why. The caller prints the reason and the
// player still has nes.cfg, which is where the same settings live.
//
// A GTK or Cocoa implementation goes beside this file and takes the same two
// functions. Nothing above this line would change -- everything the dialog
// decides is in PluginSettings already.
//

#include "SettingsDialog.h"

#if !defined(_WIN32)

namespace nesfe {

bool settingsDialogAvailable() {
	return false;
}

bool showSettingsDialog(PluginSettings* /*settings*/, void* /*parent*/) {
	return false;
}

} // namespace nesfe

#endif // !_WIN32
