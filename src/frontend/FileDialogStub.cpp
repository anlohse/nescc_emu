#include "FileDialog.h"

#if !defined(_WIN32)

namespace nesfe {

bool fileDialogAvailable() {
	return false;
}

bool chooseRomFile(void*, const std::string&, std::string*) {
	// No picker here yet. A ROM named on the command line still works, which is
	// how every run worked before there was a menu.
	return false;
}

} // namespace nesfe

#endif // !_WIN32
