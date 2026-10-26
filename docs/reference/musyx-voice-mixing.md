# MusyX (Rogue Squadron 64) per-tick voice modulation and mixing

Sources:
- C: `src/main/832A0.c` (line numbers below refer to this file unless stated).
- Asm: `asm/nonmatchings/main/832A0/{tickAllAudioChannels,tickAudioChannel,musyxRenderVoiceSamples,allocateSynthVoice}.s`.
- Asm: `src/main/94A80.c` (hand asm blocks, `computeVoiceVolumePanCoefficients`, pitch tables).
- Tables: `asm/data/main/A2390.data.s`, `A1B50.data.s`.
- Recomp callers: `RecompiledFuncs/funcs_34.c`, `funcs_37.c`.

Status tags: CONFIRMED means matched or compilable C exists in the repo. READ means the behavior comes from asm (the function is still `INCLUDE_ASM`). UNRESOLVED means not yet determined.

## 0. Where the per-tick work happens

The per-tick modulation code is `tickAllAudioChannels` (asm, 0x8008C9AC..0x8008D95C). `tickAudioChannel` is only the SoundMacro opcode interpreter and just writes channel fields. `tickAllAudioChannels` runs once per audio tick over up to 32 channels (ch base 0x8013D880, stride 0x17C). For each channel it calls `tickAudioChannel`, advances the LFOs, sweeps and portamento, evaluates the controller records, computes the final pitch, volume, pan, span and reverb send, and then calls `musyxSetVoicePitchBend(i, rate)` and `setVoiceModulationParams(i, vol, pan, span, rev)`.

The voice slot index equals the channel index (voice array 0x801496F8, stride 0x88).

`sfMixOne` (L5146) calls `tickAudioSequencerFrame` (L3858) every `gSfTickReload` mixer blocks of 192 samples. The reload is `rate/60/192 + 0.5` (L5334-5337), so ticks run at about 57-60 Hz. `dt` (global 0x8013ACC8) is `(samples_per_tick*0x7D00/mixerRate)*8` (L3865), in units of 1/256 ms, and every time field (`ch+0x40`, `0x78`, `0x98`, `0xA4`, ...) uses those units. A macro time field is either `u16<<8` (ms) or tempo-scaled via `*32000/budget<<3`.

The mixer works in 192-sample blocks (`musyxMixActiveVoices` L5013), and the RSP ucode (`aspMain`) does the actual DSP. On the CPU side, `musyxKeyOnVoice` (L4948) and `musyxBuildVoiceCommandList` (L5045) build a 0x50-byte record per voice holding a pitch step, 4 gain ramps (start and end) and the sample stream position.

### Corrections to earlier opcode notes

- Opcode 0x1C (`soundMacroOpSetPanSweep`, L3189) sets up the vibrato (a sine pitch LFO), not a pan sweep. It sets flag `0x2000`, the period `ch+0x78` and the depth bytes `ch+0x80` and `ch+0x81`. One bit in the opcode sets flag `0x4000` (mod-wheel scaling).
- Opcode 0xE (named `soundMacroOpSetVolumeRamp`, L3312) is the pan sweep. It sets flag `0x8000`, `ch+0x38` (start), `ch+0xA8` (end), `ch+0xA4` (time) and `ch+0x3C` (step).
- `ch+0x4C/0x4D` hold the sawtooth period in ticks (see 1b), not LFO depths.
- `ch+0x28/0x2C/0x2E` hold voice-steal priority (see 3 and 4), not volume or send.
- Opcode 0x1F (`soundMacroOpStartPitchEnvelope`, L3241) sets the target playback rate from an absolute Hz value. It does not set a portamento time.
- Opcode 0x11 calls `musyxStartVoiceRelease(voice, 1)`: a 20 ms (0x14) release fade. Opcode 0x12 only sets `ch+0x24` bit `0x80`; `tickAllAudioChannels` (0x8008CEA0) then clears bits `0x90` and calls `musyxStopVoice` (voice slot +1 = 0, an immediate cut) unless the sustain flag `0x04000000` holds it.
- Opcode 0x17 (`soundMacroPlayRandomKeyNote`, 0x8008B0A8) picks a key in `[b0, b2]` (absolute), or `[note - b2, note + b0]` clamped to 0..127 when w1 byte 2 is set. The detune is `b1`, or `rand % 201 - 100` when w1 byte 3 is set. RNG: `seed *= 0xA8351D63` (global 0x800A0F54), value `(seed >> 6) & 0xFFFF`. The command then rewrites itself in the macro data as op 0x19 (absolute key, that detune, no wait) and runs it, so the first pick sticks for every later play.
- The command word layout is `[b0][b1][b2][op]` (big-endian), with the opcode in the low byte (`ACC0 & 0x7F`). That is why m2c's `ACC0 >> 8` looks odd.

