#pragma once

#ifdef _WIN32
struct _EXCEPTION_POINTERS;
#endif

// Crash reporting on Windows: symbolized stack traces to stderr and minidumps under dumps/crash-dumps/. No-ops elsewhere.
namespace rs64::crash {
#ifdef _WIN32
    // ep may be null (abort and manual dumps). True when the dump was written; last_minidump_path() names it.
    bool write_minidump_safe(_EXCEPTION_POINTERS* ep);
#else
    bool write_minidump_safe(void* ep);
#endif
    const char* last_minidump_path();
    // The unhandled-exception filter alone (the D3D11 shader precompile run).
    void install_exception_filter();
    // The filter plus the terminate, SIGABRT, CRT report and invalid-parameter handlers.
    void install_handlers();
    // One line counting what the run survived by suppressing it: CRT bounds reports, calls to unmapped addresses and guest faults. All zero on a clean run.
    void log_suppressed();
}
