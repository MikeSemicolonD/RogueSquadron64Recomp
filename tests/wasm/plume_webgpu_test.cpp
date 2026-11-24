// Runs in Chrome (plume_webgpu_test.html, which loads rs64tint first): plume's WebGPU backend against RT64's own shaders.

#include "plume_webgpu.h"
#include "plume_webgpu_as.h"
#include "video/web_shader_convert.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <functional>
#include <random>
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
    extern const char ReadSceneHeaderCSBlobSPIRV[];
    extern const size_t ReadSceneHeaderCSBlobSPIRV_size;
    extern const char RS64SoftRayQueryTestCSBlobSPIRV[];
    extern const size_t RS64SoftRayQueryTestCSBlobSPIRV_size;
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

        const auto* web = static_cast<const WebGPUDevice*>(gDevice.get());
        const bool rayQuery = (web->limits.maxStorageBuffersPerShaderStage >= 15) && web->textureFormatsTier1 && !web->phone;

        if (!caps.computeShaders || caps.raytracing || (caps.rayQuery != rayQuery) || (caps.maxTexturesPerStage < 16))
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

    namespace was = plume::webgpu_as;

    std::unique_ptr<RenderBuffer> as_buffer(uint64_t size)
    {
        return gDevice->createBuffer(RenderBufferDesc::AccelerationStructureBuffer(size));
    }

    // ReadSceneHeaderCS over a bound acceleration structure: its first 16 words.
    std::string read_scene_words(const RenderAccelerationStructure* as, uint32_t out[16])
    {

        RenderDescriptorSetBuilder setBuilder;

        setBuilder.begin();
        setBuilder.addAccelerationStructure(0);
        setBuilder.addReadWriteByteAddressBuffer(1);
        setBuilder.end();

        RenderPipelineLayoutBuilder layoutBuilder;

        layoutBuilder.begin();
        layoutBuilder.addDescriptorSet(setBuilder);
        layoutBuilder.end();

        std::unique_ptr<RenderPipelineLayout> layout = layoutBuilder.create(gDevice.get());
        std::unique_ptr<RenderShader> shader = gDevice->createShader(ReadSceneHeaderCSBlobSPIRV, ReadSceneHeaderCSBlobSPIRV_size, "CSMain", RenderShaderFormat::SPIRV);
        std::unique_ptr<RenderPipeline> pipeline = gDevice->createComputePipeline(RenderComputePipelineDesc(layout.get(), shader.get(), 16, 1, 1));
        const auto* webPipeline = static_cast<const WebGPUPipeline*>(pipeline.get());

        if (!webPipeline->valid)
        {
            return "pipeline invalid: " + webPipeline->error;
        }

        std::unique_ptr<RenderDescriptorSet> set = setBuilder.create(gDevice.get());
        std::unique_ptr<RenderBuffer> words = gDevice->createBuffer(RenderBufferDesc::DefaultBuffer(64, RenderBufferFlag::STORAGE | RenderBufferFlag::UNORDERED_ACCESS));
        std::unique_ptr<RenderBuffer> readback = gDevice->createBuffer(RenderBufferDesc::ReadbackBuffer(64));
        std::unique_ptr<RenderCommandList> list = gQueue->createCommandList();

        set->setAccelerationStructure(0, as);
        set->setBuffer(1, words.get());
        list->begin();
        list->setComputePipelineLayout(layout.get());
        list->setPipeline(pipeline.get());
        list->setComputeDescriptorSet(set.get(), 0);
        list->dispatch(1, 1, 1);
        list->copyBuffer(readback.get(), words.get());
        list->end();
        submit_and_wait(list.get());
        std::memcpy(out, readback->map(), 64);
        readback->unmap();
        return "";
    }

    std::string as_create_and_bind()
    {

        std::unique_ptr<RenderBuffer> blasBuffer = as_buffer(was::blasSize(3));
        std::unique_ptr<RenderBuffer> tlasBuffer = as_buffer(was::tlasSize(2));
        std::unique_ptr<RenderAccelerationStructure> blas = gDevice->createAccelerationStructure(RenderAccelerationStructureDesc(RenderAccelerationStructureType::BOTTOM_LEVEL, RenderBufferReference(blasBuffer.get()), was::blasSize(3)));
        std::unique_ptr<RenderAccelerationStructure> tlas = gDevice->createAccelerationStructure(RenderAccelerationStructureDesc(RenderAccelerationStructureType::TOP_LEVEL, RenderBufferReference(tlasBuffer.get()), was::tlasSize(2)));

        if (!blas || !tlas)
        {
            return "no acceleration structure";
        }

        uint32_t words[16] = {};
        const std::string read = read_scene_words(tlas.get(), words);

        if (!read.empty())
        {
            return read;
        }

        for (uint32_t w : words) {
            if (w != 0)
            {
                return "an unbuilt TLAS reads non-zero";
            }
        }

        const std::string error = static_cast<WebGPUDevice*>(gDevice.get())->takeErrors();

        return error.empty() ? "" : "device error: " + error;
    }

    std::string as_heap_full_fails_cleanly()
    {

        const uint64_t huge = uint64_t(64) << 30;
        std::unique_ptr<RenderBuffer> buffer = as_buffer(256);
        std::unique_ptr<RenderAccelerationStructure> as = gDevice->createAccelerationStructure(RenderAccelerationStructureDesc(RenderAccelerationStructureType::BOTTOM_LEVEL, RenderBufferReference(buffer.get()), huge));
        const std::string error = static_cast<WebGPUDevice*>(gDevice.get())->takeErrors();

        if (as)
        {
            return "a structure larger than the heap was created";
        }

        return error.empty() ? "no error reported" : "";
    }

    std::string as_build_info()
    {

        std::unique_ptr<RenderBuffer> index = gDevice->createBuffer(RenderBufferDesc::DefaultBuffer(3000 * 4, RenderBufferFlag::STORAGE));
        std::unique_ptr<RenderBuffer> vertex = gDevice->createBuffer(RenderBufferDesc::DefaultBuffer(3000 * 16, RenderBufferFlag::STORAGE));
        const RenderBottomLevelASMesh mesh(RenderBufferReference(index.get()), RenderBufferReference(vertex.get()), RenderFormat::R32_UINT, RenderFormat::R32G32B32_FLOAT, 3000, 3000, 16, true);
        RenderBottomLevelASBuildInfo info;

        gDevice->setBottomLevelASBuildInfo(info, &mesh, 1, true, false);

        if ((info.primitiveCount != 1000) || (info.accelerationStructureSize != was::blasSize(1000)) || (info.scratchSize != was::blasScratchSize(1000)))
        {
            return "wrong BLAS sizes";
        }

        const RenderBottomLevelASMesh two[2] = { mesh, mesh };
        RenderBottomLevelASBuildInfo bad;

        gDevice->setBottomLevelASBuildInfo(bad, two, 2, true, false);

        if ((bad.accelerationStructureSize != 0) || static_cast<WebGPUDevice*>(gDevice.get())->takeErrors().empty())
        {
            return "two meshes were accepted";
        }

        std::unique_ptr<RenderBuffer> blasBuffer = as_buffer(info.accelerationStructureSize);
        std::unique_ptr<RenderAccelerationStructure> blas = gDevice->createAccelerationStructure(RenderAccelerationStructureDesc(RenderAccelerationStructureType::BOTTOM_LEVEL, RenderBufferReference(blasBuffer.get()), info.accelerationStructureSize));
        RenderAffineTransform moved;

        moved.m[0][3] = 10.0f;

        const RenderTopLevelASInstance instances[2] = {
            RenderTopLevelASInstance(RenderBufferReference(blasBuffer.get()), 4, 0x01, 0, false, RenderAffineTransform()),
            RenderTopLevelASInstance(RenderBufferReference(blasBuffer.get()), 9, 0x02, 0, true, moved)
        };

        RenderTopLevelASBuildInfo tlasInfo;

        gDevice->setTopLevelASBuildInfo(tlasInfo, instances, 2, true, false);

        if ((tlasInfo.instanceCount != 2) || (tlasInfo.accelerationStructureSize != was::tlasSize(2)) || (tlasInfo.instancesBufferData.size() != sizeof(was::TlasHeader) + 2 * sizeof(was::Instance)))
        {
            return "wrong TLAS sizes";
        }

        was::Instance second;

        std::memcpy(&second, tlasInfo.instancesBufferData.data() + sizeof(was::TlasHeader) + sizeof(was::Instance), sizeof(second));

        if ((second.instanceID != 9) || (second.mask != 0x02) || (second.worldToObject[0][3] != -10.0f) || (second.flags != was::kInstanceCullDisable))
        {
            return "wrong instance record";
        }

        const std::string error = static_cast<WebGPUDevice*>(gDevice.get())->takeErrors();

        return error.empty() ? "" : "device error: " + error;
    }

    // A BLAS built on the GPU through the real build path, read back from the AS heap.
    struct BuiltBlas {
        std::unique_ptr<RenderBuffer> asBuffer;
        std::unique_ptr<RenderAccelerationStructure> as;
        std::vector<uint8_t> bytes;
    };

    std::unique_ptr<RenderBuffer> upload(const void* data, uint64_t size)
    {

        std::unique_ptr<RenderBuffer> buffer = gDevice->createBuffer(RenderBufferDesc::UploadBuffer(std::max<uint64_t>(size, 16), RenderBufferFlag::STORAGE));

        if (size > 0) {
            std::memcpy(buffer->map(), data, size);
            buffer->unmap();
        }

        return buffer;
    }

    std::string build_blas(const std::vector<float>& pos, const std::vector<uint32_t>& idx, uint32_t stride, BuiltBlas& out, RenderAccelerationStructure* reuse = nullptr, bool opaque = true)
    {

        // Positions widened to the stride (in floats); the extra words hold garbage the builder must skip.
        std::vector<float> wide;

        for (size_t v = 0; v < pos.size() / 3; v++) {
            for (uint32_t k = 0; k < stride / 4; k++) {
                wide.push_back((k < 3) ? pos[v * 3 + k] : 12345.0f);
            }
        }

        std::unique_ptr<RenderBuffer> index = upload(idx.data(), idx.size() * 4);
        std::unique_ptr<RenderBuffer> vertex = upload(wide.data(), wide.size() * 4);
        const RenderBottomLevelASMesh mesh(RenderBufferReference(index.get()), RenderBufferReference(vertex.get()), RenderFormat::R32_UINT, RenderFormat::R32G32B32_FLOAT, uint32_t(idx.size()), uint32_t(pos.size() / 3), stride, opaque);
        RenderBottomLevelASBuildInfo info;

        gDevice->setBottomLevelASBuildInfo(info, &mesh, 1, true, false);

        if (reuse == nullptr) {
            out.asBuffer = as_buffer(was::blasSize(1 << 13));
            out.as = gDevice->createAccelerationStructure(RenderAccelerationStructureDesc(RenderAccelerationStructureType::BOTTOM_LEVEL, RenderBufferReference(out.asBuffer.get()), std::max<uint64_t>(info.accelerationStructureSize, was::blasSize(1 << 13))));
            reuse = out.as.get();
        }

        if (reuse == nullptr)
        {
            return "no acceleration structure";
        }

        std::unique_ptr<RenderBuffer> scratch = gDevice->createBuffer(RenderBufferDesc::DefaultBuffer(info.scratchSize, RenderBufferFlag::ACCELERATION_STRUCTURE_SCRATCH));
        const auto* web = static_cast<const WebGPUAccelerationStructure*>(reuse);
        std::unique_ptr<RenderBuffer> readback = gDevice->createBuffer(RenderBufferDesc::ReadbackBuffer(info.accelerationStructureSize));
        std::unique_ptr<RenderCommandList> list = gQueue->createCommandList();

        list->begin();
        list->buildBottomLevelAS(reuse, RenderBufferReference(scratch.get()), info);
        list->copyBufferRegion(RenderBufferReference(readback.get()), RenderBufferReference(static_cast<WebGPUDevice*>(gDevice.get())->asHeap.get(), web->offset), info.accelerationStructureSize);
        list->end();
        submit_and_wait(list.get());
        out.bytes.resize(info.accelerationStructureSize);
        std::memcpy(out.bytes.data(), readback->map(), out.bytes.size());
        readback->unmap();

        const std::string error = static_cast<WebGPUDevice*>(gDevice.get())->takeErrors();

        return error.empty() ? "" : "device error: " + error;
    }

    void random_tris(uint32_t seed, uint32_t n, std::vector<float>& pos, std::vector<uint32_t>& idx)
    {

        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> c(-50.0f, 50.0f);
        std::uniform_real_distribution<float> d(-3.0f, 3.0f);

        pos.clear();
        idx.clear();

        for (uint32_t t = 0; t < n; t++) {
            const float cx = c(rng);
            const float cy = c(rng);
            const float cz = c(rng);

            for (int k = 0; k < 3; k++) {
                idx.push_back(uint32_t(pos.size() / 3));
                pos.push_back(cx + d(rng));
                pos.push_back(cy + d(rng));
                pos.push_back(cz + d(rng));
            }
        }
    }

    // Every primitive once, every node's boxes around its subtree, and the tree tracing like brute force.
    std::string check_blas(const std::vector<uint8_t>& blas, const std::vector<float>& pos, const std::vector<uint32_t>& idx, uint32_t rays)
    {

        was::BlasHeader header;
        const uint32_t n = uint32_t(idx.size() / 3);

        std::memcpy(&header, blas.data(), sizeof(header));

        if ((header.triCount != n) || (header.nodeCount != was::nodeCountFor(n)))
        {
            return "header " + std::to_string(header.triCount) + "/" + std::to_string(header.nodeCount) + " for " + std::to_string(n) + " triangles";
        }

        std::vector<uint32_t> seen(n, 0);
        const auto* tris = reinterpret_cast<const was::Tri*>(blas.data() + was::blasTriOffset(n));

        for (uint32_t t = 0; t < n; t++) {
            if (tris[t].primitiveIndex >= n)
            {
                return "primitive index out of range";
            }

            seen[tris[t].primitiveIndex]++;
        }

        for (uint32_t s : seen) {
            if (s != 1)
            {
                return "a primitive is missing or repeated";
            }
        }

        const auto* nodes = reinterpret_cast<const was::Node*>(blas.data() + sizeof(was::BlasHeader));

        // A child's box must hold every vertex under it.
        std::function<bool(uint32_t, const float*, const float*)> contains = [&](uint32_t word, const float* lo, const float* hi) {

            auto inside = [&](const float* v) {

                for (int a = 0; a < 3; a++) {
                    if ((v[a] < lo[a] - 1e-3f) || (v[a] > hi[a] + 1e-3f)) return false;
                }

                return true;
            };

            if ((word & was::kLeafBit) != 0) {
                const was::Tri& tri = tris[word & ~was::kLeafBit];
                return inside(tri.v0) && inside(tri.v1) && inside(tri.v2);
            }

            const was::Node& node = nodes[word];
            return contains(node.left, lo, hi) && contains(node.right, lo, hi) && contains(node.left, node.lmin, node.lmax) && contains(node.right, node.rmin, node.rmax);
        };

        if (n > 1) {
            const float lo[3] = { -1e30f, -1e30f, -1e30f };
            const float hi[3] = { 1e30f, 1e30f, 1e30f };

            if (!contains(0, lo, hi))
            {
                return "a node's box misses part of its subtree";
            }
        }

        std::mt19937 rng(99);
        std::uniform_real_distribution<float> o(-60.0f, 60.0f);
        std::uniform_real_distribution<float> d(-1.0f, 1.0f);

        for (uint32_t r = 0; r < rays; r++) {
            const float org[3] = { o(rng), o(rng), o(rng) };
            const float dir[3] = { d(rng), d(rng), d(rng) };
            was::RefHit want;
            was::RefHit got;
            const bool w = was::traceReference(pos, idx, org, dir, 0.0f, 1e30f, false, &want);
            const bool g = was::traceBvh(blas.data(), org, dir, 0.0f, 1e30f, false, &got);

            // Overlapping coplanar triangles tie on t; either one is the closest hit then.
            if ((w != g) || (w && (std::fabs(want.t - got.t) > 1e-4f * std::max(1.0f, want.t))))
            {
                return "ray " + std::to_string(r) + " traces differently (hit " + std::to_string(w) + "/" + std::to_string(g) + " t " + std::to_string(want.t) + "/" + std::to_string(got.t) + ")";
            }

            if (w && (want.prim != got.prim) && (std::fabs(want.t - got.t) > 1e-6f * std::max(1.0f, want.t)))
            {
                return "ray " + std::to_string(r) + " hits primitive " + std::to_string(got.prim) + " instead of " + std::to_string(want.prim);
            }
        }

        return "";
    }

    std::string blas_counts()
    {

        for (uint32_t n : { 0u, 1u, 2u, 7u, 1000u, 5000u }) {
            std::vector<float> pos;
            std::vector<uint32_t> idx;
            BuiltBlas built;

            random_tris(n + 1, n, pos, idx);

            const std::string build = build_blas(pos, idx, 12, built);
            const std::string check = build.empty() ? check_blas(built.bytes, pos, idx, 0) : build;

            if (!check.empty())
            {
                return "n=" + std::to_string(n) + ": " + check;
            }
        }

        return "";
    }

    std::string blas_trace_matches_bruteforce()
    {

        std::vector<float> pos;
        std::vector<uint32_t> idx;
        BuiltBlas built;

        random_tris(5, 5000, pos, idx);

        const std::string build = build_blas(pos, idx, 12, built);

        return build.empty() ? check_blas(built.bytes, pos, idx, 4000) : build;
    }

    std::string blas_degenerate_codes()
    {

        std::vector<float> pos;
        std::vector<uint32_t> idx;

        // 300 triangles around one centroid, then two coplanar ones.
        for (uint32_t t = 0; t < 300; t++) {
            const float s = 1.0f + float(t % 7);

            for (const float v : { -s, 0.0f, 0.0f, s, 0.0f, 0.0f, 0.0f, s * float(1 + t % 3), 0.0f }) {
                pos.push_back(v);
            }

            for (int k = 0; k < 3; k++) {
                idx.push_back(t * 3 + k);
            }

            // Recentre the third vertex so all centroids coincide.
            pos[(t * 3 + 2) * 3 + 1] = 0.0f;
            pos[(t * 3 + 2) * 3 + 2] = s;
        }

        for (const float v : { 100.0f, 0.0f, 0.0f, 101.0f, 0.0f, 0.0f, 100.0f, 1.0f, 0.0f, 102.0f, 0.0f, 0.0f, 103.0f, 0.0f, 0.0f, 102.0f, 1.0f, 0.0f }) {
            pos.push_back(v);
        }

        for (uint32_t k = 900; k < 906; k++) {
            idx.push_back(k);
        }

        BuiltBlas built;
        const std::string build = build_blas(pos, idx, 12, built);

        return build.empty() ? check_blas(built.bytes, pos, idx, 2000) : build;
    }

    std::string blas_stride16()
    {

        std::vector<float> pos;
        std::vector<uint32_t> idx;
        BuiltBlas built;

        random_tris(16, 1000, pos, idx);

        const std::string build = build_blas(pos, idx, 16, built);

        return build.empty() ? check_blas(built.bytes, pos, idx, 1000) : build;
    }

    std::string blas_rebuild_same_object()
    {

        std::vector<float> pos;
        std::vector<uint32_t> idx;
        BuiltBlas built;

        random_tris(21, 1000, pos, idx);

        const std::string first = build_blas(pos, idx, 12, built);

        if (!first.empty())
        {
            return first;
        }

        random_tris(22, 10, pos, idx);

        BuiltBlas again;
        const std::string second = build_blas(pos, idx, 12, again, built.as.get());

        return second.empty() ? check_blas(again.bytes, pos, idx, 500) : second;
    }

    std::vector<uint8_t> read_heap(uint64_t offset, uint64_t size)
    {

        std::unique_ptr<RenderBuffer> readback = gDevice->createBuffer(RenderBufferDesc::ReadbackBuffer(size));
        std::unique_ptr<RenderCommandList> list = gQueue->createCommandList();
        std::vector<uint8_t> bytes(size);

        list->begin();
        list->copyBufferRegion(RenderBufferReference(readback.get()), RenderBufferReference(static_cast<WebGPUDevice*>(gDevice.get())->asHeap.get(), offset), size);
        list->end();
        submit_and_wait(list.get());
        std::memcpy(bytes.data(), readback->map(), size);
        readback->unmap();
        return bytes;
    }

    // A TLAS over BLASes built by build_blas, built through the real path; its instance table is uploaded the way RT64 does.
    struct BuiltTlas {
        std::unique_ptr<RenderBuffer> asBuffer;
        std::unique_ptr<RenderAccelerationStructure> as;
    };

    std::string build_tlas(const RenderTopLevelASInstance* instances, uint32_t count, BuiltTlas& out)
    {

        RenderTopLevelASBuildInfo info;

        gDevice->setTopLevelASBuildInfo(info, instances, count, true, false);

        std::unique_ptr<RenderBuffer> table = upload(info.instancesBufferData.data(), info.instancesBufferData.size());
        std::unique_ptr<RenderBuffer> scratch = gDevice->createBuffer(RenderBufferDesc::DefaultBuffer(info.scratchSize, RenderBufferFlag::ACCELERATION_STRUCTURE_SCRATCH));
        std::unique_ptr<RenderCommandList> list = gQueue->createCommandList();

        out.asBuffer = as_buffer(info.accelerationStructureSize);
        out.as = gDevice->createAccelerationStructure(RenderAccelerationStructureDesc(RenderAccelerationStructureType::TOP_LEVEL, RenderBufferReference(out.asBuffer.get()), info.accelerationStructureSize));

        if (!out.as)
        {
            return "no TLAS";
        }

        list->begin();
        list->buildTopLevelAS(out.as.get(), RenderBufferReference(scratch.get()), RenderBufferReference(table.get()), info);
        list->end();
        submit_and_wait(list.get());

        const std::string error = static_cast<WebGPUDevice*>(gDevice.get())->takeErrors();

        return error.empty() ? "" : "device error: " + error;
    }

    std::string tlas_two_instances()
    {

        const std::vector<float> pos = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
        const std::vector<uint32_t> idx = { 0, 1, 2 };
        BuiltBlas a;
        BuiltBlas b;
        std::string error = build_blas(pos, idx, 12, a);

        if (error.empty()) {
            error = build_blas(pos, idx, 12, b);
        }

        if (!error.empty())
        {
            return error;
        }

        RenderAffineTransform moved;

        moved.m[0][3] = 10.0f;

        const RenderTopLevelASInstance instances[2] = {
            RenderTopLevelASInstance(RenderBufferReference(a.asBuffer.get()), 4, 0x01, 0, false, RenderAffineTransform()),
            RenderTopLevelASInstance(RenderBufferReference(b.asBuffer.get()), 9, 0x02, 0, true, moved)
        };

        BuiltTlas tlas;
        error = build_tlas(instances, 2, tlas);

        if (!error.empty())
        {
            return error;
        }

        const auto* web = static_cast<const WebGPUAccelerationStructure*>(tlas.as.get());
        const std::vector<uint8_t> bytes = read_heap(web->offset, was::tlasSize(2));
        was::TlasHeader header;
        was::Instance inst[2];

        std::memcpy(&header, bytes.data(), sizeof(header));
        std::memcpy(inst, bytes.data() + sizeof(header), sizeof(inst));

        if (header.instanceCount != 2)
        {
            return "instance count " + std::to_string(header.instanceCount);
        }

        const uint64_t blasA = static_cast<const WebGPUAccelerationStructure*>(a.as.get())->offset;
        const uint64_t blasB = static_cast<const WebGPUAccelerationStructure*>(b.as.get())->offset;

        if ((web->offset + inst[0].blasOffset != blasA) || (web->offset + inst[1].blasOffset != blasB))
        {
            return "BLAS offsets are not relative to the TLAS";
        }

        if ((inst[1].worldToObject[0][3] != -10.0f) || (inst[1].mask != 0x02) || (inst[1].instanceID != 9) || (inst[0].instanceID != 4))
        {
            return "wrong instance record";
        }

        was::BlasHeader blasHeader;
        const std::vector<uint8_t> blasBytes = read_heap(blasB, sizeof(was::BlasHeader));

        std::memcpy(&blasHeader, blasBytes.data(), sizeof(blasHeader));
        return (blasHeader.triCount == 1) ? "" : "the second instance's BLAS reads triCount " + std::to_string(blasHeader.triCount);
    }

    // One instance of a CPU-side scene: the mesh, its placement and the TLAS fields that change what a ray sees.
    struct SceneInstance {
        std::vector<float> pos;
        std::vector<uint32_t> idx;
        float objectToWorld[3][4];
        uint32_t mask;
        uint32_t id;
        bool cullDisable;
        bool opaque;
    };

    struct Ray {
        float org[3];
        float tmin;
        float dir[3];
        float tmax;
        uint32_t flags;
        uint32_t mask;
        uint32_t mode;
        uint32_t count;
    };

    struct GpuHit {
        uint32_t status;
        float t;
        uint32_t prim;
        uint32_t id;
        float u;
        float v;
        uint32_t front;
        float objectToWorldX;
    };

    static_assert(sizeof(Ray) == 48 && sizeof(GpuHit) == 32, "test record layout");

    void identity(float m[3][4])
    {

        std::memset(m, 0, sizeof(float) * 12);
        m[0][0] = 1.0f;
        m[1][1] = 1.0f;
        m[2][2] = 1.0f;
    }

    // Brute force over every instance the mask lets through, the ray taken into each object space. Returns the instance id through id.
    bool cpu_trace(const std::vector<SceneInstance>& scene, const Ray& ray, was::RefHit* hit, uint32_t* id)
    {

        bool found = false;
        float best = ray.tmax;

        for (const SceneInstance& inst : scene) {
            if ((inst.mask & ray.mask) == 0) continue;

            // Non-opaque geometry is skipped unless the ray forces opaque (RS64SoftRayQuery has no candidates).
            if (!inst.opaque && ((ray.flags & 0x01) == 0)) continue;

            float inv[3][4];
            was::affineInverse(inst.objectToWorld, inv);
            float o[3];
            float d[3];

            for (int r = 0; r < 3; r++) {
                o[r] = inv[r][0] * ray.org[0] + inv[r][1] * ray.org[1] + inv[r][2] * ray.org[2] + inv[r][3];
                d[r] = inv[r][0] * ray.dir[0] + inv[r][1] * ray.dir[1] + inv[r][2] * ray.dir[2];
            }

            const bool cull = ((ray.flags & 0x10) != 0) && !inst.cullDisable;

            for (size_t t = 0; t + 2 < inst.idx.size(); t += 3) {
                const uint32_t prim = uint32_t(t / 3);


                was::RefHit h;

                if (was::intersectTri(&inst.pos[inst.idx[t] * 3], &inst.pos[inst.idx[t + 1] * 3], &inst.pos[inst.idx[t + 2] * 3], o, d, ray.tmin, best, cull, &h)) {
                    h.prim = prim;
                    best = h.t;
                    *hit = h;
                    *id = inst.id;
                    found = true;
                }
            }
        }

        return found;
    }

    // Builds the scene on the GPU, traces the rays with RS64SoftRayQueryTestCS and reads the hits back.
    std::string gpu_trace(const std::vector<SceneInstance>& scene, std::vector<Ray> rays, std::vector<GpuHit>& hits)
    {

        std::vector<BuiltBlas> blases(scene.size());
        std::vector<RenderTopLevelASInstance> instances;

        for (size_t i = 0; i < scene.size(); i++) {
            const std::string error = build_blas(scene[i].pos, scene[i].idx, 12, blases[i], nullptr, scene[i].opaque);

            if (!error.empty())
            {
                return error;
            }

            RenderAffineTransform xf;
            std::memcpy(xf.m, scene[i].objectToWorld, sizeof(xf.m));
            instances.emplace_back(RenderBufferReference(blases[i].asBuffer.get()), scene[i].id, scene[i].mask, 0, scene[i].cullDisable, xf);
        }

        BuiltTlas tlas;
        const std::string tlasError = build_tlas(instances.data(), uint32_t(instances.size()), tlas);

        if (!tlasError.empty())
        {
            return tlasError;
        }

        rays[0].count = uint32_t(rays.size());

        RenderDescriptorSetBuilder setBuilder;

        setBuilder.begin();
        setBuilder.addAccelerationStructure(0);
        setBuilder.addByteAddressBuffer(1);
        setBuilder.addReadWriteByteAddressBuffer(2);
        setBuilder.end();

        RenderPipelineLayoutBuilder layoutBuilder;

        layoutBuilder.begin();
        layoutBuilder.addDescriptorSet(setBuilder);
        layoutBuilder.end();

        std::unique_ptr<RenderPipelineLayout> layout = layoutBuilder.create(gDevice.get());
        std::unique_ptr<RenderShader> shader = gDevice->createShader(RS64SoftRayQueryTestCSBlobSPIRV, RS64SoftRayQueryTestCSBlobSPIRV_size, "CSMain", RenderShaderFormat::SPIRV);
        std::unique_ptr<RenderPipeline> pipeline = gDevice->createComputePipeline(RenderComputePipelineDesc(layout.get(), shader.get(), 64, 1, 1));
        const auto* webPipeline = static_cast<const WebGPUPipeline*>(pipeline.get());

        if (!webPipeline->valid)
        {
            return "pipeline invalid: " + webPipeline->error;
        }

        const uint64_t outBytes = rays.size() * sizeof(GpuHit);
        std::unique_ptr<RenderBuffer> rayBuffer = upload(rays.data(), rays.size() * sizeof(Ray));
        std::unique_ptr<RenderBuffer> out = gDevice->createBuffer(RenderBufferDesc::DefaultBuffer(outBytes, RenderBufferFlag::STORAGE | RenderBufferFlag::UNORDERED_ACCESS));
        std::unique_ptr<RenderBuffer> readback = gDevice->createBuffer(RenderBufferDesc::ReadbackBuffer(outBytes));
        std::unique_ptr<RenderDescriptorSet> set = setBuilder.create(gDevice.get());
        std::unique_ptr<RenderCommandList> list = gQueue->createCommandList();

        set->setAccelerationStructure(0, tlas.as.get());
        set->setBuffer(1, rayBuffer.get());
        set->setBuffer(2, out.get());
        list->begin();
        list->setComputePipelineLayout(layout.get());
        list->setPipeline(pipeline.get());
        list->setComputeDescriptorSet(set.get(), 0);
        list->dispatch(uint32_t((rays.size() + 63) / 64), 1, 1);
        list->copyBuffer(readback.get(), out.get());
        list->end();
        submit_and_wait(list.get());
        hits.resize(rays.size());
        std::memcpy(hits.data(), readback->map(), outBytes);
        readback->unmap();

        const std::string error = static_cast<WebGPUDevice*>(gDevice.get())->takeErrors();

        return error.empty() ? "" : "device error: " + error;
    }

    std::vector<Ray> random_rays(uint32_t seed, uint32_t count, uint32_t flags, uint32_t mask, uint32_t mode)
    {

        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> o(-60.0f, 90.0f);
        std::uniform_real_distribution<float> d(-1.0f, 1.0f);
        std::vector<Ray> rays(count);

        for (Ray& r : rays) {
            r = {};

            for (int a = 0; a < 3; a++) {
                r.org[a] = o(rng);
                r.dir[a] = d(rng);
            }

            r.tmin = 0.0f;
            r.tmax = 1e30f;
            r.flags = flags;
            r.mask = mask;
            r.mode = mode;
        }

        return rays;
    }

    // Closest hits must agree on hit, t, instance and primitive (a tie in t lets either primitive stand); exact=false only checks hit/miss.
    std::string compare(const std::vector<SceneInstance>& scene, const std::vector<Ray>& rays, bool exact)
    {

        std::vector<GpuHit> hits;
        const std::string error = gpu_trace(scene, rays, hits);

        if (!error.empty())
        {
            return error;
        }

        uint32_t hitCount = 0;

        for (size_t r = 0; r < rays.size(); r++) {
            was::RefHit want;
            uint32_t wantId = 0;
            const bool w = cpu_trace(scene, rays[r], &want, &wantId);
            const GpuHit& g = hits[r];
            const bool gotHit = (g.status == 1);

            if (w != gotHit)
            {
                return "ray " + std::to_string(r) + ": hit " + std::to_string(w) + " on the CPU, " + std::to_string(gotHit) + " on the GPU";
            }

            if (!w || !exact) continue;

            ++hitCount;

            if (std::fabs(want.t - g.t) > 1e-3f * std::max(1.0f, want.t))
            {
                return "ray " + std::to_string(r) + ": t " + std::to_string(want.t) + " vs " + std::to_string(g.t);
            }

            if (std::fabs(want.t - g.t) > 1e-6f * std::max(1.0f, want.t) && ((want.prim != g.prim) || (wantId != g.id)))
            {
                return "ray " + std::to_string(r) + ": primitive/instance " + std::to_string(want.prim) + "/" + std::to_string(wantId) + " vs " + std::to_string(g.prim) + "/" + std::to_string(g.id);
            }

            if ((want.prim == g.prim) && (wantId == g.id) && ((std::fabs(want.u - g.u) > 1e-3f) || (std::fabs(want.v - g.v) > 1e-3f) || (want.frontFace != (g.front != 0))))
            {
                return "ray " + std::to_string(r) + ": barycentrics or front face differ";
            }

            for (const SceneInstance& inst : scene) {
                if ((inst.id == g.id) && (std::fabs(inst.objectToWorld[0][3] - g.objectToWorldX) > 1e-3f))
                {
                    return "ray " + std::to_string(r) + ": object-to-world x " + std::to_string(g.objectToWorldX) + " for instance " + std::to_string(g.id);
                }
            }
        }

        return (exact && (hitCount == 0)) ? "no ray hit anything" : "";
    }

    std::vector<SceneInstance> two_instance_scene(uint32_t tris, bool cullDisableSecond)
    {

        std::vector<SceneInstance> scene(2);

        random_tris(31, tris, scene[0].pos, scene[0].idx);
        random_tris(32, tris, scene[1].pos, scene[1].idx);
        identity(scene[0].objectToWorld);
        identity(scene[1].objectToWorld);

        // The second instance is turned a quarter about y and moved +30 x.
        scene[1].objectToWorld[0][0] = 0.0f;
        scene[1].objectToWorld[0][2] = 1.0f;
        scene[1].objectToWorld[2][0] = -1.0f;
        scene[1].objectToWorld[2][2] = 0.0f;
        scene[1].objectToWorld[0][3] = 30.0f;
        scene[0].mask = 0x01;
        scene[1].mask = 0x02;
        scene[0].id = 4;
        scene[1].id = 9;
        scene[0].cullDisable = false;
        scene[1].cullDisable = cullDisableSecond;
        scene[0].opaque = true;
        scene[1].opaque = true;
        return scene;
    }

    std::string rq_closest_matches_bruteforce()
    {
        return compare(two_instance_scene(3000, false), random_rays(41, 4000, 0, 0xFF, 0), true);
    }

    std::string rq_mask_filters()
    {
        return compare(two_instance_scene(1500, false), random_rays(42, 2000, 0, 0x02, 0), true);
    }

    std::string rq_first_hit_ends()
    {
        return compare(two_instance_scene(1500, false), random_rays(43, 2000, 0x04, 0xFF, 0), false);
    }

    std::string rq_cull_back()
    {
        return compare(two_instance_scene(1500, true), random_rays(44, 3000, 0x10, 0xFF, 0), true);
    }

    // Non-opaque geometry is invisible unless the ray forces opaque.
    std::string rq_non_opaque_skipped()
    {

        std::vector<SceneInstance> scene = two_instance_scene(1500, false);

        scene[0].opaque = false;

        const std::string skipped = compare(scene, random_rays(45, 3000, 0, 0xFF, 0), true);

        return skipped.empty() ? compare(scene, random_rays(48, 3000, 0x01, 0xFF, 0), true) : skipped;
    }

    // A flat 40x40 grid at y=0 under rays along the axes: none may slip through the box tests.
    std::string rq_axis_aligned_rays()
    {

        std::vector<SceneInstance> scene(1);
        SceneInstance& grid = scene[0];

        for (int z = 0; z < 40; z++) {
            for (int x = 0; x < 40; x++) {
                const uint32_t base = uint32_t(grid.pos.size() / 3);

                for (const float v : { float(x), 0.0f, float(z), float(x + 1), 0.0f, float(z), float(x), 0.0f, float(z + 1), float(x + 1), 0.0f, float(z + 1) }) {
                    grid.pos.push_back(v);
                }

                for (const uint32_t k : { 0u, 2u, 1u, 1u, 2u, 3u }) {
                    grid.idx.push_back(base + k);
                }
            }
        }

        identity(grid.objectToWorld);
        grid.mask = 0x01;
        grid.id = 1;
        grid.cullDisable = true;
        grid.opaque = true;

        std::mt19937 rng(46);
        std::uniform_real_distribution<float> p(0.5f, 39.5f);
        std::vector<Ray> rays(1000);

        for (size_t r = 0; r < rays.size(); r++) {
            rays[r] = {};
            rays[r].org[0] = p(rng);
            rays[r].org[1] = ((r % 2) == 0) ? 10.0f : -10.0f;
            rays[r].org[2] = p(rng);
            rays[r].dir[1] = ((r % 2) == 0) ? -1.0f : 1.0f;
            rays[r].tmax = 1e30f;
            rays[r].mask = 0xFF;
        }

        return compare(scene, rays, true);
    }

    // 200,000 triangles in a long strip: a deep, skewed tree. Few rays, since the CPU check is brute force.
    std::string rq_deep_tree()
    {

        std::vector<SceneInstance> scene(1);
        SceneInstance& strip = scene[0];

        for (uint32_t t = 0; t < 200000; t++) {
            const float x = float(t) * 0.01f;

            for (const float v : { x, 0.0f, 0.0f, x + 0.01f, 0.0f, 0.0f, x, 1.0f, 0.0f }) {
                strip.pos.push_back(v);
            }

            for (uint32_t k = 0; k < 3; k++) {
                strip.idx.push_back(t * 3 + k);
            }
        }

        identity(strip.objectToWorld);
        strip.mask = 0x01;
        strip.id = 2;
        strip.cullDisable = true;
        strip.opaque = true;

        std::mt19937 rng(47);
        std::uniform_real_distribution<float> x(0.0f, 2000.0f);
        std::uniform_real_distribution<float> y(0.05f, 0.5f);
        std::vector<Ray> rays(64);

        for (Ray& r : rays) {
            r = {};
            r.org[0] = x(rng);
            r.org[1] = y(rng);
            r.org[2] = -5.0f;
            r.dir[2] = 1.0f;
            r.tmax = 1e30f;
            r.mask = 0xFF;
        }

        return compare(scene, rays, true);
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
        { "swap_chain_clear", swap_chain_clear },
        { "as_create_and_bind", as_create_and_bind },
        { "as_heap_full_fails_cleanly", as_heap_full_fails_cleanly },
        { "as_build_info", as_build_info },
        { "blas_counts", blas_counts },
        { "blas_trace_matches_bruteforce", blas_trace_matches_bruteforce },
        { "blas_degenerate_codes", blas_degenerate_codes },
        { "blas_stride16", blas_stride16 },
        { "blas_rebuild_same_object", blas_rebuild_same_object },
        { "tlas_two_instances", tlas_two_instances },
        { "rq_closest_matches_bruteforce", rq_closest_matches_bruteforce },
        { "rq_mask_filters", rq_mask_filters },
        { "rq_first_hit_ends", rq_first_hit_ends },
        { "rq_cull_back", rq_cull_back },
        { "rq_non_opaque_skipped", rq_non_opaque_skipped },
        { "rq_axis_aligned_rays", rq_axis_aligned_rays },
        { "rq_deep_tree", rq_deep_tree }
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