## 1. LFOs (three separate mechanisms)

### 1a. Vibrato: sine pitch LFO (opcode 0x1C; flag 0x2000). READ (asm), setter CONFIRMED (L3189-3225)

Setup in `soundMacroOpSetPanSweep`:
- `ch+0x78` is the period in dt units.
- `ch+0x80` is the depth in semitones and `ch+0x81` a 0..100% fine blend.
- A negative depth byte gives depth = -v and starts the phase at period/2.
- `cmd[0]&3` sets flag `0x4000`, which lets the mod wheel scale the depth.

Per tick (`tickAllAudioChannels` 8008CC.. / 8008D204-8008D3C4):
```
ch+0x74 += dt                                   ; phase (8008CB30-8008CB8C)
ang = ((ch+0x74 % ch+0x78) << 4) / (ch+0x78 >> 8)   ; 0..4095, 4096 steps/cycle
ch+0x7C = sine(ang)                              ; sign-extended s16
```

The waveform is a sine from `lookupSineFromAngleTable` (L403) and table `D_main_800A0F58`: a 1024-entry quarter wave folded to 4096 angles, with amplitude 4096 (entries 0, 6, 12, ... 4095, i.e. `4096*sin`).

Pitch application (`s2` is the 16.16 playback rate):
```
r = s2>>16
if (ch7C >= 0): p = scaleByPitchTableA(r, ch+0x80)     ; r * 2^(depth/12)   (table 0x800A2FD0, 12.20 ratios)
else            p = scaleByPitchTableB(r, ch+0x80)     ; r * 2^(-depth/12)  (table 0x800A31D0)
if (ch+0x81): p += (ratio(p) - p) * ch81 / 100         ; 0x51EB851F = /100 magic
d  = (ch7C * (p - r)) >> 12                            ; linear in RATE, not cents
a0 = flag0x4000 && slot!=0xFF ? (CC1_14bit >> 7) : 0x40
s2 = clamp( (s2>>16) + (d>>6)*a0 , 0, 0x7FFF ) << 16   ; (d<<10)*a0 in 16.16
```
At the sine peak the pitch moves by +/- `ch+0x80` semitones, with a0 = 64 meaning 1.0x; the mod wheel (CC1 >> 7, 0..127) scales that from 0 to about 2x. A negative `ch7C` uses the "down" table, so the sweep is roughly symmetric in log-pitch.

The two fine-blend constants, `scaleFixedPointByRatioA/B`, are 0x10D390/2^20 = 1.0516 and 0xF1A1C/2^20 = 0.9439 (94A80.c L68-120). A is slightly below 2^(1/12) = 1.0595; the code really uses that value.

### 1b. LFO1/LFO2 as sawtooth pitch ramps (opcodes 0x1D/0x1E). CONFIRMED (setter L3298), READ (consumer, 8008CC00-8008CC80, 8008D3C4)

The setter (`soundMacroOpStartVibrato`, i = 0/1) clears the accumulator `ch+0x44[4i]`, sets `ch+0x4C+i` and `ch+0xAC+i` to the period in ticks (from `cmd>>8`), and sets `ch+0x84[4i]` = `divideByMixerBaseFrequency(u16 cmd>>16) << 16`. That last value is a per-tick step in 16.16 playback-rate units, given in Hz by the macro.

