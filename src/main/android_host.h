#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace rs64::android {
    // App-specific external files dir (adb-pushable: /sdcard/Android/data/<pkg>/files); internal storage as fallback.
    std::filesystem::path data_dir();
    // Redirects stdout/stderr into logcat (tag "RS64") through a pipe and a reader thread.
    void start_logcat_pump();
    // setenv() for every NAME=VALUE line of data_dir()/roguesq_env.txt; '#' starts a comment line.
    void load_env_file();
    // Copies the APK's bundled mods (assets/mods, listed in index.txt) into data_dir()/mods, overwriting older copies.
    void install_bundled_mods();
    // Opens the system file picker (MainActivity.pickRom) and blocks until it closes. Returns a private copy's path, or "" if cancelled.
    std::string pick_rom();
    // ANativeWindow_setFrameRate(FIXED_SOURCE) at ROGUESQ_ANDROID_FRAME_RATE (default 60, 0 = leave the display alone). API 30+.
    void set_window_frame_rate(void* native_window);
    // ADPF: reports one display list's CPU time to a hint session on the render threads (Gfx, RT64 Workload/Present). API 33+; ROGUESQ_ANDROID_PERF_HINT=0 disables.
    void perf_report_work(int64_t work_ns);
}
