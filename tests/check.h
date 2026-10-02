#pragma once
#include <cstdio>
#include <cstdlib>
// Like assert, but never compiled out: the tests run in Release too.
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); std::abort(); } } while (0)
