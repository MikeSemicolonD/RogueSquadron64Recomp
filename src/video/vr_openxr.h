#pragma once

// OpenXR VR session (built with RS64_OPENXR). RT64 reaches it through the rs64_vr_* functions in hle/rt64_rs64_vr.h.

namespace RT64 {
    struct Application;
}

namespace rs64::vr {

// VR was asked for at launch (ROGUESQ_VR / --vr).
bool requested();

// Before RT64 creates its Vulkan device: OpenXR instance, headset, and the Vulkan extensions and GPU it needs. False = run flat.
bool init_before_device();

// After RT64's setup: the session on RT64's present queue, the eye swapchains and the LOCAL space. False = run flat.
bool start_session(RT64::Application& app);

void shutdown();
}
