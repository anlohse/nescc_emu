#include "Module.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dirent.h>
#  include <dlfcn.h>
#endif

namespace nesplug {

namespace {

/* ------------------------------------------------------------------------- */
/* The platform's three verbs                                                 */
/* ------------------------------------------------------------------------- */

void* openLibrary(const std::string& path, std::string* error) {
#if defined(_WIN32)
	HMODULE handle = LoadLibraryA(path.c_str());
	if (!handle && error) {
		char buffer[128];
		std::snprintf(buffer, sizeof(buffer), "LoadLibrary failed (%lu)",
				static_cast<unsigned long>(GetLastError()));
		*error = buffer;
	}
	return handle;
#else
	// RTLD_LOCAL so a plugin's symbols do not leak into the global namespace
	// and quietly satisfy some other library's undefined reference.
	void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!handle && error) {
		const char* reason = dlerror();
		*error = reason ? reason : "dlopen failed";
	}
	return handle;
#endif
}

void* findSymbol(void* handle, const char* name) {
#if defined(_WIN32)
	return reinterpret_cast<void*>(
			GetProcAddress(static_cast<HMODULE>(handle), name));
#else
	return dlsym(handle, name);
#endif
}

void closeLibrary(void* handle) {
#if defined(_WIN32)
	FreeLibrary(static_cast<HMODULE>(handle));
#else
	dlclose(handle);
#endif
}

/** Case-insensitive suffix test, because Windows filenames are. */
bool endsWith(const std::string& s, const std::string& suffix) {
	if (s.size() < suffix.size())
		return false;
	for (std::size_t i = 0; i < suffix.size(); i++) {
		const char a = static_cast<char>(
				std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i])));
		const char b = static_cast<char>(
				std::tolower(static_cast<unsigned char>(suffix[i])));
		if (a != b)
			return false;
	}
	return true;
}

void note(std::string* warnings, const std::string& message) {
	if (!warnings)
		return;
	if (!warnings->empty())
		*warnings += "\n";
	*warnings += message;
}

} // namespace

const char* moduleSuffix() {
#if defined(_WIN32)
	return ".dll";
#elif defined(__APPLE__)
	return ".dylib";
#else
	return ".so";
#endif
}

/* ------------------------------------------------------------------------- */
/* Module                                                                     */
/* ------------------------------------------------------------------------- */

Module::Module() : m_handle(nullptr), m_info(nullptr), m_api(nullptr) { }

Module::~Module() {
	if (m_handle)
		closeLibrary(m_handle);
}

std::shared_ptr<Module> Module::load(const std::string& path, std::string* error) {
	void* handle = openLibrary(path, error);
	if (!handle)
		return std::shared_ptr<Module>();

	// A shared_ptr from here on, so every early return below unloads the
	// library on the way out without a goto or a flag to get wrong.
	std::shared_ptr<Module> module(new Module());
	module->m_handle = handle;
	module->m_path = path;

	nes_plugin_abi_version_fn versionOf =
			reinterpret_cast<nes_plugin_abi_version_fn>(
					findSymbol(handle, NES_PLUGIN_ABI_VERSION_SYMBOL));
	if (!versionOf) {
		// Almost always an ordinary library that happens to live in the folder,
		// so this is worth saying plainly rather than as a failure.
		if (error) *error = "not a plugin: no " NES_PLUGIN_ABI_VERSION_SYMBOL;
		return std::shared_ptr<Module>();
	}

	// Before anything else the library offers. If the ABI moved, the shape of
	// the descriptor could have moved with it.
	const std::uint32_t version = versionOf();
	if (version != NES_PLUGIN_ABI_VERSION) {
		if (error) {
			char buffer[128];
			std::snprintf(buffer, sizeof(buffer),
					"ABI version %u, expected %u", version, NES_PLUGIN_ABI_VERSION);
			*error = buffer;
		}
		return std::shared_ptr<Module>();
	}

	nes_plugin_describe_fn describe = reinterpret_cast<nes_plugin_describe_fn>(
			findSymbol(handle, NES_PLUGIN_DESCRIBE_SYMBOL));
	nes_plugin_api_fn apiOf = reinterpret_cast<nes_plugin_api_fn>(
			findSymbol(handle, NES_PLUGIN_API_SYMBOL));
	if (!describe || !apiOf) {
		if (error) *error = "incomplete plugin: missing describe or api";
		return std::shared_ptr<Module>();
	}

	module->m_info = describe();
	module->m_api = apiOf();
	if (!module->m_info || !module->m_api
			|| module->m_info->size < sizeof(nes_plugin_info)
			|| !module->m_info->id || !module->m_info->name) {
		if (error) *error = "incomplete descriptor";
		return std::shared_ptr<Module>();
	}

	switch (module->m_info->kind) {
	case NES_PLUGIN_VIDEO:
	case NES_PLUGIN_AUDIO:
	case NES_PLUGIN_INPUT:
		break;
	default:
		if (error) *error = "unknown plugin kind";
		return std::shared_ptr<Module>();
	}

	return module;
}

/* ------------------------------------------------------------------------- */
/* Discovery                                                                  */
/* ------------------------------------------------------------------------- */

std::vector<std::shared_ptr<Module> > loadModules(const std::string& directory,
		std::string* warnings) {
	std::vector<std::string> candidates;
	const std::string suffix = moduleSuffix();

#if defined(_WIN32)
	WIN32_FIND_DATAA found;
	const std::string pattern = directory + "\\*" + suffix;
	HANDLE search = FindFirstFileA(pattern.c_str(), &found);
	if (search != INVALID_HANDLE_VALUE) {
		do {
			if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				candidates.push_back(directory + "\\" + found.cFileName);
		} while (FindNextFileA(search, &found));
		FindClose(search);
	}
#else
	DIR* dir = opendir(directory.c_str());
	if (dir) {
		while (struct dirent* entry = readdir(dir))
			if (endsWith(entry->d_name, suffix))
				candidates.push_back(directory + "/" + entry->d_name);
		closedir(dir);
	}
#endif

	// A directory listing has no promised order, and load order decides which
	// plugin wins a tie. Sorting makes two runs on the same machine agree.
	std::sort(candidates.begin(), candidates.end());

	std::vector<std::shared_ptr<Module> > modules;
	for (std::size_t i = 0; i < candidates.size(); i++) {
		std::string reason;
		std::shared_ptr<Module> module = Module::load(candidates[i], &reason);
		if (module)
			modules.push_back(module);
		else
			note(warnings, candidates[i] + ": " + reason);
	}
	return modules;
}

} // namespace nesplug
