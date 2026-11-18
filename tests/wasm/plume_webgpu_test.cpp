// Runs in Chrome (plume_webgpu_test.html, which loads rs64tint first): plume's WebGPU backend against RT64's own shaders.

#include "plume_webgpu.h"
#include "video/web_shader_convert.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
    extern const char IdleCSBlobSPIRV[];
    extern const size_t IdleCSBlobSPIRV_size;
    extern const char FullScreenVSBlobSPIRV[];
    extern const size_t FullScreenVSBlobSPIRV_size;
    extern const char ComposePSBlobSPIRV[];
    extern const size_t ComposePSBlobSPIRV_size;
    extern const char TextureCopyPSBlobSPIRV[];
    extern const size_t TextureCopyPSBlobSPIRV_size;
}

namespace {

    using namespace plume;

    std::unique_ptr<RenderInterface> gInterface;
    std::unique_ptr<RenderDevice> gDevice;
    std::unique_ptr<RenderCommandQueue> gQueue;

    bool convert(const std::vector<uint32_t>& spirv, const RenderSpecConstant*, uint32_t, std::string& wgsl, std::string& error)
    {

        rs64::wgsl::ConvertResult result = rs64::wgsl::convert_on_main(spirv);

        wgsl = std::move(result.wgsl);
        error = std::move(result.error);
        return result.ok;
    }

    void submit_and_wait(RenderCommandList* list)
    {

        std::unique_ptr<RenderCommandFence> fence = gDevice->createCommandFence();

        gQueue->executeCommandLists(list, fence.get());
        gQueue->waitForCommandFence(fence.get());
    }

    std::string creates_device()
    {

        gInterface = CreateWebGPUInterface(convert);

        if (!gInterface)
        {
            return "no interface";
        }

        gDevice = gInterface->createDevice();

        if (!gDevice)
        {
            return "no device";
        }

        const RenderDeviceCapabilities& caps = gDevice->getCapabilities();

        if (!caps.computeShaders || caps.raytracing || caps.rayQuery || (caps.maxTexturesPerStage < 16))
        {
            return "unexpected capabilities";
        }

        gQueue = gDevice->createCommandQueue(RenderCommandListType::DIRECT);
        return gQueue ? "" : "no queue";
    }

    std::string buffer_upload_readback()
    {

        std::unique_ptr<RenderBuffer> upload = gDevice->createBuffer(RenderBufferDesc::UploadBuffer(256));
        std::unique_ptr<RenderBuffer> readback = gDevice->createBuffer(RenderBufferDesc::ReadbackBuffer(256));
        uint8_t* src = static_cast<uint8_t*>(upload->map());

        for (uint32_t i = 0; i < 256; i++) {
            src[i] = uint8_t(i * 7 + 3);
        }

        upload->unmap();

        std::unique_ptr<RenderCommandList> list = gQueue->createCommandList();

        list->begin();
        list->copyBuffer(readback.get(), upload.get());
        list->end();
        submit_and_wait(list.get());

        const uint8_t* dst = static_cast<const uint8_t*>(readback->map());

        for (uint32_t i = 0; i < 256; i++) {
            if (dst[i] != uint8_t(i * 7 + 3))
            {
                return "byte " + std::to_string(i) + " differs";
            }
        }

        readback->unmap();
        return "";
    }

