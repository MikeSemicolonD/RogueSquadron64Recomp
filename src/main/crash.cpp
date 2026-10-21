#include "main/crash.h"

#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <typeinfo>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>
#include <crtdbg.h>
#pragma comment(lib, "Dbghelp.lib")
#endif
#include "ultramodern/ultramodern.hpp"
#include "util/debug_logs.h"

using recomp::dbg::env_on;

#ifdef _WIN32

// One-shot init of DbgHelp symbol resolution — done lazily on first crash.
static void ensure_dbghelp_init()
{

    static bool init = false;

    if (init) return;

    init = true;
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
}

// Print stack frames with symbol resolution, one "[ N] 0xADDR  module!function+0xOFF  (file:line)" per frame.
// Global and non-static so the RT64 d3d12 allocator-failure tracer can extern-declare and call it.
void print_stack_with_symbols(void** frames, USHORT count) {

    ensure_dbghelp_init();
    HANDLE proc = GetCurrentProcess();
    HMODULE exe_base = GetModuleHandleW(nullptr);
    constexpr DWORD kNameMax = 512;
    char buf[sizeof(SYMBOL_INFO) + kNameMax];
    SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buf);

    for (USHORT i = 0; i < count; i++) {
        DWORD64 addr = (DWORD64)(uintptr_t)frames[i];
        uintptr_t rva = (uintptr_t)frames[i] - (uintptr_t)exe_base;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = kNameMax - 1;
        DWORD64 disp = 0;
        const char* name = "?";

        if (SymFromAddr(proc, addr, &disp, sym)) {
            name = sym->Name;
        }

        IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;

        if (SymGetLineFromAddr64(proc, addr, &lineDisp, &line))
        {
            fprintf(stderr, "  [%2u] 0x%llX rva 0x%llX  %s+0x%llX  (%s:%lu)\n",
                (unsigned)i, (unsigned long long)addr, (unsigned long long)rva,
                name, (unsigned long long)disp, line.FileName, (unsigned long)line.LineNumber);
        } else {
            fprintf(stderr, "  [%2u] 0x%llX rva 0x%llX  %s+0x%llX\n",
                (unsigned)i, (unsigned long long)addr, (unsigned long long)rva,
                name, (unsigned long long)disp);
        }
    }
}

#endif

namespace rs64::crash {
#ifdef _WIN32

// Full-memory minidump so we can inspect rdram contents post-mortem.
// ep may be null (SIGABRT path) — we still capture process+thread state.
void write_minidump_safe(EXCEPTION_POINTERS* ep)
{

    char path[MAX_PATH];
    SYSTEMTIME st;
    GetLocalTime(&st);
    CreateDirectoryA("dumps", NULL);
    CreateDirectoryA("dumps/crash-dumps", NULL);
    snprintf(path, sizeof(path),
        "dumps/crash-dumps/crash_%04u%02u%02u_%02u%02u%02u.dmp",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hFile == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[CRASH] CreateFile(%s) failed err=%lu\n",
            path, GetLastError());

        return;
    }

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    PMINIDUMP_EXCEPTION_INFORMATION pmei = nullptr;

    if (ep) {
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        pmei = &mei;
    }

