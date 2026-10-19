// node tools/rs64-explorer/tests/test_modexport.mjs <rom.z64>   (US v1.0 ROM)
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import assert from 'node:assert/strict';
createRequire(import.meta.url)('../rs64assets.js');
const { RS64 } = globalThis;
const romPath = process.argv[2];
assert.ok(romPath, 'usage: test_modexport.mjs <rom.z64>');
const buf = readFileSync(romPath);
const rom = RS64.openRom(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.length));
let failed = 0;
const check = (name, fn) => Promise.resolve().then(fn).then(() => console.log('ok  ', name), (e) => { failed++; console.log('FAIL', name, e.message); });

// Same rules as librecomp's recomp::patcher::patch_rom (src/patcher.cpp), plus the checksums it skips.
function applyBPS(src, patch) {
  let p = 4;
  const num = () => {
    let data = 0, shift = 1;
    for (;;) {
      const x = patch[p++];
      data += (x & 0x7f) * shift;
      if (x & 0x80) return data;
      shift *= 128;
      data += shift;
    }
  };
  assert.equal(String.fromCharCode(...patch.subarray(0, 4)), 'BPS1');
  const srcSize = num(), dstSize = num(), meta = num();
  p += meta;
  assert.equal(srcSize, src.length);
  const out = [];
  let srcOff = 0;
  while (p < patch.length - 12) {
    const a = num(), len = Math.floor(a / 4) + 1, type = a % 4;
    if (type === 0) for (let k = 0; k < len; k++) out.push(src[out.length]);
    else if (type === 1) for (let k = 0; k < len; k++) out.push(patch[p++]);
    else if (type === 2) {
      const raw = num();
      srcOff += (raw & 1 ? -1 : 1) * Math.floor(raw / 2);
      assert.ok(srcOff >= 0 && srcOff + len <= src.length, 'SourceCopy out of range');
      for (let k = 0; k < len; k++) out.push(src[srcOff++]);
    } else throw new Error('TargetCopy is not written by encodeBPS');
  }
  const u32 = (o) => (patch[o] | (patch[o + 1] << 8) | (patch[o + 2] << 16) | (patch[o + 3] << 24)) >>> 0;
  const dst = Uint8Array.from(out);
  assert.equal(dst.length, dstSize);
  assert.equal(u32(patch.length - 12), RS64.crc32(src));
  assert.equal(u32(patch.length - 8), RS64.crc32(dst));
  assert.equal(u32(patch.length - 4), RS64.crc32(patch.subarray(0, patch.length - 4)));
  return dst;
}

await check('crc32 matches the zip/BPS polynomial', () => assert.equal(RS64.crc32(new TextEncoder().encode('123456789')), 0xCBF43926));

await check('BPS round-trips edits, growth and shrink', () => {
  let seed = 7;
  const rnd = () => (seed = (seed * 1103515245 + 12345) >>> 0) / 2 ** 32;
  for (let n = 0; n < 50; n++) {
    const src = Uint8Array.from({ length: 1 + Math.floor(rnd() * 3000) }, () => rnd() * 256);
    const dst = Uint8Array.from({ length: Math.max(1, src.length + Math.floor((rnd() - 0.5) * 200)) }, (_, i) => (i < src.length && rnd() < 0.9 ? src[i] : rnd() * 256));
    assert.deepEqual(applyBPS(src, RS64.encodeBPS(src, dst)), dst);
  }
});

await check('BPS of an unchanged asset is tiny', () => {
  const src = new Uint8Array(100000).fill(3);
  assert.ok(RS64.encodeBPS(src, src).length < 32);
});

await check('zipStore writes a well-formed archive', () => {
  const z = RS64.zipStore([{ name: 'a/mod.json', data: new TextEncoder().encode('{}') }, { name: 'a/assets/x.bps', data: new Uint8Array([1, 2, 3]) }]);
  const dv = new DataView(z.buffer);
  assert.equal(dv.getUint32(0, true), 0x04034b50);
  const end = z.length - 22;
  assert.equal(dv.getUint32(end, true), 0x06054b50);
  assert.equal(dv.getUint16(end + 10, true), 2);
  assert.equal(dv.getUint32(dv.getUint32(end + 16, true), true), 0x02014b50);
});

// Every writable texture re-encoded from its own decoded pixels must decode back the same (CI only when the image fits its palette).
const bytesOf = (dv) => new Uint8Array(dv.buffer.slice(dv.byteOffset, dv.byteOffset + dv.byteLength));
for (const path of ['data/pl_crafts/xwing_HMT', 'data/pl_crafts/falcon_HMT', 'data/frontend/factor/factor_HMT', 'data/frontend/hangar/hangar_HMT']) {
  await check(`${path} textures survive encode/decode`, async () => {
    const bytes = bytesOf(await rom.read(path));
    const before = RS64.parseHMT(new DataView(bytes.buffer));
    let written = 0;
    for (const t of before.textures) {
      if (!t.rgba || t.type > 5) continue;
      try { RS64.encodeTexture(bytes, t, t.rgba, before.textures); } catch { continue; }
      written++;
    }
    const after = RS64.parseHMT(new DataView(bytes.buffer));
    before.textures.forEach((t, i) => {
      if (!t.rgba) return;
      const a = t.rgba, b = after.textures[i].rgba;
      let worst = 0;
      for (let k = 0; k < t.w * t.rows * 4; k += 4) if (a[k + 3] >= 128) for (let c = 0; c < 3; c++) worst = Math.max(worst, Math.abs(a[k + c] - b[k + c]));
      assert.ok(worst <= 8, `${t.name}: channel error ${worst}`);
    });
    assert.ok(written > 0);
  });
}

await check('moving a level item rewrites only its floats and round-trips through BPS', async () => {
  const orig = bytesOf(await rom.read('data/level/lv_1/dat'));
  const bytes = orig.slice(), dv = new DataView(bytes.buffer);
  const before = RS64.parseDAT(new DataView(orig.buffer));
  const it = before.items.find((x) => x.type === 0 && x.model);
  dv.setFloat32(it.offset + 0x10, it.pos[0] + 12.5);
  dv.setFloat32(it.offset + 0x20, 45);
  const after = RS64.parseDAT(new DataView(bytes.buffer));
  const moved = after.items.find((x) => x.offset === it.offset);
  assert.equal(moved.pos[0], Math.fround(it.pos[0] + 12.5));
  assert.equal(moved.rot[1], 45);
  assert.equal(after.items.length, before.items.length);
  assert.deepEqual(applyBPS(orig, RS64.encodeBPS(orig, bytes)), bytes);
});

