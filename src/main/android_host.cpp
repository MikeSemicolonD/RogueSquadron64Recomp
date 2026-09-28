#include "android_host.h"

#include <algorithm>
#include <android/log.h>
#include <android/native_window.h>
#include <cstdio>
#include <dlfcn.h>
#include <jni.h>
#include <vector>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include "SDL.h"
#include "SDL_system.h"
#include "debug_logs.h"

namespace rs64::android {
    std::filesystem::path data_dir() {
        if (const char* ext = SDL_AndroidGetExternalStoragePath()) {
            return std::filesystem::path(ext);
        }
        return std::filesystem::path(SDL_AndroidGetInternalStoragePath());
    }

    void start_logcat_pump() {
        static int fds[2];
        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IOLBF, 0);
        if (pipe(fds) != 0) {
            return;
        }
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        std::thread([] {
            char buf[1024];
            ssize_t n;
            while ((n = read(fds[0], buf, sizeof(buf) - 1)) > 0) {
                buf[n] = 0;
                __android_log_write(ANDROID_LOG_INFO, "RS64", buf);
            }
        }).detach();
    }

    void load_env_file() {
        std::ifstream in(data_dir() / "roguesq_env.txt");
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty() || line[0] == '#') {
                continue;
            }
            size_t eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            setenv(line.substr(0, eq).c_str(), line.substr(eq + 1).c_str(), 1);
        }
    }

    namespace {
        // SDL resolves relative paths against the APK's assets.
        bool read_asset(const std::string& path, std::string* out) {
            SDL_RWops* rw = SDL_RWFromFile(path.c_str(), "rb");
            if (!rw) {
                return false;
            }
            const Sint64 n = SDL_RWsize(rw);
            out->resize(n > 0 ? (size_t)n : 0);
            const size_t got = n > 0 ? SDL_RWread(rw, out->data(), 1, (size_t)n) : 0;
            SDL_RWclose(rw);
            return got == out->size();
        }
    }

    void install_bundled_mods() {
        std::string index;
        if (!read_asset("mods/index.txt", &index)) {
            fprintf(stderr, "[mods] no bundled mods\n");
            return;
        }
        const std::filesystem::path root = data_dir() / "mods";
        size_t start = 0;
        int count = 0;
        while (start < index.size()) {
            size_t end = index.find('\n', start);
            if (end == std::string::npos) {
                end = index.size();
            }
            const std::string rel = index.substr(start, end - start);
            start = end + 1;
            std::string data;
            if (rel.empty() || !read_asset("mods/" + rel, &data)) {
                continue;
            }
            const std::filesystem::path dst = root / rel;
            std::error_code ec;
            std::filesystem::create_directories(dst.parent_path(), ec);
            std::ofstream(dst, std::ios::binary) << data;
            ++count;
        }
        fprintf(stderr, "[mods] installed %d bundled mod file(s) into %s\n", count, root.string().c_str());
    }

    std::string pick_rom() {
        JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
        jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
        if (!env || !activity) {
            return {};
        }
        std::string path;
        jclass cls = env->GetObjectClass(activity);
        jmethodID mid = env->GetStaticMethodID(cls, "pickRom", "()Ljava/lang/String;");
        if (mid) {
            jstring js = static_cast<jstring>(env->CallStaticObjectMethod(cls, mid));
            if (js) {
                const char* s = env->GetStringUTFChars(js, nullptr);
                path = s ? s : "";
                env->ReleaseStringUTFChars(js, s);
                env->DeleteLocalRef(js);
            }
        }
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        env->DeleteLocalRef(cls);
        env->DeleteLocalRef(activity);
        return path;
    }

    void set_window_frame_rate(void* native_window) {
        using SetFrameRate = int32_t (*)(ANativeWindow*, float, int8_t);
        static const auto s_fn = [] {
            void* lib = dlopen("libnativewindow.so", RTLD_NOW);
            void* sym = lib ? dlsym(lib, "ANativeWindow_setFrameRate") : nullptr;
            if (!sym) {
                sym = dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate");
            }
            fprintf(stderr, "[perf] ANativeWindow_setFrameRate %s\n", sym ? "found" : "unavailable");
            return reinterpret_cast<SetFrameRate>(sym);
        }();
        static const int s_rate = recomp::dbg::env_int("ROGUESQ_ANDROID_FRAME_RATE", 60);
        if (!s_fn || !native_window || s_rate <= 0) {
            return;
        }
        constexpr int8_t kCompatFixedSource = 1;
        int32_t rc = s_fn(static_cast<ANativeWindow*>(native_window), static_cast<float>(s_rate), kCompatFixedSource);
        fprintf(stderr, "[perf] ANativeWindow_setFrameRate(%d, FIXED_SOURCE) = %d\n", s_rate, rc);
    }

    namespace {
        struct HintApi {
            using GetManager = void* (*)();
            using CreateSession = void* (*)(void*, const int32_t*, size_t, int64_t);
            using ReportActual = int (*)(void*, int64_t);
            using SetThreads = int (*)(void*, const pid_t*, size_t);
            using CloseSession = void (*)(void*);
            GetManager get_manager = nullptr;
            CreateSession create_session = nullptr;
            ReportActual report_actual = nullptr;
            SetThreads set_threads = nullptr;
            CloseSession close_session = nullptr;
        };

        constexpr int64_t kTargetWorkNs = 16'666'667;
        constexpr int kRescanReports = 600;

        // The calling thread (runs the display-list walk) plus the RT64 threads that record and present.
        std::vector<int32_t> find_render_threads(std::string& names) {
            std::vector<int32_t> tids;
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task", ec)) {
                std::ifstream comm(entry.path() / "comm");
                std::string name;
                std::getline(comm, name);
                if (name == "Gfx Thread" || name == "RT64 Workload" || name == "RT64 Present") {
                    tids.push_back(static_cast<int32_t>(std::stol(entry.path().filename().string())));
                    names += " " + name;
                }
            }
            const int32_t self = static_cast<int32_t>(gettid());
            if (std::find(tids.begin(), tids.end(), self) == tids.end()) {
                tids.push_back(self);
                names += " (caller)";
            }
            std::sort(tids.begin(), tids.end());
            return tids;
        }
    }

    void perf_report_work(int64_t work_ns) {
        static const bool s_on = recomp::dbg::env_on("ROGUESQ_ANDROID_PERF_HINT", true);
        static HintApi s_api;
        static void* s_manager = nullptr;
        static void* s_session = nullptr;
        static std::vector<int32_t> s_tids;
        static int s_reports = 0;
        static bool s_init = false;
        if (!s_on) {
            return;
        }
        if (!s_init) {
            s_init = true;
            s_api.get_manager = reinterpret_cast<HintApi::GetManager>(dlsym(RTLD_DEFAULT, "APerformanceHint_getManager"));
            s_api.create_session = reinterpret_cast<HintApi::CreateSession>(dlsym(RTLD_DEFAULT, "APerformanceHint_createSession"));
            s_api.report_actual = reinterpret_cast<HintApi::ReportActual>(dlsym(RTLD_DEFAULT, "APerformanceHint_reportActualWorkDuration"));
            s_api.set_threads = reinterpret_cast<HintApi::SetThreads>(dlsym(RTLD_DEFAULT, "APerformanceHint_setThreads"));
            s_api.close_session = reinterpret_cast<HintApi::CloseSession>(dlsym(RTLD_DEFAULT, "APerformanceHint_closeSession"));
            s_manager = s_api.get_manager ? s_api.get_manager() : nullptr;
            fprintf(stderr, "[perf] ADPF manager=%p setThreads=%s\n", s_manager, s_api.set_threads ? "yes" : "no");
        }
        if (!s_manager || !s_api.create_session || !s_api.report_actual) {
            return;
        }
        if (!s_session || (s_reports % kRescanReports) == 0) {
            std::string names;
            std::vector<int32_t> tids = find_render_threads(names);
            if (!tids.empty() && tids != s_tids) {
                if (s_session && s_api.set_threads) {
                    s_api.set_threads(s_session, tids.data(), tids.size());
                } else {
                    if (s_session && s_api.close_session) {
                        s_api.close_session(s_session);
                    }
                    s_session = s_api.create_session(s_manager, tids.data(), tids.size(), kTargetWorkNs);
                }
                s_tids = std::move(tids);
                fprintf(stderr, "[perf] ADPF session=%p on %zu threads:%s\n", s_session, s_tids.size(), names.c_str());
            }
        }
        ++s_reports;
        if (s_session && work_ns > 0) {
            s_api.report_actual(s_session, work_ns);
        }
    }
}
