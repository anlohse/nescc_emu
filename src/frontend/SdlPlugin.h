#ifndef NES_FRONTEND_SDL_PLUGIN_H
#define NES_FRONTEND_SDL_PLUGIN_H

//
// The built-in SDL plugins, offered to the registry.
//
// These are the same three things a loadable module exports -- an ABI version,
// a descriptor and an api struct -- reached by a direct call instead of by a
// symbol lookup. When they move into shared libraries, the loader will produce
// exactly these values and nothing on the host side changes.
//

#include "../plugin/nes_plugin.h"

#include <cstdint>

namespace nesfe {

std::uint32_t sdlPluginAbiVersion();

const nes_plugin_info* sdlVideoInfo();
const void* sdlVideoApi();

const nes_plugin_info* sdlAudioInfo();
const void* sdlAudioApi();

const nes_plugin_info* sdlInputInfo();
const void* sdlInputApi();

} // namespace nesfe

#endif // NES_FRONTEND_SDL_PLUGIN_H