const levels = [...rom.assets.keys()].filter((p) => /^data\/level\/lv_.\/hmp$/.test(p));
await check(`terrain grid round-trips through writeHMP on all ${levels.length} levels`, async () => {
  for (const p of levels) {
    const orig = bytesOf(await rom.read(p));
    const hmp = RS64.parseHMP(new DataView(orig.buffer));
    const out = RS64.writeHMP(orig, RS64.hmpGrid(hmp));
    const again = RS64.parseHMP(new DataView(out.buffer));
    const a = RS64.hmpGrid(hmp), b = RS64.hmpGrid(again);
    assert.deepEqual(b.hts, a.hts, p);
    assert.deepEqual(b.tex, a.tex, p);
    assert.deepEqual(again.light, hmp.light, p);
    // Unedited terrain writes back byte for byte. Exceptions: lv_j and lv_k (unused extra levels) store tiles whose shared edges
    // disagree, which the grid snaps together; lv_4 stores one tile twice and its cells are pointed at the first copy.
    if (!/lv_[4jk]\//.test(p)) assert.deepEqual(out, orig, p);
  }
});

await check('a small terrain edit makes a small patch (no original tile data)', async () => {
  const orig = bytesOf(await rom.read('data/level/lv_0/hmp'));
  const grid = RS64.hmpGrid(RS64.parseHMP(new DataView(orig.buffer)));
  for (let y = 40; y < 52; y++) for (let x = 40; x < 52; x++) grid.hts[y * grid.W + x] -= 20;
  const bps = RS64.encodeBPS(orig, RS64.writeHMP(orig, grid));
  assert.ok(bps.length < 2000, `patch is ${bps.length} bytes`);
});

await check('a raised hill keeps tile edges shared and re-tiles under the index limit', async () => {
  const orig = bytesOf(await rom.read('data/level/lv_0/hmp'));
  const grid = RS64.hmpGrid(RS64.parseHMP(new DataView(orig.buffer)));
  for (let y = 0; y < grid.H; y++) for (let x = 0; x < grid.W; x++) {
    const d = Math.hypot(x - 100, y - 100);
    if (d < 30) grid.hts[y * grid.W + x] -= 60 * (1 - d / 30);
  }
  const out = RS64.writeHMP(orig, grid), hmp = RS64.parseHMP(new DataView(out.buffer)), dv = new DataView(out.buffer);
  const back = RS64.hmpGrid(hmp);
  for (let i = 0; i < grid.hts.length; i++) assert.equal(back.hts[i], Math.max(-128, Math.min(127, Math.round(grid.hts[i]))) || 0);
  for (let i = 0; i < hmp.tiles.length; i++) {
    const p = dv.getUint32(0x1C) + i * 0x1E;
    assert.equal(dv.getInt8(p + 3), Math.min(...hmp.tiles[i].hts));
    assert.equal(dv.getInt8(p + 4), Math.max(...hmp.tiles[i].hts));
  }
  assert.ok(hmp.tiles.length < 0x1FFF);
});

// Every item of every type must parse the same after an add or remove, apart from the one added or removed.
const strip = (it) => { const { offset, index, ...rest } = it; return JSON.stringify(rest); };
const datLevels = [...rom.assets.keys()].filter((p) => /^data\/level\/lv_.\/dat$/.test(p));
await check(`duplicating and deleting objects keeps every other item intact (${datLevels.length} levels)`, async () => {
  for (const p of datLevels) {
    const orig = bytesOf(await rom.read(p));
    const before = RS64.parseDAT(new DataView(orig.buffer)).items;
    const objs = before.filter((x) => x.type === 0);
    if (!objs.length) continue;
    const src = objs[Math.floor(objs.length / 2)], size = new DataView(orig.buffer).getUint32(src.offset + 8);
    const added = RS64.datAddItem(orig, 0, orig.slice(src.offset, src.offset + size));
    const a = RS64.parseDAT(new DataView(added.buffer)).items;
    assert.equal(a.length, before.length + 1, p);
    const newOne = a.filter((x) => x.type === 0).at(-1);
    assert.equal(strip(newOne), strip(src), p);
    assert.deepEqual(a.filter((x) => x !== newOne).map(strip), before.map(strip), p);
    const removed = RS64.datRemoveItem(orig, 0, src.index);
    const r = RS64.parseDAT(new DataView(removed.buffer)).items;
    assert.deepEqual(r.map(strip), before.filter((x) => x !== src).map(strip), p);
    // The shifted sections and the copied record come from the original (SourceCopy); only the rewritten offset list is new data.
    assert.deepEqual(applyBPS(orig, RS64.encodeBPS(orig, added)), added, p);
    assert.deepEqual(applyBPS(orig, RS64.encodeBPS(orig, removed)), removed, p);
    assert.ok(RS64.encodeBPS(orig, removed).length < 300 + objs.length * 5, `${p} delete patch ${RS64.encodeBPS(orig, removed).length} bytes`);
    assert.ok(RS64.encodeBPS(orig, added).length < 300 + objs.length * 5, `${p} duplicate patch ${RS64.encodeBPS(orig, added).length} bytes`);
    // Header block at 0x40 (trailing data) moves with the sections and keeps its bytes.
    const o0 = new DataView(orig.buffer), o1 = new DataView(added.buffer);
    assert.deepEqual(added.subarray(o1.getUint32(0x44), o1.getUint32(0x44) + o1.getUint32(0x40)), orig.subarray(o0.getUint32(0x44), o0.getUint32(0x44) + o0.getUint32(0x40)), p);
  }
});

await check('new part geometry reads back through the explorer parser', async () => {
  const orig = bytesOf(await rom.read('data/pl_crafts/xwing_HOB'));
  const hob = RS64.parseHOB(new DataView(orig.buffer));
  const part = hob.objects[0].lods[0].flat().find((p) => p.verts.length === 144 * 3);
  const geo = {
    verts: [[0, 0, 0], [100, 0, 0], [100, 100, 0], [0, 100, -50], [50, 50, 200]],
    faces: [
      { idx: [0, 1, 2, 3], mat: 3, colors: [0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff], uvs: [[0, 0], [1, 0], [1, 1], [0, 1]] },
      { idx: [0, 1, 4], mat: 5, colors: [0x112233ff, 0x445566ff, 0x778899ff], uvs: null },
    ],
  };
  const out = RS64.hobSetPartGeometry(orig, part.offset, geo);
  assert.deepEqual(out.subarray(0, orig.length).filter((_, i) => i < part.offset - 4 || i >= part.offset + 0x28), orig.filter((_, i) => i < part.offset - 4 || i >= part.offset + 0x28));
  const back = RS64.parseHOB(new DataView(out.buffer)).objects[0].lods[0].flat().find((p) => p.offset === part.offset);
  assert.deepEqual([...back.verts], geo.verts.flat());
  assert.equal(back.faces.length, 2);
  assert.deepEqual(back.faces[0].idx, [0, 1, 2, 3]);
  assert.deepEqual(back.faces[0].colors, [0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff]);
  assert.deepEqual(back.faces[0].uvs, [[0, 0], [1, 0], [1, 1], [0, 1]]);
  assert.equal(back.faces[0].mat, 3);
  assert.deepEqual(back.faces[1].idx, [0, 1, 4]);
  assert.equal(back.faces[1].uvs, null);
  const dv = new DataView(out.buffer);
  assert.equal(dv.getUint32(part.offset - 4), out.length);
});

// The A-wing's own triangles in display space (HOB (x, y, z) shows as (x, -y, -z)), as a glTF export of it would hold them.
async function craftTris(path) {
  const h = RS64.parseHOB(await rom.read(path)), tris = [];
  for (const list of h.objects[0].lods[0]) for (const p of list) for (const f of p.faces) {
    const v = (i) => [p.verts[f.idx[i] * 3], -p.verts[f.idx[i] * 3 + 1], -p.verts[f.idx[i] * 3 + 2]];
    const c = (i) => (f.colors ? f.colors[i] ?? f.colors[0] : 0xffffffff);
    for (const [a, b, d] of f.idx.length === 4 ? [[0, 1, 2], [0, 2, 3]] : [[0, 1, 2]]) tris.push({ p: [v(a), v(b), v(d)], uv: [[0.5, 0.5], [0.5, 0.5], [0.5, 0.5]], rgba: [c(a), c(b), c(d)], node: '' });
  }
  return tris;
}
await check('importing a model into a craft fills its body, empties the other unnamed parts and keeps named ones', async () => {
  const orig = bytesOf(await rom.read('data/pl_crafts/xwing_HOB'));
  const hob = RS64.parseHOB(new DataView(orig.buffer)), hmt = RS64.parseHMT(await rom.read('data/pl_crafts/xwing_HMT'));
  const slots = RS64.craftImportSlots(hob, hmt), pick = slots[0];
  assert.ok(slots.length >= 4 && pick.tex.w * pick.tex.h >= 64 * 32, 'several slots, the first a big one');
  assert.equal(new Set(slots.map((s) => s.tex)).size, slots.length, 'one slot per texture');
  for (let i = 1; i < slots.length; i++) assert.ok(slots[i - 1].tex.w * slots[i - 1].tex.h >= slots[i].tex.w * slots[i].tex.h);
  const namedTex = new Set(hob.objects[0].lods[0].flat().filter((p) => p.name).flatMap((p) => p.faces.map((f) => hmt.materials[f.mat].tex)));
  assert.ok(slots.every((s) => !namedTex.has(hmt.materials[s.mat].tex)), 'no slot drawn by an engine, glow or canopy part');
  const tris = await craftTris('data/pl_crafts/awing_HOB');
  tris.forEach((t, i) => { if (i % 2) t.mat = slots[1].mat; });
  const out = RS64.hobImportCraft(orig, hob, tris, { mat: pick.mat, plusZ: false });
  const back = RS64.parseHOB(new DataView(out.buffer)).objects[0].lods[0];
  const parts = back.flat(), body = back[0].find((p) => !p.name);
  assert.equal(body.faces.length, tris.length);
  assert.ok(body.faces.every((f, i) => f.mat === (i % 2 ? slots[1].mat : pick.mat)));
  for (const p of parts) if (!p.name && p !== body) assert.equal(p.faces.length, 0);
  const before = hob.objects[0].lods[0].flat();
  for (const p of parts) if (p.name) assert.deepEqual([...p.verts], [...before.find((q) => q.offset === p.offset).verts]);
  // Fitted onto the X-wing: the largest dimension matches the original's.
  const ext = (list) => {
    const mn = [Infinity, Infinity, Infinity], mx = [-Infinity, -Infinity, -Infinity];
    for (const p of list) for (let i = 0; i < p.verts.length; i++) { mn[i % 3] = Math.min(mn[i % 3], p.verts[i]); mx[i % 3] = Math.max(mx[i % 3], p.verts[i]); }
    return Math.max(...mx.map((v, a) => v - mn[a]));
  };
  assert.ok(Math.abs(ext([body]) - ext(before)) <= 2, `${ext([body])} vs ${ext(before)}`);
});

const cuts = [...rom.assets.keys()].filter((p) => /^data\/cuts\/id\d+_/.test(p));
await check(`all ${cuts.length} cutscenes parse into actor trees with well-formed, continuous channels`, async () => {
  assert.equal(cuts.length, 34);
  for (const p of cuts) {
    const c = RS64.parseCutscene(await rom.read(p));
    assert.ok(c.frames > 0 && c.length > 0 && Math.abs(c.frames / c.length - c.fps) < 0.5, `${p} timing`);
    c.actors.forEach((a, i) => assert.ok(a.parent === -1 || (a.parent < c.actors.length && a.parent !== i), `${p} actor ${i} parent`));
    const chans = [...(c.camera || []), ...c.actors.flatMap((a) => a.ch || [])];
    for (const k of chans) {
      assert.ok(k.length >= 6, `${p} channel has keys`);
      for (let i = 1; i < k.length / 3; i++) assert.ok(k[i * 3 + 2] >= k[(i - 1) * 3 + 2], `${p} key times rise`);
      assert.equal(k[2], 0, `${p} first key at 0`);
    }
    // Camera position moves by less than 2 world units per frame (x 0.25), i.e. no jumps from a misread channel.
    if (c.camera) for (let f = 1; f < c.frames; f++) {
      const at = (fr) => [0, 1, 2].map((ch) => RS64.cutsceneValue(c.camera[ch], fr / c.frames, c.frames, 8) * 0.25);
      const a = at(f - 1), b = at(f);
      assert.ok(Math.hypot(...a.map((v, i) => v - b[i])) < 2, `${p} camera jump at frame ${f}`);
    }
  }
  const id0 = RS64.parseCutscene(await rom.read('data/cuts/id0_intro'));
  assert.deepEqual(id0.actors.map((a) => a.parent), [-1, 0, -1, 2, -1, 4]);
  assert.ok(id0.events.some((e) => e.type === 0), 'a voice line');
  const x0 = RS64.cutsceneValue(id0.actors[0].ch[0], 3 / 18, id0.frames, 6), x1 = RS64.cutsceneValue(id0.actors[0].ch[0], 8 / 18, id0.frames, 6);
  assert.ok(x0 > 400 && x1 < 300, 'the X-wing flies toward -x');
});

await check('every stock cutscene writes back byte-for-byte', async () => {
  for (const p of cuts) {
    const dv = await rom.read(p), src = new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength);
    assert.deepEqual(RS64.writeCutscene(RS64.parseCutscene(dv)), src, p);
  }
});
await check('an edited cutscene re-parses to the edit', async () => {
  const c = RS64.parseCutscene(await rom.read('data/cuts/id0_intro'));
  c.events[0].frame += 5;
  c.actors[0].ch[0][3] = 123.5;
  const back = RS64.parseCutscene(new DataView(RS64.writeCutscene(c).buffer));
  assert.equal(back.events[0].frame, c.events[0].frame);
  assert.equal(back.actors[0].ch[0][3], 123.5);
});

