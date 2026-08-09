#ifndef NES_FRONTEND_HOST_SERVICES_H
#define NES_FRONTEND_HOST_SERVICES_H

//
// The host's side of the plugin boundary.
//
// nes_plugin.h says what a plugin may ask of the program that loaded it; this
// is the program answering. It exists as a class rather than a few free
// functions because the answers need state -- the configuration, the window,
// the frame currently on screen -- and a C callback has nowhere to keep any of
// it except the context pointer it is handed back.
//
// Everything here is a thunk: unpack the context, call a C++ method, and let
// nothing escape. No exception may cross back into a plugin's C frame, so the
// thunks are written so that none can be thrown in the first place.
//

#include "../GuiConfig.h"
#include "../plugin/nes_plugin.h"

#include <cstdint>
#include <functional>
#include <string>

namespace nesfe {

class HostServices {
public:
	/**
	 * @param config     the live configuration, which settings are written into
	 * @param configPath where to persist it; empty means do not
	 */
	HostServices(nesgui::Config* config, const std::string& configPath);

	/**
	 * The struct to hand a plugin, valid as long as this object is.
	 *
	 * Which is the reason this must outlive every plugin created with it: a
	 * plugin is entitled to hold the pointer and call through it whenever it
	 * likes, and an instance that outlived its host would be calling into a
	 * destroyed object.
	 */
	const nes_host* handle() const { return &m_host; }

	/** Where the picture is, for a plugin that needs to see it. */
	void setFrameSource(
			std::function<const std::uint8_t*(int*, int*)> frames);
	/** The native window handle, for a plugin parenting a dialog. */
	void setWindowSource(std::function<void*()> window);

	/** How many settings writes have been persisted; for tests. */
	int writes() const { return m_writes; }

private:
	static const std::uint8_t* frameThunk(void* context, int* width, int* height);
	static void* windowThunk(void* context);
	static void logThunk(void* context, const char* message);
	static std::size_t getSettingThunk(void* context, const char* id,
			const char* key, char* value, std::size_t valueSize);
	static void setSettingThunk(void* context, const char* id, const char* key,
			const char* value);

	nes_host m_host;
	nesgui::Config* m_config;
	std::string m_configPath;
	std::function<const std::uint8_t*(int*, int*)> m_frames;
	std::function<void*()> m_window;
	int m_writes;
};

} // namespace nesfe

#endif // NES_FRONTEND_HOST_SERVICES_H
