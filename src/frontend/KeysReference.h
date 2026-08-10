#ifndef NES_FRONTEND_KEYS_REFERENCE_H
#define NES_FRONTEND_KEYS_REFERENCE_H

//
// What every key and button currently does, as text.
//
// Read from the configuration rather than from a list written here, because a
// reference page that does not follow the bindings is worse than none: somebody
// rebinds a key, reads this, and is told something untrue.
//
// A string rather than a window, so what it says can be tested. The one thing
// worth checking about a help page is that it describes the machine somebody is
// actually holding.
//

#include "../GuiConfig.h"

#include <string>

namespace nesfe {

/**
 * The whole reference, ready to put in a text box.
 *
 * Lines are separated by "\n"; the caller decides what a line break means to
 * whatever control it is using.
 */
std::string keysReferenceText(const nesgui::Config& config);

} // namespace nesfe

#endif // NES_FRONTEND_KEYS_REFERENCE_H
