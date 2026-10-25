// node tools/web/rs64-explorer/tests/test_audio.mjs <rom.z64>   (US v1.0 ROM)
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
// PCM hashes captured from the original by-ear verified decoders (MusyX ADPCM and tools/re/mort_decode.py).
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
// A GSM 06.10 coder round trip keeps roughly 10 dB SNR on speech; a broken bit order or wrong parameter decodes to noise (< 1 dB).
const snr = (a, b) => {
  // Per 800-sample block at its best lag: the offline decoder emits 158 samples per silence frame, so silence runs shift what follows.
  let s = 0, n = 0;
  for (let o = 200; o + 1000 < Math.min(a.length, b.length); o += 800) {
    let bs = 0, bn = Infinity;
    for (let lag = -96; lag <= 4; lag++) {
      let ss = 0, nn = 0;
      for (let i = o; i < o + 800; i++) {
        ss += a[i] * a[i];
        nn += (a[i] - (b[i + lag] ?? 0)) ** 2;
      }
      if (nn < bn) [bs, bn] = [ss, nn];
    }
    s += bs;
    n += bn;
  }
  return 10 * Math.log10(s / n);
};
await check('encodeMort round-trips a stock line through decodeMort', () => {
  const e = vBy.get(5), src = RS64.decodeMort(bytes(speechDv), e.offset, e.size);
  const mort = RS64.encodeMort(src);
  const dv = new DataView(mort.buffer);
  assert.equal(dv.getUint32(0), 0x4D4F5254);
  assert.equal(dv.getUint16(4), Math.ceil(src.length / 160));
  assert.equal(dv.getUint16(6), 8000);
  assert.equal(dv.getUint32(8) * 4, mort.length);
  const back = RS64.decodeMort(mort, 0, mort.length);
  assert.ok(Math.abs(back.length - src.length) <= 320, `${back.length} vs ${src.length}`);
  const db = snr(src, back);
  assert.ok(db > 6, `SNR ${db.toFixed(1)} dB`);
});
await check('encodeMort: silence and a long line (more than 128 frames per run)', () => {
  const tone = Int16Array.from({ length: 160 * 300 }, (_, i) => Math.round(8000 * Math.sin(i * 0.05) * Math.sin(i * 0.0007)));
  const back = RS64.decodeMort(RS64.encodeMort(tone), 0, RS64.encodeMort(tone).length);
  assert.ok(snr(tone, back) > 6);
  const quiet = RS64.encodeMort(new Int16Array(800));
  assert.ok(RS64.decodeMort(quiet, 0, quiet.length).every((v) => Math.abs(v) < 64));
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
const sfxBank = { pool: RS64.parsePool(await rom.read('data/sound/pool_SND')), sdir: new Map(sdir.map((e) => [e.id, e])), samp, tables: RS64.parseMixTables(romBytes) };
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
await check('note-set ops 0x18/0x19 change the playing pitch, detune by b1 %, and wait w1 (soundMacroOpStartNote*Sweep)', () => {
  const cmd = (op, b0, b1, b2, w1) => [b0, b1, b2, op, w1 >>> 24, (w1 >>> 16) & 0xFF, (w1 >>> 8) & 0xFF, w1 & 0xFF];
  // StartSample 1; SetNote 72 + wait 768 ticks (1 s); RelativeNote +2, detune +50 %, + wait 384 ticks; End.
  const macro = new Uint8Array([...cmd(0x10, 0, 0, 1, 0), ...cmd(0x19, 0, 0, 72, 768 << 16), ...cmd(0x18, 0, 50, 2, 384 << 16), ...cmd(0, 0, 0, 0, 0)]);
  const bank = { pool: [new Map([[1, macro]]), new Map(), new Map(), new Map()], sdir: new Map([[1, { off: 0, num: 22050 * 4, rate: 22050, root: 60, loopLen: 0 }]]), samp: new Uint8Array(256 + 0x28 * 3000), tables: sfxBank.tables };
  const r = RS64.renderSfx(bank, { macro: 1, key: 60, vel: 127 });
  const p = r.pitch[0].map(([t, n]) => [+t.toFixed(2), +n.toFixed(2)]);
  assert.deepEqual(p, [[0, 60], [0, 72], [1.01, 74.5]]);
});
await check('mix tables from ROM and the pan law (computeVoiceVolumePanCoefficients)', () => {
  const T = sfxBank.tables;
  assert.ok(Math.abs(T.k[1] - 0.7079) < 1e-4);
  assert.deepEqual([...T.pan].map((x) => +x.toFixed(4)), [0, 0.7079, 1]);
  assert.equal(T.vol[127], 1);
  assert.ok(Math.abs(T.semiUp[12] - 2) < 1e-6 && Math.abs(T.fineUp[768] - 2) < 1e-6);
});
await check('waits: key-off flag ends a forever wait at keyOff; sample-end flag ends at the sample end', () => {
  const cmd = (op, b0, b1, b2, w1) => [b0, b1, b2, op, w1 >>> 24, (w1 >>> 16) & 0xFF, (w1 >>> 8) & 0xFF, w1 & 0xFF];
  const mk = (waitB0, waitB2) => new Uint8Array([...cmd(0x10, 0, 0, 1, 0), ...cmd(0x04, waitB0, 0, waitB2, 0xFFFF << 16), ...cmd(0x19, 0, 0, 72, 0), ...cmd(0x04, 0, 0, 0, 0xFFFF << 16), ...cmd(0, 0, 0, 0, 0)]);
  const ctxFor = (m, loop) => ({ pool: [new Map([[1, m]]), new Map(), new Map(), new Map()], sdir: new Map([[1, { off: 0, num: 22050, rate: 22050, root: 60, loopStart: 0, loopLen: loop ? 22050 : 0 }]]),
    samp: new Uint8Array(256 + 0x28 * 400), tables: sfxBank.tables });
  const out = () => ({ L: new Float32Array(22050 * 4), R: new Float32Array(22050 * 4) });
  const k = RS64.renderVoice(ctxFor(mk(0, 1), true), { macro: 1, note: 60, vel: 127, pan: 64, keyOff: 0.5, tickRate: 768, maxT: 4 }, out());
  assert.equal(+k.pitch[1][0].toFixed(2), +(Math.ceil(0.5 * 22050 / 384) * 384 / 22050).toFixed(2));
  const e = RS64.renderVoice(ctxFor(mk(1, 0), false), { macro: 1, note: 60, vel: 127, pan: 64, keyOff: Infinity, tickRate: 768, maxT: 4 }, out());
  assert.ok(Math.abs(e.pitch[1][0] - 1) < 0.04, 'sample end ~1 s: ' + e.pitch[1][0]);
});
await check('every sfx renders; nearly all produce sound', () => {
  const r = sfx.map((s) => RS64.renderSfx(sfxBank, s));
  for (const x of r) assert.ok(x.pcm instanceof Int16Array && x.rate === 22050);
  const audible = r.filter((x) => x.pcm.some((v) => Math.abs(v) > 64)).length;
  assert.ok(audible >= 120, `audible ${audible}/128`);
  assert.ok(r.every((x) => x.pcm.length <= 22050 * 10));
});

// ---- music ----
const songTable = RS64.parseSongTable(romBytes);
const groups = RS64.parseSongGroups(await rom.read('data/sound/proj_SND'));
await check('song table: 96 songs, each bound to exactly one group', () => {
  assert.equal(songTable.length, 96);
  assert.equal(songTable[0].name, 'c1l1_theme');
  for (const s of songTable) assert.ok(groups.get(s.id), s.name);
  assert.equal(groups.get(6).group, 4);
  assert.equal(groups.get(6).setup.length, 16);
});
await check('every _SNG parses: 3275 regions, 50717 events, all terminated', async () => {
  let regions = 0, events = 0;
  for (const a of rom.assets.values()) {
    if (!/_SNG$/.test(a.name)) continue;
    const s = RS64.parseSong(await rom.read(a.path));
    regions += s.regions.size;
    for (const r of s.regions.values()) events += r.events.length;
  }
  assert.equal(regions, 3275);
  assert.equal(events, 50717);
});
await check('note resolution: program -> keymap / layer -> macro voices', async () => {
  const pool = RS64.parsePool(await rom.read('data/sound/pool_SND'));
  let notes = 0, silent = 0;
  for (const s of songTable) {
    const g = groups.get(s.id), song = RS64.parseSong(await rom.read(`data/sound/${s.name}_SNG`));
    for (const t of song.tracks) {
      let program = g.setup[t.channel]?.program ?? 0;
      for (const r of t.regions) {
        if (r.region == null) continue;
        if (r.program !== 0xFF) program = r.program;
        for (const e of song.regions.get(r.region).events) {
          if (e.a >= 0x80 && e.b === 0) program = e.a & 0x7F;
          if (e.a >= 0x80) continue;
          notes++;
          const entry = (t.channel === 9 ? g.drums : g.progs).get(program);
          const v = entry ? RS64.resolveNote(pool, entry.target, Math.min(127, Math.max(0, e.a + r.transpose)), e.b, 0x40, 0) : [];
          if (!v.length) silent++;
          for (const x of v) assert.ok(pool[0].has(x.macro), 'macro ' + x.macro);
        }
      }
    }
  }
  console.log(`      ${notes} notes, ${silent} resolve to nothing`);
  assert.equal(silent, 34, `${silent}/${notes} silent`);
});
await check('renderSong: c1l1_theme renders; notes match events; reverb only on menu songs', async () => {
  const ctx = { ...sfxBank, groups };
  const song = RS64.parseSong(await rom.read('data/sound/c1l1_theme_SNG'));
  const t0 = Date.now(), r = RS64.renderSong(ctx, song, 6);
  console.log(`      c1l1_theme: ${r.length.toFixed(1)} s, ${r.notes.length} notes, rendered in ${Date.now() - t0} ms`);
  assert.ok(r.length > 1 && r.L.length === r.R.length);
  let nanOrSilent = true;
  for (let i = 0; i < r.L.length; i += 97) { assert.ok(Number.isFinite(r.L[i]) && Number.isFinite(r.R[i])); if (r.L[i]) nanOrSilent = false; }
  assert.ok(!nanOrSilent, 'audible');
  const evNotes = song.tracks.reduce((n, t) => n + t.regions.filter((x) => x.region != null).reduce((m, x) => m + song.regions.get(x.region).events.filter((e) => e.a < 0x80).length, 0), 0);
  assert.ok(r.notes.length <= evNotes && r.notes.length > evNotes * 0.5, `${r.notes.length} of ${evNotes}`);
  assert.equal(r.reverb, null);
  const menu = songTable.find((s) => s.id === 1);
  const rm = RS64.renderSong(ctx, RS64.parseSong(await rom.read(`data/sound/${menu.name}_SNG`)), 1, { maxSeconds: 5 });
  assert.equal(rm.reverb.delayMs, 155);
});
await check('macro ops 0x12 (voice stop) and 0x17 (random key) resolve: roguetheme and credits render with nothing unresolved', async () => {
  const ctx = { ...sfxBank, groups };
  for (const name of ['roguetheme', 'credits']) {
    const s = songTable.find((x) => x.name === name), r = RS64.renderSong(ctx, RS64.parseSong(await rom.read(`data/sound/${name}_SNG`)), s.id);
    assert.deepEqual([...r.unresolved], [], name);
  }
  assert.ok(ctx.randomKeys?.size, 'op 0x17 ran');
});
await check('tempo: a tick-0 tempo entry overrides the header BPM (hangar1 = 126, header says 175)', async () => {
  const s = songTable.find((x) => x.name === 'hangar1'), r = RS64.renderSong({ ...sfxBank, groups }, RS64.parseSong(await rom.read('data/sound/hangar1_SNG')), s.id, { maxSeconds: 1 });
  assert.deepEqual(r.bpm, [126]);
});
await check('dry per-track stems plus one reverb on their summed send equal the full render (reverb song)', async () => {
  const ctx = { ...sfxBank, groups }, menu = songTable.find((s) => s.id === 1);
  const song = RS64.parseSong(await rom.read(`data/sound/${menu.name}_SNG`)), o = { maxSeconds: 8 };
  const all = [...new Set(song.tracks.map((t) => t.index))], full = RS64.renderSong(ctx, song, 1, o);
  const L = new Float32Array(full.L.length), R = new Float32Array(full.L.length), aux = new Float32Array(full.L.length);
  for (const t of all) {
    const st = RS64.renderSong(ctx, song, 1, { ...o, dry: true, skipTracks: new Set(all.filter((x) => x !== t)) });
    for (let i = 0; i < L.length; i++) { L[i] += st.L[i]; R[i] += st.R[i]; aux[i] += st.aux[i]; }
  }
  RS64.applyMusyxReverb(L, R, aux, full.reverb);
  let m = 0;
  for (let i = 0; i < L.length; i++) m = Math.max(m, Math.abs(L[i] - full.L[i]), Math.abs(R[i] - full.R[i]));
  assert.ok(m <= 1.01, `max diff ${m} (16-bit units; float summation order can move one rounding step)`);
});

process.exitCode = failed ? 1 : 0;
