//
// RT64
//

#include "rt64_interpreter.h"

#include <cassert>

extern "C" void mqdiag_dump(const char *path);

//#define DUMP_DISPLAY_LISTS

namespace RT64 {
    static FILE *displayListFp = nullptr;

    // One-shot opcode 0x02 capture. A ring buffer of the last N DL commands is
    // maintained here, and rt64_gbi_f3dfactor5.cpp's op02 handler dumps it
    // alongside full RDRAM to disk on the first 0x02 dispatch.
    struct DLHistEntry { uint32_t w0, w1, dlAddr; uint8_t opcode; };
    static constexpr size_t kDLHistLen = 65536;
    DLHistEntry g_dlHist[kDLHistLen];
    size_t g_dlHistCount = 0;  // total commands seen
    bool g_op02Captured = false;
    // Dump after the Nth processDisplayLists call returns (0-based).
    // Frame 0 is usually just state setup; frame 3 should include real draws.
    static constexpr int kFrameToDump = 20;
    int g_frameCounter = 0;

    // Interpreter

    Interpreter::Interpreter() {
        state = nullptr;
        hleGBI = nullptr;
        extendedFunction = gbiManager.getExtendedFunction();
    }

    void Interpreter::setup(State *state) {
        this->state = state;
    }

    void Interpreter::loadUCodeGBI(uint32_t textAddress, uint32_t dataAddress, bool resetFromTask) {
        if (!resetFromTask) {
            state->flush();
        }

        const uint32_t AddressMask = 0xFFFFF8;
        const uint32_t maskedTextAddress = textAddress & AddressMask;
        const uint32_t maskedDataAddress = dataAddress & AddressMask;
        if ((UCode.textAddress != maskedTextAddress) || (UCode.dataAddress != maskedDataAddress)) {
            hleGBI = gbiManager.getGBIForUCode(state->RDRAM, maskedTextAddress, maskedDataAddress);
            if (hleGBI != nullptr) {
                state->rsp->setGBI(hleGBI);
            }

            UCode.textAddress = maskedTextAddress;
            UCode.dataAddress = maskedDataAddress;
        }

        if (hleGBI != nullptr) {
            GBIReset resetFunction = resetFromTask ? hleGBI->resetFromTask : hleGBI->resetFromLoad;
            if (resetFunction != nullptr) {
                resetFunction(state);
            }
        }
    }

    void Interpreter::processRDPLists(uint32_t dlStartAdddress, DisplayList *dlStart, DisplayList *dlEnd) {
        state->dlCpuProfiler.start();

        // Update the state with the current display list address.
        state->displayListAddress = dlStartAdddress;
        state->displayListCounter++;

        // Check RDRAM if required.
        state->checkRDRAM();

        GBI *rdpGBI = state->rdp->gbi;
        constexpr unsigned int opCodeMask = 0x3F;

        // Run the command interpreter.
        assert(rdpGBI != nullptr);
        DisplayList *dl = dlStart;
        uint8_t opCode;
        GBIFunction func;
        uint32_t cmdLength;
        size_t pendingCommandRemainingBytes = state->rdp->pendingCommandRemainingBytes;

        if (dlStart >= dlEnd) {
            state->dlCpuProfiler.end();
            return;
        }

        if (pendingCommandRemainingBytes != 0) {
            // Copy the remaining command bytes from the current displaylist
            uint32_t toCopy = (uint32_t)std::min(pendingCommandRemainingBytes, (uintptr_t)dlEnd - (uintptr_t)dl);
            memcpy(state->rdp->pendingCommandBuffer.data() + state->rdp->pendingCommandCurrentBytes, dl, toCopy);

            // Modify start to skip the copied bytes
            dl = (DisplayList *)(toCopy + (uintptr_t)dl);

            // Check if we've copied all of the bytes of the command into the buffer
            if (pendingCommandRemainingBytes == toCopy) {
                // All bytes have been copied, so run the completed command
                DisplayList *pendingCommand = (DisplayList *)state->rdp->pendingCommandBuffer.data();
                opCode = (pendingCommand->w0 >> 24) & opCodeMask;
                func = rdpGBI->map[opCode];

                if (func != nullptr) {
                    func(state, &pendingCommand);
                }
                else {
                    RT64_LOG_PRINTF("DL Parser ran into an unknown RDP opCode: %u / 0x%X", opCode, opCode);
                }

                state->rdp->pendingCommandCurrentBytes = 0;
                state->rdp->pendingCommandRemainingBytes = 0;
            }
            // Not all of the bytes were copied, so adjust RDP state accordingly and exit.
            else {
                state->rdp->pendingCommandCurrentBytes += toCopy;
                state->rdp->pendingCommandRemainingBytes -= toCopy;
                state->dlCpuProfiler.end();
                return;
            }
        }

        // Create a dummy pointer and pass that, since displaylist pointer incrementing is handled differently in LLE.
        DisplayList *dummy;
        while ((dl != nullptr) && ((dlEnd == nullptr) || (dl < dlEnd))) {
            opCode = (dl->w0 >> 24) & opCodeMask;

            if ((extendedOpCode != 0) && (opCode == extendedOpCode)) {
                dummy = dl;
                extendedFunction(state, &dl);
                cmdLength = 1;
            }
            else {
                func = rdpGBI->map[opCode];
                cmdLength = state->rdp->commandWordLengths[opCode];

#       ifdef DUMP_DISPLAY_LISTS
                RT64_LOG_PRINTF("0x%08X 0x%08X", dl->w0, dl->w1);
#       endif

                // Check if this command is unfinished and store the partial contents if so.
                if (dl + cmdLength > dlEnd) {
                    uint32_t toCopy = (uint32_t)((uintptr_t)dlEnd - (uintptr_t)dl);
                    memcpy(state->rdp->pendingCommandBuffer.data(), dl, toCopy);
                    state->rdp->pendingCommandCurrentBytes = toCopy;
                    state->rdp->pendingCommandRemainingBytes = cmdLength * sizeof(DisplayList) - toCopy;
                    break;
                }

                if (func != nullptr) {
                    dummy = dl;
                    func(state, &dummy);
                }
                else {
                    RT64_LOG_PRINTF("DL Parser ran into an unknown RDP opCode: %u / 0x%X", opCode, opCode);
                }
            }

            if (dl != nullptr) {
                dl += cmdLength;
            }
        }

        state->dlCpuProfiler.end();
    }

