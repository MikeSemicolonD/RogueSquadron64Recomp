#include "main/renderdoc_capture.h"

#include <cstdio>
#include <cstring>
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

std::string rs64_renderdoc_last_capture()
{

    const uint32_t count = (g_rdoc != nullptr) ? g_rdoc->GetNumCaptures() : 0;
    uint32_t length = 0;

    if (count == 0)
    {
        return {};
    }

    g_rdoc->GetCapture(count - 1, nullptr, &length, nullptr);
    std::string file(length, '\0');

    if (length > 0) {
        g_rdoc->GetCapture(count - 1, file.data(), &length, nullptr);
        file.resize(std::strlen(file.c_str()));
    }

    return file;
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
        rs64::app::notify("renderdoc", "RenderDoc: capturing a frame. The game freezes while it saves.", 60.0f);
    }

    s_capturing = capturing;

    if (count <= s_seen)
    {
        return;
    }

    s_seen = count;
    rs64::app::notify("renderdoc", "RenderDoc capture saved: " + rs64_renderdoc_last_capture(), 8.0f);
}

#else

void rs64_renderdoc_init() {}
bool rs64_renderdoc_active() { return false; }
void rs64_renderdoc_trigger(uint32_t) {}
void rs64_renderdoc_poll() {}
std::string rs64_renderdoc_last_capture() { return {}; }

#endif