    std::string texture_upload()
    {

        constexpr uint32_t kSize = 64;
        constexpr uint32_t kBytes = kSize * kSize * 4;
        std::unique_ptr<RenderBuffer> upload = gDevice->createBuffer(RenderBufferDesc::UploadBuffer(kBytes));
        std::unique_ptr<RenderBuffer> readback = gDevice->createBuffer(RenderBufferDesc::ReadbackBuffer(kBytes));
        std::unique_ptr<RenderTexture> texture = gDevice->createTexture(RenderTextureDesc::Texture2D(kSize, kSize, 1, RenderFormat::R8G8B8A8_UNORM));
        uint32_t* src = static_cast<uint32_t*>(upload->map());

        for (uint32_t i = 0; i < kSize * kSize; i++) {
            src[i] = 0xFF000000u | (i * 2654435761u >> 8);
        }

        upload->unmap();

        std::unique_ptr<RenderCommandList> list = gQueue->createCommandList();
        const RenderTextureCopyLocation footprintSrc = RenderTextureCopyLocation::PlacedFootprint(upload.get(), RenderFormat::R8G8B8A8_UNORM, kSize, kSize, 1, kSize);
        const RenderTextureCopyLocation footprintDst = RenderTextureCopyLocation::PlacedFootprint(readback.get(), RenderFormat::R8G8B8A8_UNORM, kSize, kSize, 1, kSize);

        list->begin();
        list->copyTextureRegion(RenderTextureCopyLocation::Subresource(texture.get()), footprintSrc);
        list->copyTextureRegion(footprintDst, RenderTextureCopyLocation::Subresource(texture.get()));
        list->end();
        submit_and_wait(list.get());

        const uint32_t* dst = static_cast<const uint32_t*>(readback->map());

        for (uint32_t i = 0; i < kSize * kSize; i++) {
            if (dst[i] != (0xFF000000u | (i * 2654435761u >> 8)))
            {
                return "texel " + std::to_string(i) + " differs";
            }
        }

        readback->unmap();
        return "";
    }

    std::string compute_pipeline_runs()
    {

        std::unique_ptr<RenderShader> shader = gDevice->createShader(IdleCSBlobSPIRV, IdleCSBlobSPIRV_size, "CSMain", RenderShaderFormat::SPIRV);
        RenderPipelineLayoutBuilder layoutBuilder;

        layoutBuilder.begin();
        layoutBuilder.end();

        std::unique_ptr<RenderPipelineLayout> layout = layoutBuilder.create(gDevice.get());
        std::unique_ptr<RenderPipeline> pipeline = gDevice->createComputePipeline(RenderComputePipelineDesc(layout.get(), shader.get(), 1, 1, 1));
        const auto* webPipeline = static_cast<const WebGPUPipeline*>(pipeline.get());

        if (!webPipeline->valid)
        {
            return "pipeline invalid: " + webPipeline->error;
        }

        std::unique_ptr<RenderCommandList> list = gQueue->createCommandList();

        list->begin();
        list->setComputePipelineLayout(layout.get());
        list->setPipeline(pipeline.get());
        list->dispatch(1, 1, 1);
        list->end();
        submit_and_wait(list.get());

        const std::string error = static_cast<WebGPUDevice*>(gDevice.get())->takeErrors();

        return error.empty() ? "" : "device error: " + error;
    }

    std::unique_ptr<RenderPipeline> compose_pipeline(RenderPipelineLayout* layout, uint32_t targetCount)
    {

        std::unique_ptr<RenderShader> vertex = gDevice->createShader(FullScreenVSBlobSPIRV, FullScreenVSBlobSPIRV_size, "VSMain", RenderShaderFormat::SPIRV);
        std::unique_ptr<RenderShader> pixel = gDevice->createShader(ComposePSBlobSPIRV, ComposePSBlobSPIRV_size, "PSMain", RenderShaderFormat::SPIRV);
        RenderGraphicsPipelineDesc desc;

        desc.pipelineLayout = layout;
        desc.vertexShader = vertex.get();
        desc.pixelShader = pixel.get();
        desc.renderTargetCount = targetCount;

        for (uint32_t i = 0; i < targetCount; i++) {
            desc.renderTargetFormat[i] = RenderFormat::R8G8B8A8_UNORM;
            desc.renderTargetBlend[i] = RenderBlendDesc::Copy();
        }

        return gDevice->createGraphicsPipeline(desc);
    }

    std::unique_ptr<RenderPipelineLayout> compose_layout()
    {

        RenderDescriptorSetBuilder setBuilder;

        setBuilder.begin();
        setBuilder.addSampler(0);

        for (uint32_t binding = 2; binding <= 7; binding++) {
            setBuilder.addTexture(binding);
        }

        setBuilder.end();

        RenderPipelineLayoutBuilder layoutBuilder;

        layoutBuilder.begin();
        layoutBuilder.addDescriptorSet(setBuilder);
        layoutBuilder.end();
        return layoutBuilder.create(gDevice.get());
    }

