// A renderer that draws nothing: the browser build boots with it until the WebGPU backend exists. The runtime raises the DP event after send_dl returns, so the game runs normally.

#include "video/null_render_context.h"

namespace {

    class NullRenderContext : public ultramodern::renderer::RendererContext {
    public:
        NullRenderContext()
        {
            setup_result = ultramodern::renderer::SetupResult::Success;
            chosen_api = ultramodern::renderer::GraphicsApi::Auto;
        }

        bool valid() override
        {
            return true;
        }

        bool update_config(const ultramodern::renderer::GraphicsConfig&, const ultramodern::renderer::GraphicsConfig&) override
        {
            return true;
        }

        void enable_instant_present() override
        {
        }

        void send_dl(const OSTask*) override
        {
        }

        void update_screen() override
        {
        }

        void shutdown() override
        {
        }

        uint32_t get_display_framerate() const override
        {
            return 60;
        }

        float get_resolution_scale() const override
        {
            return 1.0f;
        }
    };

}

std::unique_ptr<ultramodern::renderer::RendererContext> recomp::create_null_render_context(uint8_t*, ultramodern::renderer::WindowHandle, bool)
{
    return std::make_unique<NullRenderContext>();
}
