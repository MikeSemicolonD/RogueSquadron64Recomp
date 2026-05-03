//
// RT64
//

#include "rt64_interpreter.h"

#include <cassert>

#ifdef _MSC_VER
#include <crtdbg.h>
#include <stdlib.h>
#include <cwchar>
#endif

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
    static constexpr int kFrameToDump = 38;  // captures state just before frame-40 stall
    int g_frameCounter = 0;

    // Interpreter

    // Dump the last N entries of g_dlHist on a CRT assert/invalid-parameter event.
    // Triggered by std::vector/std::array bounds-checks that fire when the dl
    // pointer drifts into non-DL memory and RT64 indexes a draw-data array
    // with a corrupted index. See post_credits memory file Update 14.
    static void dump_dl_history_tail(const char *reason) {
        constexpr size_t kTailLen = 32;
        size_t start = (g_dlHistCount > kTailLen) ? (g_dlHistCount - kTailLen) : 0;
        fprintf(stderr, "[crash-dump] %s — last %zu DL cmds (of %zu total):\n",
            reason, g_dlHistCount - start, g_dlHistCount);
        for (size_t i = start; i < g_dlHistCount; i++) {
            const auto &e = g_dlHist[i % kDLHistLen];
            fprintf(stderr, "  %05zu  op=0x%02X  w0=0x%08X  w1=0x%08X  addr=0x%08X\n",
                i, e.opcode, e.w0, e.w1, e.dlAddr);
        }
        fflush(stderr);
    }

#ifdef _MSC_VER
    static void __cdecl rt64_invalid_param_handler(
        const wchar_t* /*expr*/, const wchar_t* /*func*/,
        const wchar_t* /*file*/, unsigned int /*line*/, uintptr_t /*reserved*/)
    {
        dump_dl_history_tail("invalid_parameter");
    }

    static int __cdecl rt64_crt_report_hook(int /*type*/, char *msg, int* /*ret*/) {
        // _CrtDbgReport hook fires for vector/array subscript checks before abort.
        if (msg != nullptr) {
            fprintf(stderr, "[crash-dump] CRT hook: %s\n", msg);
        }
        dump_dl_history_tail("crt-report");
        return 0;  // 0 = continue normal handling (assert dialog → abort)
    }