    std::string graphics_pipeline_from_rt64_shaders()
    {

        std::unique_ptr<RenderPipelineLayout> layout = compose_layout();
        std::unique_ptr<RenderPipeline> pipeline = compose_pipeline(layout.get(), 1);
        const auto* webPipeline = static_cast<const WebGPUPipeline*>(pipeline.get());

        return webPipeline->valid ? "" : "pipeline invalid: " + webPipeline->error;
    }

    std::string bad_pipeline_fails_loudly()
    {

        // ComposePS writes one target; a second with writes enabled is a WebGPU validation error.
        std::unique_ptr<RenderPipelineLayout> layout = compose_layout();
        std::unique_ptr<RenderPipeline> pipeline = compose_pipeline(layout.get(), 2);
        const auto* webPipeline = static_cast<const WebGPUPipeline*>(pipeline.get());

        if (webPipeline->valid)
        {
            return "pipeline was accepted";
        }

        return webPipeline->error.empty() ? "no error text" : "";
    }

    constexpr uint32_t kTarget = 4;
    constexpr uint32_t kRowTexels = 64;

    // Reads a 4x4 RGBA8 texture back; rows are padded to 256 bytes, as WebGPU copies need.
    std::vector<uint32_t> read_target(RenderTexture* texture)
    {

        std::unique_ptr<RenderBuffer> readback = gDevice->createBuffer(RenderBufferDesc::ReadbackBuffer(kRowTexels * 4 * kTarget));
        std::unique_ptr<RenderCommandList> list = gQueue->createCommandList();

        list->begin();
        list->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(readback.get(), RenderFormat::R8G8B8A8_UNORM, kTarget, kTarget, 1, kRowTexels), RenderTextureCopyLocation::Subresource(texture));
        list->end();
        submit_and_wait(list.get());

        const uint32_t* rows = static_cast<const uint32_t*>(readback->map());
        std::vector<uint32_t> texels;

        for (uint32_t y = 0; y < kTarget; y++) {
            texels.insert(texels.end(), rows + y * kRowTexels, rows + y * kRowTexels + kTarget);
        }

