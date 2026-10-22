// rt64_render_context.h — public surface of rt64_render_context.cpp: the
// RendererContext factory (used by main.cpp) and the host hooks around it.
#ifndef RS64_RT64_RENDER_CONTEXT_H
#define RS64_RT64_RENDER_CONTEXT_H

#include <cstdint>
#include <memory>
#include "ultramodern/renderer_context.hpp"

namespace recomp {
    std::unique_ptr<ultramodern::renderer::RendererContext>
    create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window, bool developer_mode);
}

// Nonzero while RT64's F1 developer inspector is open. Declared here for the
// input layer, which releases mouse-steering capture when the inspector is up.
extern "C" int rs64_rt64_inspector_open(void);

// Android lifecycle: stop presenting before the OS destroys the window, and move RT64's swap chain to the new window on
// return. resume returns false (and stays suspended) until SDL has a live native window with a size.
extern "C" void rs64_render_suspend_surface(void);
extern "C" bool rs64_render_resume_surface(void);
extern "C" bool rs64_render_surface_suspended(void);

#endif // RS64_RT64_RENDER_CONTEXT_H
