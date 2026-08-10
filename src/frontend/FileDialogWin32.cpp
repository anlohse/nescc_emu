#include "FileDialog.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

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


std::string fileWrittenAt(const std::string& path) {
	if (path.empty())
		return std::string();
	struct _stat64 info;
	if (_stat64(path.c_str(), &info) != 0)
		return std::string();

	std::tm parts;
	if (localtime_s(&parts, &info.st_mtime) != 0)
		return std::string();
	char text[32];
	std::snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d",
			parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
			parts.tm_hour, parts.tm_min);
	return text;
}

} // namespace nesfe

#endif // _WIN32
