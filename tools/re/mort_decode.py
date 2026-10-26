"""MORT audio codec decoder (Factor 5 N64) — Python port.

Ported from jombo23/SubDrag's CMORTDecoder C++ transcription
(tools/re/MORTDecoder.cpp), which itself was hand-transcribed from
the N64 MIPS R4300 disassembly of Rogue Squadron 64.

This implements the DECODE path only (no re-encoding).  It reads
a MORT-encoded byte stream and produces 16-bit signed mono PCM
at the sample rate given in the MORT header.

MORT header layout (big-endian):
    +0  5 bytes  magic        "MORT\\0"
    +5  u8       unk05
    +6  u16      freq         (Hz, typically 8000 or 16000)
    +8  u32      word_count   (payload length / 4)
    +12 ...      encoded payload

Function-by-function correspondence with the C++ source is kept in
comments referencing the original MIPS addresses (`Function80045FF0`
etc.) so the two can be cross-checked.

CLI:
    python tools/re/mort_decode.py --speech speech.raw --out-dir wav/
    python tools/re/mort_decode.py --rom rs64.n64.us.1.0.z64 --out-dir wav/
"""

from __future__ import annotations

import argparse
import struct
import sys
import wave
from pathlib import Path

# ---------------------------------------------------------------------------
# Signed/unsigned masking helpers
# ---------------------------------------------------------------------------

def s16(x: int) -> int:
    x &= 0xFFFF
    return x - 0x10000 if x & 0x8000 else x


def s32(x: int) -> int:
    x &= 0xFFFFFFFF
    return x - 0x100000000 if x & 0x80000000 else x


def u32(x: int) -> int:
    return x & 0xFFFFFFFF


def sar32(x: int, n: int) -> int:
    """Arithmetic right shift on a 32-bit value (MIPS `sra`)."""
    return s32(x) >> n


# ---------------------------------------------------------------------------
# Constant tables (from the disassembled ROM)
# ---------------------------------------------------------------------------

# Signed-char table at 0x8004867C (8 entries). Used in Function80048590.
TABLE_8004867C_V1 = [-4, -3, -2, -2, -1, -1, -1, -1]  # 0xFC,0xFD,0xFE,0xFE,0xFF,...

# Unsigned-char table at 0x80048684 (8 entries). Used in Function80048590.
TABLE_8004867C_V2 = [7, 7, 3, 7, 1, 3, 5, 7]

# Unsigned-short table at 0x80048738 (6 entries). Used in Function80048684.
TABLE_80048738 = [0x0CCD, 0x2CCD, 0x5333, 0x7FFF, 0x852A, 0x0000]


# ---------------------------------------------------------------------------
# Byte-stream helpers (big-endian, mirroring CharArrayToLong/Short)
# ---------------------------------------------------------------------------

def read_u32_be(buf, off: int) -> int:
    return ((buf[off] << 24) | (buf[off + 1] << 16) |
            (buf[off + 2] << 8) | buf[off + 3]) & 0xFFFFFFFF


def read_u16_be(buf, off: int) -> int:
    return ((buf[off] << 8) | buf[off + 1]) & 0xFFFF


def write_u16_be(buf, off: int, val: int) -> None:
    buf[off] = (val >> 8) & 0xFF
    buf[off + 1] = val & 0xFF


# ---------------------------------------------------------------------------
# MORT decoder
# ---------------------------------------------------------------------------