Each tick:
```
if (ch[0x4C+i] != 0) {
    if (--ch[0xAC+i] == 0) { ch[0xAC+i] = ch[0x4C+i]; acc[i] = 0; }
    else                    acc[i] += step[i];
}
```

Then `s2 += acc0 + acc1` (8008D3C4-8008D3F4), which adds a rising sawtooth to the 16.16 playback rate that restarts every `period` ticks. There is no sine and no depth parameter; the amplitude is `step * period`. It ignores dt and advances once per tick call.

### 1c. Two general sine LFOs feeding the controller records. CONFIRMED (setter L3227), READ (8008CA40-8008CB08)

Opcode 0x50 (`soundMacroOpSetProgramTableValue`) sets the period of table entry n (stride 0xC) at `ch+0x168+12n` to `(w>>16)<<8`, and resets the phase `ch+0x164+12n` if the old period was nonzero. Each tick, for n = 0 and 1:
```
ph += dt; ang = ((ph % period) << 4) / (period>>8); out(ch+0x16C / ch+0x178) = sine(ang)   ; +/-4096
```

`evalModRecE` (L675-750) reads them as controller sources `0xA0` (`f16C*2`) and `0xA1` (`f178*2`), i.e. +/-0x2000 around center (section 5). Tremolo, auto-pan or pitch wobble come from pointing a record at one of these sources with sequence/macro commands 0x40..0x46, which remap source ids 0x82 and 0x83 to `0xA0` and `0xA1` (`soundMacroPushEffectEvent`, L3647).

## 2. Pan: from `ch+0x38` to L/R gains

### Pan value path (READ, tickAllAudioChannels 8008D5F8-8008D690)

1. `s3 = ch+0x38` (16.16, 0..0x7F0000). Three things write it:
   - op 0x0B, key-scaled (8008C330-8008C394): `pan = clamp( (b0<<16) + ((key - b1) * (s8)b2 << 16) >> 7 )`, i.e. `b0 + (key - b1) * b2 / 128` in 0..127 units (`key` is `ch+0x54`);
   - op 0x0E, the linear pan sweep under flag `0x8000` (8008CF64-8008D05C): `pan = target - (remaining>>8) * step`, clamped to 0..0x7F0000;
   - the initial value `ch+0x57<<16`.
2. With a sequencer slot (`ch+0x4E != 0xFF`) and `s3 != 0x800000`, CC10 adds an offset around center 64: `s3 = clamp(s3 + (((rec1 >> 7) - 0x40) << 16), 0, 0x7F0000)`. Pan `0x800000` (128) is a special surround/anti-phase value and skips this adjustment.
3. When the global mono flag (byte `0x801483C8`) is set, `s3 = 0x400000` (center) and `span = 0`.
4. `setVoiceModulationParams(i, vol, pan=s3, span, reverb)` (L5698) calls `computeVoiceVolumePanCoefficients(v+0x46, v+0x44, vol, pan, span, v+0x48, rev, v+0x4A)`.

### Gain law (READ, `computeVoiceVolumePanCoefficients`, 94A80.c L222-384 / asm/nonmatchings/main/94A80/...)

Constants (`D_main_800A4DD8`): k0 = 1.5259e-5 (1/65536), k1 = 0.7079, k2 = 32767.0, k3 = 2.38e-7 (1/2^22). The pan table `T` (`D_main_800A4DE8`) is {0, 0.7079, 1.0}, and the volume curve `C` is the 128-entry float table at 0x800A4BD8.

```
V   = C[vol>>16] lerp (vol&0xFFFF)*k0           ; vol clamped to 0x7F0000
SF  = T[(0x7FFFFF-span)]  ; SB = T[span]         ; piecewise-linear 3-point law
backGain  (v+0x48) = V * SB * k1 * k2
V' = V * SF                                      ; front reduced as span rises
pan == 0x800000:  Rout = +V'*k1*k2 ; Lout = -V'*k1*k2   (phase inverted pair)
else:             Rout (v+0x46) = V' * T[pan]       * k2
                  Lout (v+0x44) = V' * T[0x7FFFFF-pan] * k2
aux  (v+0x4A) = C[rev>>16] lerp * k1 * k2
```

