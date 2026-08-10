#include "FileDialog.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>

#include <cstring>

namespace nesfe {

bool fileDialogAvailable() {
	return true;
}

bool chooseRomFile(void* parent, const std::string& startDir, std::string* chosen) {
	if (!chosen)
		return false;

	char file[MAX_PATH] = { 0 };

	// The filter is a run of NUL-terminated pairs ending in a second NUL, which
	// is why it cannot be an ordinary string literal.
	static const char FILTER[] =
			"NES ROMs (*.nes)\0*.nes\0"
			"All files (*.*)\0*.*\0";

	OPENFILENAMEA open;
	ZeroMemory(&open, sizeof(open));
	open.lStructSize = sizeof(open);
	open.hwndOwner = static_cast<HWND>(parent);
	open.lpstrFilter = FILTER;
	open.lpstrFile = file;
	open.nMaxFile = sizeof(file);
	open.lpstrTitle = "Load ROM";
	open.lpstrInitialDir = startDir.empty() ? nullptr : startDir.c_str();
	// NOCHANGEDIR because a file picker has no business moving the process's
	// working directory: nes.cfg and the plugins folder are found relative to
	// the executable, but a screenshot is written where the program is standing.
	open.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR
			| OFN_EXPLORER;

	if (!GetOpenFileNameA(&open))
		return false;      // cancelled, or closed; not an error
	*chosen = file;
	return true;
}

} // namespace nesfe

#endif // _WIN32
