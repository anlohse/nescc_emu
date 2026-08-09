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
	/**
	 * @param host what a plugin's own dialog gets to ask of the program. Null
	 *             is allowed and means a plugin opening its dialog has nowhere
	 *             to store what it decides, which is why the emulator always
	 *             passes one and only the tests leave it out.
	 */
	PluginSettings(const nesplug::Registry& registry, const nesgui::Config& config,
			const nes_host* host = nullptr);

	/* --- Display -------------------------------------------------------- */
	/*
	 * Not a plugin's business, despite living in the same dialog. The host
	 * reads these and passes them to whichever video plugin is loaded, so the
	 * host is where they are edited -- a plugin that also wrote them would be a
	 * second author of one setting, and the two would disagree the first time
	 * somebody used the command line.
	 */
	static const int MIN_SCALE = 1;
	static const int MAX_SCALE = 8;

	int scale() const { return m_scale; }
	/** @return false when @p scale is outside MIN_SCALE..MAX_SCALE. */
	bool setScale(int scale);

	bool fullscreen() const { return m_fullscreen; }
	void setFullscreen(bool fullscreen);

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
	 * Write the selections and the display settings into @p config.
	 *
	 * Nothing else: the bindings, and every plugin's own settings, belong to
	 * somebody else and are left exactly as they were found.
	 */
	void apply(nesgui::Config* config) const;

private:
	const nesplug::Registry& m_registry;
	const nes_host* m_host;
	std::vector<PluginChoice> m_choices[KIND_COUNT];
	int m_selected[KIND_COUNT];
	int m_scale;
	bool m_fullscreen;
	bool m_changed;
};

} // namespace nesfe

#endif // NES_FRONTEND_PLUGIN_SETTINGS_H