        readback->unmap();
        return texels;
    }

    std::string expect_texels(const std::vector<uint32_t>& texels, uint32_t (*expected)(uint32_t x, uint32_t y))
    {

        char message[96];

        for (uint32_t y = 0; y < kTarget; y++) {
            for (uint32_t x = 0; x < kTarget; x++) {
                if (texels[y * kTarget + x] != expected(x, y))
                {
                    std::snprintf(message, sizeof(message), "texel %u,%u is %08X, expected %08X", x, y, texels[y * kTarget + x], expected(x, y));
                    return message;
                }
            }
        }

        return "";
    }

    constexpr uint32_t kRed = 0xFF0000FFu;
    constexpr uint32_t kBlue = 0xFFFF0000u;

    std::string render_pass_clear()
    {

        std::unique_ptr<RenderTexture> target = gDevice->createTexture(RenderTextureDesc::ColorTarget(kTarget, kTarget, RenderFormat::R8G8B8A8_UNORM));
        const RenderTexture* attachment = target.get();
        std::unique_ptr<RenderFramebuffer> framebuffer = gDevice->createFramebuffer(RenderFramebufferDesc(&attachment, 1));
        std::unique_ptr<RenderCommandList> list = gQueue->createCommandList();

        list->begin();
        list->setFramebuffer(framebuffer.get());
        list->clearColor(0, RenderColor(1.0f, 0.0f, 0.0f, 1.0f));
        list->end();
        submit_and_wait(list.get());
        return expect_texels(read_target(target.get()), [](uint32_t, uint32_t) { return kRed; });
    }

    std::string partial_clear()
    {

        std::unique_ptr<RenderTexture> target = gDevice->createTexture(RenderTextureDesc::ColorTarget(kTarget, kTarget, RenderFormat::R8G8B8A8_UNORM));
        const RenderTexture* attachment = target.get();
        std::unique_ptr<RenderFramebuffer> framebuffer = gDevice->createFramebuffer(RenderFramebufferDesc(&attachment, 1));
        std::unique_ptr<RenderCommandList> list = gQueue->createCommandList();
        const RenderRect corner(0, 0, 2, 2);

        list->begin();
        list->setFramebuffer(framebuffer.get());
        list->clearColor(0, RenderColor(0.0f, 0.0f, 1.0f, 1.0f));
        list->clearColor(0, RenderColor(1.0f, 0.0f, 0.0f, 1.0f), &corner, 1);
        list->end();
        submit_and_wait(list.get());
        return expect_texels(read_target(target.get()), [](uint32_t x, uint32_t y) { return ((x < 2) && (y < 2)) ? kRed : kBlue; });
    }

    uint32_t source_texel(uint32_t x, uint32_t y)
    {
        return 0xFF000000u | (y * 0x40u << 8) | (x * 0x40u);
    }

    std::string draw_texture_copy()
    {

        // TextureCopyPS reads gInput at uvScroll + uv * uvScale through push constants, so a 4x4 scale copies the texture.
        std::unique_ptr<RenderBuffer> upload = gDevice->createBuffer(RenderBufferDesc::UploadBuffer(kRowTexels * 4 * kTarget));
        std::unique_ptr<RenderTexture> source = gDevice->createTexture(RenderTextureDesc::Texture2D(kTarget, kTarget, 1, RenderFormat::R8G8B8A8_UNORM));
        std::unique_ptr<RenderTexture> target = gDevice->createTexture(RenderTextureDesc::ColorTarget(kTarget, kTarget, RenderFormat::R8G8B8A8_UNORM));
        uint32_t* rows = static_cast<uint32_t*>(upload->map());

        for (uint32_t y = 0; y < kTarget; y++) {
            for (uint32_t x = 0; x < kTarget; x++) {
                rows[y * kRowTexels + x] = source_texel(x, y);
            }
        }

        upload->unmap();

        RenderDescriptorSetBuilder setBuilder;

        setBuilder.begin();
        setBuilder.addTexture(1);
        setBuilder.end();

        RenderPipelineLayoutBuilder layoutBuilder;

        layoutBuilder.begin();
        layoutBuilder.addPushConstant(0, 0, 16, RenderShaderStageFlag::PIXEL);
        layoutBuilder.addDescriptorSet(setBuilder);
        layoutBuilder.end();

        std::unique_ptr<RenderPipelineLayout> layout = layoutBuilder.create(gDevice.get());
        std::unique_ptr<RenderDescriptorSet> set = setBuilder.create(gDevice.get());
        std::unique_ptr<RenderShader> vertex = gDevice->createShader(FullScreenVSBlobSPIRV, FullScreenVSBlobSPIRV_size, "VSMain", RenderShaderFormat::SPIRV);
        std::unique_ptr<RenderShader> pixel = gDevice->createShader(TextureCopyPSBlobSPIRV, TextureCopyPSBlobSPIRV_size, "PSMain", RenderShaderFormat::SPIRV);
        RenderGraphicsPipelineDesc desc;

        desc.pipelineLayout = layout.get();
        desc.vertexShader = vertex.get();
        desc.pixelShader = pixel.get();
        desc.renderTargetCount = 1;
        desc.renderTargetFormat[0] = RenderFormat::R8G8B8A8_UNORM;
        desc.renderTargetBlend[0] = RenderBlendDesc::Copy();

        std::unique_ptr<RenderPipeline> pipeline = gDevice->createGraphicsPipeline(desc);
        const auto* webPipeline = static_cast<const WebGPUPipeline*>(pipeline.get());

        if (!webPipeline->valid)
        {
            return "pipeline invalid: " + webPipeline->error;
        }

        set->setTexture(0, source.get(), RenderTextureLayout::SHADER_READ);

        const RenderTexture* attachment = target.get();
        std::unique_ptr<RenderFramebuffer> framebuffer = gDevice->createFramebuffer(RenderFramebufferDesc(&attachment, 1));
        const float constants[4] = { 0.0f, 0.0f, float(kTarget), float(kTarget) };
        std::unique_ptr<RenderCommandList> list = gQueue->createCommandList();

        list->begin();
        list->copyTextureRegion(RenderTextureCopyLocation::Subresource(source.get()), RenderTextureCopyLocation::PlacedFootprint(upload.get(), RenderFormat::R8G8B8A8_UNORM, kTarget, kTarget, 1, kRowTexels));
        list->setFramebuffer(framebuffer.get());
        list->setViewports(RenderViewport(0.0f, 0.0f, float(kTarget), float(kTarget)));
        list->setScissors(RenderRect(0, 0, kTarget, kTarget));
        list->setGraphicsPipelineLayout(layout.get());
        list->setPipeline(pipeline.get());
        list->setGraphicsDescriptorSet(set.get(), 0);
        list->setGraphicsPushConstants(0, constants);
        list->drawInstanced(3, 1, 0, 0);
        list->end();
        submit_and_wait(list.get());

        const std::string error = static_cast<WebGPUDevice*>(gDevice.get())->takeErrors();

        if (!error.empty())
        {
            return "device error: " + error;
        }

        return expect_texels(read_target(target.get()), source_texel);
    }

    std::string fence_from_worker()
    {

        std::unique_ptr<RenderCommandFence> fence = gDevice->createCommandFence();
        std::unique_ptr<RenderCommandList> list = gQueue->createCommandList();

        list->begin();
        list->end();
        gQueue->executeCommandLists(list.get(), fence.get());

        std::thread waiter([&] { gQueue->waitForCommandFence(fence.get()); });

        waiter.join();
        return static_cast<WebGPUCommandFence*>(fence.get())->pending.load() == 0 ? "" : "fence still pending";
    }

    // Kept alive so the presented frame stays on the page; destroying it unconfigures and clears the canvas.
    std::unique_ptr<RenderSwapChain> gSwapChain;

    std::string swap_chain_clear()
    {

        gSwapChain = gQueue->createSwapChain(RenderSwapChainDesc(nullptr, RenderFormat::B8G8R8A8_UNORM, 2));

        RenderSwapChain* swapChain = gSwapChain.get();

        if (!swapChain || (swapChain->getWidth() != 8) || (swapChain->getHeight() != 8))
        {
            return "no 8x8 swap chain for the page's canvas";
        }

        uint32_t index = 0;

        if (!swapChain->acquireTexture(nullptr, &index))
        {
            return "acquire failed";
        }

        const RenderTexture* attachment = swapChain->getTexture(index);
        std::unique_ptr<RenderFramebuffer> framebuffer = gDevice->createFramebuffer(RenderFramebufferDesc(&attachment, 1));
        std::unique_ptr<RenderCommandList> list = gQueue->createCommandList();

        list->begin();
        list->setFramebuffer(framebuffer.get());
        list->clearColor(0, RenderColor(0.0f, 1.0f, 0.0f, 1.0f));
        list->end();
        submit_and_wait(list.get());

        if (!swapChain->present(index, nullptr, 0))
        {
            return "present failed";
        }

        const std::string error = static_cast<WebGPUDevice*>(gDevice.get())->takeErrors();

        return error.empty() ? "" : "device error: " + error;
    }

}

int main()
{

    struct Case {
        const char* name;
        std::string (*run)();
    };

    const Case cases[] = {
        { "creates_device", creates_device },
        { "buffer_upload_readback", buffer_upload_readback },
        { "texture_upload", texture_upload },
        { "compute_pipeline_runs", compute_pipeline_runs },
        { "graphics_pipeline_from_rt64_shaders", graphics_pipeline_from_rt64_shaders },
        { "bad_pipeline_fails_loudly", bad_pipeline_fails_loudly },
        { "render_pass_clear", render_pass_clear },
        { "partial_clear", partial_clear },
        { "draw_texture_copy", draw_texture_copy },
        { "fence_from_worker", fence_from_worker },
        { "swap_chain_clear", swap_chain_clear }
    };

    int failed = 0;

    for (const Case& c : cases) {
        const std::string message = (gDevice || (std::strcmp(c.name, "creates_device") == 0)) ? c.run() : "no device";

        if (!message.empty()) {
            ++failed;
        }

        std::printf("%s %s%s%s\n", message.empty() ? "PASS" : "FAIL", c.name, message.empty() ? "" : ": ", message.c_str());
    }

    std::printf("plume_webgpu_test %s\n", failed ? "FAILED" : "DONE");
    return failed;
}