`T` is evaluated as `T[x>>22] + (T[(x>>22)+1] - T[x>>22]) * (x & 0x3FFFFF) * k3`, which makes the pan law a 3-point piecewise-linear approximation of equal power rather than a sin/cos table. Pan 0 gives L = 1.0 and R = 0; center (64) gives L = R = 0.7079, about -3 dB; pan 127 gives R about 0.99 and L about 0.011.

The volume curve `C` rises from 0 to 1.0 at index 127 roughly as x^2.2 (index 32 is 0.0496, 64 is 0.214, 96 is 0.54, from the data file). Outputs are s16 with full scale at 32767.

### How the gains reach the RSP (READ, `musyxRenderVoiceSamples` 80090908-800909F4)

For each 192-sample block, `target[k] = (s16 gain[k] at v+0x44/0x46/0x48/0x4A) * envelope(v+0x5C, f32 0..1)`, truncated to s16. The targets are stored to `v+0x18/0x1A/0x1C/0x1E` and copied into the cmd entry at `+0x66/0x68/0x6A/0x6C`, and the previous block's targets (`v+0x20..0x26`) go to `+0x6E..0x74`. `musyxKeyOnVoice` (L4948) turns each pair into `cur<<16` and `step = ((target - cur)<<16) / blockLen`, so the ucode ramps each gain linearly across the block. The 4 channels are L, R, S (surround/rear) and AUX (reverb send).

The gain law is CONFIRMED (matched or hand-written asm read in full); the pan law constants are READ values.

## 3. Volume

`tickAllAudioChannels` builds the volume passed to the coefficient function:

1. Macro volume `V0 = ch+0x30` (16.16, 0..0x7F0000). `soundMacroSetVoiceVolume` (op 0xD, L3442) sets it to `vol*b2/128 + (b1<<16)`, clamped and optionally remapped through a 128-entry curve table (`evaluateSoundMacroCurveTable`, id in `b0`). `soundMacroSetupVolumeRamp` (op 0xF, L3521, flag `0x10000`) ramps it as `ch+0x30 = target - (remaining>>8) * step`, clamped to 0..0x7F0000. The reset value is `ch+0x56<<16`.
2. Each following stage multiplies by `x/128`, i.e. `V = (V>>7) * x` (8008D528-8008D5F0):
   - `x = lh(0x80141B30 + 0x28*ch[0xBA])` and `x = lh(0x80141B18 + 0x28*ch[0xBA])`, two fader values for the channel's group `ch+0xBA` (a table of 32 faders, stride 0x28, ramped at 8008D73C-8008D92C);
   - the master fader `lh(0x80141B18 + (ch[0x50]==0 ? 0x348 : 0x370))` (fader 21 or 22, with `ch+0x50` choosing music or SFX);
   - if `ch+0x2F != 0xFF`, the per-group volume `x = byte 0x80141AD8[ch+0x2F]`;
   - if the sequencer slot `ch+0x4E != 0xFF`, CC7 as `x = rec0 >> 7` (14-bit to 7-bit, default 0x7F).
3. The resulting `vol` goes through the volume curve `C` (section 2).
4. The ADSR envelope is applied later, per block, on the voice slot rather than the channel (see below).

`ch+0x28` and `ch+0x2C` play no part in volume. `ch+0x28` is a voice-steal priority that fades by `lhu(ch+0x2C) * dt` each tick (8008CF24-8008CF60), and `allocateSynthVoice` uses it as a tie-break (see 4). Opcode 0x31 sets it (`u16<<16`), 0x30 adds a signed value with clamping, and 0x38 sets the rate (`ch+0x2C = (ch28>>8)/time`). The setters are CONFIRMED as C (L2936 `setChannelProgHalfwordAt28`, `soundMacroOpSetPitchBendGlide` L2941); the consumers are READ.

