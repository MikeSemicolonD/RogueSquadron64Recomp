// The browser build links this mod into the game (it can't load library files); this registers it under the name its mod.json loads.
// Compiled into the game target, so the linker keeps it and, through it, the mod.
#include "librecomp/static_native_libs.hpp"

extern "C" uint32_t recomp_api_version;
extern "C" void rs64_mod_init(uint8_t* rdram, recomp_context* ctx);

namespace {

const bool s_registered = [] {

    recomp::mods::register_static_native_library("multiplayer_native", recomp_api_version, { { "rs64_mod_init", rs64_mod_init } });
    return true;
}();

}
