#pragma once

#include <cstdint>
#include <memory>
#include "ultramodern/renderer_context.hpp"

namespace recomp {
    std::unique_ptr<ultramodern::renderer::RendererContext>
    create_null_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window, bool developer_mode);
}
