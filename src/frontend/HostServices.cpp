#include "HostServices.h"

#include <SDL.h>

#include <cstring>

namespace nesfe {

HostServices::HostServices(nesgui::Config* config, const std::string& configPath) :
		m_config(config), m_configPath(configPath), m_writes(0) {
	std::memset(&m_host, 0, sizeof(m_host));
	m_host.size = sizeof(nes_host);
	m_host.context = this;
	m_host.get_frame = &HostServices::frameThunk;
	m_host.window_handle = &HostServices::windowThunk;
	m_host.log = &HostServices::logThunk;
	m_host.get_setting = &HostServices::getSettingThunk;
	m_host.set_setting = &HostServices::setSettingThunk;
}

void HostServices::setFrameSource(
		std::function<const std::uint8_t*(int*, int*)> frames) {
	m_frames = frames;
}

void HostServices::setWindowSource(std::function<void*()> window) {
	m_window = window;
}

const std::uint8_t* HostServices::frameThunk(void* context, int* width, int* height) {
	HostServices* self = static_cast<HostServices*>(context);
	if (!self || !self->m_frames)
		return nullptr;
	return self->m_frames(width, height);
}

void* HostServices::windowThunk(void* context) {
	HostServices* self = static_cast<HostServices*>(context);
	if (!self || !self->m_window)
		return nullptr;
	return self->m_window();
}

void HostServices::logThunk(void* context, const char* message) {
	(void)context;
	if (message)
		SDL_Log("%s", message);
}

std::size_t HostServices::getSettingThunk(void* context, const char* id,
		const char* key, char* value, std::size_t valueSize) {
	HostServices* self = static_cast<HostServices*>(context);
	if (!self || !self->m_config || !id || !key)
		return 0;

	const std::string stored = self->m_config->pluginSetting(id, key);
	if (value && valueSize > 0) {
		// Copy what fits and terminate regardless. The return is the full
		// length either way, so a caller that cares can tell it was cut short
		// and ask again with more room -- the same contract as snprintf, which
		// is the one every C programmer already knows.
		const std::size_t room = valueSize - 1;
		const std::size_t copied = stored.size() < room ? stored.size() : room;
		std::memcpy(value, stored.data(), copied);
		value[copied] = '\0';
	}
	return stored.size();
}

void HostServices::setSettingThunk(void* context, const char* id, const char* key,
		const char* value) {
	HostServices* self = static_cast<HostServices*>(context);
	if (!self || !self->m_config || !id || !key)
		return;

	self->m_config->setPluginSetting(id, key, value ? value : "");
	self->m_writes++;
	// Written through rather than at shutdown. A plugin's dialog is a place
	// people change one thing and then close the program, and losing it to a
	// crash on the way out would be indistinguishable from the dialog not
	// working.
	if (!self->m_configPath.empty())
		self->m_config->save(self->m_configPath);
}

} // namespace nesfe