    static void dumpFrameCaptureIfNeeded(State *state) {
        if (g_frameCounter != kFrameToDump) return;
        g_op02Captured = true;  // disable any older op02 trigger
        static bool dumped = false;
        if (dumped) return;
        dumped = true;

        constexpr size_t kRDRAMSize = 8 * 1024 * 1024;
        if (FILE *fp = fopen("rdram_frame.bin", "wb")) {
            fwrite(state->RDRAM, 1, kRDRAMSize, fp);
            fclose(fp);
        }

        if (FILE *fp = fopen("dlhist_frame.txt", "w")) {
            fprintf(fp, "# Full DL history through end of frame %d\n", kFrameToDump);
            fprintf(fp, "# total cmds captured: %zu (ring capacity %zu)\n",
                    g_dlHistCount, kDLHistLen);
            size_t start = (g_dlHistCount > kDLHistLen) ? (g_dlHistCount - kDLHistLen) : 0;
            for (size_t i = start; i < g_dlHistCount; i++) {
                const auto &e = g_dlHist[i % kDLHistLen];
                fprintf(fp, "%06zu  op=0x%02X  w0=0x%08X  w1=0x%08X  dlAddr=0x%08X\n",
                    i, e.opcode, e.w0, e.w1, e.dlAddr);
            }
            fclose(fp);
        }

        mqdiag_dump("mqdiag_frame.txt");

        fprintf(stderr, "[frame %d] captured RDRAM + %zu DL commands + mqdiag\n",
                kFrameToDump, g_dlHistCount);
        fflush(stderr);
    }

    void Interpreter::processDisplayLists(uint32_t dlStartAdddress, DisplayList *dlStart) {
        assert(hleGBI != nullptr);

        state->dlCpuProfiler.start();

        // Update the state with the current display list address.
        state->displayListAddress = dlStartAdddress;
        state->displayListCounter++;

        // Check RDRAM if required.
        state->checkRDRAM();

        // Run the command interpreter.
        DisplayList *dl = dlStart;
        uint8_t opCode;
        GBIFunction func;
        while (dl != nullptr) {
            opCode = (dl->w0 >> 24);

            {
                size_t slot = g_dlHistCount % kDLHistLen;
                g_dlHist[slot].w0 = dl->w0;
                g_dlHist[slot].w1 = dl->w1;
                g_dlHist[slot].dlAddr = dlStartAdddress + uint32_t((uintptr_t)dl - (uintptr_t)dlStart);
                g_dlHist[slot].opcode = opCode;
                g_dlHistCount++;
            }

            if ((extendedOpCode != 0) && (opCode == extendedOpCode)) {
                extendedFunction(state, &dl);
            }
            else {
                func = hleGBI->map[opCode];

#       ifdef DUMP_DISPLAY_LISTS
                RT64_LOG_PRINTF("0x%08X 0x%08X", dl->w0, dl->w1);
#       endif

                if (func != nullptr) {
                    func(state, &dl);
                }
                else {
                    RT64_LOG_PRINTF("DL Parser ran into an unknown opCode (GBI %u): %u / 0x%X", uint32_t(hleGBI->ucode), opCode, opCode);
                }
            }

            if (dl != nullptr) {
                dl++;
            }
        }

        state->dlCpuProfiler.end();

        // Mark frame boundary in DL history with a sentinel entry (opcode 0xFF is
        // SETCIMG in F3DEX, but having dlAddr=0 makes this recognizable as a marker).
        if (g_dlHistCount < kDLHistLen * 8) {
            size_t slot = g_dlHistCount % kDLHistLen;
            g_dlHist[slot] = { 0xDEADBEEF, uint32_t(g_frameCounter), 0, 0xFF };
            g_dlHistCount++;
        }
        g_frameCounter++;
        if (g_frameCounter <= 10 || g_frameCounter % 5 == 0) {
            fprintf(stderr, "[heartbeat] frame %d, %zu DL cmds\n", g_frameCounter, g_dlHistCount);
            fflush(stderr);
        }
        dumpFrameCaptureIfNeeded(state);
    }
};