const cutOf = async (p) => RS64.parseCutscene(await rom.read(p));
const reparse = (c) => RS64.parseCutscene(new DataView(RS64.writeCutscene(c).buffer));
const actorRefs = (c) => c.events.filter((e) => RS64.CUT_ACTOR_EVENTS.has(e.type)).map((e) => (e.type === 11 ? e.p[0] & 0xffff : e.p[0]));
await check('cutMoveEvent changes only that event', async () => {
  const c = await cutOf('data/cuts/id0_intro'), before = c.events.map((e) => ({ ...e, p: [...e.p] }));
  const i = c.events.findIndex((e) => e.type === 0), to = before[i].frame + 7;
  RS64.cutMoveEvent(c, i, to);
  const back = reparse(c), moved = back.events.find((e) => e.type === 0 && e.frame === to && e.p[0] === before[i].p[0]);
  assert.ok(moved, 'moved event present');
  assert.deepEqual(back.events.filter((e) => e !== moved).map((e) => [e.type, e.frame, ...e.p]).sort(),
    before.filter((_, k) => k !== i).map((e) => [e.type, e.frame, ...e.p]).sort());
});
await check('cutSetKey inserts and slopes match the next key', async () => {
  const c = await cutOf('data/cuts/id0_intro');
  RS64.cutSetKey(c, { actor: 0, ch: 1 }, 0.4321, -7.5);
  const keys = reparse(c).actors[0].ch[1], n = keys.length / 3;
  const k = [...Array(n).keys()].find((j) => Math.abs(keys[j * 3 + 2] - 0.4321) < 1e-6);
  assert.ok(k != null, 'key inserted');
  assert.equal(keys[k * 3], -7.5);
  // The edited key and the one before it get the delta slope; untouched keys keep their stored slopes.
  for (const j of [k - 1, k]) {
    const want = (keys[(j + 1) * 3] - keys[j * 3]) / (keys[(j + 1) * 3 + 2] - keys[j * 3 + 2]);
    assert.ok(Math.abs(keys[j * 3 + 1] - want) < 1e-4 * Math.max(1, Math.abs(want)), `slope ${j}`);
  }
  const orig = (await cutOf('data/cuts/id0_intro')).actors[0].ch[1];
  assert.equal(keys[(n - 1) * 3 + 1], orig[orig.length - 2], 'last key untouched');
});
await check('cutSetKey on a shared run splits it', async () => {
  // No stock file shares a run, so make one: point id0_intro actor 0's channel 1 at channel 0's keys.
  const src = await rom.read('data/cuts/id0_intro'), bytes = new Uint8Array(src.buffer, src.byteOffset, src.byteLength).slice(), bdv = new DataView(bytes.buffer);
  bdv.setUint32(0x13d8 + 0x24, bdv.getUint32(0x13d8 + 0x20));
  const c = RS64.parseCutscene(bdv), arr = (r) => c.actors[r.actor].ch[r.ch];
  const found = { c, a: { actor: 0, ch: 0 }, b: { actor: 0, ch: 1 }, arr };
  assert.equal(c.keyStarts.get(arr(found.a)).start, c.keyStarts.get(arr(found.b)).start, 'shared run');
  const other = found.arr(found.b).slice();
  RS64.cutSetKey(found.c, found.a, 0.5, 42);
  const back = reparse(found.c), get = (r) => (r.cam != null ? back.cameras[r.cam][r.ch] : back.actors[r.actor].ch[r.ch]);
  assert.deepEqual([...get(found.b)], [...other]);
  assert.ok([...get(found.a)].includes(42));
});
await check('cutRemoveActor drops its events and renumbers the rest', async () => {
  const c = await cutOf('data/cuts/id0_intro');
  RS64.cutRemoveActor(c, 2);
  const back = reparse(c);
  assert.deepEqual(back.actors.map((a) => a.parent), [-1, 0, -1, 2]);
  assert.ok(actorRefs(back).every((r) => r < 4), 'no dangling actor index');
});
await check('cutRemoveCamera deletes its cuts', async () => {
  const p = [];
  for (const q of cuts) if ((await cutOf(q)).cameras.filter(Boolean).length > 1) p.push(q);
  const c = await cutOf(p[0]), k = c.cameras.findLastIndex(Boolean);
  RS64.cutRemoveCamera(c, k);
  const back = reparse(c);
  assert.ok(!back.cameras[k]);
  assert.ok(!back.events.some((e) => e.type === 4 && e.p[0] === k));
  assert.throws(() => { const one = { ...back }; while (one.cameras.filter(Boolean).length > 1) RS64.cutRemoveCamera(one, one.cameras.findLastIndex(Boolean)); RS64.cutRemoveCamera(one, one.cameras.findIndex(Boolean)); }, /last camera/);
});
await check('cutSetLength keeps absolute times', async () => {
  const c = await cutOf('data/cuts/id0_intro'), L = c.length, frames = c.frames, ev = c.events.map((e) => e.frame);
  const keyT = c.actors[0].ch[0][5], camAt = (d, fr) => RS64.cutsceneValue(d.camera[0], fr / d.frames, d.frames, 8);
  const v = camAt(c, frames / 2);
  RS64.cutSetLength(c, L + 2);
  const back = reparse(c);
  assert.ok(Math.abs(back.length - (L + 2)) < 1e-4);
  assert.equal(back.frames, Math.round((L + 2) * c.fps));
  assert.ok(Math.abs(back.actors[0].ch[0][5] - keyT * L / (L + 2)) < 1e-6);
  assert.deepEqual(back.events.map((e) => e.frame), ev);
  assert.ok(Math.abs(camAt(back, frames / 2) - v) < 0.05 * Math.max(1, Math.abs(v)), 'camera value kept');
});
await check('limits throw and leave data unchanged', async () => {
  const c = await cutOf('data/cuts/id0_intro');
  while (c.events.length < 200) RS64.cutAddEvent(c, { type: 10, frame: 1, p: [0, 0, 0, 0] });
  let snap = RS64.writeCutscene(c);
  assert.throws(() => RS64.cutAddEvent(c, { type: 10, frame: 1, p: [0, 0, 0, 0] }), /200 events/);
  assert.deepEqual(RS64.writeCutscene(c), snap);
  while (c.cameras.filter(Boolean).length < 6) RS64.cutAddCamera(c);
  snap = RS64.writeCutscene(c);
  assert.throws(() => RS64.cutAddCamera(c), /6 cameras/);
  assert.deepEqual(RS64.writeCutscene(c), snap);
  while (c.actors.length < 60) RS64.cutAddActor(c, 'xwing');
  snap = RS64.writeCutscene(c);
  assert.throws(() => RS64.cutAddActor(c, 'xwing'), /60 actors/);
  assert.deepEqual(RS64.writeCutscene(c), snap);
  assert.equal(reparse(c).actors.length, 60);
});

