#ifndef NES_FRONTEND_PLUGIN_SETTINGS_H
#define NES_FRONTEND_PLUGIN_SETTINGS_H

//
// What the plugin dialog is about, with no dialog in it.
//
// Which plugins exist, which is chosen for each job, whether the chosen one has
// settings of its own, and what gets written back to nes.cfg. All of that is
// ordinary logic and is tested as such; the window that displays it is a
// separate file per platform and contains no decisions.
//
// The split matters more here than it looks. A settings dialog is the part of a
// program most likely to be wrong in ways nobody notices -- a choice that does
// not persist, a plugin that vanishes from the list, a Cancel that applies
// anyway -- and none of those need a window to catch.
//

#include "../GuiConfig.h"
#include "../plugin/PluginHost.h"

#include <string>
#include <vector>

namespace nesfe {

/** One selectable plugin, as a dialog wants it. */
struct PluginChoice {
	std::string id;
	std::string name;
	std::string version;
	/** Empty for a built-in; the file it was loaded from otherwise. */
	std::string path;
	/** True when this plugin offers a settings dialog of its own. */
	bool configurable = false;
};

/**
 * The three jobs, and what can do each of them.
 *
 * Built from the registry the host already assembled, so the list is exactly
 * what the emulator would use -- there is no second discovery pass that could
 * disagree with the first.
 */
class PluginSettings {
public:
	PluginSettings(const nesplug::Registry& registry, const nesgui::Config& config);

	static const int KIND_COUNT = 3;
	/** Index 0, 1, 2 in the order a dialog should show them. */
	static nes_plugin_kind kindAt(int index);
	static const char* kindLabel(int index);

	const std::vector<PluginChoice>& choicesFor(int index) const;

	/** Index into choicesFor(), or -1 when nothing of that kind exists. */
	int selectedIndex(int index) const;
	std::string selectedId(int index) const;

	/** @return false if @p choice is out of range. */
	bool select(int index, int choice);

	/** True when the selected plugin of that kind has a dialog to open. */
	bool canConfigure(int index) const;

	/**
	 * Open the selected plugin's own dialog.
	 *
	 * Creates a throwaway instance to do it. A plugin's settings belong to the
	 * plugin, not to a live instance of it, and asking the one currently
	 * driving the emulator to put up a modal window mid-frame is a good way to
	 * find out which plugins are not reentrant.
	 *
	 * @return false when there is nothing to open.
	 */
	bool configure(int index);

	/** True once anything has been selected that differs from the start. */
	bool changed() const { return m_changed; }

	/**
	 * Write the selections into @p config.
	 *
	 * Only the plugin ids -- everything else in the configuration belongs to
	 * somebody else and is left alone.
	 */
	void apply(nesgui::Config* config) const;

private:
	const nesplug::Registry& m_registry;
	std::vector<PluginChoice> m_choices[KIND_COUNT];
	int m_selected[KIND_COUNT];
	bool m_changed;
};

} // namespace nesfe

#endif // NES_FRONTEND_PLUGIN_SETTINGS_H
