#ifndef NES_FRONTEND_SETTINGS_DIALOG_H
#define NES_FRONTEND_SETTINGS_DIALOG_H

#include "PluginSettings.h"

namespace nesfe {

/**
 * Show the plugin chooser and, if the player accepts it, apply the result.
 *
 * The only platform-bound function in the front end. Everything it decides
 * lives in PluginSettings, which is tested; this puts controls on a window and
 * reports which button was pressed.
 *
 * @param parent  native window handle to sit over, or null.
 * @return true when the player accepted, in which case @p settings holds the
 *         new selection. False on cancel, and nothing is changed.
 */
bool showSettingsDialog(PluginSettings* settings, void* parent);

/**
 * Whether this build can show one at all.
 *
 * False on platforms with no implementation yet. A caller should say so rather
 * than opening nothing and leaving the player wondering which key they missed.
 */
bool settingsDialogAvailable();

} // namespace nesfe

#endif // NES_FRONTEND_SETTINGS_DIALOG_H
