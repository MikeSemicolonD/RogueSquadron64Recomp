#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include "librecomp/fault_guard.hpp"

static void ok_fn(void* p) { *static_cast<int*>(p) = 42; }
static void bad_fn(void*) { volatile int* wild = reinterpret_cast<int*>(0x10); *wild = 1; }

int main() {
    // A fault outside any guarded call must still kill the process with SIGSEGV.
    pid_t child = fork();
    if (child == 0) {
        int dummy = 0;
        recomp::fault_guard::run(ok_fn, &dummy, nullptr);
        bad_fn(nullptr);
        _exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV) { puts("FAIL unguarded fault not fatal"); return 1; }

    int v = 0;
    recomp::fault_guard::Info info{};
    if (!recomp::fault_guard::run(ok_fn, &v, &info) || v != 42) { puts("FAIL ok_fn"); return 1; }
    if (recomp::fault_guard::run(bad_fn, nullptr, &info)) { puts("FAIL bad_fn not caught"); return 1; }
    if (info.addr != 0x10) { printf("FAIL addr=%p\n", (void*)info.addr); return 1; }
    bool thread_ok = false;
    std::thread t([&] { recomp::fault_guard::Info i{}; thread_ok = !recomp::fault_guard::run(bad_fn, nullptr, &i); });
    t.join();
    if (!thread_ok) { puts("FAIL thread"); return 1; }
    if (recomp::fault_guard::run(bad_fn, nullptr, &info)) { puts("FAIL second catch"); return 1; }

    // A C++ exception (thread_terminated) unwinding out of run() must not leave a stale jump target behind.
    child = fork();
    if (child == 0) {
        try {
            recomp::fault_guard::run([](void*) { throw 1; }, nullptr, nullptr);
            _exit(3);
        } catch (int) {
        }
        bad_fn(nullptr);
        _exit(0);
    }
    waitpid(child, &status, 0);
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV) { puts("FAIL stale jump after exception"); return 1; }
    puts("PASS");
    return 0;
}
