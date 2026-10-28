#ifndef RS64_RENDERDOC_CAPTURE_H
#define RS64_RENDERDOC_CAPTURE_H

#include <cstdint>
#include <string>

// ROGUESQ_RENDERDOC=<path to renderdoc.dll>: load RenderDoc's in-app API before the GPU device exists (Windows only), captures go to
// ROGUESQ_RENDERDOC_OUT (a path prefix). The replay triggers them ahead of each ROGUESQ_LS_PAUSE_AT frame (tools/debug/recordings/capture-frames.ps1 -RenderDoc).
void rs64_renderdoc_init();
bool rs64_renderdoc_active();
// Captures the next `frames` presents.
void rs64_renderdoc_trigger(uint32_t frames);
// Each present: notices for a capture starting and the file it saved (also captures taken from an injecting RenderDoc UI).
void rs64_renderdoc_poll();
// The newest capture's file path, empty when there is none.
std::string rs64_renderdoc_last_capture();

#endif
