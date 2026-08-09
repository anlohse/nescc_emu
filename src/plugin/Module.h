#ifndef NES_PLUGIN_MODULE_H
#define NES_PLUGIN_MODULE_H

//
// A loaded plugin library, and the typed way to get an object out of it.
//
// The lifetime rule is the whole reason this class exists. Every function a
// plugin instance calls lives in the library's address space, so unloading the
// library while an instance is alive turns every one of those calls into a jump
// into unmapped memory. It crashes at shutdown, on somebody else's machine, in
// a stack trace that names nothing.
//
// So an instance keeps its module alive rather than the other way round: the
// adapters hold a shared_ptr to the Module they came from, and a Module is only
// ever handed out as a shared_ptr. Getting that ordering wrong is not something
// a comment can prevent, so the types prevent it.
//

#include "PluginHost.h"
#include "nes_plugin.h"

#include <memory>
#include <string>
#include <vector>

namespace nesplug {

/**
 * Which api struct and which descriptor kind an adapter expects.
 *
 * The api is a C struct, so there is no RTTI to ask and no dynamic_cast to
 * make safe. A static_cast from void* would happily reinterpret an audio api as
 * a video one and crash on the third call. Instead the type says what it
 * expects at compile time and Module checks the descriptor at run time -- which
 * is the only combination available here that cannot be fooled.
 */
template <class T> struct PluginTraits;

template <> struct PluginTraits<VideoPlugin> {
	static const nes_plugin_kind kind = NES_PLUGIN_VIDEO;
	typedef nes_video_api ApiType;
};

template <> struct PluginTraits<AudioPlugin> {
	static const nes_plugin_kind kind = NES_PLUGIN_AUDIO;
	typedef nes_audio_api ApiType;
};

template <> struct PluginTraits<InputPlugin> {
	static const nes_plugin_kind kind = NES_PLUGIN_INPUT;
	typedef nes_input_api ApiType;
};

/**
 * One plugin library, open for as long as this object lives.
 *
 * A module provides exactly one kind. That keeps discovery and versioning
 * simple -- one descriptor, one api, one answer to "what is this file for" --
 * and matches how the plugins are split. A library wanting to provide several
 * would need a different entry-point shape, and that is a decision better made
 * before there are files on disk than after.
 */
class Module : public std::enable_shared_from_this<Module> {
public:
	~Module();

	Module(const Module&) = delete;
	Module& operator=(const Module&) = delete;

	/**
	 * Open @p path and check it before believing anything it says.
	 *
	 * The ABI version is read first, and nothing else is called if it does not
	 * match: if the ABI changed, the descriptor could have changed shape too,
	 * so even reading its name would be a guess.
	 *
	 * @return null on any failure, with the reason in @p error. A file that is
	 *         not a plugin, or is a stale one, is not a fatal condition -- the
	 *         caller skips it and carries on.
	 */
	static std::shared_ptr<Module> load(const std::string& path, std::string* error);

	const nes_plugin_info* info() const { return m_info; }
	nes_plugin_kind kind() const { return static_cast<nes_plugin_kind>(m_info->kind); }
	const std::string& path() const { return m_path; }
	const void* api() const { return m_api; }

	/**
	 * Make one instance, as @p T.
	 *
	 * @return null when this module does not provide T's kind. Asking an audio
	 *         library for a video sink is a mistake worth surviving, because
	 *         the answer comes from a file on disk rather than from this code.
	 */
	template <class T>
	std::unique_ptr<T> create(const nes_host* host) const {
		if (kind() != PluginTraits<T>::kind)
			return std::unique_ptr<T>();
		typedef typename PluginTraits<T>::ApiType Api;
		std::unique_ptr<T> instance(new T(static_cast<const Api*>(m_api), host));
		// The instance now owns a reference to this module, so the library
		// cannot be unloaded while the instance is still calling into it.
		instance->keepAlive(shared_from_this());
		return instance;
	}

private:
	Module();

	void* m_handle;
	const nes_plugin_info* m_info;
	const void* m_api;
	std::string m_path;
};

/**
 * Instantiate a registry entry as @p T.
 *
 * The only way anything should be created from an Entry. Going through here
 * means an instance from a loaded module inherits a reference to that module,
 * and one from a built-in does not need to -- neither case relies on a vector
 * somewhere staying in scope.
 *
 * @return null if the entry does not provide T's kind.
 */
template <class T>
std::unique_ptr<T> createFrom(const Entry& entry, const nes_host* host) {
	if (entry.module)
		return entry.module->create<T>(host);
	if (!entry.info || entry.info->kind != PluginTraits<T>::kind)
		return std::unique_ptr<T>();
	typedef typename PluginTraits<T>::ApiType Api;
	return std::unique_ptr<T>(new T(static_cast<const Api*>(entry.api), host));
}

/**
 * Every plugin in @p directory, in a stable order.
 *
 * Files that are not plugins, or are built against another ABI, are reported
 * through @p warnings and skipped: one bad file in the folder should cost that
 * file, not the run. A directory that does not exist is not an error either --
 * a build with no plugins beside it still has its built-ins.
 */
std::vector<std::shared_ptr<Module> > loadModules(const std::string& directory,
		std::string* warnings);

/** The platform's shared-library suffix: ".dll", ".so" or ".dylib". */
const char* moduleSuffix();

} // namespace nesplug

#endif // NES_PLUGIN_MODULE_H
