#pragma once

#ifdef _WIN32
struct _EXCEPTION_POINTERS;
#endif

// Crash reporting on Windows: symbolized stack traces to stderr and minidumps under dumps/crash-dumps/. No-ops elsewhere.
namespace rs64::crash {
#ifdef _WIN32
    // ep may be null (abort and manual dumps).
    void write_minidump_safe(_EXCEPTION_POINTERS* ep);
#else
    void write_minidump_safe(void* ep);
#endif
    // The unhandled-exception filter alone (the D3D11 shader precompile run).
    void install_exception_filter();
    // The filter plus the terminate, SIGABRT, CRT report and invalid-parameter handlers.
    void install_handlers();
}