await check('history groups, undoes, redoes and drops redo on a new commit', () => {
  const h = RS64.createHistory(), b = (...v) => new Uint8Array(v);
  h.commit('a', b(0), b(1), 'A');
  h.begin('BC');
  h.commit('b', b(0), b(2));
  h.commit('c', b(0), b(3));
  h.end();
  assert.equal(h.steps.length, 2);
  assert.deepEqual(h.undo(), [{ path: 'c', bytes: b(0) }, { path: 'b', bytes: b(0) }]);
  assert.deepEqual(h.redo(), [{ path: 'b', bytes: b(2) }, { path: 'c', bytes: b(3) }]);
  h.undo();
  h.commit('d', b(0), b(4), 'D');
  assert.equal(h.steps.length, 2);
  assert.deepEqual(h.redo(), []);
  assert.deepEqual(h.undo(), [{ path: 'd', bytes: b(0) }]);
  assert.deepEqual(h.undo(), [{ path: 'a', bytes: b(0) }]);
  assert.deepEqual(h.undo(), []);
});

const romCrc = RS64.crc32(new Uint8Array(buf.buffer, buf.byteOffset, buf.length));
async function sampleProject() {
  const cutP = 'data/cuts/id0_intro', datP = 'data/level/lv_0/dat';
  const originals = new Map([[cutP, bytesOf(await rom.read(cutP))], [datP, bytesOf(await rom.read(datP))]]), h = RS64.createHistory();
  const c = RS64.parseCutscene(new DataView(originals.get(cutP).slice().buffer));
  RS64.cutMoveEvent(c, 0, c.events[0].frame + 9);
  const cut1 = RS64.writeCutscene(c);
  h.commit(cutP, originals.get(cutP), cut1, 'move event');
  const dat1 = originals.get(datP).slice();
  new DataView(dat1.buffer).setFloat32(dat1.length - 8, 12.5);
  h.begin('two');
  h.commit(datP, originals.get(datP), dat1);
  RS64.cutSetKey(c, { actor: 0, ch: 0 }, 0.5, 99);
  const cut2 = RS64.writeCutscene(c);
  h.commit(cutP, cut1, cut2);
  h.end();
  h.undo();
  return { originals, h, current: new Map([[cutP, cut1], [datP, originals.get(datP)]]) };
}
await check('project round-trips edits and history', async () => {
  const { originals, h, current } = await sampleProject();
  const zip = RS64.projectWrite({ romCrc, originals, current, history: h });
  const back = await RS64.projectRead(zip, romCrc, async (p) => originals.get(p).slice());
  for (const [p, b] of current) assert.deepEqual(back.current.get(p) ?? originals.get(p), b, p);
  assert.equal(back.history.at, h.at);
  assert.equal(back.history.steps.length, h.steps.length);
  assert.deepEqual(back.history.redo(), h.redo());
  back.history.undo();
  assert.deepEqual(back.history.undo(), [{ path: 'data/cuts/id0_intro', bytes: originals.get('data/cuts/id0_intro') }]);
  // No patch carries a 64-byte run of an original that the edited bytes no longer have.
  const files = RS64.zipRead(zip);
  for (const [name, data] of files) {
    if (!name.endsWith('.bps')) continue;
    const pb = Buffer.from(data);
    for (const [p, orig] of originals) {
      const now = Buffer.from(current.get(p));
      for (let o = 0; o + 64 <= orig.length; o += 64) {
        const run = Buffer.from(orig.subarray(o, o + 64));
        if (now.indexOf(run) < 0) assert.ok(pb.indexOf(run) < 0, `${name} carries original bytes of ${p} at ${o}`);
      }
    }
  }
});
await check('project from another ROM is refused', async () => {
  const { originals, h, current } = await sampleProject();
  const zip = RS64.projectWrite({ romCrc, originals, current, history: h });
  await assert.rejects(RS64.projectRead(zip, romCrc ^ 1, async (p) => originals.get(p).slice()), /different ROM/);
});
await check('damaged project is refused', async () => {
  const { originals, h, current } = await sampleProject();
  const zip = RS64.projectWrite({ romCrc, originals, current, history: h });
  await assert.rejects(RS64.projectRead(zip.subarray(0, zip.length >> 1), romCrc, async (p) => originals.get(p).slice()), /Not an RS64 Explorer project/);
  await assert.rejects(RS64.projectRead(RS64.zipStore([{ name: 'x.txt', data: new Uint8Array(3) }]), romCrc, async () => new Uint8Array()), /Not an RS64 Explorer project/);
});

