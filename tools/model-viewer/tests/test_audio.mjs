// node tools/model-viewer/tests/test_audio.mjs <rom.z64>   (US v1.0 ROM)
import { readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { createRequire } from 'node:module';
import assert from 'node:assert/strict';
createRequire(import.meta.url)('../rs64assets.js');
const { RS64 } = globalThis;
const romPath = process.argv[2];
assert.ok(romPath, 'usage: test_audio.mjs <rom.z64>');
const buf = readFileSync(romPath);
const rom = RS64.openRom(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.length));
const bytes = (dv) => new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength);
const sha1 = (pcm) => createHash('sha1').update(new Uint8Array(pcm.buffer, pcm.byteOffset, pcm.byteLength)).digest('hex');
// PCM hashes from the reference decoders tools/musyx_n64_decode.py and tools/mort_decode.py (by-ear verified).
const SAMPLE_REF = [[0, 17093, '93ef0521f4e492327dd2fb868ec7a76f4c12a11c'], [1, 25361, '0c6b4ae8e38c7288a7cd74ac3ed1a12cc490fbd1'],
  [2, 40545, 'f65a4c9b81c92be0c740f6d5d02d505b0faf85aa'], [3, 15897, '4981774f271929f7dd3a9aab86b1a88028562876'], [4, 41851, 'eed6dd75f50cd22d53328940204a77b7acc2c57d']];
const VOICE_REF = [[0, 2214, 'e4fe08b49e639c4b1958490fe088902fa4a2ad9c', 8000], [5, 36040, 'd09a153da0deacf62a1072f426ed336471a81ec2', 8000],
  [12, 22490, 'ce97c8c491511cea3c45d4354148d4b359c4de2a', 8000], [14, 17730, '7972767369eb9c45ff24215d7b5af753d527772e', 8000],
  [17, 34464, 'a653939076624641b3b6d4c49166f768403bd21a', 8000], [35, 6858, '72a19f7a0101eeff990541150f21a2908a9b8b6e', 8000],
  [90, 141454, '0dbc8be1d124ab3fef3cf10f736e144197341a9e', 16000], [4, 67058, '4276fd3b1667d70350903a21bbab2338b79c2f4e', 8000]];
let failed = 0;
const check = (name, fn) => Promise.resolve().then(fn).then(() => console.log('ok  ', name), (e) => { failed++; console.log('FAIL', name, e.message); });

// ---- samples ----
const sdir = RS64.parseSdir(await rom.read('data/sound/sdir_SND'));
const samp = bytes(await rom.read('data/sound/samp_SND'));
await check('sdir has 165 entries', () => assert.equal(sdir.length, 165));
for (const [idx, n, hash] of SAMPLE_REF) await check(`sample ${idx} matches reference`, () => {
  const e = sdir[idx];
  const pcm = RS64.decodeMusyxAdpcm(samp, e.off, e.num);
  assert.equal(pcm.length, n);
  assert.equal(sha1(pcm), hash);
});
await check('zero-length sample decodes to empty', () => assert.equal(RS64.decodeMusyxAdpcm(samp, 0, 0).length, 0));
await check('wav header', () => {
  const w = RS64.wavBytes(new Int16Array([1, -1, 300]), 8000);
  const dv = new DataView(w.buffer);
  assert.equal(String.fromCharCode(...w.slice(0, 4)), 'RIFF');
  assert.equal(dv.getUint32(4, true), 36 + 6);
  assert.equal(dv.getUint32(24, true), 8000);
  assert.equal(dv.getUint32(40, true), 6);
  assert.equal(dv.getInt16(44, true), 1);
  assert.equal(dv.getInt16(46, true), -1);
});

// ---- voice ----
const speechDv = await rom.read('data/speech');
const speech = RS64.parseSpeech(speechDv);
await check('speech has flag-1 and 16 kHz entries', () => {
  assert.ok(speech.some((e) => e.flag === 1));
  assert.ok(speech.some((e) => e.freq === 16000));
});
const vBy = new Map(speech.map((e) => [e.index, e]));
for (const [idx, n, hash, freq] of VOICE_REF) await check(`voice ${idx} matches reference`, () => {
  const e = vBy.get(idx);
  assert.equal(e.freq, freq);
  const pcm = RS64.decodeMort(bytes(speechDv), e.offset, e.size);
  assert.equal(pcm.length, n);
  assert.equal(sha1(pcm), hash);
});
const romBytes = new Uint8Array(buf.buffer, buf.byteOffset, buf.length);
const txt = await rom.read('data/voice_TXT');
const vt = RS64.parseVoiceTable(romBytes, txt);
await check('voice table', () => {
  assert.equal(vt.voiceMap.length, 0x2F4);
  assert.equal(vt.strings[5].slice(0, 7), 'Rogues!');
  assert.equal(RS64.parseVoiceLines(romBytes, txt)(5), vt.strings[vt.voiceMap[5]]);
});
await check('front text: mission names by level', async () => {
  const front = RS64.parseFrontText(await rom.read('data/front_TXT'));
  assert.equal(front[0], 'AMBUSH AT MOS EISLEY');
  assert.equal(front[3], 'DEFECTION AT CORELLIA');
  assert.equal(front[18], 'BATTLE OF HOTH');
});
const mv = RS64.parseMenuVoice(romBytes);
await check('menu voice groups', () => {
  assert.deepEqual(mv.crafts.map((g) => g.length), [2, 2, 3, 3, 4, 3, 3]);
  assert.deepEqual(mv.roster.map((g) => g.length), [4, 3, 4, 3, 3, 3]);
  assert.deepEqual(mv.objectives[0], [704, 705, 706, 707]);
  assert.deepEqual(mv.objectives[13], [740, 739]);
  assert.equal(mv.briefings.length, 19);
  for (const g of [...mv.crafts, ...mv.roster]) assert.equal(new Set(g.map((v) => vBy.get(vt.voiceMap[v]).freq)).size, 1, 'parts share a rate');
});

