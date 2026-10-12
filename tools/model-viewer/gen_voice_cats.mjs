// Generate voice_cats.js (speech sample -> [category, levels, source]).
// node tools/model-viewer/gen_voice_cats.mjs <rom.z64> [RecompiledFuncs dir] [decomp repo]
import { readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { createRequire } from 'node:module';
createRequire(import.meta.url)('./rs64assets.js');
const { RS64 } = globalThis;
const [romPath, recomp = 'RecompiledFuncs', decomp = '../rogue_squadron64'] = process.argv.slice(2);
if (!romPath) throw new Error('usage: gen_voice_cats.mjs <rom.z64> [RecompiledFuncs] [decomp]');

const buf = readFileSync(romPath), bytes = new Uint8Array(buf.buffer, buf.byteOffset, buf.length);
const rom = RS64.openRom(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.length));
const { voiceMap } = RS64.parseVoiceTable(bytes, await rom.read('data/voice_TXT'));
const speech = RS64.parseSpeech(await rom.read('data/speech')).map((e) => e.index);
const lvId = (l) => 'lv_' + l.toString(36);
const add = (map, k, v) => (map.get(k) ?? map.set(k, new Set()).get(k)).add(v);

// Voice ids named by LINE_n event triggers (not "Spline_01").
const lineLevels = new Map();
for (const p of rom.assets.keys()) {
  const m = /^data\/level\/([^/]+)\/dat$/.exec(p);
  if (!m) continue;
  for (const it of RS64.parseDAT(await rom.read(p)).items) {
    if (it.type !== 7) continue;
    for (const s of [it.name, ...(it.strings || [])]) {
      const l = /(?<![a-z])LINE_(\d+)/i.exec(s || '');
      if (l) add(lineLevels, +l[1], m[1]);
    }
  }
}

// Constant voice ids at speech calls in the recompiled code, with the caller's address.
const SPEECH_FNS = new Set(['playSpeechClip', 'enqueueSpeechClip', 'playSpeechClipGated', 'playSpeechClipOncePerChange',
  'playSpeechClipOncePerLevel', 'playObjectiveVoiceLine1', 'playObjectiveVoiceLine2']);
