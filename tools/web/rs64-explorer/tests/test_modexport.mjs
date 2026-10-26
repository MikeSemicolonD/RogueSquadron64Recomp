// node tools/web/rs64-explorer/tests/test_modexport.mjs <rom.z64>   (US v1.0 ROM)
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

// The game finds items by name only through the DAT's name map (getLevelDatItemByName 0x80047B48).
const lookupAll = (bytes) => {
  const items = RS64.parseDAT(new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)).items;
  for (const it of items.filter((x) => x.name)) {
    const hit = RS64.datLookup(bytes, it.name), first = items.find((x) => x.name === it.name);
    assert.deepEqual(hit, { type: first.type, index: first.index }, it.name);
  }
};
await check('the name map rebuilds every stock level byte for byte', async () => {
  for (const p of [...rom.assets.keys()].filter((p) => /^data\/level\/lv_.\/dat$/.test(p))) {
    const orig = bytesOf(await rom.read(p));
    assert.deepEqual(RS64.datRebuildNameMap(orig), orig, p);
    lookupAll(orig);
  }
});
await check('added, removed and replaced items stay findable by name', async () => {
  const orig = bytesOf(await rom.read('data/level/lv_3/dat')), dv = new DataView(orig.buffer), it = RS64.parseDAT(dv).items.find((x) => x.name === 'TIE_Attack_Madine_1');
  const rec = orig.slice(it.offset, it.offset + dv.getUint32(it.offset + 8)), rv = new DataView(rec.buffer);
  const no = rv.getUint32(0x0C), name = 'Brand_New_TIE_X';
  const renamed = new Uint8Array(no + name.length + 1 + 3 & ~3);
  renamed.set(rec.subarray(0, no));
  renamed.set(new TextEncoder().encode(name), no);
  new DataView(renamed.buffer).setUint32(8, renamed.length);
  new DataView(renamed.buffer).setUint32(0x8C, 0);
  const added = RS64.datAddItem(orig, 0, renamed);
  const count0 = RS64.parseDAT(new DataView(added.buffer)).items.filter((x) => x.type === 0).length;
  assert.deepEqual(RS64.datLookup(added, name), { type: 0, index: count0 - 1 });
  assert.equal(RS64.datLookup(orig, name), null);
  lookupAll(added);
  const removed = RS64.datRemoveItem(added, 0, 0);
  assert.deepEqual(RS64.datLookup(removed, name), { type: 0, index: count0 - 2 });
  lookupAll(removed);
  const grown = RS64.waveSet(removed, RS64.parseDAT(new DataView(removed.buffer)).items.find((x) => x.name === 'TIE_Attack_Madine_2'), { path: 'A_Longer_Path_Name_Than_Before_X' });
  lookupAll(grown);
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
    // The name map changes too: a delete shifts later indices, an add shifts the later bucket offsets (0x400 list).
    assert.ok(RS64.encodeBPS(orig, removed).length < 300 + objs.length * 5 + before.length * 8, `${p} delete patch ${RS64.encodeBPS(orig, removed).length} bytes`);
    assert.ok(RS64.encodeBPS(orig, added).length < 300 + objs.length * 5 + 0x400, `${p} duplicate patch ${RS64.encodeBPS(orig, added).length} bytes`);
    // The name map (block at 0x40) moves with the sections and lists the copy too.
    lookupAll(added);
    lookupAll(removed);
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
  assert.deepEqual(back.events.map((e) => e.frame), c.events.map((e, i) => (e.type === 10 ? ev[i] + back.frames - frames : ev[i])));
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
  assert.ok(rotate(rot(0.1), [0, 0, 1])[0] > 0.95, 'nose (model +Z, as every stock craft) along +X');
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
  const t = 0.45, cam = camAt(c, k, t), a = [4, 5, 6, 7].map((ch) => RS64.cutsceneValue(c.actors[i].ch[ch], t, c.frames, 1));
  const look = rotate([cam[3], cam[4], cam[5], cam[6]], [0, 0, 1]), nose = rotate(a, [0, 0, 1]);
  assert.ok(look[0] * nose[0] + look[1] * nose[1] + look[2] * nose[2] > 0.99, 'ride looks where the nose points');
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
  const nose = rotate([...RS64.cutQuatToAxisAngle(s.q)], [0, 0, 1]), dir = [0, 1, 2].map((k) => tp[k] - s.p[k]), dist = Math.hypot(...dir);
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
await check('cutRetimeSnaps keeps absolute times and drops snapshots past the new end', async () => {
  const snaps = [{ t: 0, p: [0, 0, 0] }, { t: 0.25, p: [1, 0, 0] }, { t: 0.5, p: [2, 0, 0] }, { t: 1, p: [3, 0, 0] }];
  const short = RS64.cutRetimeSnaps(snaps, 10, 5);
  assert.deepEqual(short.map((s) => s.t), [0, 0.5, 1]);
  assert.deepEqual(short[1].p, [1, 0, 0]);
  assert.deepEqual(RS64.cutRetimeSnaps(snaps, 10, 20).map((s) => s.t), [0, 0.125, 0.25, 0.5]);
});
await check('cutSetLength keeps the ending transition its distance from the end', async () => {
  const dv = await rom.read('data/cuts/id0_intro'), c = RS64.parseCutscene(new DataView(new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength).slice().buffer));
  const end = (d) => d.frames - d.events.find((e) => e.type === 10).frame, before = end(c);
  RS64.cutSetLength(c, 30);
  assert.equal(end(c), before);
  RS64.cutSetLength(c, 10);
  assert.equal(end(c), before);
});
// Unbake the old sidecar, bake the new one, and write / parse as an edit does.
const rebake = (c, old, next) => {
  RS64.cutUnbake(c, old);
  RS64.cutBake(c, next);
  return reparse(c);
};
const bakeScene = async () => {
  const c = await cutOf('data/cuts/id5_intro');
  RS64.cutBlank(c, 10);
  return { c, i: RS64.cutAddActor(c, 'pl_crafts/xwing', -1, 0.056) };
};
const keyCount = (c, i) => c.actors[i].ch.reduce((n, k) => n + k.length / 3, 0);
const allFinite = (c, i) => c.actors[i].ch.every((k) => k.every(Number.isFinite));
const xAt = (c, i, t) => RS64.cutsceneValue(c.actors[i].ch[0], t, c.frames, 1);
await check('re-baking the same snapshots keeps the key count and finite slopes (float32 key times)', async () => {
  let { c, i } = await bakeScene();
  const doc = { tracks: { [`act:${i}`]: { snaps: [{ t: 0.1, p: [0, 0, 0] }, { t: 0.2, p: [100, 0, 0] }, { t: 0.3, p: [200, 0, 50] }] } } };
  const counts = [];
  for (let k = 0; k < 4; k++) {
    c = rebake(c, k ? doc : { tracks: {} }, doc);
    counts.push(keyCount(c, i));
  }
  assert.ok(allFinite(c, i), 'finite');
  assert.deepEqual(counts.slice(1), [counts[0], counts[0], counts[0]]);
});
await check('moving the last snapshot earlier leaves no key of the old path', async () => {
  let { c, i } = await bakeScene();
  const a = { tracks: { [`act:${i}`]: { snaps: [{ t: 0.1, p: [0, 0, 0] }, { t: 0.3, p: [400, 0, 0] }] } } };
  const b = { tracks: { [`act:${i}`]: { snaps: [{ t: 0.1, p: [0, 0, 0] }, { t: 0.2, p: [100, 0, 0] }] } } };
  c = rebake(c, { tracks: {} }, a);
  c = rebake(c, a, b);
  assert.ok(Math.abs(xAt(c, i, 0.5) - 100) < 1, `x at 0.5 = ${xAt(c, i, 0.5)}`);
});
await check('a single snapshot holds its value everywhere and moves cleanly', async () => {
  let { c, i } = await bakeScene();
  const a = { tracks: { [`act:${i}`]: { snaps: [{ t: 0.3, p: [400, 0, 0] }] } } }, b = { tracks: { [`act:${i}`]: { snaps: [{ t: 0.6, p: [800, 0, 0] }] } } };
  c = rebake(c, { tracks: {} }, a);
  for (const t of [0, 0.15, 0.3, 0.9]) assert.ok(Math.abs(xAt(c, i, t) - 400) < 1e-3, `t ${t}: ${xAt(c, i, t)}`);
  const n = keyCount(c, i);
  c = rebake(c, a, b);
  c = rebake(c, b, b);
  for (const t of [0, 0.3, 0.6, 0.9]) assert.ok(Math.abs(xAt(c, i, t) - 800) < 1e-3, `t ${t}: ${xAt(c, i, t)}`);
  assert.equal(keyCount(c, i), n);
  assert.ok(allFinite(c, i));
});
await check('a path holds its first and last snapshot outside them', async () => {
  let { c, i } = await bakeScene();
  c = rebake(c, { tracks: {} }, { tracks: { [`act:${i}`]: { snaps: [{ t: 0.3, p: [400, 0, 0] }, { t: 0.6, p: [800, 0, 0] }] } } });
  assert.ok(Math.abs(xAt(c, i, 0) - 400) < 1e-3 && Math.abs(xAt(c, i, 0.15) - 400) < 1e-3, 'held before');
  assert.ok(Math.abs(xAt(c, i, 0.9) - 800) < 1e-3, 'held after');
});
await check('cutBlank keeps the ending transition its distance from the end at any length', async () => {
  const c0 = await cutOf('data/cuts/id0_intro'), end = (d) => d.frames - d.events.find((e) => e.type === 10).frame, d0 = end(c0);
  for (const s of [6, 10, 30]) {
    const c = await cutOf('data/cuts/id0_intro');
    RS64.cutBlank(c, s);
    assert.equal(end(c), d0, `${s} s`);
  }
});
await check('re-baking a shot keeps its eased aim and its three events', async () => {
  let { c, i } = await bakeScene();
  const j = RS64.cutAddActor(c, 'imp_stuff/probe_droid', -1, 0.24);
  [0, 1].forEach((t) => RS64.cutSetKey(c, { actor: j, ch: 0 }, t, 200));
  c = reparse(c);
  const doc = { tracks: { [`act:${i}`]: { shots: [{ target: j, t: 0.4 }] } } }, rot = (d) => [0.3, 0.37, 0.4, 0.43, 0.5].map((t) => d.actors[i].ch.slice(4, 8).map((k) => +RS64.cutsceneValue(k, t, d.frames, 1).toFixed(4)));
  c = rebake(c, { tracks: {} }, doc);
  const first = rot(c), evs = c.events.length;
  c = rebake(c, doc, doc);
  c = rebake(c, doc, doc);
  assert.deepEqual(rot(c), first);
  assert.equal(c.events.length, evs);
  assert.ok(allFinite(c, i));
});
await check('cutSyncModeSpans moves a camera mode with its shot when a cut moves', async () => {
  const { c } = await bakeScene();
  const k = RS64.cutAddCamera(c), F = c.frames, cut = { type: 4, frame: 100, p: [k, 0, 0, 0] };
  RS64.cutAddEvent(c, cut);
  const doc = { tracks: { [`cam:${k}`]: { modes: [{ mode: 'aim', actor: 0, t0: 100 / F, t1: 1 }] } } };
  cut.frame = 140;
  RS64.cutSyncModeSpans(doc, c);
  assert.deepEqual([doc.tracks[`cam:${k}`].modes[0].t0, doc.tracks[`cam:${k}`].modes[0].t1], [140 / F, 1]);
});
await check('shotLinkSet adds, replaces and removes one link and keeps the rest', async () => {
  let t = RS64.shotLinkSet('# mine\nlv_1 a cam_x\n', 'lv_0', 't', 'cam_a');
  assert.equal(t, '# mine\nlv_1 a cam_x\nlv_0 t cam_a\n');
  t = RS64.shotLinkSet(t, 'lv_0', 't', 'cam_b');
  assert.equal(t, '# mine\nlv_1 a cam_x\nlv_0 t cam_b\n');
  assert.equal(RS64.shotLinkSet(t, 'lv_0', 't', null), '# mine\nlv_1 a cam_x\n');
  assert.equal(RS64.shotLinkSet(t, 'lv_0', 't', 'cam_b', true), '# mine\nlv_1 a cam_x\nlv_0 t cam_b personal\n');
});
await check('shotDestroySet keeps camera lines and shotLinkSet keeps destroy lines', async () => {
  let t = RS64.shotDestroySet('# mine\nlv_0 t cam_a\nlv_0 t destroy old\nlv_1 t destroy x\n', 'lv_0', 't', ['tower_1', 'tower_2']);
  assert.equal(t, '# mine\nlv_0 t cam_a\nlv_1 t destroy x\nlv_0 t destroy tower_1\nlv_0 t destroy tower_2\n');
  assert.deepEqual(RS64.shotDestroyGet(t, 'lv_0', 't'), ['tower_1', 'tower_2']);
  assert.deepEqual(RS64.shotDestroyGet('lv_0 t destroy a\r\nlv_0 t destroy a\nlv_0 t destroy\n', 'lv_0', 't'), ['a']);
  assert.deepEqual(RS64.shotLinkGet(t, 'lv_0', 't'), { camera: 'cam_a', personal: false });
  assert.equal(RS64.shotLinkGet('lv_0 u destroy gate\n', 'lv_0', 'u'), null);
  t = RS64.shotLinkSet(t, 'lv_0', 't', 'cam_b');
  assert.equal(t, '# mine\nlv_0 t cam_b\nlv_1 t destroy x\nlv_0 t destroy tower_1\nlv_0 t destroy tower_2\n');
  t = RS64.shotLinkSet(t, 'lv_0', 't', null);
  assert.deepEqual(RS64.shotDestroyGet(t, 'lv_0', 't'), ['tower_1', 'tower_2']);
  assert.equal(RS64.shotDestroySet(t, 'lv_0', 't', []), '# mine\nlv_1 t destroy x\n');
});
const lv3 = async () => bytesOf(await rom.read('data/level/lv_3/dat'));
const itemNamed = (bytes, name) => RS64.parseDAT(new DataView(bytes.buffer)).items.find((x) => x.name === name);
await check('waveGet reads stock TIE gating', async () => {
  const b = await lv3(), dv = new DataView(b.buffer);
  assert.deepEqual(RS64.waveGet(dv, itemNamed(b, 'TIE_Attack_Madine_1')), { flag: 0x1D, counter: 0, atLeast: 0, gridSpawn: 0, path: 'Madine_Attack' });
  assert.ok(RS64.WAVE_FIELDS[0x0C] && RS64.WAVE_FIELDS[0x13] && RS64.WAVE_FIELDS[0x1F]);
  assert.ok(!RS64.WAVE_FIELDS[0x0D] && !RS64.WAVE_FIELDS[0x37] && !RS64.WAVE_FIELDS[0x26]);
  assert.equal(RS64.waveGet(dv, itemNamed(b, 'Wedge')), null);
});
await check('waveSet then waveGet round-trips and changes only those bytes', async () => {
  const b = await lv3(), it = itemNamed(b, 'TIE_Attack_Madine_1');
  const same = RS64.waveSet(b, it, { flag: 7, counter: 3, atLeast: 2 });
  assert.equal(same.length, b.length);
  for (let i = 0; i < b.length; i++) if (same[i] !== b[i]) assert.ok(i >= it.offset + 0x60 && i < it.offset + 0x6C, `byte ${i - it.offset} changed`);
  assert.deepEqual(RS64.waveGet(new DataView(same.buffer), itemNamed(same, 'TIE_Attack_Madine_1')), { flag: 7, counter: 3, atLeast: 2, gridSpawn: 0, path: 'Madine_Attack' });
  const short = RS64.waveSet(b, it, { path: 'Ab' });
  assert.equal(short.length, b.length);
  assert.equal(RS64.waveGet(new DataView(short.buffer), itemNamed(short, 'TIE_Attack_Madine_1')).path, 'Ab');
  // A longer path grows the record; it keeps its index and every other item parses the same.
  const long = RS64.waveSet(b, it, { path: 'A_Much_Longer_Spline_Name_Than_Before', gridSpawn: 0 });
  const before = RS64.parseDAT(new DataView(b.buffer)).items, after = RS64.parseDAT(new DataView(long.buffer)).items;
  assert.equal(after.length, before.length);
  const moved = after.find((x) => x.name === 'TIE_Attack_Madine_1');
  assert.equal(moved.index, it.index);
  assert.equal(RS64.waveGet(new DataView(long.buffer), moved).path, 'A_Much_Longer_Spline_Name_Than_Before');
  assert.deepEqual(after.filter((x) => x !== moved).map(strip), before.filter((x) => x.name !== 'TIE_Attack_Madine_1').map(strip));
  const ldv = new DataView(long.buffer), odv = new DataView(b.buffer);
  for (let k = 0; k < 0x8C; k += 4) if (k !== 8) assert.equal(ldv.getUint32(moved.offset + k), odv.getUint32(it.offset + k), `word +${k.toString(16)}`);
});
await check('wave flags avoid the level\'s flags and reuse a wave\'s own', async () => {
  const b = bytesOf(await rom.read('data/level/lv_2/dat')), used = RS64.levelUsedFlags(new DataView(b.buffer));
  assert.ok(used.has(128) && used.has(99) && used.has(1) && !used.has(127) && !used.has(0));
  assert.equal(RS64.waveFlagFor({}, 'Ambush', used), 127);
  assert.equal(RS64.waveFlagFor({ Ambush: 127 }, 'Ambush', used), 127);
  assert.equal(RS64.waveFlagFor({ Ambush: 127 }, 'Second', used), 126);
  assert.equal(RS64.waveFlagFor({ Old: 126 }, 'Old', new Set([126])), 126);
  const lv3b = await lv3(), lv3used = RS64.levelUsedFlags(new DataView(lv3b.buffer));
  assert.ok(lv3used.has(0x1D) && lv3used.has(40));
});
await check('an enemy copied from another level parses intact', async () => {
  const from = await lv3(), it = itemNamed(from, 'TIE_Attack_Madine_1'), fdv = new DataView(from.buffer);
  const to = bytesOf(await rom.read('data/level/lv_0/dat'));
  const out = RS64.datAddItem(to, 0, from.slice(it.offset, it.offset + fdv.getUint32(it.offset + 8)));
  const odv = new DataView(out.buffer), copy = RS64.parseDAT(odv).items.filter((x) => x.type === 0).at(-1);
  assert.equal(strip(copy), strip(it));
  assert.deepEqual(RS64.waveGet(odv, copy), RS64.waveGet(fdv, it));
});
await check('a trigger whose enter block sets or clears a stock flag is not free for a wave', async () => {
  assert.equal(RS64.triggerWave({ flag: 0, value: 0 }, {}), 'free');
  assert.equal(RS64.triggerWave({ flag: 128, value: 1 }, { Ambush: 128 }), 'wave');
  assert.equal(RS64.triggerWave({ flag: 5, value: 1 }, { Ambush: 128 }), 'stock');
  assert.equal(RS64.triggerWave({ flag: 5, value: 0 }, {}), 'stock');
});
await check('datAddEnemy places a copied enemy with no wave and no path, whatever its source', async () => {
  const from = await lv3(), it = itemNamed(from, 'TIE_Attack_Madine_1'), fdv = new DataView(from.buffer);
  const to = bytesOf(await rom.read('data/level/lv_0/dat'));
  const out = RS64.datAddEnemy(to, from.slice(it.offset, it.offset + fdv.getUint32(it.offset + 8)), [1, 2, 3]);
  const odv = new DataView(out.buffer), copy = RS64.parseDAT(odv).items.filter((x) => x.type === 0).at(-1);
  assert.deepEqual(copy.pos, [1, 2, 3]);
  assert.deepEqual(RS64.waveGet(odv, copy), { flag: 0, counter: 0, atLeast: 0, gridSpawn: 0, path: '-' });
  assert.ok(RS64.WAVE_FOREIGN.has(0x0C) && RS64.WAVE_FOREIGN.has(0x12) && !RS64.WAVE_FOREIGN.has(0x0F));
});
const splineTable = (dv, s) => {
  const n = dv.getUint32(s.offset + 0x1C), fo = s.offset + dv.getUint32(s.offset + 0x20) + n * 12;
  return Array.from({ length: n - 2 }, (_, k) => dv.getFloat32(fo + k * 4));
};
await check('spline recompute matches stock on every path', async () => {
  let n = 0;
  for (const p of datLevels) {
    const b = bytesOf(await rom.read(p)), dv = new DataView(b.buffer);
    for (const s of RS64.parseDAT(dv).items.filter((x) => x.type === 3 && x.sub === 3 && x.points?.length >= 4)) {
      const out = RS64.splineSetPoints(b, s, RS64.splinePoints(dv, s)), odv = new DataView(out.buffer), o = RS64.parseDAT(odv).items.find((x) => x.type === 3 && x.index === s.index);
      assert.ok(Math.abs(odv.getFloat32(o.offset + 0x24) - dv.getFloat32(s.offset + 0x24)) <= 0.01 * dv.getFloat32(s.offset + 0x24) + 1e-3, `${p} ${s.name} length`);
      splineTable(odv, o).forEach((v, k) => assert.ok(Math.abs(v - splineTable(dv, s)[k]) < 0.01, `${p} ${s.name} k${k}`));
      n++;
    }
  }
  assert.ok(n > 600, `${n} paths`);
});
await check('dragging a path point recomputes length and table', async () => {
  const b = await lv3(), s = itemNamed(b, 'Madine_Attack'), dv = new DataView(b.buffer), pts = RS64.splinePoints(dv, s);
  assert.deepEqual(pts[0].map((v) => +v.toFixed(2)), [7.93, -7.5, 134.52]);
  pts[4] = [pts[4][0] + 20, pts[4][1], pts[4][2]];
  const out = RS64.splineSetPoints(b, s, pts), odv = new DataView(out.buffer), o = itemNamed(out, 'Madine_Attack');
  assert.ok(odv.getFloat32(o.offset + 0x24) > dv.getFloat32(s.offset + 0x24) + 5);
  assert.deepEqual(RS64.splinePoints(odv, o).map((p) => p.map((v) => +v.toFixed(3))), pts.map((p) => p.map((v) => +v.toFixed(3))));
  const t = splineTable(odv, o);
  assert.equal(t[0], 0);
  assert.equal(t.at(-1), 1);
  for (let k = 1; k < t.length; k++) assert.ok(t[k] >= t[k - 1]);
  pts[3] = pts[2].slice();
  const flat = RS64.splineSetPoints(b, s, pts), fdv = new DataView(flat.buffer);
  assert.ok(splineTable(fdv, itemNamed(flat, 'Madine_Attack')).every(Number.isFinite));
});
const eventSubs = (bytes, s) => {
  const items = RS64.parseDAT(new DataView(bytes.buffer)).items;
  return (s.events ?? []).map((e) => items.find((x) => x.type === 7 && x.index === e.trigger)).map((t) => (t.sub === 0x4E ? '78:' + t.pathEvent.target : t.sub));
};
await check('a path copied from another level keeps its shape and safe events, under a findable unique name', async () => {
  const from = await lv3(), to = bytesOf(await rom.read('data/level/lv_0/dat'));
  const src = itemNamed(from, 'Madine_Attack'), name = RS64.uniqueName(to, 'Madine_Attack');
  assert.equal(name, 'Madine_Attack');
  const e0 = bytesOf(await rom.read('data/level/lv_e/dat')), t0 = RS64.parseDAT(new DataView(e0.buffer)).items.find((x) => x.type === 7 && x.sub === 0x4E && !x.pathEvent.target);
  const attack = e0.slice(t0.offset, t0.offset + new DataView(e0.buffer).getUint32(t0.offset + 8));
  const { bytes, dropped } = RS64.splineCopy(from, src, to, { name, at: [10, -5, 20], attack });
  assert.equal(dropped, 0);
  // A copy with no continuation would run its craft off the end (a crash), so it ends in an attack on the player.
  assert.equal(RS64.splineAttackEnd(bytes, itemNamed(bytes, name)), true);
  const dv = new DataView(bytes.buffer), copy = itemNamed(bytes, name);
  assert.deepEqual(RS64.datLookup(bytes, name), { type: 3, index: copy.index });
  const a = RS64.splinePoints(new DataView(from.buffer), src), b = RS64.splinePoints(dv, copy);
  assert.deepEqual(b[1].map((v) => +v.toFixed(3)), [10, -5, 20]);
  b.forEach((p, k) => p.forEach((v, ax) => assert.ok(Math.abs(v - a[k][ax] - (b[1][ax] - a[1][ax])) < 1e-3)));
  // Continuation blocks no longer name Madine_Attack_2.
  for (let k = 0; k < 8; k++) assert.equal(dv.getUint8(copy.offset + dv.getUint32(copy.offset + 0x60 + k * 16)), 0);
  assert.equal(RS64.uniqueName(bytes, 'Madine_Attack'), 'Madine_Attack_2');
  // Names other files use (rules naming a deleted object) are skipped too.
  assert.equal(RS64.uniqueName(bytes, 'Madine_Attack', new Set(['Madine_Attack_2', 'Madine_Attack_3'])), 'Madine_Attack_4');
  lookupAll(bytes);
  // A path with events: kept ones point at copied items of the same kind, the others are dropped and counted.
  let found = false;
  for (const p of datLevels) {
    const f = bytesOf(await rom.read(p));
    for (const s of RS64.parseDAT(new DataView(f.buffer)).items.filter((x) => x.type === 3 && x.events?.length > 1)) {
      const subs = eventSubs(f, s), keep = subs.filter((x) => [0x1A, 0x1B, 0x1C, 0x23].includes(x) || x === '78:');
      if (!keep.length || keep.length === subs.length) continue;
      const r = RS64.splineCopy(f, s, to, { name: 'Copied_Path_X', at: [0, 0, 0] });
      assert.equal(r.dropped, subs.length - keep.length);
      assert.deepEqual(eventSubs(r.bytes, itemNamed(r.bytes, 'Copied_Path_X')), keep);
      found = true;
      break;
    }
    if (found) break;
  }
  assert.ok(found, 'a stock path with mixed events');
});
await check('the attack-at-end switch adds and removes one fly-at-player event', async () => {
  const b = await lv3(), s = itemNamed(b, 'Madine_Attack'), e = bytesOf(await rom.read('data/level/lv_e/dat'));
  const tpl = RS64.parseDAT(new DataView(e.buffer)).items.find((x) => x.type === 7 && x.sub === 0x4E && !x.pathEvent.target), tdv = new DataView(e.buffer);
  const template = e.slice(tpl.offset, tpl.offset + tdv.getUint32(tpl.offset + 8));
  assert.equal(RS64.splineAttackEnd(b, s), false);
  const on = RS64.splineSetAttackEnd(b, s, true, template), so = itemNamed(on, 'Madine_Attack');
  assert.equal(RS64.splineAttackEnd(on, so), true);
  assert.deepEqual(eventSubs(on, so), ['78:']);
  assert.ok(Math.abs(so.events[0].at - RS64.PATH_ATTACK_AT) < 1e-6 && RS64.PATH_ATTACK_AT < 1);
  lookupAll(on);
  const off = RS64.splineSetAttackEnd(on, so, false), sf = itemNamed(off, 'Madine_Attack');
  assert.equal(RS64.splineAttackEnd(off, sf), false);
  assert.equal(sf.events?.length ?? 0, 0);
});
await check('levels.json round-trips and a new level gets a unique plain folder', async () => {
  let levels = RS64.modLevelsParse('');
  assert.deepEqual(levels, []);
  const a = RS64.modLevelNew(levels, 'Dune Sea Ambush!', 0);
  assert.deepEqual(a, { id: 'dune_sea_ambush', title: 'Dune Sea Ambush!', description: '', template: 0, folder: 'dune_sea_ambush', medals: RS64.MEDALS_SIMPLE });
  levels = [a];
  assert.equal(RS64.modLevelNew(levels, 'dune sea ambush', 3).folder, 'dune_sea_ambush_2');
  const text = RS64.modLevelsWrite(levels);
  assert.deepEqual(RS64.modLevelsParse(text), levels);
  assert.deepEqual(RS64.modLevelsParse('not json'), []);
  assert.ok(RS64.modLevelNew([], 'x'.repeat(80), 1).folder.length <= 48);
});
await check('objectives wire units and triggers in the DAT and read back; re-applying clears the old wiring', async () => {
  const dv = await rom.read('data/level/lv_1/dat'), orig = new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength).slice();
  const items = RS64.parseDAT(dv).items, units = items.map((it, i) => [it, i]).filter(([it]) => RS64.isUnit(it)).map(([, i]) => i);
  const zone = items.findIndex((it) => it.type === 7 && it.sub === 0x19);
  const objs = [
    { text: 'Kill', goal: 'destroy', targets: units.slice(0, 3), voice: 93, camera: '' },
    { text: 'Go', goal: 'reach', targets: [zone] },
    { text: 'Guard', goal: 'protect', targets: [units[5]] },
    { text: 'Wait', goal: 'survive', seconds: 30, targets: [] },
  ];
  const a = RS64.objectivesApply(orig, null, objs), adv = new DataView(a.bytes.buffer);
  assert.equal(a.bytes.length, orig.length);
  const [kill, go, guard, wait] = a.objectives;
  assert.equal(kill.count, 3);
  assert.equal(kill.voice, 93);
  assert.ok(!('camera' in kill));
  assert.ok(kill.counter >= 1 && kill.counter <= 128 && go.flag !== guard.flag);
  assert.deepEqual(RS64.objectiveTargets(adv, kill).map((it) => items.indexOf(items.find((x) => x.offset === it.offset))), units.slice(0, 3));
  assert.equal(RS64.objectiveTargets(adv, go).length, 1);
  assert.equal(RS64.objectiveTargets(adv, guard)[0].offset, items[units[5]].offset);
  assert.deepEqual(wait, { text: 'Wait', goal: 'survive', seconds: 30 });
  const b = RS64.objectivesApply(a.bytes, a.objectives, [{ text: 'Kill', goal: 'destroy', targets: [units[0]] }]);
  assert.equal(RS64.objectiveTargets(new DataView(b.bytes.buffer), b.objectives[0]).length, 1);
  assert.equal(RS64.objectiveTargets(new DataView(b.bytes.buffer), go).length, 0);
});
await check('deleting a zone or trigger keeps every path event and zone link on the same item', async () => {
  for (const lv of ['lv_0', 'lv_1', 'lv_f']) {
    const dv = await rom.read(`data/level/${lv}/dat`), orig = new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength).slice();
    const items = RS64.parseDAT(dv).items, key = (it) => it && `${it.sub}:${it.name}:${it.pos.join(',')}`;
    const t7 = (list, i) => list.find((x) => x.type === 7 && x.index === i), t6 = (list, i) => list.find((x) => x.type === 6 && x.index === i);
    const fired = new Set(items.filter((x) => x.type === 3).flatMap((x) => (x.events ?? []).map((e) => e.trigger)));
    const victims = [items.find((x) => x.type === 7 && !x.pathEvent && !fired.has(x.index)), items.find((x) => x.type === 6)].filter(Boolean);
    for (const v of victims) {
      const out = RS64.datRemoveIndexed(orig, v.type, v.index), after = RS64.parseDAT(new DataView(out.buffer, out.byteOffset, out.byteLength)).items;
      const paths = (list) => list.filter((x) => x.type === 3).map((p) => (p.events ?? []).map((e) => key(t7(list, e.trigger))).join('|'));
      assert.deepEqual(paths(after), paths(items), `${lv} path events after deleting ${v.name}`);
      const links = (list) => list.filter((x) => x.type === 6 && x !== v && key(x) !== key(v)).map((z) => (z.zoneLinks ?? []).filter((i) => !(v.type === 6 && list === items && i === v.index)).map((i) => key(t6(list, i))).join('|'));
      assert.deepEqual(links(after), links(items), `${lv} zone links after deleting ${v.name}`);
    }
    const used = items.find((x) => x.type === 7 && fired.has(x.index));
    if (used) assert.throws(() => RS64.datRemoveIndexed(orig, 7, used.index), /fires it/);
  }
});
await check('a music region plays the picked cue on entry (slots 0-2 set, slot 3 kept)', async () => {
  const dv = await rom.read('data/level/lv_1/dat'), orig = new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength).slice();
  const zone = RS64.parseDAT(dv).items.find((it) => it.type === 6 && it.sub === 0x10 && it.songs);
  const out = RS64.musicZoneSet(orig, zone, 41), back = RS64.parseDAT(new DataView(out.buffer)).items.find((it) => it.offset === zone.offset);
  assert.deepEqual(back.songs, [41, 41, 41, zone.songs[3]]);
  assert.equal(out.length, orig.length);
});
await check('music cues: 49 songs, cue 2 = c1l1_theme, the level starting songs name cue-table songs', () => {
  const m = RS64.parseMusicCues(new Uint8Array(buf.buffer, buf.byteOffset, buf.length));
  assert.equal(m.cues.length, 49);
  assert.deepEqual([m.cues[0].cue, m.cues[0].name], [2, 'c1l1_theme']);
  assert.equal(m.cues.at(-1).name, 'addtl');
  const songs = new Set(m.cues.map((c) => c.song));
  assert.ok(m.levelSongs.every((s) => songs.has(s)));
});
await check('naming an unnamed path event: the game finds it by name and its fields stay', async () => {
  const dv = await rom.read('data/level/lv_1/dat'), orig = new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength).slice();
  const ev = RS64.parseDAT(dv).items.find((it) => it.pathEvent && !it.name);
  const out = RS64.datNameItem(orig, ev, 'rule_point_1'), back = RS64.parseDAT(new DataView(out.buffer)).items.find((it) => it.type === 7 && it.index === ev.index);
  assert.equal(back.name, 'rule_point_1');
  assert.deepEqual(back.pathEvent, ev.pathEvent);
  assert.equal(RS64.datLookup(out, 'rule_point_1').index, ev.index);
});
await check('rules round-trip and list their references', () => {
  const doc = RS64.rulesParse(JSON.stringify({ version: 1, rules: [
    { when: 'enter', object: 'bridge', who: 'player', actions: [{ do: 'spawn', kind: 'TIE fighter', count: 3, path: 'tie_path' }, { do: 'camera', camera: 'cam1' }] },
    { when: 'start', actions: [{ do: 'destroy', unit: 'shuttle' }] }] }));
  assert.equal(doc.rules.length, 2);
  assert.deepEqual(RS64.rulesParse(RS64.rulesWrite(doc)), doc);
  assert.deepEqual(RS64.ruleRefs(doc.rules[0]).map((r) => r.name), ['bridge', 'player', 'tie_path', 'cam1'].filter((n) => n !== 'player'));
  assert.deepEqual(RS64.ruleRefs(doc.rules[1]).map((r) => r.name), ['shuttle']);
  assert.deepEqual(RS64.rulesParse('not json'), { version: 1, rules: [] });
});
await check('renaming an object rewrites every rule that names it', () => {
  const doc = RS64.rulesParse(JSON.stringify({ version: 1, rules: [
    { when: 'enter', object: 'bridge', actions: [] }, { when: 'leave', object: 'bridge', actions: [] },
    { when: 'start', actions: [{ do: 'destroy', unit: 'bridge' }] }, { when: 'start', actions: [{ do: 'destroy', unit: 'other' }] }] }));
  assert.equal(RS64.rulesRename(doc, 'bridge', 'bridge_2'), 3);
  assert.ok(!JSON.stringify(doc).includes('"bridge"'));
  assert.equal(doc.rules[3].actions[0].unit, 'other');
});
await check('a rule whose object is gone is reported broken', async () => {
  const items = RS64.parseDAT(await rom.read('data/level/lv_1/dat')).items;
  const doc = RS64.rulesParse(JSON.stringify({ version: 1, rules: [
    { when: 'enter', object: 'event_win_the_level', actions: [] }, { when: 'destroyed', object: 'no_such_unit', actions: [] }] }));
  assert.deepEqual(RS64.rulesBroken(doc, items), [{ rule: 1, field: 'object', name: 'no_such_unit' }]);
});
await check('RULE_WHENS offer only kinds that fit', () => {
  const on = (kind) => RS64.RULE_WHENS.filter((w) => w.on === kind).map((w) => w.when);
  assert.deepEqual(on('area'), ['enter', 'leave']);
  assert.deepEqual(on('unit'), ['destroyed', 'healthBelow']);
  assert.deepEqual(on('path'), ['pathPoint']);
  assert.ok(on('level').includes('start') && on('level').includes('after'));
  assert.equal(RS64.RULES_PATH('ice'), 'mod/assets/level/ice/rules.json');
  assert.ok(RS64.RULE_ACTIONS.find((a) => a.do === 'spawn').params.some((p) => p.key === 'path' && p.picker === 'path'));
});
await check('rulesExtParse reads a mod\'s actions and events, skipping nameless ones', () => {
  const ext = RS64.rulesExtParse(JSON.stringify({
    actions: [{ do: 'shake', label: 'Shake the camera', params: [{ key: 'seconds', picker: 'seconds' }, { key: 'how', picker: 'teleporter' }] }, { label: 'no name' }],
    events: [{ custom: 'mod.tripped', label: 'The tripwire is crossed' }, { custom: '' }],
  }), 'shaker');
  assert.deepEqual(ext, [
    { kind: 'action', name: 'shake', label: 'Shake the camera', mod: 'shaker', params: [{ key: 'seconds', picker: 'seconds' }, { key: 'how', picker: 'text' }] },
    { kind: 'event', name: 'mod.tripped', label: 'The tripwire is crossed', mod: 'shaker', params: [] },
  ]);
  assert.deepEqual(RS64.rulesExtParse('not json', 'x'), []);
});
await check('a mod action\'s object parameters are references too', () => {
  const ext = RS64.rulesExtParse(JSON.stringify({ actions: [{ do: 'tow', params: [{ key: 'target', picker: 'unit' }, { key: 'force', picker: 'number' }] }] }), 'tower');
  const doc = { version: 1, rules: [{ when: 'start', actions: [{ do: 'tow', target: 'walker', force: 3 }] }] };
  assert.deepEqual(RS64.ruleRefs(doc.rules[0], ext), [{ field: 'actions.0.target', name: 'walker' }]);
  assert.deepEqual(RS64.ruleRefs(doc.rules[0]), []);
  assert.deepEqual(RS64.rulesBroken(doc, [{ name: 'other' }], ext), [{ rule: 0, field: 'actions.0.target', name: 'walker' }]);
  assert.equal(RS64.rulesRename(doc, 'walker', 'walker_2', ext), 1);
  assert.equal(doc.rules[0].actions[0].target, 'walker_2');
});
await check('a blank level keeps only the player start, on flat ground', async () => {
  const dv = await rom.read('data/level/lv_1/dat'), blank = RS64.datBlank(new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength).slice());
  const items = RS64.parseDAT(new DataView(blank.buffer, blank.byteOffset, blank.byteLength)).items;
  assert.deepEqual(items.map((it) => it.name), ['player_StartPosition']);
  const hv = await rom.read('data/level/lv_1/hmp'), flat = RS64.hmpFlat(new Uint8Array(hv.buffer, hv.byteOffset, hv.byteLength).slice());
  const g = RS64.hmpGrid(RS64.parseHMP(new DataView(flat.buffer, flat.byteOffset, flat.byteLength)));
  assert.ok(g.hts.every((h) => h === g.hts[0]));
});
await check('a mod level aliases its template folder and cutscenes, and an alias reads the template bytes', async () => {
  const al = RS64.modLevelAliases({ folder: 'dune', template: 0 }, rom.assets.keys());
  const map = new Map(al);
  assert.equal(map.get('data/level/dune/dat'), 'data/level/lv_0/dat');
  assert.equal(map.get('data/level/dune/hmp'), 'data/level/lv_0/hmp');
  assert.equal(map.get('data/cuts/dune_intro'), 'data/cuts/id0_intro');
  assert.equal(map.get('data/cuts/dune_extro'), 'data/cuts/id0_extro');
  assert.ok(![...map.keys()].some((k) => k.includes('lv_0_TEX') || k.startsWith('data/level/lv_1')));
  const r = RS64.openRom(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.length));
  for (const [to, from] of al) r.alias(to, from);
  assert.deepEqual(bytesOf(await r.read('data/level/dune/dat')), bytesOf(await r.read('data/level/lv_0/dat')));
  assert.equal(r.aliases.get('data/cuts/dune_intro'), 'data/cuts/id0_intro');
  // Templates 16-18 have no intro/extro of their own: none are aliased.
  assert.ok(!RS64.modLevelAliases({ folder: 'b', template: 16 }, rom.assets.keys()).some(([k]) => k.startsWith('data/cuts/')));
});
await check('shotLinkGet reads hand-written files', async () => {
  const t = '# c\r\nlv_0\tt\tcam_a\r\n\n  lv_0 t   cam_b  \nlv_2 only_two\n';
  assert.deepEqual(RS64.shotLinkGet(t, 'lv_0', 't'), { camera: 'cam_b', personal: false });
  assert.deepEqual(RS64.shotLinkGet('lv_0 t cam_c personal\nlv_0 u cam_d other\n', 'lv_0', 't'), { camera: 'cam_c', personal: true });
  assert.deepEqual(RS64.shotLinkGet('lv_0 u cam_d other\n', 'lv_0', 'u'), { camera: 'cam_d', personal: false });
  assert.equal(RS64.shotLinkGet(t, 'lv_2', 'only_two'), null);
  assert.equal(RS64.shotLinkGet('', 'lv_0', 't'), null);
});
await check('a project carries mod/camera_shots.txt with an empty original', async () => {
  const crc = RS64.crc32(new Uint8Array(buf.buffer, buf.byteOffset, buf.length)), p = 'mod/camera_shots.txt';
  const now = new TextEncoder().encode('lv_0 t cam_a\n'), history = RS64.createHistory();
  history.begin('Link');
  history.commit(p, new Uint8Array(0), now, 'Link');
  history.end();
  const bytes = RS64.projectWrite({ romCrc: crc, originals: new Map([[p, new Uint8Array(0)]]), current: new Map([[p, now]]), history });
  const back = await RS64.projectRead(bytes, crc, async () => new Uint8Array(0));
  assert.deepEqual([...back.current.get(p)], [...now]);
});

