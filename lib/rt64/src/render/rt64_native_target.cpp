//
// RT64
//

#include <cstring>

#include "rt64_native_target.h"

#include "gbi/rt64_f3d.h"
#include "shared/rt64_fb_common.h"

#include "rt64_render_target.h"
#include "rt64_render_worker.h"

namespace RT64 {
    // NativeTarget

    NativeTarget::NativeTarget() { }

    NativeTarget::~NativeTarget() { }
    
    void NativeTarget::resetBufferHistory() {
        if (readBufferHistoryCount > 1) {
            std::swap(readBufferHistory.front(), readBufferHistory.back());
            readBufferHistoryCount = 1;
        }

        writeBufferHistoryCount = 0;
        writeBufferHistoryIndex = 0;
    }

    void NativeTarget::createReadBuffer(RenderWorker *worker, ReadBuffer &readBuffer, uint32_t bufferSize) {
        readBuffer.readDescSet.reset();
        readBuffer.nativeBufferView.reset();
        readBuffer.nativeBufferNextView.reset();
        readBuffer.nativeBufferWriteView.reset();
        readBuffer.nativeUploadBuffer.reset();
        readBuffer.nativeBuffer = worker->device->createBuffer(RenderBufferDesc::DefaultBuffer(bufferSize, RenderBufferFlag::STORAGE | RenderBufferFlag::UNORDERED_ACCESS | RenderBufferFlag::FORMATTED));
        readBuffer.nativeBufferSize = bufferSize;
    }

    RenderFormat NativeTarget::getBufferFormat(uint8_t siz) const {
        switch (siz) {
        case G_IM_SIZ_32b:
            return RenderFormat::R32_UINT;
        case G_IM_SIZ_16b:
            return RenderFormat::R16_UINT;
        case G_IM_SIZ_8b:
            return RenderFormat::R8_UINT;
        case G_IM_SIZ_4b:
            return RenderFormat::R8_UINT;
        default:
            assert(false && "Unknown buffer format.");
            return RenderFormat::UNKNOWN;
        }
    }

    uint32_t NativeTarget::getNativeSize(uint32_t width, uint32_t height, uint8_t siz) {
        const uint32_t rowSize = width << siz >> 1;
        return rowSize * height;
    }

