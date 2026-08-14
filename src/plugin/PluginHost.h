#ifndef NES_PLUGIN_HOST_H
#define NES_PLUGIN_HOST_H

//
// The host side of the plugin boundary.
//
// Two jobs. First, a registry: what plugins exist, of what kind, and which one
// is selected. Second, a set of adapters that present a C api struct as the
// C++ interface the run loop already speaks -- so App.cpp does not know that
// anything crosses a module boundary, and neither do its tests.
//
// Nothing here loads a library yet. The built-in SDL backends register
// themselves through the same C structs a loadable module would export, which
// means the boundary is exercised on every run and a mistake in its shape shows
// up as a compiler error rather than as a crash inside somebody's .dll.
//

#include "nes_plugin.h"
#include "../frontend/Backend.h"

#include <memory>
#include <string>
#include <vector>

namespace nesplug {

class Module;

/**
 * Base for the three adapters: holds the module the instance came from.
 *
 * A plugin instance calls functions that live inside its library, so the
 * library must outlive it. Rather than trusting an ordering somewhere in
 * main(), every adapter keeps a reference to its own module and the question
 * answers itself. A built-in plugin has no module and keeps nothing.
 */
class ModuleBound {
public:
	virtual ~ModuleBound();
	void keepAlive(const std::shared_ptr<const Module>& module) { m_module = module; }

private:
	std::shared_ptr<const Module> m_module;
};

/** One available plugin: its descriptor and its api, however it got here. */
struct Entry {
	const nes_plugin_info* info;
	const void* api;
	/** Empty for a built-in; the file it came from otherwise. */
	std::string path;
	/**
	 * The library this came out of, or null for a built-in.
	 *
	 * Held so that an entry outliving the loader's own vector still keeps its
	 * library mapped, and so that anything created from it can inherit the
	 * reference rather than depending on somebody's scope.
	 */
	std::shared_ptr<const Module> module;

	Entry() : info(nullptr), api(nullptr) { }
};

/**
 * Everything the host knows about plugins.
 *
 * Registration is explicit rather than magic: a built-in calls add(), and a
 * future loader will call the same function after checking the version. There
 * is exactly one path in, so there is exactly one place the version handshake
 * can be skipped -- and it is not skipped.
 */
class Registry {
public:
	/**
	 * Offer a plugin.
	 *
	 * @param abiVersion  what the module reports. A mismatch is refused, said
	 *                    out loud through @p warning, and is not a fatal error:
	 *                    one stale plugin should cost that plugin, not the run.
	 * @return false if it was refused.
	 */
	bool add(std::uint32_t abiVersion, const nes_plugin_info* info, const void* api,
			const std::string& path = std::string(), std::string* warning = nullptr,
			const std::shared_ptr<const Module>& module = std::shared_ptr<const Module>());

	/** Everything registered of one kind, in registration order. */
	std::vector<const Entry*> ofKind(nes_plugin_kind kind) const;

	/** By id, or null. Ids are what the config file stores. */
	const Entry* find(nes_plugin_kind kind, const std::string& id) const;

	/**
	 * The one to use: @p preferredId if it is present, else the first of that
	 * kind, else null. Falling back rather than failing means a config naming a
	 * plugin that is no longer installed still starts.
	 */
	const Entry* select(nes_plugin_kind kind, const std::string& preferredId) const;

	std::size_t size() const { return m_entries.size(); }
	unsigned long refused() const { return m_refused; }

private:
	std::vector<Entry> m_entries;
	unsigned long m_refused = 0;
};

/**
 * True when @p entry's plugin offers a settings dialog of its own.
 *
 * Two ways it may not: the pointer is null, or the module was built against an
 * older header that had no such field and its declared size stops short of one.
 * Both mean the same thing to a caller, so both answer false.
 */
bool hasConfigureDialog(const Entry& entry);

/**
 * What became of an attempt to apply changed settings to a running plugin.
 *
 * Three answers rather than two, because "it did not work" and "it cannot be
 * asked" deserve different words to a person: one is a limit of this plugin, the
 * other a limit of what any plugin can do to itself while running. The host says
 * which, instead of leaving somebody to wonder whether pressing OK did anything.
 */
enum ApplyResult {
	APPLY_UNSUPPORTED,   /**< the plugin offers no such call at all */
	APPLY_PARTIAL,       /**< some of it needs a restart, and the plugin says so */
	APPLY_DONE           /**< everything the dialog changed is in effect now */
};

/* ------------------------------------------------------------------------- */
/* Adapters                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Each of these owns one plugin instance and presents it as the corresponding
 * nesfe interface. They are deliberately thin: no policy, no caching, no
 * cleverness -- anything that thinks belongs in App.cpp where it can be tested
 * without a plugin at all.
 */

class VideoPlugin : public nesfe::VideoSink, public ModuleBound {
public:
	VideoPlugin(const nes_video_api* api, const nes_host* host);
	~VideoPlugin();

	bool open(const nesfe::VideoOptions& options, nesfe::Error* error) override;
	void close() override;
	void present(const std::uint8_t* indices, int width, int height) override;
	void setTitle(const char* title) override;
	bool saveScreenshot(const char* path) override;

	/** Show the plugin's own dialog, if it has one. */
	void configure();

	/** Re-read the settings and act on them now. @see ApplyResult */
	ApplyResult applySettings();

	/** Native window handle for a dialog to sit over, or null. */
	void* nativeWindow() const;

	/**
	 * Which console pixel a window pixel falls on.
	 *
	 * @return false when the point is outside the picture, or when this plugin
	 *         is too old to answer. Both mean the same to a light gun: pointed
	 *         somewhere that is not the television.
	 */
	bool windowToFrame(int windowX, int windowY, int* frameX, int* frameY) const;

private:
	const nes_video_api* m_api;
	void* m_self;
};

class AudioPlugin : public nesfe::AudioSink, public ModuleBound {
public:
	AudioPlugin(const nes_audio_api* api, const nes_host* host);
	~AudioPlugin();

	bool open(int sampleRate, nesfe::Error* error) override;
	void close() override;
	bool isOpen() const override;
	void queue(const float* samples, std::size_t count) override;
	double queuedSeconds() const override;
	void clear() override;

	void configure();

	/** Re-read the settings and act on them now. @see ApplyResult */
	ApplyResult applySettings();

private:
	const nes_audio_api* m_api;
	void* m_self;
};

class InputPlugin : public nesfe::InputSource, public ModuleBound {
public:
	InputPlugin(const nes_input_api* api, const nes_host* host);
	~InputPlugin();

	bool open(nesfe::Error* error) override;
	void close() override;
	void poll(nesfe::InputState* out) override;

	/**
	 * Ask for the light gun, if this plugin has one.
	 * @return false when it does not, leaving @p out untouched.
	 */
	bool pollZapper(nesfe::ZapperState* out);

	void configure();

	/** Re-read the settings and act on them now. @see ApplyResult */
	ApplyResult applySettings();

private:
	const nes_input_api* m_api;
	void* m_self;
};

} // namespace nesplug

#endif // NES_PLUGIN_HOST_H