await check('stock crawls read from the ROM', async () => {
  const crawls = RS64.stockCrawls(RS64.parseFrontText(await rom.read('data/front_TXT')));
  assert.equal(crawls.length, 4);
  assert.deepEqual(crawls.map((c) => c.planet), [0, 1, 2, 3]);
  assert.equal(crawls[0].chapter, 'Chapter I');
  assert.equal(crawls[0].title, 'THE REBEL OPPOSITION');
  assert.ok(crawls[0].body.startsWith('Six months have passed') && !crawls[0].body.includes('*'));
  assert.equal(crawls[3].title, 'DARK EMPIRE');
  assert.equal(RS64.CRAWL_PLANETS.length, 4);
  assert.equal(RS64.CRAWL_PATH('shadows'), 'mod/assets/crawl/shadows/crawl.json');
});
await check('crawl json round trip', () => {
  const camera = [{ at: 0, pos: [0, 0, 0], rot: [0, 0, 0] }, { at: 88.5, pos: [0, 5, 1], rot: [20, 0, 0] }];
  const c = { chapter: 'Chapter V', title: 'SHADOWS', body: 'Text\n\nMore', planet: 'own', base: 2, replaces: 1, camera };
  assert.deepEqual(RS64.crawlParse(RS64.crawlWrite(c)), c);
  const s = { chapter: '', title: 'T', body: '', planet: 3, replaces: -1, camera: [] };
  assert.deepEqual(RS64.crawlParse(RS64.crawlWrite(s)), { ...s, base: 0 });
  const plain = JSON.parse(RS64.crawlWrite(s));
  assert.ok(!('base' in plain) && !('replaces' in plain) && !('camera' in plain));
  const none = { chapter: '', title: '', body: '', planet: 0, base: 0, replaces: -1, camera: [] };
  assert.deepEqual(RS64.crawlParse('not json'), none);
  assert.deepEqual(RS64.crawlParse('{"planet":9,"replaces":"x","title":5}'), none);
  // Camera keys: sorted by time, rotation optional, malformed ones dropped (as the host's parseCrawl).
  const keyed = RS64.crawlParse('{"camera":[{"at":5,"pos":[1,2,3]},{"at":1,"pos":[0,0,0],"rot":[1,2,3]},{"at":"x","pos":[1]}]}');
  assert.deepEqual(keyed.camera, [{ at: 1, pos: [0, 0, 0], rot: [1, 2, 3] }, { at: 5, pos: [1, 2, 3], rot: [0, 0, 0] }]);
});
await check('crawl camera eases between keys like the stock pan', () => {
  const near = (a, b) => assert.ok(Math.abs(a - b) < 1e-6, `${a} != ${b}`);
  const stock = RS64.CRAWL_STOCK_CAMERA;
  near(RS64.crawlCameraAt(stock, 50).pos[1], 0);
  near(RS64.crawlCameraAt(stock, 85.5).pos[1], 0.625);
  near(RS64.crawlCameraAt(stock, 86.5).pos[1], 2.5);
  near(RS64.crawlCameraAt(stock, 200).pos[1], 5);
  near(RS64.crawlCameraAt([{ at: 0, pos: [0, 0, 0], rot: [0, 0, 0] }, { at: 2, pos: [0, 0, 0], rot: [40, 0, 0] }], 1).rot[0], 20);
});
await check('crawl font gaps', () => {
  assert.deepEqual(RS64.crawlFontGaps('Rebels & "spies"#'), ['&', '"', '#']);
  assert.deepEqual(RS64.crawlFontGaps("It's (5/6): fine!\n"), []);
});
await check('levels.json crawl by name', () => {
  assert.equal(RS64.modLevelsParse(RS64.modLevelsWrite([{ id: 'a', crawl: 'shadows' }]))[0].crawl, 'shadows');
});
await check('crawl lines in the starwars50 font', async () => {
  const font = RS64.crawlFont(new Uint8Array(buf.buffer, buf.byteOffset, buf.length), await rom.read('data/fonts/starwars50'));
  assert.equal(font.cell, 64);
  assert.equal(font.pixels.length, (64 * 64 * 101) / 2);
  assert.equal(font.glyphs.get('A').width, 26);
  assert.equal(font.glyphs.get('A').index, 0);
  assert.ok(!font.glyphs.has('&'));
});
await check('crawl layout as buildModelViewerTextPage', async () => {
  const font = RS64.crawlFont(new Uint8Array(buf.buffer, buf.byteOffset, buf.length), await rom.read('data/fonts/starwars50'));
  // W advances 36/64 = 0.5625, a space 0.28. The fourth word passes 7.7 and leaves the line 0.19 a space too wide, so it is re-broken
  // before it and the three words are justified to 8.7; the paragraph's last line stays left. Centred lines: x += (8.7 - W) / 2.
  const { glyphs, height } = RS64.crawlLayout(font, '*}A\n\n*{T\n\nWWWW WWWW WWWW WWWW WWWW');
  const near = (a, b) => assert.ok(Math.abs(a - b) < 1e-4, `${a} != ${b}`);
  near(height, 7.2);
  const [chapter, title] = glyphs, body = glyphs.slice(2);
  assert.deepEqual([chapter.ch, chapter.xs, chapter.ys, chapter.y], ['A', 0.869565, 0.769231, 0]);
  near(chapter.x, -0.0652174 / 2);
  assert.deepEqual([title.ch, title.xs, title.ys, title.y], ['T', 1.2, 1.8, 2.4]);
  near(title.x, 0.05);
  assert.deepEqual(body.map((g) => g.y), [...Array(12).fill(4.8), ...Array(8).fill(6)]);
  const gap = (8.7 - (2 * 2.53 + 3 * 0.5625)) / 2;
  near(body[0].x, -4.35);
  near(body[4].x, 2.53 + gap - 4.35);
  near(body[8].x, 2 * (2.53 + gap) - 4.35);
  near(body[12].x, -4.35);
  near(body[16].x, 2.53 - 4.35);
  assert.equal(body[0].glyph, font.glyphs.get('W').index);
});