    uint32_t NativeTarget::copyFromRAM(RenderWorker *worker, FramebufferChange &emptyFbChange, uint32_t width, uint32_t height, uint32_t rowStart, uint8_t siz, uint8_t fmt, const uint8_t *data, bool invalidateTargets, const ShaderLibrary *shaderLibrary) {
        assert(worker != nullptr);

        // Create the buffers for change count readback if they've not been created yet.
        if ((changeCountBuffer == nullptr) || (changeReadbackBuffer == nullptr)) {
            changeCountBuffer = worker->device->createBuffer(RenderBufferDesc::DefaultBuffer(sizeof(uint32_t), RenderBufferFlag::STORAGE | RenderBufferFlag::UNORDERED_ACCESS));
            changeReadbackBuffer = worker->device->createBuffer(RenderBufferDesc::ReadbackBuffer(sizeof(uint32_t)));
            clearDescSet = std::make_unique<FramebufferClearChangesDescriptorSet>(worker->device);
            clearDescSet->setBuffer(clearDescSet->gOutputCount, changeCountBuffer.get(), RenderBufferStructuredView(sizeof(uint32_t)));
        }

        // Determine the correct type of texture to use.
        RenderTexture *colorOrDepthRes = nullptr;
        RenderTexture *booleanRes = emptyFbChange.booleanTexture.get();
        switch (emptyFbChange.type) {
        case FramebufferChange::Type::Color:
            colorOrDepthRes = emptyFbChange.pixelTexture.get();
            break;
        case FramebufferChange::Type::Depth:
            colorOrDepthRes = emptyFbChange.pixelTexture.get();
            break;
        default:
            assert(false && "Invalid framebuffer change type.");
            break;
        }

        // Copy to the native upload resource.
        const bool hasCurrentResource = !invalidateTargets && (readBufferHistoryCount > 0);
        const uint32_t bufferSize = getNativeSize(width, height, siz);
        while (readBufferHistoryCount >= readBufferHistory.size()) {
            readBufferHistory.emplace_back();
        }

        ReadBuffer &readBuffer = readBufferHistory[readBufferHistoryCount];
        ReadBuffer *previousReadBuffer = hasCurrentResource ? &readBufferHistory[readBufferHistoryCount - 1] : nullptr;
        readBufferHistoryCount++;

        if (readBuffer.nativeBufferSize < bufferSize) {
            createReadBuffer(worker, readBuffer, bufferSize);
        }

        if (readBuffer.nativeUploadBuffer == nullptr) {
            readBuffer.nativeUploadBuffer = worker->device->createBuffer(RenderBufferDesc::UploadBuffer(readBuffer.nativeBufferSize));
        }

        if ((readBuffer.nativeBufferView == nullptr) || (readBuffer.nativeBufferViewFormat != siz)) {
            readBuffer.nativeBufferView = readBuffer.nativeBuffer->createBufferFormattedView(getBufferFormat(siz));
            readBuffer.nativeBufferViewFormat = siz;
        }

        if (readBuffer.readDescSet == nullptr) {
            readBuffer.readDescSet = std::make_unique<FramebufferReadChangesDescriptorBufferSet>(worker->device);
        }

        readBuffer.readDescSet->setBuffer(readBuffer.readDescSet->gNewInput, readBuffer.nativeBuffer.get(), readBuffer.nativeBufferSize, readBuffer.nativeBufferView.get());
        readBuffer.readDescSet->setBuffer(readBuffer.readDescSet->gOutputCount, changeCountBuffer.get(), RenderBufferStructuredView(sizeof(uint32_t)));

        if (previousReadBuffer != nullptr) {
            const RenderBufferFormattedView *previousBufferView = nullptr;
            if ((previousReadBuffer->nativeBufferView != nullptr) && (previousReadBuffer->nativeBufferViewFormat == siz)) {
                previousBufferView = previousReadBuffer->nativeBufferView.get();
            }
            else if ((previousReadBuffer->nativeBufferNextView != nullptr) && (previousReadBuffer->nativeBufferNextViewFormat == siz)) {
                previousBufferView = previousReadBuffer->nativeBufferNextView.get();
            }
            else {
                previousReadBuffer->nativeBufferNextView = previousReadBuffer->nativeBuffer->createBufferFormattedView(getBufferFormat(siz));
                previousReadBuffer->nativeBufferNextViewFormat = siz;
                previousBufferView = previousReadBuffer->nativeBufferNextView.get();
            }

            readBuffer.readDescSet->setBuffer(readBuffer.readDescSet->gCurInput, previousReadBuffer->nativeBuffer.get(), previousReadBuffer->nativeBufferSize, previousBufferView);
        }

        void *dstData = readBuffer.nativeUploadBuffer->map();
        memcpy(dstData, data, bufferSize);
        readBuffer.nativeUploadBuffer->unmap();

        if (hasCurrentResource) {
            // Clear the change count resource with a compute shader.
            worker->commandList->barriers(RenderBarrierStage::COMPUTE, RenderBufferBarrier(changeCountBuffer.get(), RenderBufferAccess::WRITE));
            worker->commandList->setPipeline(shaderLibrary->fbChangesClear.pipeline.get());
            worker->commandList->setComputePipelineLayout(shaderLibrary->fbChangesClear.pipelineLayout.get());
            worker->commandList->setComputeDescriptorSet(clearDescSet->get(), 0);
            worker->commandList->dispatch(1, 1, 1);
            worker->commandList->barriers(RenderBarrierStage::COMPUTE, RenderBufferBarrier(changeCountBuffer.get(), RenderBufferAccess::WRITE));
        }
        
        // Copy the native upload resource to the dedicated resource.
        worker->commandList->barriers(RenderBarrierStage::COPY, RenderBufferBarrier(readBuffer.nativeBuffer.get(), RenderBufferAccess::WRITE));
        worker->commandList->copyBufferRegion(readBuffer.nativeBuffer.get(), readBuffer.nativeUploadBuffer.get(), bufferSize);
        worker->commandList->barriers(RenderBarrierStage::COMPUTE, RenderBufferBarrier(readBuffer.nativeBuffer.get(), RenderBufferAccess::READ));

        // Setup the native constants.
        interop::FbCommonCB nativeCB;
        nativeCB.offset = { 0, 0 };
        nativeCB.resolution = { width, height };
        nativeCB.fmt = fmt;
        nativeCB.siz = siz;
        nativeCB.ditherPattern = 0;
        nativeCB.ditherRandomSeed = 0;
        nativeCB.usesHDR = shaderLibrary->usesHDR;

        // Originally these were assert()s. For Rogue Squadron (Factor5) we hit
        // 4-bit readback during the explosion/X-wing intro scene, which kills
        // the game. Skip the readback for unsupported formats — visual artifacts
        // (missing reflections/effects) are preferable to a hard crash.
        bool unsupported_readback =
            (nativeCB.siz == G_IM_SIZ_4b) ||
            ((nativeCB.fmt == G_IM_FMT_RGBA) && (nativeCB.siz == G_IM_SIZ_8b)) ||
            ((nativeCB.fmt == G_IM_FMT_IA) && (nativeCB.siz == G_IM_SIZ_8b)) ||
            ((nativeCB.fmt == G_IM_FMT_CI) && (nativeCB.siz == G_IM_SIZ_16b)) ||
            ((nativeCB.fmt == G_IM_FMT_IA) && (nativeCB.siz == G_IM_SIZ_16b)) ||
            ((nativeCB.fmt == G_IM_FMT_I) && (nativeCB.siz == G_IM_SIZ_16b)) ||
            ((nativeCB.fmt == G_IM_FMT_DEPTH) && (nativeCB.siz != G_IM_SIZ_16b)) ||
            ((nativeCB.fmt == G_IM_FMT_CI) && (nativeCB.siz == G_IM_SIZ_32b));
        if (unsupported_readback) {
            static int n = 0;
            if (++n <= 5 || (n % 200) == 0) {
                fprintf(stderr, "[native_target] skip unsupported readback fmt=%d siz=%d (n=%d)\n",
                    (int)nativeCB.fmt, (int)nativeCB.siz, n);
                fflush(stderr);
            }
            return 0;
        }
        assert(((nativeCB.fmt != G_IM_FMT_IA) || (nativeCB.siz != G_IM_SIZ_32b)) && "Unimplemented IA32 Readback mode.");
        assert(((nativeCB.fmt != G_IM_FMT_I) || (nativeCB.siz != G_IM_SIZ_32b)) && "Unimplemented I32 Readback mode.");

        // Run the compute shader that generates the texture with the differences.
        const uint32_t BlockSize = FB_COMMON_WORKGROUP_SIZE;
        uint32_t dispatchX = (width + BlockSize - 1) / BlockSize;
        uint32_t dispatchY = (height + BlockSize - 1) / BlockSize;
        RenderBufferBarrier counterBarrier = RenderBufferBarrier(changeCountBuffer.get(), RenderBufferAccess::WRITE);
        RenderTextureBarrier beforeBarriers[] = {
            RenderTextureBarrier(emptyFbChange.pixelTexture.get(), RenderTextureLayout::GENERAL),
            RenderTextureBarrier(emptyFbChange.booleanTexture.get(), RenderTextureLayout::GENERAL)
        };

        RenderTextureBarrier afterBarriers[] = {
            RenderTextureBarrier(emptyFbChange.pixelTexture.get(), RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(emptyFbChange.booleanTexture.get(), RenderTextureLayout::SHADER_READ)
        };

        const ShaderRecord &shaderReadRecord = hasCurrentResource ? shaderLibrary->fbReadAnyChanges : shaderLibrary->fbReadAnyFull;
        worker->commandList->barriers(RenderBarrierStage::COMPUTE, &counterBarrier, 1, beforeBarriers, uint32_t(std::size(beforeBarriers)));
        worker->commandList->setPipeline(shaderReadRecord.pipeline.get());
        worker->commandList->setComputePipelineLayout(shaderReadRecord.pipelineLayout.get());
        worker->commandList->setComputeDescriptorSet(readBuffer.readDescSet->get(), 0);
        worker->commandList->setComputeDescriptorSet(emptyFbChange.readChangesSet->get(), 1);
        worker->commandList->setComputePushConstants(0, &nativeCB);
        worker->commandList->dispatch(dispatchX, dispatchY, 1);
        worker->commandList->barriers(RenderBarrierStage::ALL, afterBarriers, uint32_t(std::size(afterBarriers)));

        if (hasCurrentResource) {
            // Copy the modified count resource to the readback.
            RenderBufferBarrier beforeCopyBarriers[] = {
                RenderBufferBarrier(changeCountBuffer.get(), RenderBufferAccess::READ),
                RenderBufferBarrier(changeReadbackBuffer.get(), RenderBufferAccess::WRITE)
            };

            worker->commandList->barriers(RenderBarrierStage::COPY, beforeCopyBarriers, uint32_t(std::size(beforeCopyBarriers)));
            worker->commandList->copyBuffer(changeReadbackBuffer.get(), changeCountBuffer.get());
        }
        
        // Read the modified count by executing and waiting.
        uint32_t modifiedCount = 0;
        if (hasCurrentResource) {
            worker->commandList->end();
            worker->execute();
            worker->wait();
            worker->commandList->begin();
            RenderRange readRange(0, sizeof(uint32_t));
            uint32_t *readbackData = reinterpret_cast<uint32_t *>(changeReadbackBuffer->map(0, &readRange));
            modifiedCount = *readbackData;
            changeReadbackBuffer->unmap();
        }
        // Consider all pixels as modified.
        else {
            modifiedCount = width * height;
        }

        return modifiedCount;
    }

    void NativeTarget::copyToNative(RenderWorker *worker, RenderTarget *srcTarget, uint32_t rowWidth, uint32_t rowStart, uint32_t rowEnd, uint8_t siz, uint8_t fmt, uint32_t ditherPattern, uint32_t ditherRandomSeed, const ShaderLibrary *shaderLibrary) {
        assert(worker != nullptr);

        srcTarget->resolveTarget(worker, shaderLibrary);

        if (srcTarget->fbWriteDescSet == nullptr) {
            srcTarget->fbWriteDescSet = std::make_unique<FramebufferWriteDescriptorTextureSet>(worker->device);
            srcTarget->fbWriteDescSet->setTexture(srcTarget->fbWriteDescSet->gInput, srcTarget->getResolvedTexture(), RenderTextureLayout::SHADER_READ, srcTarget->getResolvedTextureView());
        }
        
        interop::FbCommonCB nativeCB;
        nativeCB.offset = { 0, rowStart };
        nativeCB.resolution = { rowWidth, rowEnd - rowStart };
        nativeCB.fmt = fmt;
        nativeCB.siz = siz;
        nativeCB.ditherPattern = ditherPattern;
        nativeCB.ditherRandomSeed = ditherRandomSeed;
        nativeCB.usesHDR = shaderLibrary->usesHDR;

        // Skip combinations that have not been implemented yet because hardware verification is pending.
        bool unsupported_writeback =
            (nativeCB.siz == G_IM_SIZ_4b) ||
            ((nativeCB.fmt == G_IM_FMT_RGBA) && (nativeCB.siz == G_IM_SIZ_8b)) ||
            ((nativeCB.fmt == G_IM_FMT_IA)   && (nativeCB.siz == G_IM_SIZ_8b)) ||
            ((nativeCB.fmt == G_IM_FMT_CI)   && (nativeCB.siz == G_IM_SIZ_16b)) ||
            ((nativeCB.fmt == G_IM_FMT_IA)   && (nativeCB.siz == G_IM_SIZ_16b)) ||
            ((nativeCB.fmt == G_IM_FMT_I)    && (nativeCB.siz == G_IM_SIZ_16b)) ||
            ((nativeCB.fmt == G_IM_FMT_CI)   && (nativeCB.siz == G_IM_SIZ_32b)) ||
            ((nativeCB.fmt == G_IM_FMT_IA)   && (nativeCB.siz == G_IM_SIZ_32b)) ||
            ((nativeCB.fmt == G_IM_FMT_I)    && (nativeCB.siz == G_IM_SIZ_32b));
        if (unsupported_writeback) {
            static int n = 0; ++n;
            if (n <= 10 || (n % 50) == 0) {
                fprintf(stderr, "[native_target] skip unsupported writeback fmt=%u siz=%u (n=%d)\n",
                        (unsigned)nativeCB.fmt, (unsigned)nativeCB.siz, n);
                fflush(stderr);
            }
            return;
        }
        
        // Select shader based on the format and pixel size.
        const ShaderRecord *writeShader;
        if (fmt == G_IM_FMT_DEPTH) {
            const bool usesMSAA = (srcTarget->multisampling.sampleCount > 1);
            writeShader = usesMSAA ? &shaderLibrary->fbWriteDepthMS : &shaderLibrary->fbWriteDepth;
        }
        else {
            writeShader = &shaderLibrary->fbWriteColor;
        }
        
        assert(((fmt != G_IM_FMT_DEPTH) || (siz == G_IM_SIZ_16b)) && "Depth format is not allowed outside of 16-bits.");
        
        // We need at least one read buffer in the history to use to write the output to.
        if (readBufferHistory.empty()) {
            readBufferHistory.emplace_back();
            readBufferHistoryCount = 1;
        }

        while (writeBufferHistoryCount >= writeBufferHistory.size()) {
            writeBufferHistory.emplace_back();
        }

        ReadBuffer *smallerReadBuffer = nullptr;
        ReadBuffer *readBuffer = &readBufferHistory[readBufferHistoryCount - 1];
        WriteBuffer &writeBuffer = writeBufferHistory[writeBufferHistoryCount];
        writeBufferHistoryCount++;
        
        const uint32_t bufferSize = getNativeSize(rowWidth, rowEnd, siz);
        if (readBuffer->nativeBufferSize < bufferSize) {
            // If a buffer already existed we don't destroy it, we just copy it to a bigger one instead.
            if (readBuffer->nativeBuffer != nullptr) {
                if (readBufferHistoryCount == readBufferHistory.size()) {
                    readBufferHistory.emplace_back();
                }

                smallerReadBuffer = &readBufferHistory[readBufferHistoryCount - 1];
                readBuffer = &readBufferHistory[readBufferHistoryCount];
                readBufferHistoryCount++;
            }

            createReadBuffer(worker, *readBuffer, bufferSize);
        }

        if ((readBuffer->nativeBufferWriteView == nullptr) || (readBuffer->nativeBufferWriteViewFormat != siz)) {
            readBuffer->nativeBufferWriteView = readBuffer->nativeBuffer->createBufferFormattedView(getBufferFormat(siz));
            readBuffer->nativeBufferWriteViewFormat = siz;
        }

        if (writeBuffer.nativeReadbackBufferSize < bufferSize) {
            writeBuffer.nativeReadbackBuffer = worker->device->createBuffer(RenderBufferDesc::ReadbackBuffer(bufferSize));
            writeBuffer.nativeReadbackBufferSize = bufferSize;
        }

        if (writeBuffer.writeDescSet == nullptr) {
            writeBuffer.writeDescSet = std::make_unique<FramebufferWriteDescriptorBufferSet>(worker->device);
        }

        writeBuffer.writeDescSet->setBuffer(writeBuffer.writeDescSet->gOutput, readBuffer->nativeBuffer.get(), bufferSize, readBuffer->nativeBufferWriteView.get());

        if (smallerReadBuffer != nullptr) {
            RenderBufferBarrier copyBarriers[] = {
                RenderBufferBarrier(smallerReadBuffer->nativeBuffer.get(), RenderBufferAccess::READ),
                RenderBufferBarrier(readBuffer->nativeBuffer.get(), RenderBufferAccess::WRITE)
            };

            worker->commandList->barriers(RenderBarrierStage::COPY, copyBarriers, uint32_t(std::size(copyBarriers)));
            worker->commandList->copyBufferRegion(readBuffer->nativeBuffer.get(), smallerReadBuffer->nativeBuffer.get(), smallerReadBuffer->nativeBufferSize);
        }

        worker->commandList->barriers(RenderBarrierStage::COMPUTE,
            RenderBufferBarrier(readBuffer->nativeBuffer.get(), RenderBufferAccess::WRITE),
            RenderTextureBarrier(srcTarget->getResolvedTexture(), RenderTextureLayout::SHADER_READ)
        );

        const int BlockSize = FB_COMMON_WORKGROUP_SIZE;
        uint32_t dispatchX = (nativeCB.resolution.x + BlockSize - 1) / BlockSize;
        uint32_t dispatchY = (nativeCB.resolution.y + BlockSize - 1) / BlockSize;
        worker->commandList->setPipeline(writeShader->pipeline.get());
        worker->commandList->setComputePipelineLayout(writeShader->pipelineLayout.get());
        worker->commandList->setComputePushConstants(0, &nativeCB);
        worker->commandList->setComputeDescriptorSet(writeBuffer.writeDescSet->get(), 0);
        worker->commandList->setComputeDescriptorSet(srcTarget->fbWriteDescSet->get(), 1);
        worker->commandList->dispatch(dispatchX, dispatchY, 1);

        RenderBufferBarrier copyBarriers[] = {
            RenderBufferBarrier(readBuffer->nativeBuffer.get(), RenderBufferAccess::READ),
            RenderBufferBarrier(writeBuffer.nativeReadbackBuffer.get(), RenderBufferAccess::WRITE)
        };

        worker->commandList->barriers(RenderBarrierStage::COPY, copyBarriers, uint32_t(std::size(copyBarriers)));

        const uint64_t regionBufferOffset = getNativeSize(rowWidth, rowStart, siz);
        const uint64_t regionBufferSize = getNativeSize(rowWidth, rowEnd - rowStart, siz);
        worker->commandList->copyBufferRegion(writeBuffer.nativeReadbackBuffer->at(regionBufferOffset), readBuffer->nativeBuffer->at(regionBufferOffset), regionBufferSize);
    }

    void NativeTarget::copyToRAM(uint32_t rowStart, uint32_t rowEnd, uint32_t width, uint8_t siz, uint8_t *data) {
        // Was assert(writeBufferHistoryCount > 0). Fires when copyToRAM is
        // called without a preceding copyFromTarget — happens during the post-
        // N64-logo path where Factor5's framebuffer dance doesn't always queue
        // a writeback. Skip-and-log instead of aborting.
        if (writeBufferHistoryCount == 0) {
            static int n = 0;
            if (++n <= 10 || (n % 500) == 0) {
                fprintf(stderr, "[trace] copyToRAM skip #%d (no writeBuffer history) row=%u..%u w=%u siz=%u\n",
                    n, rowStart, rowEnd, width, (unsigned)siz);
                fflush(stderr);
            }
            return;
        }

        const WriteBuffer &writeBuffer = writeBufferHistory[writeBufferHistoryIndex];
        const uint32_t bufferOffset = getNativeSize(width, rowStart, siz);
        const uint32_t bufferSize = getNativeSize(width, rowEnd - rowStart, siz);
        RenderRange readRange = { bufferOffset, bufferOffset + bufferSize };
        uint8_t *readbackData = reinterpret_cast<uint8_t *>(writeBuffer.nativeReadbackBuffer->map(0, &readRange));
        memcpy(data, readbackData + bufferOffset, bufferSize);
        writeBuffer.nativeReadbackBuffer->unmap();
        writeBufferHistoryIndex++;
    }
};