const CONST = /ctx->r4 = (?:ADD32\(0, (0X[0-9A-F]+|\d+)\)|0 \| (0X[0-9A-F]+)|(0X[0-9A-F]+|\d+));/y;
const calls = new Map();
// SFX starts: bsearchFactor5TableByU16Key(id, vol, pan) is the game's SFX start; play3DSoundEvent* pass the id as their 7th argument (stack +0x18).
const SFX_ARG = { bsearchFactor5TableByU16Key: 'r4', play3DSoundEvent: 'r', play3DSoundEventVariant: 'r', play3DSoundEventF: 'r' };
const IMM = /^(?:ADD32\(0, (0X[0-9A-F]+|\d+)\)|0 \| (0X[0-9A-F]+)|(0X[0-9A-F]+|\d+))$/;
const sfxCallers = new Map();
// Per function: address, body and the constant voice ids it plays in order (for trigger handlers below).
const funcs = new Map();
const lastImm = (before, reg) => {
  const c = IMM.exec([...before.matchAll(new RegExp(`ctx->${reg} = ([^;]+);`, 'g'))].pop()?.[1] ?? '');
  return c ? parseInt(c[1] ?? c[2] ?? c[3]) : null;
};
for (const f of readdirSync(recomp).filter((f) => /^funcs_\d+\.c$/.test(f))) {
  const parts = readFileSync(join(recomp, f), 'utf8').split(/^RECOMP_FUNC void (\w+)\(uint8_t\* rdram, recomp_context\* ctx\) \{/m);
  for (let i = 1; i < parts.length; i += 2) {
    const name = parts[i], body = parts[i + 1], voices = [];
    funcs.set(name, { addr: parseInt(/\/\/ 0x([0-9A-F]{8}):/.exec(body)?.[1] ?? '0', 16), body, voices });
    for (const m of body.matchAll(/^\s*(\w+)\(rdram, ctx\);/gm)) {
      if (!SFX_ARG[m[1]] || name in SFX_ARG) continue;
      const before = body.slice(0, m.index);
      const reg = SFX_ARG[m[1]] === 'r4' ? 'r4' : 'r' + ([...before.matchAll(/MEM_[WH]\(0X18, ctx->r29\) = ctx->r(\d+);/g)].pop()?.[1] ?? 'x');
      const c = IMM.exec([...before.matchAll(new RegExp(`ctx->${reg} = ([^;]+);`, 'g'))].pop()?.[1] ?? '');
      if (c) add(sfxCallers, parseInt(c[1] ?? c[2] ?? c[3]), name);
    }
    if (SPEECH_FNS.has(name)) continue;
    const addr = parseInt(/\/\/ 0x([0-9A-F]{8}):/.exec(body)?.[1] ?? '0', 16);
    for (const m of body.matchAll(/^\s*(\w+)\(rdram, ctx\);/gm)) {
      if (!SPEECH_FNS.has(m[1])) continue;
      const ids = [];
      const constAt = (i) => {
        CONST.lastIndex = i;
        const c = CONST.exec(body);
        if (c) ids.push(parseInt(c[1] ?? c[2] ?? c[3]));
      };
      // A call right after a label with no $a0 write in between is shared: branches set $a0 in a jump's delay slot
      // ("ctx->r4 = id; goto L;"), plus whatever falls through into the label.
      const label = [...body.slice(0, m.index).matchAll(/^(L_[0-9A-F]+):$/gm)].pop();
      if (label && !body.slice(label.index, m.index).includes('ctx->r4 = ')) {
        for (const g of body.matchAll(new RegExp(`ctx->r4 = [^;]+;\\s*\\n\\s*goto ${label[1]};`, 'g'))) constAt(g.index);
        const fall = body.lastIndexOf('ctx->r4 = ', label.index);
        if (fall >= 0) constAt(fall);
      } else {
        const last = body.lastIndexOf('ctx->r4 = ', m.index);
        if (last >= 0) constAt(last);
      }
      for (const v of new Set(ids)) {
        add(calls, v, addr);
        voices.push(v);
      }
    }
  }
}

// Level of a caller = the decomp's lv_N.s file whose address range holds it.
const ranges = [];
const lvDir = join(decomp, 'asm/mission_overlay/level_objective_handling');
for (const f of readdirSync(lvDir).filter((f) => /^lv_\w+\.s$/.test(f))) {
  const ad = [...readFileSync(join(lvDir, f), 'utf8').matchAll(/\/\* [0-9A-F]+ (80[0-9A-F]{6}) /g)].map((m) => parseInt(m[1], 16));
  if (ad.length) ranges.push([f.slice(0, -2), Math.min(...ad), Math.max(...ad)]);
}

// Level scripts register handlers with addBooleanCountHandleWrapper(handler, booleanIndex, booleanValue, countIndex, countValue): the handler runs
// once that objective flag / counter is reached. Event triggers (DAT type 7) set those flags and counters, so this links triggers to voice lines.
const mission = new Set(/section_\d+_ovl_mission_funcs\[\] = \{([\s\S]*?)\};/.exec(readFileSync(join(recomp, 'recomp_overlays.inl'), 'utf8'))[1].match(/(?<=\.func = )\w+/g));
const missionByAddr = new Map([...funcs].filter(([n]) => mission.has(n)).map(([n, f]) => [f.addr, n]));
const voicesOf = (name, depth = 2, seen = new Set()) => {
  const f = funcs.get(name);
  if (!f || seen.has(name)) return [];
  seen.add(name);
  const out = [...f.voices];
  if (depth) for (const m of f.body.matchAll(/^\s*(\w+)\(rdram, ctx\);/gm)) if (mission.has(m[1]) && !SPEECH_FNS.has(m[1])) out.push(...voicesOf(m[1], depth - 1, seen));
  return out;
};
const triggerVoices = {};
for (const [name, f] of funcs) {
  if (!mission.has(name)) continue;
  for (const m of f.body.matchAll(/^\s*addBooleanCountHandleWrapper\(rdram, ctx\);/gm)) {
    const before = f.body.slice(0, m.index);
    const hi = /S32\(0X([0-9A-F]+) << 16\)/.exec([...before.matchAll(/ctx->r4 = (S32\(0X[0-9A-F]+ << 16\));/g)].pop()?.[1] ?? '');
    const lo = /ADD32\(ctx->r4, (-?0X[0-9A-F]+)\)/.exec([...before.matchAll(/ctx->r4 = (ADD32\(ctx->r4, -?0X[0-9A-F]+\));/g)].pop()?.[1] ?? '');
    if (!hi || !lo) continue;
    const handler = missionByAddr.get(((parseInt(hi[1], 16) << 16) + parseInt(lo[1])) >>> 0);
    const lv = ranges.find(([, a, b]) => a <= f.addr && f.addr <= b)?.[0];
    const ids = handler ? [...new Set(voicesOf(handler))] : [];
    if (!lv || !ids.length) continue;
    const t = (triggerVoices[lv] ??= { c: {}, b: {} });
    const b = lastImm(before, 'r5'), c = lastImm(before, 'r7');
    if (c) (t.c[c] ??= []).push(...ids.filter((v) => !t.c[c].includes(v)));
    if (b) (t.b[b] ??= []).push(...ids.filter((v) => !t.b[b].includes(v)));
  }
}


// Categories decided outright by code tables. First writer wins, except cutscenes merge their levels.
const byCode = new Map();
const set = (v, cat, levels = []) => byCode.has(voiceMap[v]) || byCode.set(voiceMap[v], [cat, levels]);
const menu = RS64.parseMenuVoice(bytes);
menu.crafts.flat().forEach((v) => set(v, 'Hangar: craft descriptions'));
menu.roster.flat().forEach((v) => set(v, 'Pilot roster bios'));
menu.briefings.forEach((v, l) => set(v, 'Mission select: briefings', [lvId(l)]));
menu.objectives.forEach((g, l) => g.forEach((v) => set(v, 'Mission objectives', [lvId(l)])));
// npcWingmanBehaviorByMode (mission 0D8450.c): voice ids per wingman kind (DAT item +0x90; 7 = silent).
// Names from the DAT items using each kind (x_wing_wedge/Wedge 0, x_wing_zev 2, x_wing_kasan 4, x_wing_dack 5) and the menu's "Rogue N standing by" lines; user-confirmed by ear.
const span = (a, n) => Array.from({ length: n }, (_, i) => a + i);
const WING = [['Wedge', [0x1E4, 0x221, 0x1EE, 0x1EA, 0x1E7, ...span(0x1FD, 6)]], ['Rogue Ten', [0x26, 0x23, 0x29, 0x24, ...span(0x1E, 5)]],
  ['Zev', [0x257, 0x254, 0x25A, 0x255, ...span(0x260, 3)]], ['Rogue Eight', [0x96, 0x93, 0x99, 0x9F, 0x94]],
  ['Kasan', [0xBC, 0xB5, 0xCB, 0xCC, 0xBB, ...span(0xD1, 4)]], ['Dack', [0x1AF, 0x1AC, 0x1B2, 0x1AD, ...span(0x1B8, 6)]],
  ['Rogue Seven', [0x3B, 0x3E, 0x41, 0x47, 0x3C]]];
for (const [name, ids] of WING) ids.forEach((v) => set(v, 'Wingman: ' + name));
// Cutscene type-0 events (idN_intro / idN_extro; id3_special is the medal ceremony).
for (const a of rom.assets.values()) {
  const m = a.dir === 'data/cuts' && /^id(\d+)_(\w+)$/.exec(a.name);
  if (!m) continue;
  const cat = { intro: 'Cutscene: intro', extro: 'Cutscene: outro' }[m[2]] ?? 'Cutscene: medal ceremony';
  for (const { voiceId: v } of RS64.parseCutsceneVoice(await rom.read(a.path)).events) {
    const prev = byCode.get(voiceMap[v]);
    if (prev && prev[0] !== cat) continue;
    byCode.set(voiceMap[v], [cat, [...new Set([...(prev?.[1] ?? []), ...(m[2] === 'special' ? [] : [lvId(+m[1])])])]]);
  }
}

// populateCutsceneSubtitleTextIds: player chatter (0x206+ when gGameSettings == 0xF, plus 0x134 / 0x272); playRandomCutsceneVoiceLine adds 0x102;
// 293-308 are craft engine/flight lines.
const PLAYER = new Set([...span(0x104, 0x31), ...span(0x206, 0xF), ...span(293, 16), 0x102, 0x272]);
set(0x278, 'Hangar: other');
const vids = new Map();
voiceMap.forEach((s, v) => add(vids, s, v));

const out = {}, counts = {};
for (const s of speech) {
  if (!s) continue;
  const levels = new Set();
  let coded = false;
  for (const v of vids.get(s) ?? []) {
    for (const lv of lineLevels.get(v) ?? []) levels.add(lv);
    for (const addr of calls.get(v) ?? []) {
      const r = ranges.find(([, lo, hi]) => lo <= addr && addr <= hi);
      if (r) levels.add(r[0]);
      else coded = true;
    }
  }
  let cat, src = 'code';
  if (byCode.has(s)) {
    cat = byCode.get(s)[0];
    byCode.get(s)[1].forEach((l) => levels.add(l));
  } else if (levels.size) cat = 'Mission dialogue';
  else if (coded || [...(vids.get(s) ?? [])].some((v) => PLAYER.has(v))) cat = 'Player chatter';
  else [cat, src] = ['No reference found (possibly unused)', 'text'];
  out[s] = [cat, [...levels].sort(), src];
  counts[cat] = (counts[cat] ?? 0) + 1;
}
console.log(counts);
const sfxOut = Object.fromEntries([...sfxCallers].filter(([id]) => id > 0).sort((a, b) => a[0] - b[0]).map(([id, s]) => [id, [...s]]));
console.log('sfx ids with constant callers:', Object.keys(sfxOut).length);
writeFileSync(new URL('./voice_cats.js', import.meta.url), '// Generated by gen_voice_cats.mjs: speech sample -> [category, levels, source (code|text)]; SFX id -> calling functions.\n'
  + 'globalThis.RS64_VOICE_CATS = ' + JSON.stringify(out) + ';\n'
  + 'globalThis.RS64_SFX_CALLERS = ' + JSON.stringify(sfxOut) + ';\n'
  + '// Level -> { c: counter index -> voice ids, b: flag index -> voice ids } played by the handler the level script registers for it.\n'
  + 'globalThis.RS64_TRIGGER_VOICES = ' + JSON.stringify(triggerVoices) + ';\n');