await check('crawl backdrop JPEG has the stock layout', async () => {
  // The game's JFIF decoder takes what the stock backdrops use: baseline, Y 2x1 (4:2:2), one DQT and one DHT segment.
  const layout = (b) => {
    const out = [];
    for (let i = 2; i < b.length - 1;) {
      const m = b[i + 1], len = (b[i + 2] << 8) | b[i + 3];
      out.push(m >= 0xC0 && m <= 0xC2 ? `sof${m - 0xC0}:${b[i + 9]}:${b[i + 11].toString(16)},${b[i + 14].toString(16)},${b[i + 17].toString(16)}:${(b[i + 5] << 8) | b[i + 6]}x${(b[i + 7] << 8) | b[i + 8]}` : m.toString(16));
      if (m === 0xDA) break;
      i += 2 + len;
    }
    return out.join(' ');
  };
  const stock = new Uint8Array((await rom.read('data/backdrop/planet_0')).buffer.slice(0));
  const rgba = new Uint8Array(640 * 128 * 4).map((_, i) => (i % 4 === 3 ? 255 : (i * 7) & 255));
  const jpeg = RS64.encodeJpeg(rgba, 640, 128);
  assert.equal(layout(jpeg), layout(stock));
  assert.deepEqual([jpeg[0], jpeg[1], jpeg.at(-2), jpeg.at(-1)], [0xFF, 0xD8, 0xFF, 0xD9]);
});