class MortDecoder:
    """Stateful decoder for one MORT stream."""

    # Sizes from the original VRAM layout.
    INTERMEDIATE_BUF_SIZE = 0x1400      # 800D2AD8 .. 800D3ED8
    RAW_INPUT_BUF_SIZE = 0x1000         # 800D3ED8 .. 800D4ED8
    OUTPUT_BUF_SIZE = 0x5000            # 800D4F20 .. (oversized)
    PREDICTOR_BUF_SIZE = 0xA0           # buffer800D2940Predictor

    def __init__(self) -> None:
        self.reset()

    # --- state init -------------------------------------------------------

    def reset(self) -> None:
        self.predictor = [0] * self.PREDICTOR_BUF_SIZE          # 0x800D2940
        self.numberSkipResetPredictorCheck = 0                  # 0x800D2940[0x18A]
        self.numberResetPredictor = 0                           # 0x800D2940[0x18C]
        self.lastPredictorUpdateBase = 0
        self.lastSampleValue = 0
        self.sampleBuffer = [0] * 8                             # signed 32-bit
        self.currentSmootherPredictor = 0                       # 0 or 1
        self.smootherPredictorA = [0] * 8                       # signed 16-bit
        self.smootherPredictorB = [0] * 8

        self.intermediate = bytearray(self.INTERMEDIATE_BUF_SIZE)   # 0x800D2AD8
        self.raw_input = bytearray(self.RAW_INPUT_BUF_SIZE)         # 0x800D3ED8
        self.output_buf = bytearray(self.OUTPUT_BUF_SIZE)           # 0x800D4F20

        # Status / cursor variables.
        self.amount_sound_left = 0          # 0x800D4ED8 u16 (samples-to-decode counter)
        self.unk_3E80 = 0                   # 0x800D4EDA u16
        self.compressed_words_left = 0      # 0x800D4EDC u32
        self.bit_position = 0               # 0x800D4EFC (input-chunk current read bit position)
        self.input_chunk_used = 0           # 0x800D4F00
        self.input_chunk_amount_left = 0    # 0x800D4F04
        self.cursor_F08 = 0                 # 0x800D4F08 (output-write base)
        self.cursor_F0C = 0                 # 0x800D4F0C (output-emit head)
        self.status1 = 0                    # 0x800D4F10
        self.status2 = 4                    # 0x800D4F11
        self.status3 = 2                    # 0x800D4F12

        self.rom_address = 0                # 0x800D2AD4 (ROM offset of current MORT data)
        self.rom_mort_address = 0           # 0x800FCEFC

        # Mixer/volume cursors.
        self.volume = 0x7F                  # 0x800FCF0C
        self.counter_play1 = 0xFFFFFFFF     # 0x800FCF30
        self.counter_play2 = 0xFFFFFFFF     # 0x800FCF34
        self.counter_inc1 = 0x1F8           # 0x800FCF38 (PJ64 Rogue cadence)
        self.counter_inc2 = 0x1F8           # 0x800FCF3C
        self.mort_status1 = 2               # 0x800FCEF0
        self.mort_status2 = 0               # 0x800FCEF1
        self.amount_left_eee8 = 0xB8        # 0x800FCEE8
        self.counter_eeec = 0x194           # 0x800FCEEC
        self.var_ced0 = 0                   # 0x800FCED0
        self.var_cf04 = 0                   # 0x800FCF04 (current volume scratch)
        self.var_cf20 = 1                   # 0x800FCF20
        self.var_cf21 = 1                   # 0x800FCF21

        self.S5_const = 0x8A0
        self.S6_const = 0xA00

        # PCM accumulator (signed 16-bit ints).
        self.pcm: list[int] = []
        self.started = False

    # --- main entry point ------------------------------------------------

    def decode(self, rom_bytes: bytes, address: int, length: int) -> list[int]:
        """Decode `length` bytes of MORT data starting at `address` in `rom_bytes`.

        Returns signed 16-bit PCM samples in range [-32768, 32767].
        """
        self.reset()
        # Wrap rom as bytes; ensure we can index past length safely up to read_u32.
        rom = rom_bytes
        self.rom_address = address
        self.rom_mort_address = address

        # Driver loop counter / state-machine bookkeeping.
        endedLoopAmount = -1
        loops = 0
        # The C++ driver computes S7 = a counter-determined chunk size each iter.
        # We replicate that, then run Function80045780 / Function800459E0.
        while True:
            # Tick the counter_play / counter_inc cadence (per-frame mixer step).
            if self.counter_play1 == 0xFFFFFFFF:
                self.counter_play1 = 0xB8
                self.counter_play2 = 0xB8
            else:
                for _ in range(3):
                    if self.counter_play1 >= 0x8A0:
                        self.counter_play1 = 0xB8
                        self.counter_play2 = 0xB8
                    else:
                        self.counter_play1 = (self.counter_play1 + 0xB8) & 0xFFFFFFFF
                        self.counter_play2 = (self.counter_play2 + 0xB8) & 0xFFFFFFFF

            # For decode-only path we only process soundIndex==1 (matching C++ `int soundIndex = 1`).
            T6 = self.mort_status2
            if T6 != 2:
                A0 = self.counter_inc2
                V1 = self.counter_play2
                if V1 < A0:
                    T4 = A0 - V1
                    if T4 < self.counter_eeec:
                        S7 = self.counter_eeec
                    else:
                        S7 = self.amount_left_eee8
                else:
                    T6 = V1 - A0
                    T5 = self.S5_const - self.counter_eeec
                    if T5 < T6:
                        S7 = self.counter_eeec
                    else:
                        S7 = self.amount_left_eee8

                # Refill & decode MORT frames into the intermediate buffer.
                self._fn_80045780(rom)

                # Stage transitions if status2==2.
                if self.status2 == 2:
                    # _fn_80045A48: status2 2->3
                    if self.status2 == 2:
                        self.status2 = 3
                    self.var_ced0 = 1
                    self.var_cf20 = 0
                    # Skip the C++ "label80045084" stuff (mixer-side, irrelevant to PCM).

                # Advance the output-emit cursor by S7 (the per-tick chunk length).
                # Function800459E0 — returns V0 = number of samples actually advanced.
                V0_after = self._fn_800459E0(S7)
                # The C++ code then performs the output-mix copy loop. Since the
                # game uses 800D4F20 only for the DSP-side mix-out, and the actual
                # 16-bit PCM samples have already been pushed by Function80048740
                # into self.pcm via the synthesis call below, we skip the mixer
                # ring-buffer copy entirely (it would overwrite our pcm list).
                # The C++ also runs the state-byte transitions at label80045424
                # — we replicate the bits relevant to the decode loop's exit.

                # First MORT play start: status moves 2 -> 1 once the input loader
                # initialises (Function800456D0). On the very first iteration the
                # status2 is 4, with status1=0, mort_status2=0 — wait, actually we
                # start with status2=4 per init (matches C++).  Drive Function800456D0
                # the first time we hit mort_status2==0.

                if self.mort_status2 == 0:
                    # 80045488 branch: call Function800456D0 to seed the loader.
                    self._fn_800456D0()
                    self.mort_status2 = 4
                    self.var_ced0 = 0
                    self.var_cf04 = self.volume

            loops += 1

            if not self.started:
                # Mirror C++: started becomes true once status2==1 in Function80045780,
                # OR via Function800459E0 stop check. We don't gate writing samples on
                # `started` because every push_back happens inside Function80048740
                # which is reached only when the input buffer is primed (status1==0
                # and status2 in {1,2,3}). Set `started` whenever we first push PCM.
                if len(self.pcm) > 0:
                    self.started = True
            else:
                if self.status2 == 4:
                    if endedLoopAmount == -1:
                        endedLoopAmount = loops + 1000
                    else:
                        if loops > endedLoopAmount:
                            break
                elif loops >= 2000:
                    if endedLoopAmount == -1:
                        endedLoopAmount = loops + 1000
                    else:
                        if loops > endedLoopAmount:
                            break

            # Safety: huge upper bound on loops to prevent runaway.
            if loops > 50000:
                break

        # Convert pcm list (which already contains 16-bit signed samples as Python ints)
        # to signed range [-32768, 32767].
        return [s16(x) for x in self.pcm]

    # ------------------------------------------------------------------
    # Function80045780 — chunk reader/state machine (line 1097 in C++)
    # ------------------------------------------------------------------
    def _fn_80045780(self, rom: bytes) -> None:
        if self.status1 != 0:
            # 800457A4 — first-call header read after Function800456D0.
            V1 = self.input_chunk_amount_left
            if V1 == 0:
                T9 = read_u32_be(self.raw_input, 4)
                T0 = read_u32_be(self.raw_input, 8)
                self.bit_position = 0x60
                self.amount_sound_left = (T9 >> 0x10) & 0xFFFF
                self.compressed_words_left = T0 & 0x00FFFFFF
                self.unk_3E80 = T9 & 0xFFFF
                V1 = self.input_chunk_amount_left
            V0 = self.compressed_words_left
            self.input_chunk_amount_left = (V1 + 0x400) & 0xFFFFFFFF
            if V0 < 0x100:
                self.compressed_words_left = 0
            else:
                self.compressed_words_left = (V0 - 0x100) & 0xFFFFFFFF
            self.status1 = 0

        # 80045820 — refill if available data is low and process MORT frames.
        S2 = self.status3
        if self.status1 == 0:
            V0 = self.status2
            if V0 in (1, 2, 3):
                V1 = self.input_chunk_amount_left
                T6 = self.input_chunk_used
                T7 = (V1 - T6) & 0xFFFFFFFF
                if s32(T7) < 0xC01:
                    T9 = V1 & 0xFFF
                    if V1 != 0:
                        V0c = self.compressed_words_left
                        if V0c < 0x101:
                            A2 = V0c << 2
                        else:
                            A2 = 0x400
                    else:
                        A2 = 0x400
                    T0 = T9 >> 3
                    if s32(A2) > 0:
                        T1 = T0 << 3
                        A1 = T1
                        self.status1 = 1
                        A0 = (self.rom_address + V1) & 0xFFFFFFFF
                        # Function800455DC: DMA copy A2 bytes from ROM[A0] -> raw_input[A1].
                        self._fn_800455DC(rom, A0, A1, A2)

            S1 = self.cursor_F0C
            T2 = self.cursor_F08
            V1 = (S1 - T2) & 0xFFFFFFFF
            if s32(V1) < 0x960:
                T3 = self.status2
                if T3 == 1:
                    T5 = self.input_chunk_amount_left
                else:
                    T4 = self.amount_sound_left
                    if s16(T4) <= 0:
                        # goto label800459AC
                        self._fn_80045780_tail(V1)
                        return
                    T5 = self.input_chunk_amount_left
                T6 = self.input_chunk_used
                T7 = (T5 - T6) & 0xFFFFFFFF
                if s32(T7) <= 0:
                    self._fn_80045780_tail(V1)
                    return
                if s32(S2) <= 0:
                    self._fn_80045780_tail(V1)
                    return

                # Inner frame loop, label80045910.
                while True:
                    T8 = S1 % self.S6_const
                    T0 = T8 >> 2
                    T1 = T0 << 3
                    A1 = T1
                    # Function80045FF0 — decodes one MORT frame into self.intermediate,
                    # pushing 0x9E = 158 PCM samples (0xD+0xE+0xD+0x78) into self.pcm.
                    self._fn_80045FF0(A1)

                    T9 = self.bit_position
                    T4 = self.cursor_F0C
                    T8 = self.cursor_F08
                    T6 = self.amount_sound_left
                    T2 = T9 >> 3
                    S1 = (T4 + 0xA0) & 0xFFFFFFFF
                    T3 = T2 & 0xFFFFFFFC
                    V1 = (S1 - T8) & 0xFFFFFFFF
                    T7 = (s32(T6) - 1)
                    if T7 < 0:
                        T7 = 0
                    self.input_chunk_used = T3
                    self.cursor_F0C = S1
                    self.amount_sound_left = T7 & 0xFFFF
                    S2 = (S2 - 1) & 0xFFFFFFFF
                    if s32(V1) < 0x960:
                        T0_s = self.status2
                        T1_s = T7 & 0xFFFF
                        if T0_s != 1:
                            if s16(T1_s) <= 0:
                                break
                            T9 = self.input_chunk_amount_left
                        else:
                            T9 = self.input_chunk_amount_left
                        T2 = self.input_chunk_used
                        T3 = (T9 - T2) & 0xFFFFFFFF
                        if s32(T3) <= 0:
                            pass  # stay, do not iterate inner
                            break
                        if s32(S2) > 0:
                            continue
                        else:
                            break
                    else:
                        break
                self._fn_80045780_tail(V1)
                return

        self._fn_80045780_tail(0)

    def _fn_80045780_tail(self, V1: int) -> None:
        # label800459AC: state transition to 2 (playing) once primed.
        T4 = self.status2
        if T4 == 1:
            if s32(V1) >= 0x8C0:
                self.status2 = 2
                self.started = True

    # ------------------------------------------------------------------
    # Function800459E0 — emit-cursor advance / EOS detection (line 1446)
    # ------------------------------------------------------------------
    def _fn_800459E0(self, A1: int) -> int:
        if self.status2 != 3:
            return 0
        V1 = self.cursor_F08
        T7 = self.cursor_F0C
        V0 = (T7 - V1) & 0xFFFFFFFF
        if A1 < V0:
            A2 = A1
        else:
            T8 = self.amount_sound_left
            A2 = V0
            if s16(T8) <= 0:
                self.status2 = 4
                V1 = self.cursor_F08
        self.cursor_F08 = (V1 + A2) & 0xFFFFFFFF
        return A2

    # ------------------------------------------------------------------
    # Function800455DC — input chunk DMA (line 1777)
    # ------------------------------------------------------------------
    def _fn_800455DC(self, rom: bytes, src: int, dst: int, n: int) -> None:
        n = int(n)
        # Clip to safety: avoid reading past rom end.
        if src >= len(rom):
            # Pad with zeros.
            for i in range(n):
                self.raw_input[(dst + i) % self.RAW_INPUT_BUF_SIZE] = 0
            return
        avail = min(n, len(rom) - src)
        for i in range(avail):
            self.raw_input[(dst + i) % self.RAW_INPUT_BUF_SIZE] = rom[src + i]
        for i in range(avail, n):
            self.raw_input[(dst + i) % self.RAW_INPUT_BUF_SIZE] = 0

    # ------------------------------------------------------------------
    # Function800456D0 — initialise input loader (line 1584)
    # ------------------------------------------------------------------
    def _fn_800456D0(self) -> None:
        # Reset predictors + smoother state.
        self.predictor = [0] * self.PREDICTOR_BUF_SIZE
        self.numberSkipResetPredictorCheck = 0
        self.numberResetPredictor = 0
        self.lastPredictorUpdateBase = 0x28
        self.lastSampleValue = 0
        self.sampleBuffer = [0] * 8
        self.currentSmootherPredictor = 0
        self.smootherPredictorA = [0] * 8
        self.smootherPredictorB = [0] * 8

        # The C++ sets rom_address back to rom_mort_address; we already do that on reset().
        self.rom_address = self.rom_mort_address

        # Function8005E3A0 just stores DMA-config scratch state — no decoder effect.
        # status2 becomes 1 (input loader running).
        self.status2 = 1
        self.status1 = 0
        self.amount_sound_left = 0
        self.unk_3E80 = 0
        self.bit_position = 0
        self.input_chunk_amount_left = 0
        self.input_chunk_used = 0
        self.cursor_F0C = 0
        self.cursor_F08 = 0
        self.status3 = 2

    # ------------------------------------------------------------------
    # Bit-stream reader (line 2018)
    # ------------------------------------------------------------------
    def _read_bits(self, num_bits: int, state: list) -> int:
        """state = [currentInputData, bitsleft, currentOverallBitPosition].

        Mirrors ReadBitsFrom80045FF0Buffer.  Returns numBits drawn from the
        little-end of currentInputData (LSB-first), refilling the 32-bit
        register from raw_input[] when bitsleft is exhausted.
        """
        currentInputData, bitsleft, currentOverallBitPosition = state
        if s32(bitsleft) >= (num_bits + 1):
            bitmask = (1 << num_bits) - 1
            returnValue = currentInputData & bitmask
            currentInputData = currentInputData >> num_bits
            bitsleft -= num_bits
            state[0] = currentInputData & 0xFFFFFFFF
            state[1] = bitsleft & 0xFFFFFFFF
            state[2] = currentOverallBitPosition & 0xFFFFFFFF
            return returnValue & 0xFFFF
        else:
            # Bit-spanning: combine `bitsleft` LSBs of currentInputData with
            # the next 32-bit word from raw_input[].
            t9 = (1 << bitsleft) - 1
            currentOverallBitPosition = (currentOverallBitPosition + 0x20) & 0xFFFFFFFF
            t8 = currentOverallBitPosition >> 5
            t6 = t8 & 0x3FF
            t7 = t9 & currentInputData
            t8_off = (t6 << 2) & 0xFFFF
            returnValue = t7 & 0xFFFF
            currentInputData = read_u32_be(self.raw_input, t8_off)
            if bitsleft != num_bits:
                t6b = num_bits - bitsleft
                t8b = (1 << t6b) - 1
                t6c = currentInputData & t8b
                t8c = t6c << bitsleft
                returnValue = (t7 | t8c) & 0xFFFF
            t9b = num_bits - bitsleft
            currentInputData = (currentInputData >> t9b) & 0xFFFFFFFF
            bitsleft = (bitsleft + (0x20 - num_bits)) & 0xFFFFFFFF
            state[0] = currentInputData
            state[1] = bitsleft
            state[2] = currentOverallBitPosition
            return returnValue

    # ------------------------------------------------------------------
    # Function80045FF0 — main MORT frame unpack (line 2072)
    # ------------------------------------------------------------------
    def _fn_80045FF0(self, currentIntermediateValueOffset: int) -> None:
        currentOverallBitPosition = self.bit_position
        # Load current 32-bit input word.
        t6 = sar32(currentOverallBitPosition, 5)
        t7 = t6 & 0x3FF
        t8 = (t7 << 2) & 0xFFFF
        currentInputWord = read_u32_be(self.raw_input, t8)
        bitsUsed = currentOverallBitPosition & 0x1F
        bitsleft = (0x20 - bitsUsed) & 0xFFFFFFFF
        currentInputData = (currentInputWord >> bitsUsed) & 0xFFFFFFFF
        state = [currentInputData, bitsleft, currentOverallBitPosition]

        if self.numberSkipResetPredictorCheck == 0:
            if self.numberResetPredictor == 0:
                bit = self._read_bits(1, state)
                if bit != 0:
                    self.numberResetPredictor = self._read_bits(4, state) + 1
                else:
                    self.numberSkipResetPredictorCheck = self._read_bits(7, state) + 1

        if self.numberResetPredictor != 0:
            # Clear 0x140 bytes of intermediate buffer at the offset; effectively
            # a silence frame, then decrement the reset countdown.
            base = (0x800D2AD8 + currentIntermediateValueOffset) - 0x800D2AD8
            for i in range(0x140):
                if 0 <= base + i < self.INTERMEDIATE_BUF_SIZE:
                    self.intermediate[base + i] = 0
            self.numberResetPredictor -= 1
            # In the game the MORT decoder is wall-clock-driven: silence frames
            # consume real time on the mixer side, so the audible content stays
            # at the correct cadence. For offline extraction we have to emit
            # the silence ourselves — push 158 zero samples (matching the
            # 0xD+0xE+0xD+0x78 emit count of Function80048740's normal path) so
            # the gap between words gets preserved in the WAV.
            self.pcm.extend([0] * 0x9E)
        else:
            shortsSPE8 = [0] * 8
            shortsSPE0 = [0] * 4
            shortsSPD0 = [0] * 4
            stackBuffer2Offsets = [0] * 4
            shortsSPC8 = [0] * 4
            shortsSP60 = [[0] * 0xD for _ in range(4)]

            shortsSPE8[0] = self._read_bits(6, state)
            shortsSPE8[1] = self._read_bits(6, state)
            shortsSPE8[2] = self._read_bits(5, state)
            shortsSPE8[3] = self._read_bits(5, state)
            shortsSPE8[4] = self._read_bits(4, state)
            shortsSPE8[5] = self._read_bits(4, state)
            shortsSPE8[6] = self._read_bits(3, state)
            shortsSPE8[7] = self._read_bits(3, state)

            for x in range(4):
                shortsSPE0[x] = self._read_bits(7, state)
                shortsSPD0[x] = self._read_bits(2, state)
                stackBuffer2Offsets[x] = self._read_bits(2, state)
                shortsSPC8[x] = self._read_bits(6, state)
                for y in range(0xD):
                    shortsSP60[x][y] = self._read_bits(3, state)

            self.numberSkipResetPredictorCheck -= 1

            self._fn_80045C78(currentIntermediateValueOffset, shortsSP60,
                              shortsSPC8, shortsSPD0, stackBuffer2Offsets,
                              shortsSPE0, shortsSPE8)

        # Update bit_position from local state.
        currentOverallBitPosition = state[2]
        bitsleft = state[1]
        t9 = currentOverallBitPosition & 0xFFFFFFE0
        t8 = (t9 - bitsleft + 0x20) & 0xFFFFFFFF
        self.bit_position = t8

    # ------------------------------------------------------------------
    # Function80045C78 — predictor + smoother orchestration (line 2319)
    # ------------------------------------------------------------------
    def _fn_80045C78(self, currentIntermediateValueOffset, shortsSP60,
                    shortsSPC8, shortsSPD0, stackBuffer2Offsets,
                    shortsSPE0, shortsSPE8) -> None:
        stackBuffer2 = [0] * 0x28
        for x in range(4):
            self._fn_80048590(shortsSPC8[x], stackBuffer2Offsets[x], stackBuffer2, shortsSP60[x])
            self._fn_80048684(shortsSPE0[x], shortsSPD0[x], stackBuffer2)
        self._fn_80045A80(currentIntermediateValueOffset, shortsSPE8)

    # ------------------------------------------------------------------
    # Function80048590 — quantise/scale residuals into stackBuffer2 (line 2404)
    # ------------------------------------------------------------------
    def _fn_80048590(self, shortsSPC8Value: int, stackBuffer2Offset: int,
                     stackBuffer2: list, shortsSP60_row: list) -> None:
        shortsSPC8Value = s16(shortsSPC8Value)
        stackBuffer2Offset = s16(stackBuffer2Offset)
        if shortsSPC8Value < 8 and shortsSPC8Value >= 0:
            T0 = TABLE_8004867C_V1[shortsSPC8Value]   # signed
            T1 = TABLE_8004867C_V2[shortsSPC8Value]
        else:
            # The C++ takes (shortsSPC8Value - 8) >> 3 and & 7.  For an
            # ordinary signed-short value this matches MIPS `sra`.
            T0 = (shortsSPC8Value - 8) >> 3
            T1 = shortsSPC8Value & 7
        T0 = 6 - T0
        T4 = (T1 * 0x800) + 0x47FF
        for x in range(0x28):
            stackBuffer2[x] = 0
        for x in range(0xD):
            v = s16(shortsSP60_row[x])
            t5 = s32(((v * 0x2000) - 0x7000) * T4 + 0x4000) >> 0xF
            # Sort-of-round-up: add (1 << (T0 - 1)) then arithmetic shift right by T0.
            # MIPS `sllv`/`srav` mask the shift amount with 0x1F, so (T0-1) wraps to
            # 31 when T0==0, putting 0x80000000 into the bias. We honour that here.
            shift_bias = (T0 - 1) & 0x1F
            bias = (1 << shift_bias)
            # Treat bias as signed 32-bit (0x80000000 -> -0x80000000).
            bias = bias - 0x100000000 if bias & 0x80000000 else bias
            t5 = s32(t5 + bias) >> (T0 & 0x1F)
            shortsSP60_row[x] = t5 & 0xFFFF
            stackBuffer2[stackBuffer2Offset + (x * 3)] = s16(t5 & 0xFFFF)

    # ------------------------------------------------------------------
    # Function80048684 — predictor-buffer update (line 2499)
    # ------------------------------------------------------------------
    def _fn_80048684(self, shortsSPE0Value: int, shortsSPD0Value: int,
                     stackBuffer2: list) -> None:
        shortsSPE0Value = s16(shortsSPE0Value)
        if 0x28 <= shortsSPE0Value <= 0x78:
            self.lastPredictorUpdateBase = shortsSPE0Value & 0xFFFF
        # Shift predictors back: predictor[0..0x78) <- predictor[0x28..0xA0).
        for x in range(0x78):
            self.predictor[x] = self.predictor[x + 0x28]
        # Apply table coefficient + delta from stackBuffer2.
        tbl_coef = TABLE_80048738[shortsSPD0Value & 0xFFFF]
        # tbl_coef is unsigned-short literal (0x852A is high-bit set but stored as u16).
        # In the C++ source the multiplication uses the unsigned short value directly
        # but the (int) cast on `(T1 * (unsigned short)table80048738[...])` then ` + 0x4000) >> 0xF`
        # is a 32-bit signed shift.  We honour that.
        for x in range(0x28):
            idx = 0x78 - self.lastPredictorUpdateBase + x
            # The predictor[] is conceptually a sliding window of signed shorts.
            # `idx` can be negative or > 0xA0 for malformed input; clamp.
            if 0 <= idx < self.PREDICTOR_BUF_SIZE:
                T1 = s16(self.predictor[idx])
            else:
                T1 = 0
            T1 = s32(T1 * tbl_coef + 0x4000) >> 0xF
            self.predictor[0x78 + x] = (T1 + s16(stackBuffer2[x])) & 0xFFFF

    # ------------------------------------------------------------------
    # Function80045A80 — generate smoothing coefficients (line 2588)
    # ------------------------------------------------------------------
    def _fn_80045A80(self, currentIntermediateValueOffset: int, shortsSPE8: list) -> None:
        if not self.currentSmootherPredictor:
            sp = self.smootherPredictorA
        else:
            sp = self.smootherPredictorB

        # Each line below replicates `(int)(((((signed short)spe8[i] - K) * 0x400) * COEF) + BIAS) >> 0xF << 1`.
        # In C++ left-to-right precedence on >> << means: ((... >> 15) << 1).
        def calc(idx: int, k: int, coef: int, bias: int) -> int:
            v = s16(shortsSPE8[idx]) - k
            x = (v * 0x400 * coef) + bias
            x = s32(x) >> 0xF
            x = (x << 1) & 0xFFFF
            return x

        sp[0] = calc(0, 0x20, 0x3333, 0x4000)
        sp[1] = calc(1, 0x20, 0x3333, 0x4000)
        # 0x332F000 stored with leading minus is -0x332F000 (a 32-bit signed bias).
        sp[2] = calc(2, 0x10, 0x3333, -0x332F000)
        sp[3] = calc(3, 0x10, 0x3333, 0x04003C00)
        sp[4] = calc(4, 8, 0x4B17, s32(0xFFC91B1C))
        sp[5] = calc(5, 8, 0x4444, 0x03BBF800)
        sp[6] = calc(6, 4, 0x7ADE, 0x0147936C)
        sp[7] = calc(7, 4, 0x740C, 0x040D6B40)

        self._fn_80048904(currentIntermediateValueOffset)
        self.currentSmootherPredictor = 0 if self.currentSmootherPredictor else 1

    # ------------------------------------------------------------------
    # Helpers 80048AFC/B14/B24/B3C — adjuster blend + saturating curve
    # ------------------------------------------------------------------
    def _fn_80048B3C(self, T2: int) -> int:
        # Saturating non-linear curve.
        T2 = s32(T2)
        is_neg = T2 < 0
        if is_neg:
            T2 = -T2
        if T2 < 0x2B33:
            T2 = T2 * 2
        elif T2 < 0x4E66:
            T2 = T2 + 0x2B33
        else:
            T2 = (T2 >> 2) + 0x6600
        if T2 > 0x7FFF:
            T2 = 0x7FFF
        if is_neg:
            T2 = -T2
        return T2 & 0xFFFF

    def _fn_80048AFC(self, T2: int, T4: int) -> int:
        T2 = s32(T2); T4 = s32(T4)
        # (T2 / 4) + (T4 / 2) + (T4 / 4)
        T2 = (T2 >> 2) + (T4 >> 1) + (T4 >> 2)
        return self._fn_80048B3C(T2)

    def _fn_80048B14(self, T2: int, T4: int) -> int:
        T2 = s32(T2); T4 = s32(T4)
        T2 = (T2 >> 1) + (T4 >> 1)
        return self._fn_80048B3C(T2)

    def _fn_80048B24(self, T2: int, T4: int) -> int:
        T2 = s32(T2); T4 = s32(T4)
        # (T2 / 2) + (T4 / 4) + (T2 / 4) = (T2 * 3/4) + (T4/4)
        T2 = (T2 >> 1) + (T4 >> 2) + (T2 >> 2)
        return self._fn_80048B3C(T2)

    def _call_t3(self, algorithm: int, T2: int, T4: int) -> int:
        if algorithm == 0x80048AFC:
            return self._fn_80048AFC(T2, T4)
        if algorithm == 0x80048B14:
            return self._fn_80048B14(T2, T4)
        if algorithm == 0x80048B24:
            return self._fn_80048B24(T2, T4)
        if algorithm == 0x80048B3C:
            return self._fn_80048B3C(T2)
        raise AssertionError(f"unknown blend algorithm {algorithm:#x}")

    # ------------------------------------------------------------------
    # Function80048A58 — build the 8 adjuster coefficients (line 3013)
    # ------------------------------------------------------------------
    def _fn_80048A58(self, algorithm: int) -> list:
        if not self.currentSmootherPredictor:
            sp1, sp2 = self.smootherPredictorA, self.smootherPredictorB
        else:
            sp1, sp2 = self.smootherPredictorB, self.smootherPredictorA
        adjusters = [0] * 8
        for x in range(8):
            T2 = s16(sp1[x])
            T4 = s16(sp2[x])
            adjusters[x] = self._call_t3(algorithm, T2, T4)
        return adjusters

    # ------------------------------------------------------------------
    # Function80048904 — four-band IIR synthesis driver (line 2676)
    # ------------------------------------------------------------------
    def _fn_80048904(self, currentIntermediateValueOffset: int) -> None:
        S = self.sampleBuffer[:]   # local working copy of [S0..S7], 32-bit signed
        A3_box = [s16(self.lastSampleValue)]

        adj = self._fn_80048A58(0x80048AFC)
        self._fn_80048740(currentIntermediateValueOffset, 0, 0xD, adj, S, A3_box)

        adj = self._fn_80048A58(0x80048B14)
        self._fn_80048740(currentIntermediateValueOffset + 0x1A, 0xD, 0xE, adj, S, A3_box)

        adj = self._fn_80048A58(0x80048B24)
        self._fn_80048740(currentIntermediateValueOffset + 0x36, 0x1B, 0xD, adj, S, A3_box)

        adj = self._fn_80048A58(0x80048B3C)
        self._fn_80048740(currentIntermediateValueOffset + 0x50, 0x28, 0x78, adj, S, A3_box)

        self.sampleBuffer = S[:]
        self.lastSampleValue = s16(A3_box[0])

    # ------------------------------------------------------------------
    # Function80048740 — core synthesis inner loop (line 2910)
    # ------------------------------------------------------------------
    def _fn_80048740(self, intermediateValueOffset: int, predictorBufferOffset: int,
                     countValues: int, adjusters: list, S: list, A3_box: list) -> None:
        # `S` is mutated in place (indices 0..7).  `A3_box[0]` likewise.
        # Lattice-filter structure (verbatim from C++ 80048740):
        #   T2 -= (S7 * adj[7] + 0x4000) >> 15           # tap 7 only updates T2
        #   T2 -= (S6 * adj[6] + 0x4000) >> 15
        #   S7  = (adj[6] * T2 + 0x4000) >> 15 + S6      # S7 uses adj[6]
        #   T2 -= (S5 * adj[5] + 0x4000) >> 15
        #   S6  = (adj[5] * T2 + 0x4000) >> 15 + S5      # S6 uses adj[5]
        #   ... (pattern: state[i] update uses adj[i-1] one stage later)
        #   T2 -= (S0 * adj[0] + 0x4000) >> 15
        #   S1  = (adj[0] * T2 + 0x4000) >> 15 + S0
        #   S0  = T2
        for x in range(countValues):
            pidx = predictorBufferOffset + x
            if 0 <= pidx < self.PREDICTOR_BUF_SIZE:
                T2 = s16(self.predictor[pidx])
            else:
                T2 = 0

            adj = [s16(a) for a in adjusters]

            # Tap 7: subtract only, no S update yet.
            T2 = s32(T2 - (s32(s32(S[7]) * adj[7] + 0x4000) >> 0xF))
            # Stages 6 down to 0 each subtract using S[i], then update S[i+1] with adj[i].
            for i in (6, 5, 4, 3, 2, 1, 0):
                T2 = s32(T2 - (s32(s32(S[i]) * adj[i] + 0x4000) >> 0xF))
                S[i + 1] = s32((s32(adj[i] * T2 + 0x4000) >> 0xF) + s32(S[i]))
            S[0] = T2

            # DC-removal-ish first-order tap with A3.
            T2 = s32(T2 + (s32(0x6E14 * s32(A3_box[0]) + 0x4000) >> 0xF))
            A3_box[0] = T2 & 0xFFFF if T2 >= 0 else (T2 + 0x100000000) & 0xFFFF
            # Re-extract as signed 16 since C++ stores back through (signed short) cast.
            A3_box[0] = s16(A3_box[0])

            T2_shift = s32(T2 << 1) & 0xFFFFFFFF
            # Saturate: check if (T2_shift >> 15 + 1) >> 1 != 0 (i.e. magnitude exceeds 0x7FFF).
            AT = (s32(T2_shift) >> 0xF) + 1
            AT = s32(AT) >> 1
            if AT != 0:
                if AT < 0:
                    T2_out = -0x8000
                else:
                    T2_out = 0x7FFF
            else:
                T2_out = s32(T2_shift)
            T2_out &= 0xFFF8  # bottom 3 bits cleared
            T2_out &= 0xFFFF

            # Write into intermediate buffer as big-endian s16 at offset (intermediateValueOffset + x*2)
            off = (intermediateValueOffset + x * 2) % self.INTERMEDIATE_BUF_SIZE
            write_u16_be(self.intermediate, off, T2_out)
            self.pcm.append(T2_out)


