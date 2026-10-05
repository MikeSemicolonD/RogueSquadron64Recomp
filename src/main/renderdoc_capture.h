#ifndef RS64_RENDERDOC_CAPTURE_H
#define RS64_RENDERDOC_CAPTURE_H

#include <cstdint>

// ROGUESQ_RENDERDOC=<path to renderdoc.dll>: load RenderDoc's in-app API before the GPU device exists (Windows only), captures go to
// ROGUESQ_RENDERDOC_OUT (a path prefix). The replay triggers them ahead of each ROGUESQ_LS_PAUSE_AT frame (tools/recordings/capture-frames.ps1 -RenderDoc).
void rs64_renderdoc_init();
bool rs64_renderdoc_active();
// Captures the next `frames` presents.
void rs64_renderdoc_trigger(uint32_t frames);

#endif
