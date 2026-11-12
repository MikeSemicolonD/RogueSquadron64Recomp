#include "recomp.h"
#include "check.h"
#include <cmath>
#include <cstdint>
#include <cstdio>

// The recompiled code's float-to-int truncations must give x86's results on every host (wasm traps or saturates, arm64 saturates).
int main()
{

    CHECK(TRUNC_W_S(-2.5f) == -2);
    CHECK(TRUNC_W_S(2147483520.0f) == 2147483520);
    CHECK(TRUNC_W_S(3e9f) == INT32_MIN);
    CHECK(TRUNC_W_S(-3e9f) == INT32_MIN);
    CHECK(TRUNC_W_S(NAN) == INT32_MIN);
    CHECK(TRUNC_W_D(2147483647.9) == 2147483647);
    CHECK(TRUNC_W_D(2147483648.0) == INT32_MIN);
    CHECK(TRUNC_L_S(-1.75f) == -1);
    CHECK(TRUNC_L_D(1e30) == INT64_MIN);
    CHECK(TRUNC_L_D(NAN) == INT64_MIN);
    std::printf("recomp_fp_convert_test OK\n");
    return 0;
}
