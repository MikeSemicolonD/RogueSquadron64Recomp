// Stub. N64Recomp's single_file_output mode (used by patches.toml) emits
// `#include "disable_warnings.h"` in its output to suppress warnings on the
// translated MIPS code. A copy ships with Zelda64Recompiled; we provide our
// own minimal version here.
#pragma once

#ifdef _MSC_VER
#  pragma warning(disable : 4101) // unreferenced local variable
#  pragma warning(disable : 4133) // incompatible types
#  pragma warning(disable : 4146) // unary minus on unsigned
#  pragma warning(disable : 4244) // truncation
#  pragma warning(disable : 4267) // size_t to int truncation
#endif

#if defined(__clang__)
#  pragma clang diagnostic ignored "-Wunused-variable"
#  pragma clang diagnostic ignored "-Wimplicit-function-declaration"
#  pragma clang diagnostic ignored "-Wparentheses"
#elif defined(__GNUC__)
#  pragma GCC diagnostic ignored "-Wunused-variable"
#  pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
#  pragma GCC diagnostic ignored "-Wparentheses"
#endif