await check('glyph pack round trip (RSG1, as lang_core.h parse_glyph_pack)', () => {
  // The fixture from docs/modding-languages.md: U+30A2 width 3, space width 2, 4 x 2 cells.
  const bytes = new Uint8Array([0x52, 0x53, 0x47, 0x31, 0, 4, 0, 2, 0, 0, 0, 2, 0, 0, 0x30, 0xA2, 3, 0x12, 0x34, 0x56, 0x78, 0, 0, 0, 0x20, 2, 0, 0, 0, 0]);
  const pack = RS64.glyphPackParse(bytes);
  assert.equal(pack.cellW, 4);
  assert.equal(pack.cellH, 2);
  assert.deepEqual(pack.glyphs.map((g) => [g.cp, g.advance]), [[0x30A2, 3], [0x20, 2]]);
  assert.deepEqual([...pack.glyphs[0].pixels], [1, 2, 3, 4, 5, 6, 7, 8]);
  assert.deepEqual([...RS64.glyphPackWrite(pack)], [...bytes]);
  // The host skips these: wrong magic, odd width, a length that does not match the count.
  assert.equal(RS64.glyphPackParse(new Uint8Array([0x52, 0x53, 0x47, 0x32, 0, 4, 0, 2, 0, 0, 0, 0])), null);
  assert.equal(RS64.glyphPackParse(new Uint8Array([0x52, 0x53, 0x47, 0x31, 0, 3, 0, 2, 0, 0, 0, 0])), null);
  assert.equal(RS64.glyphPackParse(bytes.subarray(0, bytes.length - 1)), null);
});
await check('game fonts from the font table 0x8009FC10', () => {
  const fonts = RS64.gameFonts(new Uint8Array(buf.buffer, buf.byteOffset, buf.length));
  const by = Object.fromEntries(fonts.map((f) => [f.name, f]));
  assert.deepEqual(fonts.map((f) => f.name).sort(), ['credits', 'hatten13', 'italic35', 'normal10', 'normal11', 'starwars50', 'subtitle12', 'subtitle20', 'subtitle8']);
  assert.deepEqual([by.italic35.cellW, by.italic35.cellH, by.italic35.drawn, by.italic35.count], [22, 35, 33, 54]);
  assert.deepEqual([by.subtitle8.cellW, by.subtitle8.cellH, by.subtitle8.drawn], [6, 12, 10]);
  assert.ok(by.italic35.extended && !by.starwars50.extended && !by.credits.extended);
  assert.equal(by.starwars50.charset[0], 'A');
});
await check('font file coverage from its cmap (formats 4 and 12)', () => {
  // A minimal sfnt with one cmap table: a format 4 subtable for A-C, and a format 12 one for U+30A2-U+30A3.
  const be = (n, v) => (n === 2 ? [v >> 8, v & 255] : [v >>> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255]);
  const f4 = [...be(2, 4), ...be(2, 32), ...be(2, 0), ...be(2, 4), 0, 0, 0, 0, 0, 0, ...be(2, 0x43), ...be(2, 0xFFFF), 0, 0, ...be(2, 0x41), ...be(2, 0xFFFF), ...be(2, 1 - 0x41 + 65536), ...be(2, 1), ...be(2, 0), ...be(2, 0)];
  const f12 = [...be(2, 12), 0, 0, ...be(4, 28), ...be(4, 0), ...be(4, 1), ...be(4, 0x30A2), ...be(4, 0x30A3), ...be(4, 5)];
  const cmap = [...be(2, 0), ...be(2, 2), ...be(2, 3), ...be(2, 1), ...be(4, 20), ...be(2, 3), ...be(2, 10), ...be(4, 20 + f4.length), ...f4, ...f12];
  const sfnt = new Uint8Array([...be(4, 0x10000), ...be(2, 1), 0, 0, 0, 0, 0, 0, ...[0x63, 0x6D, 0x61, 0x70], ...be(4, 0), ...be(4, 28), ...be(4, cmap.length), ...cmap]);
  const has = RS64.fontCovers(sfnt);
  assert.deepEqual([0x40, 0x41, 0x43, 0x44, 0x30A2, 0x30A3, 0x30A4].map(has), [false, true, true, false, true, true, false]);
  assert.equal(RS64.fontCovers(new Uint8Array([0x77, 0x4F, 0x46, 0x32])), null);
});
await check('language text characters', () => {
  const front = JSON.stringify({ 162: '★ START ★', 163: 'Ok' }), game = JSON.stringify({ 0: 'モス・アイズリー', 1: 'é' });
  assert.deepEqual(RS64.langChars([front, game, 'not json']), [0xE9, 0x2605, 0x30A2, 0x30A4, 0x30B9, 0x30BA, 0x30EA, 0x30E2, 0x30FB, 0x30FC].sort((a, b) => a - b));
});
await check('change kinds by path', () => {
  const k = RS64.modChangeKind;
  for (const p of ['data/level/lv_3/dat', 'data/cuts/intro_Barkhesh_3', 'data/backdrop/planet_0', 'data/medal_info', 'mod/levels.json', 'mod/camera_shots.txt', 'mod/assets/level/x/rules.json', 'mod/assets/crawl/x/crawl.json']) assert.equal(k(p), 'levels', p);
  for (const p of ['data/pl_crafts/xwing_HOB', 'data/weapons_HOB', 'data/part_spr', 'data/explo2_IMGS', 'data/smkexp_PAL', 'data/gnrc_stuff/wtrspray_HOB', 'data/imp_stuff/wrld_dvfx']) assert.equal(k(p), 'models', p);
  for (const p of ['data/sound/sdir_SND', 'data/speech', 'data/voice_TXT']) assert.equal(k(p), 'audio', p);
  assert.equal(k('mod/roguesq_menu.json'), 'ui');
  for (const p of ['mod/lang/front.json', 'mod/lang/glyphs/italic35.bin', 'mod/lang/glyphs/FONTS.txt']) assert.equal(k(p), 'fonts', p);
  for (const p of ['data/reb_stuff/atpt_walk', 'data/blueprints/xwing']) assert.equal(k(p), 'models', p);
  for (const p of ['data/ov/radar', 'data/frontend/hangar/dat']) assert.equal(k(p), 'ui', p);
  assert.equal(k('data/frontend/starwars/dat'), 'levels');
  assert.equal(k('data/fonts/italic35'), 'fonts');
  assert.equal(k('data/front_TXT'), 'other');
});
await check('sidecars are no kind', () => {
  assert.equal(RS64.modChangeKind('explorer/glyph-fonts.json'), null);
  assert.equal(RS64.modChangeKind('explorer/menu-groups.json'), null);
});
await check('items go to their kind or their group', () => {
  const plan = RS64.exportPlan(['data/level/lv_3/dat', 'data/sound/sdir_SND', 'menu:ds_open', 'menu:gyro'],
    { groups: [{ name: 'Gyro toggle' }], assign: { 'menu:gyro': 'Gyro toggle' } }, [], { levels: 'P - Levels', audio: 'P - Audio', ui: 'P - UI' });
  assert.deepEqual(plan.map((e) => [e.name, e.keys]), [['P - Levels', ['data/level/lv_3/dat']], ['P - Audio', ['data/sound/sdir_SND']], ['P - UI', ['menu:ds_open']], ['Gyro toggle', ['menu:gyro']]]);
});
await check('linked items export together', () => {
  const plan = RS64.exportPlan(['data/cuts/intro_X', 'data/pl_crafts/xwing_HOB'], { groups: [], assign: {} }, [['data/cuts/intro_X', 'data/pl_crafts/xwing_HOB']], { levels: 'L', models: 'M' });
  assert.equal(plan.length, 1);
  assert.equal(plan[0].name, 'L');
  assert.deepEqual(plan[0].keys, ['data/cuts/intro_X', 'data/pl_crafts/xwing_HOB']);
  assert.deepEqual(plan[0].merged, ['M']);
});
await check('anchor across groups is a link', () => {
  const menu = { buttons: [{ id: 'a', menu: 'options' }, { id: 'b', menu: 'options', after: 'a' }, { id: 'p', type: 'submenu', page: 'pg', menu: 'options' }, { id: 'c', menu: 'pg' }] };
  assert.deepEqual(RS64.menuLinks(menu).sort(), [['menu:b', 'menu:a'], ['page:pg', 'menu:p']].sort());
});
await check('deleted group returns changes to their kind', () => {
  const plan = RS64.exportPlan(['menu:gyro'], { groups: [], assign: { 'menu:gyro': 'Gone' } }, [], { ui: 'U' });
  assert.deepEqual(plan.map((e) => e.name), ['U']);
});
await check('menu file round trip keeps unknown fields', () => {
  for (const mod of ['display-settings', 'multiplayer-native', 'quit-prompt', 'native-demo']) {
    const text = readFileSync(new URL(`../../../../mods/${mod}/roguesq_menu.json`, import.meta.url), 'utf8');
    const { doc, error } = RS64.menuParse(text);
    assert.equal(error, null, mod);
    assert.deepEqual(RS64.menuParse(RS64.menuWrite(doc)).doc, JSON.parse(text), mod);
  }
  assert.deepEqual(RS64.menuParse('').doc, { buttons: [] });
  assert.ok(RS64.menuParse('{ "buttons": 3 }').error);
  assert.ok(RS64.menuParse('{ nope').error);
});
await check('layout replaces and anchors like the game', () => {
  const gs = ['auto_roll', 'auto_level', 'free_camera', 'crosshairs', 'high_resolution', 'restore_defaults', 'back'].map((alias) => ({ alias, label: { en: alias } }));
  const doc = { buttons: [{ id: 'ds', menu: 'game_settings', replace: 'high_resolution' }, { id: 'x', menu: 'game_settings', after: 'auto_roll' }] };
  const { rows } = RS64.menuLayout(doc, 'game_settings', gs, () => true, 8);
  assert.deepEqual(rows.map((r) => r.alias ?? r.id), ['auto_roll', 'x', 'auto_level', 'free_camera', 'crosshairs', 'ds', 'restore_defaults', 'back']);
});
await check('layout drops spacers then cuts at 8', () => {
  const nat = [...Array(7)].map((_, i) => ({ alias: 'n' + i, label: { en: 'N' + i } }));
  nat.splice(3, 0, { alias: null, label: { en: '' }, spacer: true });
  const doc = { buttons: [{ id: 'a', menu: 'options' }, { id: 'b', menu: 'options' }] };
  const { rows, dropped } = RS64.menuLayout(doc, 'options', nat, () => true, 8);
  assert.equal(rows.length, 8);
  assert.ok(!rows.some((r) => r.native && !r.alias));
  assert.deepEqual(dropped.map((r) => r.id), ['b']);
});
await check('layout hides, requires, index and button anchors', () => {
  const nat = [{ alias: 'start', label: { en: 'START' } }, { alias: 'options', label: { en: 'OPTIONS' } }];
  const doc = { hide: { main_menu: ['options'] }, buttons: [{ id: 'q', menu: 'main_menu', requires: 'desktop' }] };
  assert.deepEqual(RS64.menuLayout(doc, 'main_menu', nat, (c) => c !== 'desktop', 8).rows.map((r) => r.alias ?? r.id), ['start']);
  const doc2 = { buttons: [{ id: 'a', menu: 'main_menu' }, { id: 'b', menu: 'main_menu', before: 'a' }, { id: 'c', menu: 'main_menu', index: 0 }, { id: 'd', menu: 'main_menu', requires: ['desktop', 'x'] }] };
  assert.deepEqual(RS64.menuLayout(doc2, 'main_menu', nat, (c) => c === 'desktop', 8).rows.map((r) => r.alias ?? r.id), ['c', 'start', 'options', 'b', 'a']);
});
await check('subset keeps only its buttons and their hides', () => {
  const doc = { '//': 'c', hide: { options: ['x'], pause: ['y'] }, buttons: [{ id: 'a', menu: 'options' }, { id: 'b', menu: 'pg' }] };
  assert.deepEqual(RS64.menuWriteSubset(doc, new Set(['a'])), { '//': 'c', hide: { options: ['x'] }, buttons: [{ id: 'a', menu: 'options' }] });
});
await check('menu font gaps', () => {
  assert.deepEqual(RS64.menuFontGaps('DISPLAY: ON'), []);
  assert.deepEqual(RS64.menuFontGaps('Draw (x)'), ['r', 'a', 'w', '(', 'x', ')']);
  assert.deepEqual(RS64.menuFontGaps('AUFLÖSUNG <>'), []);
});
await check('native menus match the game', () => {
  // kNativeAliases / pause_native_alias in src/game/menu_layout.h
  const cpp = {
    main_menu: ['start', 'options'], options: ['biographies', 'elite_rogues', 'passcodes', 'game_settings', 'sound_settings', 'controller_settings', 'back'],
    game_settings: ['auto_roll', 'auto_level', 'free_camera', 'crosshairs', 'high_resolution', 'restore_defaults', 'back'],
    pause_game_settings: ['auto_roll', 'auto_level', 'free_camera', 'crosshairs', 'displays', 'cockpit', 'back'],
  };
  for (const [menu, names] of Object.entries(cpp)) assert.deepEqual(RS64.NATIVE_MENUS[menu].entries.map((e) => e.alias).filter(Boolean), names, menu);
  assert.deepEqual(RS64.NATIVE_MENUS.game_settings.entries.map((e) => e.label.en), ['AUTO ROLL: ON', 'AUTO LEVEL: ON', 'FREE CAMERA: ON', 'CROSSHAIRS: ON', 'HIGH RESOLUTION', '', 'RESTORE DEFAULT SETTINGS', 'BACK']);
  assert.equal(RS64.NATIVE_MENUS.options.entries[3].spacer, true);
  assert.equal(RS64.NATIVE_MENUS.options.max, 8);
  assert.equal(RS64.NATIVE_MENUS.pause_game_settings.max, 16);
  assert.ok(RS64.NATIVE_MENUS.pause.locked && RS64.NATIVE_MENUS.controller_settings.locked);
});
await check('defaults merge by id', () => {
  const doc = RS64.menuWithDefaults({ buttons: [{ id: 'quit', menu: 'main_menu', label: 'LEAVE' }, { id: 'x', menu: 'options' }] });
  assert.deepEqual(doc.buttons.map((b) => b.id), ['language', 'quit', 'pause_quit', 'x']);
  assert.equal(doc.buttons[1].label, 'LEAVE');
  const opts = RS64.menuLayout(doc, 'options', RS64.NATIVE_MENUS.options.entries, () => true, 8).rows.map((r) => r.alias ?? r.id);
  assert.deepEqual(opts, ['biographies', 'elite_rogues', 'passcodes', 'game_settings', 'sound_settings', 'controller_settings', 'language', 'x']);
});

if (failed) { console.log(`${failed} failed`); process.exit(1); }
console.log('all passed');
