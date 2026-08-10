#ifndef NES_FRONTEND_FILE_DIALOG_H
#define NES_FRONTEND_FILE_DIALOG_H

//
// Asking a person for a file.
//
// One platform call behind one function, for the same reason every other dialog
// here is: the run loop must not know what a file picker is, and the picker must
// not know what a console is.
//

#include <string>

namespace nesfe {

/** Whether this build can ask for a file at all. */
bool fileDialogAvailable();

/**
 * Ask for a .nes file to open.
 *
 * @param parent    native window handle to sit over, or null
 * @param startDir  where to open, or empty for wherever the platform prefers
 * @param chosen    filled in on success
 * @return false when the person cancelled, or when there is no picker here --
 *         which a caller treats identically, because both mean no file.
 */
bool chooseRomFile(void* parent, const std::string& startDir, std::string* chosen);

/**
 * When @p path was last written, as "YYYY-MM-DD HH:MM", or empty if it is not
 * there.
 *
 * Read from the file rather than remembered, so a state written by an earlier
 * run -- or deleted behind the program's back -- is still described correctly.
 */
std::string fileWrittenAt(const std::string& path);

} // namespace nesfe

#endif // NES_FRONTEND_FILE_DIALOG_H