const evKey = (e) => [e.type, e.frame, ...e.p].join(',');
await check('cutClips maps id0_intro', async () => {
  const c = await cutOf('data/cuts/id0_intro'), clips = RS64.cutClips(c, () => 1.5);
  const shots = clips.filter((k) => k.kind === 'shot'), cuts4 = c.events.filter((e) => e.type === 4);
  assert.equal(shots.length, Math.max(1, cuts4.length + (cuts4[0]?.frame > 0 ? 1 : 0)));
  for (const k of clips.filter((x) => x.kind === 'voice' || x.kind === 'sound')) assert.ok(k.lockStart && k.lockEnd, 'audio ends locked');
  for (const k of clips.filter((x) => x.kind === 'actor')) {
    if (k.startEvent) assert.equal(k.startEvent.type, 6);
    if (k.endEvent) assert.equal(k.endEvent.type, 7);
    assert.ok(k.end > k.start);
  }
  assert.ok(clips.every((k) => k.start >= 0 && k.end <= c.frames && k.end >= k.start));
});
await check('ripple trim moves only the cut', async () => {
  let c;
  for (const p of cuts) if ((c = await cutOf(p)).events.filter((e) => e.type === 4).length >= 2) break;
  const shot = RS64.cutClips(c, () => 1).filter((k) => k.kind === 'shot' && k.endEvent && k.endEvent.frame + 10 < c.frames)[0];
  const before = c.events.map(evKey), cutAt = shot.endEvent.frame;
  RS64.cutDragClip(c, shot, 'end', 10);
  const after = reparse(c).events.map(evKey);
  const gone = before.filter((k) => !after.includes(k)), added = after.filter((k) => !before.includes(k));
  assert.equal(gone.length, 1);
  assert.equal(added.length, 1);
  assert.equal(+added[0].split(',')[1], cutAt + 10);
});
await check('trimming a music end creates a stop', async () => {
  let c;
  for (const p of cuts) {
    c = await cutOf(p);
    if (c.events.some((e) => e.type === 1) && !c.events.some((e) => e.type === 2) && c.events.filter((e) => e.type === 1).length === 1) break;
  }
  const music = RS64.cutClips(c, () => 1).find((k) => k.kind === 'music');
  assert.ok(!music.endEvent && !music.lockEnd);
  const to = Math.max(music.start + 1, music.end - 20);
  RS64.cutDragClip(c, music, 'end', to - music.end);
  assert.ok(reparse(c).events.some((e) => e.type === 2 && e.frame === to));
});