# ---------------------------------------------------------------------------
# Speech-table walker + WAV writer
# ---------------------------------------------------------------------------

def parse_speech_header(data: bytes) -> list:
    """Parse the speech.raw header: u32 count, then count×u32 (top=type, bottom=offset).
    Returns list of dicts: voice_id, offset, type_byte, freq, byte_size.
    """
    n = struct.unpack_from(">I", data, 0)[0]
    out = []
    for i in range(n):
        word = struct.unpack_from(">I", data, 4 + i * 4)[0]
        type_byte = (word >> 24) & 0xFF
        offset = word & 0xFFFFFF
        if offset + 12 > len(data):
            continue
        # Magic is "MORT" (4 bytes). Byte 4 is a flag (0x00 normal, 0x01 a
        # variant — user hypothesis: resume/playback marker into a longer line.
        # Either way the rest of the header layout is the same.
        if data[offset:offset + 4] != b"MORT":
            continue
        mort_flag = data[offset + 4]
        unk05 = data[offset + 5]
        freq = struct.unpack_from(">H", data, offset + 6)[0]
        word_count = struct.unpack_from(">I", data, offset + 8)[0]
        out.append({
            "sample_idx": i,
            "offset": offset,
            "type_byte": type_byte,
            "mort_flag": mort_flag,
            "unk05": unk05,
            "freq": freq,
            "byte_size": word_count * 4,
        })
    return out


