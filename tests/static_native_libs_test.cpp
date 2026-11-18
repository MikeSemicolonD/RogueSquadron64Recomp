// librecomp's registry of native mod libraries linked into the executable (the browser build can't load library files).
#include "librecomp/static_native_libs.hpp"
#include "check.h"
#include <cstdio>

namespace {

void init_a(uint8_t*, recomp_context*) {}

void test_registered_library_resolves()
{

    recomp::mods::register_static_native_library("coop", 1, { { "rs64_mod_init", init_a } });

    const recomp::mods::StaticNativeLibrary* lib = recomp::mods::find_static_native_library("coop");

    CHECK(lib != nullptr && lib->api_version == 1);
    CHECK(lib->exports.at("rs64_mod_init") == init_a);
    CHECK(lib->exports.count("missing") == 0);
}

void test_unknown_library_is_absent()
{
    CHECK(recomp::mods::find_static_native_library("not_linked") == nullptr);
}

}

int main()
{

    test_registered_library_resolves();
    test_unknown_library_is_absent();
    std::printf("static_native_libs_test: all passed\n");
    return 0;
}
