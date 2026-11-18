#include "main/renderdoc_capture.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

#include "util/debug_logs.h"
#include "main/ui_panels.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../lib/renderdoc_app/renderdoc_app.h"

static RENDERDOC_API_1_4_1* g_rdoc = nullptr;

void rs64_renderdoc_init()
{

    const char* path = recomp::dbg::env_str("ROGUESQ_RENDERDOC");
    const bool requested = (path != nullptr) && (*path != '\0');

    if (g_rdoc != nullptr)
    {
        return;
    }

    // Already injected (launched from qrenderdoc / renderdoccmd) wins over loading a second copy; injected without ROGUESQ_RENDERDOC, only its captures are reported.
    HMODULE mod = GetModuleHandleA("renderdoc.dll");

    if ((mod == nullptr) && requested) {
        mod = LoadLibraryA(path);
    }

    if (mod == nullptr)
    {
        return;
    }

    pRENDERDOC_GetAPI getApi = (pRENDERDOC_GetAPI)GetProcAddress(mod, "RENDERDOC_GetAPI");

    if ((getApi == nullptr) || (getApi(eRENDERDOC_API_Version_1_4_1, (void**)&g_rdoc) != 1))
    {
        fprintf(stderr, "[renderdoc] could not load the in-app API from %s\n", requested ? path : "the injected renderdoc.dll");
        g_rdoc = nullptr;

        return;
    }

    const char* out = recomp::dbg::env_str("ROGUESQ_RENDERDOC_OUT");

    if ((out != nullptr) && (*out != '\0')) {
        g_rdoc->SetCaptureFilePathTemplate(out);
    }

    // F12 is the game's minidump key, so RenderDoc captures on Print Screen only.
    RENDERDOC_InputButton capture_key = eRENDERDOC_Key_PrtScrn;

    g_rdoc->SetCaptureKeys(&capture_key, 1);

    if (requested) {
        g_rdoc->MaskOverlayBits(eRENDERDOC_Overlay_None, eRENDERDOC_Overlay_None);
    }

    int major = 0, minor = 0, patch = 0;
    g_rdoc->GetAPIVersion(&major, &minor, &patch);

    fprintf(stderr, "[renderdoc] in-app API %d.%d.%d loaded, captures -> %s\n", major, minor, patch, g_rdoc->GetCaptureFilePathTemplate());
}

bool rs64_renderdoc_active() {
    return g_rdoc != nullptr;
}

void rs64_renderdoc_trigger(uint32_t frames) {

    if (g_rdoc == nullptr)
    {
        return;
    }

    g_rdoc->TriggerMultiFrameCapture(frames);
    rs64::app::notify("renderdoc", "RenderDoc: capturing " + std::to_string(frames) + " frames. The game freezes while it saves.", 60.0f);

    fprintf(stderr, "[renderdoc] capturing %u presents\n", frames);
}

// The newest capture as renamed after the game clocks, the capture count it belongs to, and the clocks noted when that capture started.
// The present thread (poll) and the MCP bridge both ask for it, so whichever comes first renames the file.
static std::mutex s_capture_mtx;
static std::string s_renamed;
static uint32_t s_renamed_count = 0;
static std::string s_label;

static std::string capture_path(uint32_t index)
{

    uint32_t length = 0;

    g_rdoc->GetCapture(index, nullptr, &length, nullptr);
    std::string file(length, '\0');

    if (length > 0) {
        g_rdoc->GetCapture(index, file.data(), &length, nullptr);
        file.resize(std::strlen(file.c_str()));
    }

    return file;
}

// RenderDoc numbers a capture by its own present count, which drifts with wall time; the game clocks name the same moment on every run.
static std::string rename_after_clocks(const std::string& file)
{

    const std::string label = s_label.empty() ? rs64_clock_label() : s_label;
    const std::filesystem::path saved = file;
    std::string stem = saved.stem().string();
    const size_t frame_at = stem.rfind("_frame");

    s_label.clear();

    if (label.empty() || file.empty())
    {
        return file;
    }

    if (frame_at != std::string::npos) {
        stem.resize(frame_at);
    }

    std::filesystem::path renamed = saved.parent_path() / (stem + "_" + label + saved.extension().string());
    std::error_code ec;

    for (int n = 2; std::filesystem::exists(renamed, ec); ++n) {
        renamed = saved.parent_path() / (stem + "_" + label + "_" + std::to_string(n) + saved.extension().string());
    }

    std::filesystem::rename(saved, renamed, ec);

    return ec ? file : renamed.string();
}

std::string rs64_renderdoc_last_capture()
{

    const uint32_t count = (g_rdoc != nullptr) ? g_rdoc->GetNumCaptures() : 0;
    std::lock_guard<std::mutex> lock(s_capture_mtx);

    if (count == 0)
    {
        return {};
    }

    if (count != s_renamed_count) {
        s_renamed = rename_after_clocks(capture_path(count - 1));
        s_renamed_count = count;
    }

    return s_renamed;
}

void rs64_renderdoc_poll()
{

    static uint32_t s_seen = UINT32_MAX;
    static bool s_capturing = false;

    if (g_rdoc == nullptr)
    {
        return;
    }

    const uint32_t count = g_rdoc->GetNumCaptures();
    const bool capturing = g_rdoc->IsFrameCapturing() != 0;

    if (s_seen == UINT32_MAX) {
        s_seen = count;
    }

    if (capturing && !s_capturing) {
        const std::string label = rs64_clock_label();
        std::lock_guard<std::mutex> lock(s_capture_mtx);

        s_label = label;
        rs64::app::notify("renderdoc", "RenderDoc: capturing a frame. The game freezes while it saves.", 60.0f);
    }

    s_capturing = capturing;

    if (count <= s_seen)
    {
        return;
    }

    s_seen = count;

    const std::string file = rs64_renderdoc_last_capture();

    fprintf(stderr, "[renderdoc] capture saved: %s\n", file.c_str());
    rs64::app::notify("renderdoc", "RenderDoc capture saved: " + file, 8.0f);
}

#else

void rs64_renderdoc_init() {}
bool rs64_renderdoc_active() { return false; }
void rs64_renderdoc_trigger(uint32_t) {}
void rs64_renderdoc_poll() {}
std::string rs64_renderdoc_last_capture() { return {}; }

#endif