def write_wav(path: Path, samples: list, sample_rate: int) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        # Pack signed 16-bit little-endian (WAV native).
        buf = bytearray(len(samples) * 2)
        for i, s in enumerate(samples):
            v = s & 0xFFFF
            buf[i * 2] = v & 0xFF
            buf[i * 2 + 1] = (v >> 8) & 0xFF
        w.writeframes(bytes(buf))


def _extract_speech_from_rom(rom_path: Path) -> bytes:
    """Re-run the data-blob walker to locate the speech asset in the ROM."""
    sys.path.insert(0, str(Path(__file__).parent))
    from extract_speech_table import walk_data_blob, load_asset_bytes
    rom = rom_path.read_bytes()
    for _seg, full_path, ent in walk_data_blob(rom):
        if full_path.lower().endswith("speech"):
            return load_asset_bytes(rom, ent)
    raise RuntimeError(f"speech asset not found in {rom_path}")


def main(argv: list) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--speech", type=Path, help="path to pre-extracted speech.raw")
    src.add_argument("--rom", type=Path, help="path to ROM (will re-extract speech.raw)")
    ap.add_argument("--out-dir", type=Path, required=True)
    ap.add_argument("--limit", type=int, default=None, help="decode only first N samples")
    ap.add_argument("--only", type=int, nargs="+", default=None,
                    help="decode only these voice ids (space-separated)")
    args = ap.parse_args(argv)

    if args.speech:
        speech = args.speech.read_bytes()
    else:
        speech = _extract_speech_from_rom(args.rom)
    print(f"[+] speech.raw bytes: {len(speech)}")

    samples = parse_speech_header(speech)
    print(f"[+] mort samples found: {len(samples)}")

    args.out_dir.mkdir(parents=True, exist_ok=True)

    todo = samples
    if args.only:
        wanted = set(args.only)
        todo = [s for s in samples if s["sample_idx"] in wanted]
    if args.limit is not None:
        todo = todo[: args.limit]

    decoder = MortDecoder()
    for s in todo:
        idx = s["sample_idx"]
        freq = s["freq"]
        size = s["byte_size"]
        off = s["offset"]
        try:
            pcm = decoder.decode(speech, off, size)
        except Exception as exc:
            print(f"  sample={idx:4d} FAILED: {type(exc).__name__}: {exc}", file=sys.stderr)
            continue
        # Header freq IS the playback rate. The N64 does some real-time
        # pitch/rate processing in MusyX that slows the playback at runtime;
        # we don't replicate that in the extractor, so the WAVs play a touch
        # fast vs the in-game audio but are otherwise correct (user-verified
        # 8000-Hz baseline 2026-05-20).
        play_rate = freq if freq else 8000
        out_path = args.out_dir / f"sample_{idx:04d}_{play_rate}hz.wav"
        write_wav(out_path, pcm, play_rate)
        dur = len(pcm) / play_rate if play_rate else 0
        print(f"  sample={idx:4d} flag={s['mort_flag']:02x} off=0x{off:06x} size={size:6d} -> {len(pcm)} samples ({dur:.2f}s @ {play_rate}Hz)  {out_path.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