    // Default to a lighter dump type. The earlier MiniDumpWithFullMemory
    // captured the full process address space (≈5 GB on this app, dominated
    // by D3D12/Vulkan render targets and texture caches). The symbolicated
    // stack trace in the .log already covers the common debug case; the
    // dump only needs threads + stacks + indirectly-referenced memory.
    // Set ROGUESQ_FULL_DUMP=1 to restore the full 5 GB dump for deep-dives.
    static const bool s_full_dump = env_on("ROGUESQ_FULL_DUMP");
    MINIDUMP_TYPE dumpType = s_full_dump
        ? MiniDumpWithFullMemory
        : (MINIDUMP_TYPE)(MiniDumpNormal
                          | MiniDumpWithIndirectlyReferencedMemory
                          | MiniDumpWithDataSegs
                          | MiniDumpWithThreadInfo
                          | MiniDumpWithUnloadedModules);
    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
        hFile, dumpType, pmei, NULL, NULL);
    CloseHandle(hFile);

    if (ok)
    {
        fprintf(stderr, "[CRASH] Minidump written: %s%s\n",
            path, s_full_dump ? " (full memory)" : " (lite)");
    } else {
        fprintf(stderr, "[CRASH] MiniDumpWriteDump failed err=%lu\n", GetLastError());
    }

    fflush(stderr);
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep)
{

    DWORD code = ep->ExceptionRecord->ExceptionCode;
    uintptr_t addr = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;

    fprintf(stderr, "[CRASH] Exception 0x%08lX at 0x%llX\n", code, (unsigned long long)addr);

    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
    {
        fprintf(stderr, "[CRASH] Access violation %s address 0x%llX\n",
            ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
            (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    }

    // Capture stack BEFORE minidump — if the process is killed mid-minidump
    // (e.g. by an external watchdog), we still want the trace in stderr.
    void* frames[32];
    USHORT count = RtlCaptureStackBackTrace(0, 32, frames, nullptr);

    fprintf(stderr, "[CRASH] Stack trace (%u frames):\n", (unsigned)count);
    print_stack_with_symbols(frames, count);
    fflush(stderr);
    write_minidump_safe(ep);
    fflush(stderr);

    return EXCEPTION_CONTINUE_SEARCH;
}

void install_exception_filter() {
    SetUnhandledExceptionFilter(crash_handler);
}

void install_handlers()
{

    SetUnhandledExceptionFilter(crash_handler);

    // Pinpoints uncaught C++ throws (notably ultramodern::thread_terminated, which
    // must be caught in _thread_func/entrypoint but escapes on any thread lacking
    // that catch). Runs on the throwing thread before abort, stack not yet unwound,
    // so the symbolized trace names the escape site. Falls through to the dump + exit.
    std::set_terminate([]{
        // Write to a file too: the Release build is /SUBSYSTEM:WINDOWS, so stderr
        // is not attached to a console and a shell redirect captures nothing.
        CreateDirectoryA("dumps", NULL);
        CreateDirectoryA("dumps/crash-dumps", NULL);
        char path[MAX_PATH];
        SYSTEMTIME st;
        GetLocalTime(&st);
        snprintf(path, sizeof(path), "dumps/crash-dumps/terminate_%04u%02u%02u_%02u%02u%02u.txt",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        FILE* tf = fopen(path, "w");
        uintptr_t modbase = (uintptr_t)GetModuleHandleW(NULL);
        char line[512];
        snprintf(line, sizeof(line),
            "[TERMINATE] tid=%lu is_game_thread=%d thread_self=0x%08X modbase=0x%p\n",
            GetCurrentThreadId(), (int)ultramodern::is_game_thread(),
            (uint32_t)ultramodern::this_thread(), (void*)modbase);
        fputs(line, stderr);

        if (tf) fputs(line, tf);

        const char* extype = "no in-flight exception";
        char exbuf[256] = {0};

        if (std::exception_ptr ep = std::current_exception()) {
            try { std::rethrow_exception(ep); }
            catch (const std::exception& e)
            {

                snprintf(exbuf, sizeof(exbuf), "uncaught %s: %s", typeid(e).name(), e.what());
                extype = exbuf;
            }
            catch (...) { extype = "uncaught non-std exception"; }
        }

        snprintf(line, sizeof(line), "[TERMINATE] %s\n", extype);
        fputs(line, stderr);

        if (tf) fputs(line, tf);

        void* frames[62];
        USHORT count = RtlCaptureStackBackTrace(0, 62, frames, nullptr);
        fputs("[TERMINATE] stack RVAs:", stderr);

        if (tf) fputs("[TERMINATE] stack RVAs:", tf);

        for (USHORT i = 0; i < count; i++) {
            snprintf(line, sizeof(line), " 0x%llX",
                (unsigned long long)((uintptr_t)frames[i] - modbase));
            fputs(line, stderr);

            if (tf) fputs(line, tf);
        }

        fputs("\n", stderr);

        if (tf) { fputs("\n", tf); fflush(tf); fclose(tf); }

        print_stack_with_symbols(frames, count);  // symbolized to stderr when a PDB exists
        write_minidump_safe(nullptr);
        _Exit(3);
    });

    signal(SIGABRT, [](int){

        fprintf(stderr, "[ABORT] caught SIGABRT, dumping stack:\n");
        void* frames[32];
        USHORT count = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
        print_stack_with_symbols(frames, count);

        // No EXCEPTION_POINTERS on the abort path — passing nullptr makes
        // VS open the dump without the "unhandled exception" dialog.
        write_minidump_safe(nullptr);
        _Exit(3);
    });

    // Route CRT debug asserts to stderr instead of the blocking dialog.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);

    // Hook fires BEFORE abort() runs — gives us a chance to dump the stack
    // for "vector subscript out of range" and similar STL bounds checks.
    _CrtSetReportHook([](int reportType, char* message, int*) -> int
    {

        fprintf(stderr, "[CRT_REPORT type=%d] %s\n", reportType,
            message ? message : "(null)");
        void* frames[48];
        USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
        HMODULE base = GetModuleHandleW(nullptr);

        for (USHORT i = 0; i < count; i++) {
            uintptr_t rva = (uintptr_t)frames[i] - (uintptr_t)base;

            fprintf(stderr, "  [%2u] 0x%llX  rva 0x%llX\n", (unsigned)i,
                (unsigned long long)(uintptr_t)frames[i],
                (unsigned long long)rva);
        }

        fflush(stderr);

        // Return 1 to SUPPRESS the abort. STL bounds-check assertions ("vector
        // subscript out of range") fire when a Factor5 ucode handler indexes
        // past a vector limit due to state we can't fully replicate yet. The
        // resulting abort kills the game even though continuing with whatever
        // garbage the out-of-bounds read returned often lets play continue.
        // Trade-off: occasional visual glitches over a hard crash. Print first.
        return 1;
    });

    // MSVC debug iterators call _invalid_parameter on bounds-check failure
    // (e.g. "vector subscript out of range"). Default handler aborts silently;
    // ours prints a stack trace first.
    _set_invalid_parameter_handler([](const wchar_t* expr, const wchar_t* func,
                                       const wchar_t* file, unsigned int line,
                                       uintptr_t)
    {

        fprintf(stderr, "[INVALID_PARAM] expr=%ls func=%ls file=%ls:%u\n",
            expr ? expr : L"(null)", func ? func : L"(null)",
            file ? file : L"(null)", line);
        void* frames[48];
        USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
        HMODULE base = GetModuleHandleW(nullptr);

        for (USHORT i = 0; i < count; i++) {
            uintptr_t rva = (uintptr_t)frames[i] - (uintptr_t)base;

            fprintf(stderr, "  [%2u] 0x%llX  rva 0x%llX\n", (unsigned)i,
                (unsigned long long)(uintptr_t)frames[i],
                (unsigned long long)rva);
        }

        fflush(stderr);
        _Exit(4);
    });
}

#else

// No DbgHelp/minidump support off-Windows; the F12 hotkey path is a no-op.
void write_minidump_safe(void*) {}
void install_exception_filter() {}
void install_handlers() {}

#endif
}
