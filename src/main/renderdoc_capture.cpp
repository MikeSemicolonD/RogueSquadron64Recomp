#include "renderdoc_capture.h"

#include <cstdio>

#include "debug_logs.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../lib/renderdoc_app/renderdoc_app.h"

static RENDERDOC_API_1_4_1* g_rdoc = nullptr;

void rs64_renderdoc_init() {
    const char* path = recomp::dbg::env_str("ROGUESQ_RENDERDOC");
    if ((path == nullptr) || (*path == '\0') || (g_rdoc != nullptr)) {
        return;
    }
    // Already injected (launched from qrenderdoc / renderdoccmd) wins over loading a second copy.
    HMODULE mod = GetModuleHandleA("renderdoc.dll");
    if (mod == nullptr) {
        mod = LoadLibraryA(path);
    }
    pRENDERDOC_GetAPI getApi = (mod != nullptr) ? (pRENDERDOC_GetAPI)GetProcAddress(mod, "RENDERDOC_GetAPI") : nullptr;
    if ((getApi == nullptr) || (getApi(eRENDERDOC_API_Version_1_4_1, (void**)&g_rdoc) != 1)) {
        fprintf(stderr, "[renderdoc] could not load the in-app API from %s\n", path);
        g_rdoc = nullptr;
        return;
    }
    const char* out = recomp::dbg::env_str("ROGUESQ_RENDERDOC_OUT");
    if ((out != nullptr) && (*out != '\0')) {
        g_rdoc->SetCaptureFilePathTemplate(out);
    }
    g_rdoc->MaskOverlayBits(eRENDERDOC_Overlay_None, eRENDERDOC_Overlay_None);
    int major = 0, minor = 0, patch = 0;
    g_rdoc->GetAPIVersion(&major, &minor, &patch);
    fprintf(stderr, "[renderdoc] in-app API %d.%d.%d loaded, captures -> %s\n", major, minor, patch, g_rdoc->GetCaptureFilePathTemplate());
}

bool rs64_renderdoc_active() {
    return g_rdoc != nullptr;
}

void rs64_renderdoc_trigger(uint32_t frames) {
    if (g_rdoc == nullptr) {
        return;
    }
    g_rdoc->TriggerMultiFrameCapture(frames);
    fprintf(stderr, "[renderdoc] capturing %u presents\n", frames);
}

#else

void rs64_renderdoc_init() {}
bool rs64_renderdoc_active() { return false; }
void rs64_renderdoc_trigger(uint32_t) {}

#endif