createRequire(import.meta.url)('../voice_cats.js');
await check('voice categories cover every non-silent sample', () => {
  const cats = globalThis.RS64_VOICE_CATS;
  const missing = speech.filter((e) => e.index && !cats[e.index]).map((e) => e.index);
  assert.deepEqual(missing, []);
  assert.ok(Object.values(cats).every(([c, l, s]) => typeof c === 'string' && Array.isArray(l) && ['code', 'text'].includes(s)));
});
await check('menu tables decide briefing / objective / hangar / roster categories', () => {
  const cats = globalThis.RS64_VOICE_CATS, cat = (v) => cats[vt.voiceMap[v]];
  for (const v of mv.briefings) assert.equal(cat(v)[0], 'Mission select: briefings');
  mv.objectives.forEach((g, l) => g.forEach((v) => {
    assert.equal(cat(v)[0], 'Mission objectives');
    assert.ok(cat(v)[1].includes('lv_' + l.toString(36)));
  }));
  for (const g of mv.crafts) for (const v of g) assert.equal(cat(v)[0], 'Hangar: craft descriptions');
  for (const g of mv.roster) for (const v of g) assert.equal(cat(v)[0], 'Pilot roster bios');
});
await check('cutscene voice events', async () => {
  const cut = RS64.parseCutsceneVoice(await rom.read('data/cuts/id1_intro'));
  assert.deepEqual(cut.events, [{ voiceId: 74, at: 0.5 }, { voiceId: 75, at: 14.05 }]);
  assert.equal(cut.length, 21.5);
  const cat = (v) => globalThis.RS64_VOICE_CATS[vt.voiceMap[v]];
  assert.deepEqual(cat(309).slice(0, 2), ['Cutscene: intro', ['lv_0']]);
  assert.equal(cat(113)[0], 'Cutscene: medal ceremony');
});
await check('wingman chatter split by pilot (npcWingmanBehaviorByMode)', () => {
  const cat = (v) => globalThis.RS64_VOICE_CATS[vt.voiceMap[v]][0];
  assert.equal(cat(0x1E4), 'Wingman: Wedge');
  assert.equal(cat(0x1FD), 'Wingman: Wedge');
  assert.equal(cat(0xBC), 'Wingman: Kasan');
  assert.equal(cat(0x26), 'Wingman: Rogue Ten');
});
// ---- SFX ids (SoundMacro render) ----
const sfxBank = { pool: RS64.parsePool(await rom.read('data/sound/pool_SND')), sdir: new Map(sdir.map((e) => [e.id, e])), samp };
const sfx = RS64.parseSfxTable(await rom.read('data/sound/proj_SND'));
await check('sfx table: 128 ids, layers 0x15 / 0x2B', () => {
  assert.equal(sfx.length, 128);
  assert.deepEqual(sfx.filter((s) => s.macro & 0x8000).map((s) => s.id).sort((a, b) => a - b), [0x15, 0x2B]);
  assert.equal(sfx.find((s) => s.id === 0x80).macro, 0x11B);
});
await check('trigger counters map to voice lines (addBooleanCountHandleWrapper)', () => {
  // lv_0 Sayline_Hey: counter #20 += 1 -> lv0_runDialogueSequenceA plays 0x26E, 0x148, 0x270.
  assert.deepEqual(globalThis.RS64_TRIGGER_VOICES.lv_0.c[20], [0x26E, 0x148, 0x270]);
});
await check('shared speech calls: lv_e Seerdon / bacta lines are mission dialogue', () => {
  const cat = (v) => globalThis.RS64_VOICE_CATS[vt.voiceMap[v]];
  for (const v of [0x1D9, 0x1DA, 0x1DE, 0x2A8, 0x2A9, 0x2AA]) assert.deepEqual(cat(v).slice(0, 2), ['Mission dialogue', ['lv_e']], 'voice ' + v);
});
await check('lv_0 Sayline_Hey trigger plays its lines', async () => {
  const dat = RS64.parseDAT(await rom.read('data/level/lv_0/dat'));
  const hey = dat.items.find((it) => it.name === 'Sayline_Hey');
  assert.deepEqual(RS64.triggerVoiceIds(hey, globalThis.RS64_TRIGGER_VOICES.lv_0), [0x26E, 0x148, 0x270]);
});
await check('sfx callers from the recompiled code', () => {
  assert.ok(globalThis.RS64_SFX_CALLERS[0x35].includes('npcIncomingMissileUpdate'));
  assert.ok(globalThis.RS64_SFX_CALLERS[0x7D].includes('npcAtAtUpdate'));
});
await check('every sfx renders; nearly all produce sound', () => {
  const r = sfx.map((s) => RS64.renderSfx(sfxBank, s));
  for (const x of r) assert.ok(x.pcm instanceof Int16Array && x.rate === 22050);
  const audible = r.filter((x) => x.pcm.some((v) => Math.abs(v) > 64)).length;
  assert.ok(audible >= 120, `audible ${audible}/128`);
  assert.ok(r.every((x) => x.pcm.length <= 22050 * 10));
});

process.exitCode = failed ? 1 : 0;