await check('cutAnimateActor turns a static actor into constant keys', async () => {
  let c, i = -1;
  for (const p of cuts) if ((i = (c = await cutOf(p)).actors.findIndex((a) => !a.animated)) >= 0) break;
  assert.ok(i >= 0, 'a static actor exists');
  const vals = [...c.actors[i].value];
  RS64.cutAnimateActor(c, i);
  const a = reparse(c).actors[i];
  assert.ok(a.animated);
  vals.forEach((v, k) => [0, 0.37, 1].forEach((t) => assert.ok(Math.fround(RS64.cutsceneValue(a.ch[k], t, c.frames, 6)) === Math.fround(v), `ch ${k} t ${t}`)));
});

await check('editing a channel twice leaves its neighbours intact', async () => {
  let c = await cutOf('data/cuts/id0_intro');
  const i = RS64.cutAddActor(c, 'tie_inter');
  c = reparse(c);
  RS64.cutSetKey(c, { actor: i, ch: 0 }, 1, 200);
  c = reparse(c);
  RS64.cutSetKey(c, { actor: i, ch: 0 }, 1, 200);
  c = reparse(c);
  for (let k = 1; k < 11; k++) assert.equal(c.actors[i].ch[k].length, 6, `channel ${k} keeps its 2 keys`);
  assert.deepEqual([...c.actors[i].ch[10]], [0, 0, 0, 0, 0, Math.fround(c.length)]);
  const stock = await cutOf('data/cuts/id0_intro');
  for (let a = 0; a < stock.actors.length; a++) if (stock.actors[a].ch) stock.actors[a].ch.forEach((k, ch) => assert.deepEqual([...c.actors[a].ch[ch]], [...k], `stock actor ${a} ch ${ch}`));
});

