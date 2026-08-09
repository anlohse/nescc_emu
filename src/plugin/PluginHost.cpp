#include "PluginHost.h"

#include <cstdio>
#include <cstring>

namespace nesplug {

// Out of line so the destructor of the shared_ptr member -- and with it any
// call to Module's destructor -- is emitted where Module is a complete type.
ModuleBound::~ModuleBound() { }

namespace {

/**
 * Is @p api big enough to contain @p member?
 *
 * Every api struct starts with its own size so the host can tell what a module
 * built against an older header actually provides. Checking that before calling
 * through a pointer is what makes adding a field a non-breaking change instead
 * of a jump into whatever follows the struct in memory.
 */
template <typename Api, typename Member>
bool provides(const Api* api, Member Api::* member) {
	const std::size_t offset =
			reinterpret_cast<const char*>(&(api->*member))
			- reinterpret_cast<const char*>(api);
	return api->size >= offset + sizeof(Member) && api->*member != nullptr;
}

} // namespace

/* ------------------------------------------------------------------------- */
/* Registry                                                                   */
/* ------------------------------------------------------------------------- */

bool Registry::add(std::uint32_t abiVersion, const nes_plugin_info* info,
		const void* api, const std::string& path, std::string* warning,
		const std::shared_ptr<const Module>& module) {
	const char* where = path.empty() ? "built-in plugin" : path.c_str();

	// The version first, and before touching anything else the module gave us:
	// if the ABI changed, the descriptor could have changed shape too, so even
	// reading its name would be a guess.
	if (abiVersion != NES_PLUGIN_ABI_VERSION) {
		if (warning) {
			char buffer[256];
			std::snprintf(buffer, sizeof(buffer),
					"%s: ABI version %u, expected %u -- not loaded",
					where, abiVersion, NES_PLUGIN_ABI_VERSION);
			*warning = buffer;
		}
		m_refused++;
		return false;
	}

	if (!info || !api || info->size < sizeof(nes_plugin_info)
			|| !info->id || !info->name) {
		if (warning)
			*warning = std::string(where) + ": incomplete descriptor -- not loaded";
		m_refused++;
		return false;
	}

	// Ids have to be unique within a kind: the config file stores one, and two
	// plugins answering to it would make which you get a matter of load order.
	if (find(static_cast<nes_plugin_kind>(info->kind), info->id)) {
		if (warning)
			*warning = std::string(where) + ": duplicate id '" + info->id
					+ "' -- not loaded";
		m_refused++;
		return false;
	}

	Entry entry;
	entry.info = info;
	entry.api = api;
	entry.path = path;
	entry.module = module;
	m_entries.push_back(entry);
	return true;
}

std::vector<const Entry*> Registry::ofKind(nes_plugin_kind kind) const {
	std::vector<const Entry*> out;
	for (std::size_t i = 0; i < m_entries.size(); i++)
		if (m_entries[i].info->kind == static_cast<int>(kind))
			out.push_back(&m_entries[i]);
	return out;
}

const Entry* Registry::find(nes_plugin_kind kind, const std::string& id) const {
	for (std::size_t i = 0; i < m_entries.size(); i++)
		if (m_entries[i].info->kind == static_cast<int>(kind)
				&& id == m_entries[i].info->id)
			return &m_entries[i];
	return nullptr;
}

const Entry* Registry::select(nes_plugin_kind kind, const std::string& preferredId) const {
	if (!preferredId.empty()) {
		const Entry* wanted = find(kind, preferredId);
		if (wanted)
			return wanted;
	}
	const std::vector<const Entry*> all = ofKind(kind);
	return all.empty() ? nullptr : all[0];
}

bool hasConfigureDialog(const Entry& entry) {
	if (!entry.info || !entry.api)
		return false;
	switch (entry.info->kind) {
	case NES_PLUGIN_VIDEO:
		return provides(static_cast<const nes_video_api*>(entry.api),
				&nes_video_api::configure);
	case NES_PLUGIN_AUDIO:
		return provides(static_cast<const nes_audio_api*>(entry.api),
				&nes_audio_api::configure);
	case NES_PLUGIN_INPUT:
		return provides(static_cast<const nes_input_api*>(entry.api),
				&nes_input_api::configure);
	default:
		return false;
	}
}

/* ------------------------------------------------------------------------- */
/* Video                                                                      */
/* ------------------------------------------------------------------------- */

VideoPlugin::VideoPlugin(const nes_video_api* api, const nes_host* host) :
		m_api(api), m_self(nullptr) {
	if (m_api && m_api->create)
		m_self = m_api->create(host);
}

VideoPlugin::~VideoPlugin() {
	if (m_self && m_api->destroy)
		m_api->destroy(m_self);
}

bool VideoPlugin::open(const nesfe::VideoOptions& options, nesfe::Error* error) {
	if (!m_self || !m_api->open) {
		if (error) *error = "video plugin has no open()";
		return false;
	}
	// A fixed buffer rather than anything that allocates: the plugin writes into
	// storage the host owns, so nothing has to be freed across the boundary.
	char reason[256];
	reason[0] = '\0';
	const int ok = m_api->open(m_self, options.scale, options.fullscreen ? 1 : 0,
			options.title, reason, sizeof(reason));
	if (!ok && error)
		*error = reason[0] ? reason : "video plugin failed to open";
	return ok != 0;
}

void VideoPlugin::close() {
	if (m_self && m_api->close)
		m_api->close(m_self);
}

void VideoPlugin::present(const std::uint8_t* indices, int width, int height) {
	if (m_self && m_api->present)
		m_api->present(m_self, indices, width, height);
}

void VideoPlugin::setTitle(const char* title) {
	if (m_self && m_api->set_title)
		m_api->set_title(m_self, title);
}

bool VideoPlugin::saveScreenshot(const char* path) {
	if (!m_self || !provides(m_api, &nes_video_api::save_screenshot))
		return false;
	return m_api->save_screenshot(m_self, path) != 0;
}

void VideoPlugin::configure() {
	if (m_self && provides(m_api, &nes_video_api::configure))
		m_api->configure(m_self);
}

void* VideoPlugin::nativeWindow() const {
	if (!m_self || !provides(m_api, &nes_video_api::native_window))
		return nullptr;
	return m_api->native_window(m_self);
}

/* ------------------------------------------------------------------------- */
/* Audio                                                                      */
/* ------------------------------------------------------------------------- */

AudioPlugin::AudioPlugin(const nes_audio_api* api, const nes_host* host) :
		m_api(api), m_self(nullptr) {
	if (m_api && m_api->create)
		m_self = m_api->create(host);
}

AudioPlugin::~AudioPlugin() {
	if (m_self && m_api->destroy)
		m_api->destroy(m_self);
}

bool AudioPlugin::open(int sampleRate, nesfe::Error* error) {
	if (!m_self || !m_api->open) {
		if (error) *error = "audio plugin has no open()";
		return false;
	}
	char reason[256];
	reason[0] = '\0';
	const int ok = m_api->open(m_self, sampleRate, reason, sizeof(reason));
	if (!ok && error)
		*error = reason[0] ? reason : "audio plugin failed to open";
	return ok != 0;
}

void AudioPlugin::close() {
	if (m_self && m_api->close)
		m_api->close(m_self);
}

bool AudioPlugin::isOpen() const {
	return m_self && m_api->is_open && m_api->is_open(m_self) != 0;
}

void AudioPlugin::queue(const float* samples, std::size_t count) {
	if (m_self && m_api->queue)
		m_api->queue(m_self, samples, count);
}

double AudioPlugin::queuedSeconds() const {
	if (!m_self || !m_api->queued_seconds)
		return 0.0;
	return m_api->queued_seconds(m_self);
}

void AudioPlugin::clear() {
	if (m_self && m_api->clear)
		m_api->clear(m_self);
}

void AudioPlugin::configure() {
	if (m_self && provides(m_api, &nes_audio_api::configure))
		m_api->configure(m_self);
}

/* ------------------------------------------------------------------------- */
/* Input                                                                      */
/* ------------------------------------------------------------------------- */

InputPlugin::InputPlugin(const nes_input_api* api, const nes_host* host) :
		m_api(api), m_self(nullptr) {
	if (m_api && m_api->create)
		m_self = m_api->create(host);
}

InputPlugin::~InputPlugin() {
	if (m_self && m_api->destroy)
		m_api->destroy(m_self);
}

bool InputPlugin::open(nesfe::Error* error) {
	if (!m_self || !m_api->open) {
		if (error) *error = "input plugin has no open()";
		return false;
	}
	char reason[256];
	reason[0] = '\0';
	const int ok = m_api->open(m_self, reason, sizeof(reason));
	if (!ok && error)
		*error = reason[0] ? reason : "input plugin failed to open";
	return ok != 0;
}

void InputPlugin::close() {
	if (m_self && m_api->close)
		m_api->close(m_self);
}

void InputPlugin::poll(nesfe::InputState* out) {
	if (!m_self || !m_api->poll)
		return;
	// The C struct and the C++ one are separate types on purpose: the C++ side
	// is free to grow fields the ABI does not have, and the ABI is free to stay
	// still. Copying three members is a small price for that independence.
	nes_input_state state;
	std::memset(&state, 0, sizeof(state));
	m_api->poll(m_self, &state);

	out->buttons[0] = state.buttons[0];
	out->buttons[1] = state.buttons[1];
	out->commands = state.commands;
	out->turbo = state.turbo != 0;
}

void InputPlugin::configure() {
	if (m_self && provides(m_api, &nes_input_api::configure))
		m_api->configure(m_self);
}

} // namespace nesplug