The envelope is CONFIRMED (`advanceVolumeFadeEnvelope` L4709, `musyxTriggerVoiceAttack` L5666, `musyxStoreVoiceAdsrParams` L5653). Each voice runs attack, decay, sustain and release stages, linear in time, with `t = elapsed / (duration<<16)` and `cur = start + (target - start)*t`. Attack goes from 0 to 1.0 over `v+0x28`, decay from 1.0 to the sustain level (`p[2]/4096.0`) over `v+0x2A`, and release from `cur` to 0 over `v+0x30` (default 0x14 = 20 ms). The unit is 1 ms; the timer advances `(blockLen*32000/mixerRate)<<11` per block, 6 units per 192 samples at 32 kHz. Op 0xC (`musyxSetVoiceAdsrFromTable`, L3276) loads the ADSR values as byte-swapped u16s from a keyed table.

Velocity has no separate stage in the code covered here; it is folded into the macro volume (op 0xD scales by `b2`, and key-on sets `ch+0x56`). Whether anything else scales by velocity is UNRESOLVED, since the key-on path in `startNoteOnVoice` is still asm.

## 4. The "send byte" `ch+0x2E` is voice-steal priority

Op 0x36 (`setChannelProgByteAt2E`, L2964: `ch[0x2E] = cmd>>8`) and op 0x37 (`soundMacroOpAddKeyByte`, L2951: add, clamped 0..255) write it, and voice stop zeroes it (L2677, 2687, 2792, 3890, 3915, 3949 and in `tickAudioChannel`). The only reader is `allocateSynthVoice` (asm, 80086684-800866BC), which compares `ch+0x2E` first and then `ch+0x28` to choose which channel to steal; nothing in `832A0.c` or `tickAllAudioChannels` reads it. Writers are CONFIRMED, the consumer READ.

The real reverb send is the CC91 record (`rec8`, `ch+0x150`) multiplied by the volume (sections 5 and 7).

## 5. Controller records (`ch+0xC0..0x160`)

There are nine records, stride 0x12, each `{ {ctrl, op, s16 scale} x4, u8 count }`. `initVoiceModulatorRecords` (L612) and the reset path in `tickAudioChannel` initialize them, and `evaluateVoiceModulatorRecord` (`evalModRecE`, L675) evaluates them. The `...Zero..Eight` wrappers (L752-800) pass the offsets `0xC0 + 0x12*k`.

Controller values come from `getChannelControllerValue` (L580), which reads per-slot/per-track byte tables (`0x80140A10` and `0x801440C8`, 0x86 bytes per row). Defaults (`resetChannelStateRecord`, L562) are CC7=0x7F, CC10=0x40, 0x80=0x40, 0x81/0x40/0x41/0x5B/0x83/0x85=0 and 0x84=0x40.
- ctrl < 0x40: 14-bit `(tbl[c]<<7)|tbl[c+0x20]`.
- ctrl 0x80/0x81: 14-bit pitch bend (`tbl[0x80]<<7 | tbl[0x81]`).
- 0x40..0x45: switches (0x3FFF if >= 0x40, else 0).
- Others: `tbl[c] << 7`.

For each entry, bipolar sources (ctrl 1, 0xA, 0x80, 0x83, 0x84, 0xA0, 0xA1) give `m = clamp(((ccv-0x2000)*scale)>>8, +/-0x2000)`, and other sources give `m = (ccv*scale)>>8` clamped to 0x3FFF. `op` is 0 = set, 1 = add (clamped) or 2 = multiply (>>13 bipolar, >>14 unipolar). `scale` defaults to 0x100 = 1.0.

Macro/sequence opcodes 0x40..0x46 (`seqCmdHandler40..46`, L3683-3757, via `soundMacroPushEffectEvent`, L3647) append entries: 0x40 to rec0, 0x41 to rec1, 0x42 to rec3, 0x43 to rec5, 0x44 to rec6, 0x45 to rec7 and 0x46 to rec8. Rec2 and rec4 cannot be pushed to. A record holds at most 4 entries, and an op-0 push (or the first push in a macro) clears it first. Pushing remaps sources 0x81 to 0x82, 0x82 to 0xA0 (LFO A), 0x83 to 0xA1 (LFO B), 0x84 to 0x83 and 0x85 to 0x84, and sets `scale = (s16 * 256)/100`, a percentage.