// Every channel ends in a key past t = 1 (stock: time = length in seconds); the game's sampler needs it to stop.
const endsWithSentinel = (keys) => keys[keys.length - 1] > 1;
await check('stock channels end in a sentinel key past t = 1', async () => {
  for (const p of cuts) {
    const c = await cutOf(p);
    for (const k of [...c.cameras.filter(Boolean).flat(), ...c.actors.flatMap((a) => a.ch ?? [])]) assert.ok(endsWithSentinel(k), p);
  }
});
await check('new and animated channels end in a sentinel at the cutscene length', async () => {
  let c = await cutOf('data/cuts/id0_intro');
  const i = RS64.cutAddActor(c, 'tie_inter'), k = RS64.cutAddCamera(c);
  c = reparse(c);
  for (const keys of [...c.actors[i].ch, ...c.cameras[k]]) assert.equal(keys[keys.length - 1], Math.fround(c.length));
  let s;
  for (const p of cuts) if ((s = (await cutOf(p))).actors.some((a) => !a.animated)) break;
  const j = s.actors.findIndex((a) => !a.animated);
  RS64.cutAnimateActor(s, j);
  for (const keys of reparse(s).actors[j].ch) assert.ok(endsWithSentinel(keys));
});
await check('cutSetLength moves the sentinel to the new length and keeps late events', async () => {
  const c = await cutOf('data/cuts/id11_intro'), L = c.length, late = c.events.filter((e) => e.frame > c.frames).map((e) => e.frame - c.frames);
  assert.ok(late.length, 'id11_intro has an event past its end');
  for (const to of [L, L + 2, L - 2]) {
    const d = await cutOf('data/cuts/id11_intro');
    RS64.cutSetLength(d, to);
    const back = reparse(d);
    for (const keys of [...back.cameras.filter(Boolean).flat(), ...back.actors.flatMap((a) => a.ch ?? [])]) assert.equal(keys[keys.length - 1], Math.fround(to), `sentinel at ${to}`);
    assert.deepEqual(back.events.filter((e) => e.frame > back.frames).map((e) => e.frame - back.frames), late, `late events kept at ${to}`);
  }
  const same = await cutOf('data/cuts/id11_intro'), before = RS64.writeCutscene(same);
  RS64.cutSetLength(same, L);
  assert.deepEqual(RS64.writeCutscene(same), before, 'unchanged length is a no-op');
});
await check('moving an event that plays after the cutscene keeps it after', async () => {
  const c = await cutOf('data/cuts/id11_intro'), i = c.events.findIndex((e) => e.frame > c.frames), f = c.events[i].frame;
  const j = RS64.cutMoveEvent(c, i, f + 5);
  assert.equal(c.events[j].frame, f + 5);
});
await check('the sentinel key cannot be removed', async () => {
  const c = await cutOf('data/cuts/id0_intro'), n = c.actors[0].ch[0].length / 3;
  assert.throws(() => RS64.cutRemoveKey(c, { actor: 0, ch: 0 }, n - 1), /last key/);
});
await check('a key after the last real key holds and the sentinel follows it', async () => {
  const c = await cutOf('data/cuts/id0_intro'), keys0 = c.actors[0].ch[0], n0 = keys0.length / 3;
  const t = (keys0[(n0 - 2) * 3 + 2] + 1) / 2;
  const j = RS64.cutSetKey(c, { actor: 0, ch: 0 }, t, 1234);
  const keys = reparse(c).actors[0].ch[0], n = keys.length / 3;
  assert.equal(j, n - 2);
  assert.equal(keys[j * 3 + 1], 0, 'held');
  assert.equal(keys[(n - 1) * 3], 1234, 'sentinel value follows');
  assert.ok(Math.abs(RS64.cutsceneValue(keys, 0.999, c.frames, 6) - 1234) < 1e-3);
});
await check('applyBPS rejects a truncated patch instead of hanging', () => {
  const p = RS64.encodeBPS(new Uint8Array([1, 2, 3]), new Uint8Array([1, 9, 3, 4]));
  assert.throws(() => RS64.applyBPS(new Uint8Array([1, 2, 3]), p.subarray(0, 6)));
});
await check('cutBlank leaves one still camera, the scene events and the new length', async () => {
  for (const p of [...rom.assets.keys()].filter((p) => p.startsWith('data/cuts/id')).sort()) {
    const c = await cutOf(p), k = c.cameras.findIndex(Boolean), start = c.cameras[k].map((keys) => keys[0]);
    const scene = c.events.filter((e) => [10, 17, 19, 21, 22].includes(e.type) || e.frame > c.frames).length;
    RS64.cutBlank(c, 10);
    const b = reparse(c);
    assert.equal(b.actors.length, 0, p);
    assert.deepEqual(b.cameras.map((x, i) => (x ? i : -1)).filter((i) => i >= 0), [k], p);
    b.cameras[k].forEach((keys, ch) => assert.deepEqual([...keys], [...new Float32Array([start[ch], 0, 0, start[ch], 0, 10])], `${p} ch ${ch}`));
    assert.equal(b.length, 10, p);
    assert.equal(b.frames, Math.round(10 * b.fps), p);
    assert.equal(b.events.length, scene, p);
    assert.ok(b.events.every((e) => ([10, 17, 19, 21, 22].includes(e.type) && e.frame <= b.frames) || e.frame > b.frames), p);
  }
});
await check('the header counts cameras in use (0x48) and non-model actors (0x4C)', async () => {
  const c = await cutOf('data/cuts/id5_intro');
  RS64.cutBlank(c, 10);
  RS64.cutAddActor(c, 'tie_fighter');
  let dv = new DataView(RS64.writeCutscene(c).buffer);
  assert.equal(dv.getUint16(0x48), 1);
  assert.equal(dv.getUint16(0x4c), 0);
  // The game takes a camera entry whose channel 0 starts at key 0 for an unused one.
  for (let i = 0; i < 6; i++) if (c.cameras[i]) assert.notEqual(dv.getUint32(0x1318 + i * 0x20), 0, `camera ${i}`);
  assert.equal(reparse(c).cameras[0][0].length, 6);
  const d = await cutOf('data/cuts/id0_intro');
  RS64.cutAddCamera(d);
  RS64.cutRemoveActor(d, d.actors.findIndex((a) => !a.model && a.parent < 0));
  dv = new DataView(RS64.writeCutscene(d).buffer);
  assert.equal(dv.getUint16(0x48), 2);
  assert.equal(dv.getUint16(0x4c), d.actors.filter((a) => !a.model).length);
});
await check('cutAddActor takes the scale channel value (default 1 / 0.2222)', async () => {
  const c = await cutOf('data/cuts/id5_intro');
  const i = RS64.cutAddActor(c, 'pl_crafts/xwing', -1, 0.056), j = RS64.cutAddActor(c, 'tie_inter');
  const r = reparse(c);
  assert.equal(r.actors[i].ch[3][0], Math.fround(0.056));
  assert.equal(r.actors[j].ch[3][0], Math.fround(1 / 0.2222));
});
// Axis-angle [ax, ay, az, deg] helpers for the motion tests.
const rotate = ([ax, ay, az, deg], v) => {
  const n = Math.hypot(ax, ay, az) || 1, [x, y, z] = [ax / n, ay / n, az / n], t = (deg * Math.PI) / 180, c = Math.cos(t), s = Math.sin(t);
  const d = x * v[0] + y * v[1] + z * v[2];
  return [0, 1, 2].map((i) => v[i] * c + [y * v[2] - z * v[1], z * v[0] - x * v[2], x * v[1] - y * v[0]][i] * s + [x, y, z][i] * d * (1 - c));
};
const angleBetween = (a, b) => {
  const q = (r) => { const n = Math.hypot(r[0], r[1], r[2]) || 1, h = (r[3] * Math.PI) / 360; return [r[0] / n * Math.sin(h), r[1] / n * Math.sin(h), r[2] / n * Math.sin(h), Math.cos(h)]; };
  const qa = q(a), qb = q(b), dot = Math.abs(qa.reduce((s, v, i) => s + v * qb[i], 0));
  return (2 * Math.acos(Math.min(1, dot)) * 180) / Math.PI;
};
await check('cutCurve passes through snapshots and holds outside', () => {
  const s = [{ t: 0.1, p: [0, 0, 0] }, { t: 0.5, p: [10, 0, 0] }, { t: 0.9, p: [10, 0, 10] }];
  assert.deepEqual(RS64.cutCurve(s, 0.5).map((v) => +v.toFixed(4)), [10, 0, 0]);
  assert.deepEqual(RS64.cutCurve(s, 0), [0, 0, 0]);
  assert.deepEqual(RS64.cutCurve(s, 1), [10, 0, 10]);
  assert.deepEqual(RS64.cutCurve([{ t: 0.3, p: [1, 2, 3] }], 0.7), [1, 2, 3]);
  assert.ok(RS64.cutCurve([{ t: 0.3, p: [1, 2, 3] }, { t: 0.3, p: [4, 5, 6] }], 0.3).every(Number.isFinite));
});
await check('cutSetChannelSpan replaces only its span and re-baking does not grow the channel', async () => {
  const c = await cutOf('data/cuts/id5_intro');
  RS64.cutBlank(c, 10);
  const i = RS64.cutAddActor(c, 'pl_crafts/xwing', -1, 0.0562);
  const s = [...Array(11).keys()].map((k) => [0.2 + k * 0.05, k]);
  RS64.cutSetChannelSpan(c, { actor: i, ch: 0 }, 0.2, 0.7, s);
  const n = c.actors[i].ch[0].length;
  RS64.cutSetChannelSpan(c, { actor: i, ch: 0 }, 0.2, 0.7, s);
  assert.equal(c.actors[i].ch[0].length, n);
  assert.ok(Math.abs(RS64.cutsceneValue(reparse(c).actors[i].ch[0], 0.45, c.frames, 1) - 5) < 1e-3);
});
await check('cutBakeActorPath points the nose along travel, banks into turns, never flips', async () => {
  const c = await cutOf('data/cuts/id5_intro');
  RS64.cutBlank(c, 10);
  const i = RS64.cutAddActor(c, 'pl_crafts/xwing', -1, 0.0562);
  RS64.cutBakeActorPath(c, i, [{ t: 0, p: [0, 0, 0] }, { t: 0.5, p: [400, 0, 0] }, { t: 1, p: [400, 0, 400] }], { rate: 10, bank: 1 });
  const rot = (t) => [4, 5, 6, 7].map((ch) => RS64.cutsceneValue(c.actors[i].ch[ch], t, c.frames, 1));
  assert.ok(rotate(rot(0.1), [0, 0, -1])[0] > 0.95, 'nose along +X');
  assert.ok(rotate(rot(0.5), [0, -1, 0])[2] > 0.05, 'banks toward the turn centre');
  for (let k = 0; k < 99; k++) assert.ok(angleBetween(rot(k / 100), rot((k + 1) / 100)) < 30, `step ${k}`);
});
const camAt = (c, k, t) => c.cameras[k].map((keys) => RS64.cutsceneValue(keys, t, c.frames, 1));
const flyingXwing = async () => {
  const c = await cutOf('data/cuts/id5_intro');
  RS64.cutBlank(c, 10);
  const i = RS64.cutAddActor(c, 'pl_crafts/xwing', -1, 0.0562);
  RS64.cutBakeActorPath(c, i, [{ t: 0, p: [0, 0, 0] }, { t: 0.5, p: [400, -40, 0] }, { t: 1, p: [400, -40, 400] }]);
  return { c, i, k: c.cameras.findIndex(Boolean) };
};
await check('cutBakeCameraPath hits each snapshot position and FOV', async () => {
  const { c, k } = await flyingXwing();
  const q = RS64.cutAxisAngleToQuat([0, 1, 0], 30);
  const snaps = [{ t: 0.2, p: [10, -5, 0], q, fov: 50 }, { t: 0.6, p: [30, -5, 20], q, fov: 40 }, { t: 0.9, p: [60, -10, 20], q, fov: 40 }];
  RS64.cutBakeCameraPath(c, k, snaps);
  for (const s of snaps) {
    const v = camAt(c, k, s.t);
    assert.ok(Math.hypot(v[0] - s.p[0], v[1] - s.p[1], v[2] - s.p[2]) < 1e-3, `pos at ${s.t}`);
    assert.ok(Math.abs(v[7] - s.fov) < 1e-4, `fov at ${s.t}`);
  }
});
await check('aim keeps the camera looking at the actor', async () => {
  const { c, i, k } = await flyingXwing();
  RS64.cutBakeCameraMode(c, k, { mode: 'aim', actor: i, t0: 0.1, t1: 0.9 });
  for (const t of [0.2, 0.5, 0.8]) {
    const v = camAt(c, k, t), a = RS64.cutActorPose(c, i, t).p, dir = [0, 1, 2].map((j) => a[j] - v[j]), n = Math.hypot(...dir);
    const z = rotate([v[3], v[4], v[5], v[6]], [0, 0, 1]);
    assert.ok((z[0] * dir[0] + z[1] * dir[1] + z[2] * dir[2]) / n > Math.cos(Math.PI / 180), `aim at ${t}`);
  }
});
await check('follow keeps the offset in the actor frame', async () => {
  const { c, i, k } = await flyingXwing();
  const offset = [0, -8, 30];
  RS64.cutBakeCameraMode(c, k, { mode: 'follow', actor: i, t0: 0.1, t1: 0.9, offset });
  for (const t of [0.2, 0.5, 0.8]) {
    const v = camAt(c, k, t), pose = RS64.cutActorPose(c, i, t), [x, y, z, w] = pose.q;
    const qr = (p) => { const u = [x, y, z], uv = [u[1] * p[2] - u[2] * p[1], u[2] * p[0] - u[0] * p[2], u[0] * p[1] - u[1] * p[0]], uuv = [u[1] * uv[2] - u[2] * uv[1], u[2] * uv[0] - u[0] * uv[2], u[0] * uv[1] - u[1] * uv[0]]; return [0, 1, 2].map((j) => p[j] + 2 * (w * uv[j] + uuv[j])); };
    const want = qr(offset).map((o, j) => pose.p[j] + o);
    assert.ok(Math.hypot(v[0] - want[0], v[1] - want[1], v[2] - want[2]) < 0.5, `follow at ${t}`);
  }
});
await check('ride hides the actor for the span and shows it after', async () => {
  const { c, i, k } = await flyingXwing();
  RS64.cutBakeCameraMode(c, k, { mode: 'ride', actor: i, t0: 0.3, t1: 0.6 });
  const ev = (type) => c.events.filter((e) => e.type === type && e.p[0] === i).map((e) => e.frame);
  assert.deepEqual(ev(7), [Math.round(0.3 * c.frames)]);
  assert.deepEqual(ev(6), [Math.round(0.6 * c.frames)]);
  const d = await flyingXwing();
  RS64.cutBakeCameraMode(d.c, d.k, { mode: 'ride', actor: d.i, t0: 0.3, t1: 1 });
  assert.equal(d.c.events.filter((e) => e.type === 6).length, 0);
});
await check('camera modes refuse a parented actor', async () => {
  const c = await cutOf('data/cuts/id5_intro'), i = c.actors.findIndex((a) => a.parent >= 0 && a.model);
  assert.throws(() => RS64.cutBakeCameraMode(c, c.cameras.findIndex(Boolean), { mode: 'aim', actor: i, t0: 0, t1: 1 }), /root actor/);
});
const shootScene = async (far = 200) => {
  const { c, i } = await flyingXwing();
  const j = RS64.cutAddActor(c, 'imp_stuff/probe_droid', -1, 0.24);
  [0, 1].forEach((t) => [far, -20, 300].forEach((v, ch) => RS64.cutSetKey(c, { actor: j, ch }, t, v)));
  return { c, i, j };
};
await check('cutShootAt aims, fires, and blows up the target on impact', async () => {
  const { c, i, j } = await shootScene();
  const n8 = c.events.length;
  const { fireFrame, impactFrame } = RS64.cutShootAt(c, i, j, 0.3);
  assert.equal(c.events.length, n8 + 3);
  const at = (type) => c.events.filter((e) => e.type === type);
  assert.deepEqual(at(8).map((e) => [e.frame, e.p[0]]), [[fireFrame, i]]);
  assert.deepEqual(at(5).map((e) => [e.frame, e.p[0]]), [[impactFrame, j]]);
  assert.deepEqual(at(7).map((e) => [e.frame, e.p[0]]), [[impactFrame, j]]);
  assert.equal(impactFrame, fireFrame + Math.round(0.5 * c.fps));
  const s = RS64.cutActorPose(c, i, fireFrame / c.frames), tp = RS64.cutActorPose(c, j, fireFrame / c.frames).p;
  const nose = rotate([...RS64.cutQuatToAxisAngle(s.q)], [0, 0, -1]), dir = [0, 1, 2].map((k) => tp[k] - s.p[k]), dist = Math.hypot(...dir);
  assert.ok((nose[0] * dir[0] + nose[1] * dir[1] + nose[2] * dir[2]) / dist > Math.cos((2 * Math.PI) / 180), 'nose on target');
  assert.ok(Math.abs((at(8)[0].p[2] / 10) * 0.5 - dist / 4) / (dist / 4) < 0.01, 'bolt reaches the target in 0.5 s');
});
await check('cutShootAt refuses a target out of bolt range', async () => {
  const { c, i, j } = await shootScene(60000);
  assert.throws(() => RS64.cutShootAt(c, i, j, 0.3), /range/);
});
await check('a project carries an explorer/ authoring sidecar next to its cutscene edit', async () => {
  const crc = RS64.crc32(new Uint8Array(buf.buffer, buf.byteOffset, buf.length));
  const cutPath = 'data/cuts/id5_intro', side = 'explorer/' + cutPath + '.json';
  const dv = await rom.read(cutPath), orig = new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength).slice();
  const c = RS64.parseCutscene(new DataView(orig.slice().buffer));
  RS64.cutBlank(c, 10);
  const now = RS64.writeCutscene(c), json = new TextEncoder().encode(JSON.stringify({ version: 1, tracks: { 'act:0': { snaps: [{ t: 0, p: [1, 2, 3] }] } } }));
  const history = RS64.createHistory();
  history.begin('Bake');
  history.commit(cutPath, orig, now, 'Bake');
  history.commit(side, new Uint8Array(0), json, 'Bake');
  history.end();
  const bytes = RS64.projectWrite({ romCrc: crc, originals: new Map([[cutPath, orig], [side, new Uint8Array(0)]]), current: new Map([[cutPath, now], [side, json]]), history });
  const original = async (p) => (p.startsWith('explorer/') ? new Uint8Array(0) : orig.slice());
  const back = await RS64.projectRead(bytes, crc, original);
  assert.deepEqual([...back.current.get(side)], [...json]);
  assert.deepEqual([...back.current.get(cutPath)], [...now]);
  assert.equal(back.history.steps.length, 1);
  assert.equal(back.history.steps[0].changes.length, 2);
});

if (failed) { console.log(`${failed} failed`); process.exit(1); }
console.log('all passed');
