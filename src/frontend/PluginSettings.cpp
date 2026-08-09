#include "PluginSettings.h"

#include "../plugin/Module.h"

namespace nesfe {

namespace {

const nes_plugin_kind KINDS[PluginSettings::KIND_COUNT] = {
	NES_PLUGIN_VIDEO, NES_PLUGIN_AUDIO, NES_PLUGIN_INPUT
};

const char* LABELS[PluginSettings::KIND_COUNT] = { "Video", "Audio", "Controller" };

std::string configuredId(const nesgui::Config& config, int index) {
	switch (index) {
	case 0:  return config.videoPlugin;
	case 1:  return config.audioPlugin;
	default: return config.inputPlugin;
	}
}

} // namespace

nes_plugin_kind PluginSettings::kindAt(int index) {
	return KINDS[index];
}

const char* PluginSettings::kindLabel(int index) {
	return LABELS[index];
}

PluginSettings::PluginSettings(const nesplug::Registry& registry,
		const nesgui::Config& config, const nes_host* host) :
		m_registry(registry), m_host(host), m_scale(config.scale),
		m_fullscreen(config.fullscreen), m_changed(false) {

	for (int index = 0; index < KIND_COUNT; index++) {
		const std::vector<const nesplug::Entry*> entries =
				registry.ofKind(KINDS[index]);
		for (std::size_t i = 0; i < entries.size(); i++) {
			PluginChoice choice;
			choice.id = entries[i]->info->id;
			choice.name = entries[i]->info->name;
			choice.version = entries[i]->info->version ? entries[i]->info->version : "";
			choice.path = entries[i]->path;
			choice.configurable = nesplug::hasConfigureDialog(*entries[i]);
			m_choices[index].push_back(choice);
		}

		// Start on whatever the emulator would actually use, which is not
		// always what the file says: a config naming a plugin that is no longer
		// installed falls back to the first, and the dialog has to show the
		// fallback rather than a name that is not in the list.
		m_selected[index] = -1;
		const nesplug::Entry* selected =
				registry.select(KINDS[index], configuredId(config, index));
		if (!selected)
			continue;
		for (std::size_t i = 0; i < m_choices[index].size(); i++)
			if (m_choices[index][i].id == selected->info->id)
				m_selected[index] = static_cast<int>(i);
	}
}

const std::vector<PluginChoice>& PluginSettings::choicesFor(int index) const {
	return m_choices[index];
}

int PluginSettings::selectedIndex(int index) const {
	return m_selected[index];
}

std::string PluginSettings::selectedId(int index) const {
	const int choice = m_selected[index];
	if (choice < 0)
		return std::string();
	return m_choices[index][choice].id;
}

bool PluginSettings::select(int index, int choice) {
	if (choice < 0 || choice >= static_cast<int>(m_choices[index].size()))
		return false;
	if (m_selected[index] != choice) {
		m_selected[index] = choice;
		m_changed = true;
	}
	return true;
}

bool PluginSettings::setScale(int scale) {
	if (scale < MIN_SCALE || scale > MAX_SCALE)
		return false;
	if (m_scale != scale) {
		m_scale = scale;
		m_changed = true;
	}
	return true;
}

void PluginSettings::setFullscreen(bool fullscreen) {
	if (m_fullscreen != fullscreen) {
		m_fullscreen = fullscreen;
		m_changed = true;
	}
}

bool PluginSettings::canConfigure(int index) const {
	const int choice = m_selected[index];
	return choice >= 0 && m_choices[index][choice].configurable;
}

bool PluginSettings::configure(int index) {
	if (!canConfigure(index))
		return false;

	const nesplug::Entry* entry =
			m_registry.find(KINDS[index], selectedId(index));
	if (!entry)
		return false;

	switch (KINDS[index]) {
	case NES_PLUGIN_VIDEO: {
		std::unique_ptr<nesplug::VideoPlugin> plugin =
				nesplug::createFrom<nesplug::VideoPlugin>(*entry, m_host);
		if (!plugin)
			return false;
		plugin->configure();
		return true;
	}
	case NES_PLUGIN_AUDIO: {
		std::unique_ptr<nesplug::AudioPlugin> plugin =
				nesplug::createFrom<nesplug::AudioPlugin>(*entry, m_host);
		if (!plugin)
			return false;
		plugin->configure();
		return true;
	}
	default: {
		std::unique_ptr<nesplug::InputPlugin> plugin =
				nesplug::createFrom<nesplug::InputPlugin>(*entry, m_host);
		if (!plugin)
			return false;
		plugin->configure();
		return true;
	}
	}
}

void PluginSettings::apply(nesgui::Config* config) const {
	config->videoPlugin = selectedId(0);
	config->audioPlugin = selectedId(1);
	config->inputPlugin = selectedId(2);
	config->scale = m_scale;
	config->fullscreen = m_fullscreen;
}

} // namespace nesfe