What each default record controls (READ, `tickAllAudioChannels`):

| rec | offset | ctrl | use |
|---|---|---|---|
| 0 | 0xC0 | 7 | Volume multiplier: `V = (V>>7) * (val>>7)` (8008D5F8-8008D614). |
| 1 | 0xD2 | 0xA | Pan offset: `pan += ((val>>7)-0x40)<<16`, clamped 0..0x7F0000. |
| 2 | 0xE4 | 0x83 | Span: `span = val<<9` (14-bit to 23-bit), clamped to 0x7F0000. Rear/surround amount, default 0. |
| 3 | 0xF6 | 0x80 | Pitch bend, evaluated at 8008D110 and cached in `ch+0xB4`. See below. |
| 4 | 0x108 | 0x84 | Final pitch scale: `rate = ((rate>>16) * val) >> 13`, unity = 0x2000 (8008D3FC-8008D414). |
| 5 | 0x11A | 1 | Mod wheel: scales vibrato depth when flag 0x4000 is set (`val>>7`, 64 = 1.0x). Opcode 0x0A also branches on the CC1 value (random/conditional). |
| 6 | 0x12C | 0x40 | Sustain pedal: `val > 0x1F80` sets flag `0x04000000` (hold, blocks macro key-off waits), else clears it (8008CC8C-8008CCEC). |
| 7 | 0x13E | 0x41 | Portamento switch, see 6. |
| 8 | 0x150 | 0x5B | Reverb send: `rev = min(0x7F0000, ((val>>7) * vol) >> 7)`, where `vol` is the final post-fader volume including CC7. Forced to 0 when `ch+0x4E == 0xFF` (8008D6B0-8008D6E4). |

Pitch bend (rec3) uses `b = val - 0x2000` (+/-8192). For `b < 0`, `idx = (-b * ch[0x6E]) >> 7` with the low 7 bits as the fraction, through `computeNoteRatePairFromTableB`; for `b > 0` it uses `ch[0x6F]` and the A tables. The range bytes default to 2/2 (op 0x33 sets them) and are in semitones: the tables have 768 steps per octave (64 per semitone), so b = 8192 with range 2 gives idx 128. The result interpolates linearly between adjacent table rates.

The records and defaults are CONFIRMED (matched C), as is the evaluation math; the consumers are READ (asm).

## 6. Portamento and pitch target

The behavior is READ from asm; the setters are CONFIRMED in C.

Opcode 0x1B (`soundMacroSetPitchTarget`, L3804) sets `ch+0xA0` from a u16 field (mode 1 restarts from t=0) and the glide time `ch+0x98`, which is `u16<<8` (ms) when the flag bit is set and tempo-scaled otherwise. `op[2] == 0` disables portamento (clears 0x800, sets 0x1000). `op[2] == 1` enables it; if it is not already active, the opcode sets the elapsed time `ch+0x94` to `(A0 != 1) ? time : 0` (so the glide either starts finished or restarts), the start rate `ch+0x9C` to `musyxComputeNotePlaybackRate(ch+0xBD = previous key, ch+0x60) << 16`, and the flags to `0x1800`.

Opcode 0x1F sets the target rate in Hz: `ch+0x70 = divideByMixerBaseFrequency(hz)<<16` (flag 0x100).

Each tick (8008D05C-8008D0D0), when flag `0x800` is set and `ch+0x94 < ch+0x98`:
```
t     = (ch94<<8) / (ch98>>8)                 ; 0..~65536
s2    = t*int(ch+0x70) + (0x10000-t)*int(ch+0x9C)   ; 16.16
ch94 += dt
```
Otherwise `s2 = ch+0x70`. The glide is linear in playback rate (Hz ratio) over the fixed time `ch+0x98`, with no time constant and no cents curve. Reset sets the default time to 500 units (about 2 ms).

CC 0x41 (rec7) can also turn portamento on when no macro owns it (flag 0x1000 clear, voice not yet attacked, `ch+0x4E != 0xFF`). A value above 0x1F80 sets flag 0x800 with the same start rate, and a value at or below 0x1F80 clears it (8008CD04-8008CE5C). Reset initializes the previous-key memory from the per-slot table `0x80142040` / `0x8013C460`.