#endif

    Interpreter::Interpreter() {
        state = nullptr;
        hleGBI = nullptr;
        extendedFunction = gbiManager.getExtendedFunction();
#ifdef _MSC_VER
        // Install once. Idempotent — re-installing the same handler is harmless.
        static bool installed = false;
        if (!installed) {
            _set_invalid_parameter_handler(rt64_invalid_param_handler);
            _CrtSetReportHook(rt64_crt_report_hook);
            installed = true;
        }
#endif
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
        // If GBI lookup failed (e.g. game submitted a task with an unrecognized
        // ucode — Rogue Squadron does this for some post-credits scenes), skip
        // the task instead of asserting. The interpreter's main loop reads
        // hleGBI->map[opcode], which would AV at a small NULL+offset address.
        if (hleGBI == nullptr) {
            static int n = 0;
            if (++n <= 10 || (n % 200) == 0) {
                fprintf(stderr, "[trace] processDisplayLists SKIP #%d (no hleGBI) dlStart=0x%08X\n",
                    n, dlStartAdddress);
                fflush(stderr);
            }
            return;
        }

        {
            static int n = 0;
            ++n;
            fprintf(stderr, "[trace] processDisplayLists ENTER #%d dlStart=0x%08X\n",
                n, dlStartAdddress);
            fflush(stderr);

            // Dump first 30 commands of any DL we haven't seen before.
            static uint32_t seen[16] = {0};
            static int seen_n = 0;
            bool already = false;
            for (int i = 0; i < seen_n; i++) {
                if (seen[i] == dlStartAdddress) { already = true; break; }
            }
            if (!already && seen_n < 16) {
                seen[seen_n++] = dlStartAdddress;
                fprintf(stderr, "[trace] NEW DL @ 0x%08X first 30 cmds:\n", dlStartAdddress);
                DisplayList *p = dlStart;
                for (int i = 0; i < 30; i++) {
                    fprintf(stderr, "  %03d  op=0x%02X  w0=0x%08X  w1=0x%08X  addr=0x%08X\n",
                        i, (unsigned)(p->w0 >> 24), p->w0, p->w1,
                        dlStartAdddress + (uint32_t)(i * sizeof(DisplayList)));
                    p++;
                }
                fflush(stderr);
            }
        }

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
        size_t loopIters = 0;
        static uint64_t opFreq[256] = {0};
        static uint64_t opFreqDumpThreshold = 100000;
        static uint64_t totalIters = 0;
        static uint64_t totalDumpThreshold = 50000;
        while (dl != nullptr) {
            // Safety bound: any single task running > 5M commands almost certainly
            // means the interpreter walked off a chunk's expected terminator.
            // Without this guard, dl++ marches past the end of valid RDRAM and
            // eventually dereferences an unmapped page (AV). Force-exit instead.
            // This guards against not-yet-identified Factor5-specific terminator
            // opcodes (we know op_B5 is one, but post-credits scenes appear to
            // have additional ones we haven't mapped yet).
            if (loopIters > 5000000) {
                static int n = 0;
                if (++n <= 5) {
                    uint32_t curAddr = dlStartAdddress + uint32_t((uintptr_t)dl - (uintptr_t)dlStart);
                    fprintf(stderr, "[trace] DL safety-exit #%d (loopIters=%zu, dlStart=0x%08X, cur=0x%08X)\n",
                        n, loopIters, dlStartAdddress, curAddr);
                    fflush(stderr);
                }
                break;
            }

            // RDRAM bounds: if dl walks past 8 MB (the expansion-pak limit), the
            // interpreter has marched off the end of any valid Factor5 chunk and
            // is reading zero/garbage. Without this check, the loop chews through
            // up to 5M iterations of op_00 before the safety-exit fires, eating
            // the entire frame budget. Bail immediately on first out-of-range dl.
            {
                uint32_t curOff = dlStartAdddress + uint32_t((uintptr_t)dl - (uintptr_t)dlStart);
                if (curOff >= 0x00800000) {
                    static int n = 0;
                    if (++n <= 10) {
                        fprintf(stderr, "[trace] DL rdram-exit #%d (loopIters=%zu, dlStart=0x%08X, cur=0x%08X)\n",
                            n, loopIters, dlStartAdddress, curOff);
                        fflush(stderr);
                    }
                    break;
                }
            }
            opCode = (dl->w0 >> 24);
            loopIters++;
            opFreq[opCode]++;
            totalIters++;

            // Drift detector: fires at the transition from "valid DL" to
            // "ASCII/string data" or "RGBA pixel data". Logs the previous 4
            // cmds — the last one is the suspect that mis-advanced dl.
            // ASCII printable range 0x20-0x7E with common-letter density is
            // the signal. Only fire once per drift transition (debounced).
            {
                static uint8_t recentOps[4] = {0, 0, 0, 0};
                static size_t recentDls[4] = {0, 0, 0, 0};
                static bool inAsciiRun = false;
                static int driftLogCount = 0;
                // ASCII string detection: ALL 4 bytes of w0 in printable range
                // (0x20-0x7E). Single ASCII-range opcodes are legitimate
                // (Factor5 op_2E, 0x2A, 0x26, etc.); only when the entire
                // word reads as text are we reading data, not opcodes.
                const uint32_t w0v = dl->w0;
                auto inPrint = [](uint32_t b) { return b >= 0x20 && b <= 0x7E; };
                bool isAsciiLike = inPrint((w0v >> 24) & 0xFF) && inPrint((w0v >> 16) & 0xFF) &&
                                   inPrint((w0v >> 8) & 0xFF) && inPrint(w0v & 0xFF);
                if (isAsciiLike && !inAsciiRun && driftLogCount < 5) {
                    driftLogCount++;
                    uint32_t curAddr = dlStartAdddress + uint32_t((uintptr_t)dl - (uintptr_t)dlStart);
                    fprintf(stderr, "[drift] #%d ASCII-word w0=0x%08X w1=0x%08X at dl=0x%08X — last 4 cmds:\n",
                        driftLogCount, dl->w0, dl->w1, curAddr);
                    for (int i = 0; i < 4; i++) {
                        fprintf(stderr, "  -%d: op=0x%02X dl=0x%08X\n",
                            4 - i, recentOps[i], (unsigned)recentDls[i]);
                    }
                    fflush(stderr);
                    inAsciiRun = true;
                }
                if (!isAsciiLike) {
                    inAsciiRun = false;
                }
                // Update sliding window.
                recentOps[0] = recentOps[1]; recentOps[1] = recentOps[2]; recentOps[2] = recentOps[3]; recentOps[3] = opCode;
                recentDls[0] = recentDls[1]; recentDls[1] = recentDls[2]; recentDls[2] = recentDls[3];
                recentDls[3] = dlStartAdddress + uint32_t((uintptr_t)dl - (uintptr_t)dlStart);
            }

            if (totalIters >= totalDumpThreshold) {
                fprintf(stderr, "[opfreq-cumul] @ %llu cumul cmds:", (unsigned long long)totalIters);
                // Print ALL nonzero opcodes sorted by count.
                int idxs[256];
                for (int i = 0; i < 256; i++) idxs[i] = i;
                for (int i = 0; i < 256; i++) {
                    int best = i;
                    for (int j = i + 1; j < 256; j++) {
                        if (opFreq[idxs[j]] > opFreq[idxs[best]]) best = j;
                    }
                    int tmp = idxs[i]; idxs[i] = idxs[best]; idxs[best] = tmp;
                    if (opFreq[idxs[i]] == 0) break;
                    fprintf(stderr, " 0x%02X=%llu", idxs[i], (unsigned long long)opFreq[idxs[i]]);
                }
                fprintf(stderr, "\n"); fflush(stderr);
                totalDumpThreshold *= 4;
            }
            if (loopIters == opFreqDumpThreshold) {
                fprintf(stderr, "[trace] opFreq dump @ iter=%zu:", loopIters);
                for (int i = 0; i < 256; i++) {
                    if (opFreq[i] > 0) {
                        fprintf(stderr, " 0x%02X=%llu", i, (unsigned long long)opFreq[i]);
                    }
                }
                fprintf(stderr, "\n"); fflush(stderr);
                opFreqDumpThreshold *= 10;
            }

            // Periodic dlAddr trace: every 100K iters, dump current dl pointer +
            // the last 8 unique dlAddrs from history. If the stuck task cycles
            // through the same chunk addresses, this will reveal the cycle.
            if ((loopIters % 100000) == 0) {
                uint32_t curAddr = dlStartAdddress + uint32_t((uintptr_t)dl - (uintptr_t)dlStart);
                fprintf(stderr, "[trace] dlAddr probe iter=%zu cur=0x%08X recent=",
                    loopIters, curAddr);
                for (int back = 0; back < 8; back++) {
                    if (g_dlHistCount < (size_t)(back + 1)) break;
                    size_t idx = (g_dlHistCount - 1 - back) % kDLHistLen;
                    fprintf(stderr, " 0x%08X[op=%02X]",
                        g_dlHist[idx].dlAddr, g_dlHist[idx].opcode);
                }
                fprintf(stderr, "\n"); fflush(stderr);
            }

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
                // Guard against hleGBI becoming nullptr mid-task. F3DEX's
                // loadUCode (op 0xAF) calls loadUCodeGBI which sets hleGBI to
                // null if the new ucode isn't recognized. Without this check
                // the next iteration AVs at NULL+(opcode*8). Force-end the task.
                if (hleGBI == nullptr) {
                    static int n = 0;
                    if (++n <= 5) {
                        fprintf(stderr, "[trace] interpreter abort: hleGBI became null mid-task (n=%d, loopIters=%zu)\n",
                            n, loopIters);
                        fflush(stderr);
                    }
                    break;
                }
                func = hleGBI->map[opCode];

#       ifdef DUMP_DISPLAY_LISTS
                RT64_LOG_PRINTF("0x%08X 0x%08X", dl->w0, dl->w1);
#       endif

                // Probe G_DL (op 0x06) targets — dump first 20 cmds of any new sub-DL.
                if (opCode == 0x06) {
                    uint32_t target = dl->w1 & 0x00FFFFFF;
                    static uint32_t seen_dl[256] = {0};
                    static int seen_dl_n = 0;
                    bool dl_already = false;
                    for (int i = 0; i < seen_dl_n; i++) {
                        if (seen_dl[i] == target) { dl_already = true; break; }
                    }
                    if (!dl_already && seen_dl_n < 256) {
                        seen_dl[seen_dl_n++] = target;
                        DisplayList *t = (DisplayList*)((uint8_t*)dlStart + (target - dlStartAdddress));
                        // Bounds check: only dump if target is within reasonable RDRAM range
                        if (target >= 0x00000000 && target < 0x00800000) {
                            DisplayList *p = (DisplayList*)(state->RDRAM + target);
                            fprintf(stderr, "[trace] NEW sub-DL @ 0x%08X (called from 0x%08X) first 35 cmds:\n",
                                target, dlStartAdddress + (uint32_t)((uintptr_t)dl - (uintptr_t)dlStart));
                            for (int i = 0; i < 35; i++) {
                                uint32_t w0 = (p + i)->w0;
                                uint32_t w1 = (p + i)->w1;
                                fprintf(stderr, "  %03d  op=0x%02X  w0=0x%08X  w1=0x%08X  addr=0x%08X\n",
                                    i, (unsigned)(w0 >> 24), w0, w1,
                                    target + (uint32_t)(i * sizeof(DisplayList)));
                            }
                            fflush(stderr);
                        }
                    }
                }

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

            // Factor5 in Rogue Squadron emits multi-million-command DLs
            // without G_RDPFULLSYNC for normal frames. processDisplayLists
            // never returns within wallclock budget, so workloads accumulate
            // and never reach the renderer. Drain periodically here, BETWEEN
            // commands, where workload state is consistent (the prior handler
            // has fully returned and any drawData updates are committed).
            {
                const int workloadCursor = state->ext.workloadQueue->writeCursor;
                Workload &wl = state->ext.workloadQueue->workloads[workloadCursor];
                if (wl.fbPairCount >= 64) {
                    static int n = 0;
                    if (++n <= 5 || (n % 50) == 0) {
                        fprintf(stderr, "[trace] inter-cmd fullSync #%d (fbPairCount=%u, loopIters=%zu)\n",
                            n, (unsigned)wl.fbPairCount, loopIters);
                        fflush(stderr);
                    }
                    state->fullSync();
                }
            }
        }

        // Factor5 in Rogue Squadron does not emit G_RDPFULLSYNC for normal frames
        // (its ucode signals DP completion through a different path), so the workload
        // accumulates fbPairs indefinitely and is never submitted to the renderer.
        // The end of processDisplayLists is the natural task boundary — texture/TMEM
        // state is consistent here — so flush the workload now if it hasn't been.
        {
            const int workloadCursor = state->ext.workloadQueue->writeCursor;
            Workload &wl = state->ext.workloadQueue->workloads[workloadCursor];
            static int n = 0;
            if (++n <= 5 || (n % 50) == 0) {
                fprintf(stderr, "[trace] DL-end #%d loopIters=%zu fbPairCount=%u\n",
                    n, loopIters, (unsigned)wl.fbPairCount);
                fflush(stderr);
            }
            if (wl.fbPairCount > 0) {
                state->fullSync();
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