## 7. Reverb / effect processor

The N64 audio path does have a reverb (CONFIRMED), but its parameters are only partly resolved.

CPU side (CONFIRMED):
- The aux send `v+0x4A` is ramped per block like L/R/S (section 2) and goes into the RSP voice record as the aux channel.
- In `initN64RspMixer` (L5301), `ms` is a reverb delay length. It computes `n = rate*ms/1000`, rounded up to a multiple of 192 plus 192, and allocates a 16-bit mono delay buffer `D_main_bss_80149770 = rs_malloc(n*2)`.
- `buildSongTempoSchedule` (L4539) is really a reverb-preset builder. It takes a "song" struct (`song` in the C) and is exposed to the game as `syncMusyXIfActive` (L4274, which calls `tickAudioBufferUpdate`). It builds a 0x48-byte descriptor at `D_main_bss_80149768`:
  - `buf` and `sz` = (song[0] ms * rate / 1000) rounded to 192 + 192, zeroed with `memset`;
  - `a8 = song[0x1A]`, the tap count (up to 8), and `aA = song[0x1B]<<8`;
  - tap delays in samples, `w[i] = ((rate * u16 delay_ms_i) / 1000 + 3) & 0xFFFC`;
  - tap gains `b[i] = song[0x12+i]<<8`;
  - filter/mix coefficients `c[0..3] = s16 song[0x1E..]` if `song[0x1C] == 1`, else the pass-through `c = {0,0,0,0x7FFF}`.
- `musyxBuildVoiceCommandList` (L5045) writes the descriptor pointer and the current position into the frame record at `+4/+8/+0xC` (`80149764` is the mix-state buffer, `8014976C` the position in samples, advanced by 192 per block and wrapped at `*(blk+4)`), and `pos/192` at `+2`.
- The game fills the preset itself. `cueMenuScreenAudioA` (funcs_34.c ~3195-3209), `cueMenuScreenAudioB` and `runModelViewerSoundDemo` (funcs_37.c ~6490) write fields at +0x1A, +0x1C and +0x20..+0x24 (values such as 0x1999 and 0x800) and then call `syncMusyXIfActive`, so each screen (menus, model viewer) configures its own reverb.
- The per-voice send is rec8 (CC91) times the post-fader volume, through the volume curve (section 5). It only applies when the voice has a sequencer slot (`ch+0x4E != 0xFF`); plain SFX macro voices send nothing, so level SFX get no reverb unless the game sets CC91 on them.

UNRESOLVED:
- The delay/feedback/filter algorithm, and how it uses `aA` and `c[]`, lives in the RSP ucode (`aspMainText.s` / the project's `rsp/musyx_rsp.toml`). Its aux/reverb section has not been read yet.
- The presets the game ships (the real ms and tap values in the `song` structs) are game data, not code. Dumping the preset the callers pass, or watching `0x80149768` during a run, would recover them.

## Summary of evidence level per question

| Q | Status |
|---|---|
| 1 LFO1/2 | READ, setters CONFIRMED. LFO1/2 are sawtooth pitch ramps (period in ticks); vibrato is a separate sine LFO, and 2 more sine LFOs feed the controller records. Wave table CONFIRMED (peak 4096). |
| 2 Pan | Pipeline READ (asm). Gain law CONFIRMED (hand asm fully read, table data read). |
| 3 Volume | READ (`tickAllAudioChannels`). ADSR CONFIRMED (C). `ch+0x28/2C` are priority, not volume. Velocity UNRESOLVED (`startNoteOnVoice` is still asm). |
| 4 `ch+0x2E` | CONFIRMED as voice-steal priority, not a reverb send. |
| 5 Controller rings | CONFIRMED (records/eval C), consumers READ. |
| 6 Portamento | READ, setters CONFIRMED. Linear in rate over a fixed time. |
| 7 Reverb | Existence CONFIRMED (aux gain, delay-buffer descriptor, game presets). The RSP ucode algorithm is UNRESOLVED. |
