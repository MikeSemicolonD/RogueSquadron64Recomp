// node tools/web/rs64-explorer/tests/test_terrain_paint.mjs [rom.z64]   (US v1.0 ROM; without it only the encoder tests run)
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import assert from 'node:assert/strict';

createRequire(import.meta.url)('../rs64assets.js');

const { RS64 } = globalThis;
let failed = 0;
const check = (name, fn) => Promise.resolve().then(fn).then(() => console.log('ok  ', name), (e) => { failed++; console.log('FAIL', name, e.message); });

const c5 = (v) => (v << 3) | (v >> 2);
const round5551 = (c) => c5(c >> 3);

const image = (fn) => {
  const out = new Uint8Array(64 * 64 * 4);

  for (let y = 0; y < 64; y++)
    for (let x = 0; x < 64; x++)
      out.set([...fn(x, y), 255], (y * 64 + x) * 4);

  return out;
};

const decode = (bytes) => RS64.decodeTerrainTex(new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength), 0);

await check('encode solid', () => {
  const out = RS64.encodeTerrainTex(image(() => [8, 200, 96]));

  assert.equal(out.length, 2080);

  const px = decode(out);

  for (let i = 0; i < 4096; i++)
    assert.deepEqual([...px.subarray(i * 4, i * 4 + 4)], [round5551(8), round5551(200), round5551(96), 255]);
});

await check('encode 16 colours exact', () => {
  const colours = Array.from({ length: 16 }, (_, k) => [c5(k * 2), c5(31 - k), c5((k * 7) % 32)]);
  const src = image((x, y) => colours[(x + y * 3) % 16]);
  const px = decode(RS64.encodeTerrainTex(src));

  assert.deepEqual([...px], [...src]);
});

await check('encode gradient', () => {
  const src = image((x) => [x * 4, 255 - x * 4, 128]);
  const px = decode(RS64.encodeTerrainTex(src));
  let err = 0;

  for (let i = 0; i < 4096; i++)
    for (let ch = 0; ch < 3; ch++)
      err += Math.abs(px[i * 4 + ch] - src[i * 4 + ch]);

  assert.ok(err / (4096 * 3) < 6, `mean error ${(err / (4096 * 3)).toFixed(2)}`);
});

await check('swizzle', () => {
  const src = image((x, y) => (x === 5 && y === 7 ? [248, 0, 0] : [0, 0, 248]));
  const px = decode(RS64.encodeTerrainTex(src));

  assert.equal(px[(7 * 64 + 5) * 4], 255);
  assert.equal(px[(7 * 64 + 13) * 4], 0);
});

await check('budget', () => {
  const distinct = Array.from({ length: 900 }, (_, i) => i);

  assert.equal(RS64.paintBudget(distinct, 30, 30), 21 * 21);
  assert.equal(RS64.paintBudget(new Array(900).fill(7), 30, 30), 1);
});

const romPath = process.argv[2];

if (romPath)
{
  const buf = readFileSync(romPath);
  const rom = RS64.openRom(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.length));
  const bank = await rom.read('data/level/tex');

  const bytes = async (path) => {
    const dv = await rom.read(path);

    return new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength).slice();
  };

  const level = async (id) => {
    const hmp = await bytes(`data/level/lv_${id}/hmp`);
    const tex = await bytes(`data/level/lv_${id}_TEX`);

    return { hmp, tex, paint: new Uint8Array(0), grid: RS64.hmpGrid(RS64.parseHMP(new DataView(hmp.buffer))), bank };
  };

  const plain = (hmp) => new DataView(hmp.buffer, hmp.byteOffset).getUint16(0x1A);
  const field = (tex, id) => new DataView(tex.buffer, tex.byteOffset).getUint16(id * 4);
  const cellTex = (hmp, cell) => RS64.hmpGrid(RS64.parseHMP(new DataView(hmp.buffer, hmp.byteOffset, hmp.byteLength))).tex[cell];
  const solid = (r, g, b) => image(() => [r, g, b]);
  const red = solid(248, 0, 0);

  await check('paint one cell', async () => {
    const lv = await level(1);
    const before = RS64.hmpGrid(RS64.parseHMP(new DataView(lv.hmp.buffer))).tex.slice();
    const n = plain(lv.hmp);
    const cell = 5 * 64 + 5;
    const out = RS64.paintApply({ ...lv, cells: new Map([[cell, red]]) });

    assert.equal(plain(out.hmp), n + 1);
    assert.equal(out.tex.length, lv.tex.length + 4);
    assert.equal(field(out.tex, n), 0x2000);
    assert.equal(out.paint.length, 2080);
    assert.equal(cellTex(out.hmp, cell), n);

    const px = RS64.terrainTexRGBA(bank, new DataView(out.paint.buffer), field(out.tex, cellTex(out.hmp, cell)));

    assert.deepEqual([...px.subarray(0, 4)], [round5551(248), 0, 0, 255]);

    const after = RS64.hmpGrid(RS64.parseHMP(new DataView(out.hmp.buffer))).tex;

    before.forEach((t, i) => i !== cell && assert.equal(after[i], t));
  });

  await check('dedupe', async () => {
    const lv = await level(1);
    const out = RS64.paintApply({ ...lv, cells: new Map(Array.from({ length: 10 }, (_, i) => [i, red])) });

    assert.equal(out.paint.length, 2080);
    assert.equal(plain(out.hmp), plain(lv.hmp) + 1);
  });

  await check('stock reuse', async () => {
    const lv = await level(1);
    const n = plain(lv.hmp);
    const stockId = lv.grid.tex[100];
    const img = RS64.terrainTexRGBA(bank, null, field(lv.tex, stockId));
    const out = RS64.paintApply({ ...lv, cells: new Map([[200, img]]) });

    assert.equal(plain(out.hmp), n);
    assert.equal(out.paint.length, 0);
    assert.equal(cellTex(out.hmp, 200), stockId);
  });

  await check('reapply', async () => {
    const lv = await level(1);
    const first = RS64.paintApply({ ...lv, cells: new Map([[10, red]]) });
    const grid = RS64.hmpGrid(RS64.parseHMP(new DataView(first.hmp.buffer)));
    const second = RS64.paintApply({ ...first, grid, bank, cells: new Map([[20, solid(0, 248, 0)]]) });

    assert.equal(cellTex(second.hmp, 10), cellTex(first.hmp, 10));
    assert.deepEqual([...second.paint.subarray(0, 2080)], [...first.paint]);
    assert.equal(second.paint.length, 4160);
  });

  // Painting over a painted cell recycles its texture once nothing else uses it, so repeated strokes don't grow the level.
  await check('repaint recycles unused painted textures', async () => {
    const lv = await level(1);
    const first = RS64.paintApply({ ...lv, cells: new Map([[10, red]]) });
    const grid = RS64.hmpGrid(RS64.parseHMP(new DataView(first.hmp.buffer)));
    const green = solid(0, 248, 0);
    const second = RS64.paintApply({ ...first, grid, bank, cells: new Map([[10, green]]) });

    assert.equal(second.paint.length, first.paint.length);
    assert.equal(second.tex.length, first.tex.length);
    assert.equal(plain(second.hmp), plain(first.hmp));

    const px = RS64.terrainTexRGBA(bank, new DataView(second.paint.buffer), field(second.tex, cellTex(second.hmp, 10)));

    assert.deepEqual([...px.subarray(0, 4)], [0, round5551(248), 0, 255]);
  });

  await check('a painted texture another cell still uses is kept', async () => {
    const lv = await level(1);
    const first = RS64.paintApply({ ...lv, cells: new Map([[10, red], [11, red]]) });
    const grid = RS64.hmpGrid(RS64.parseHMP(new DataView(first.hmp.buffer)));
    const second = RS64.paintApply({ ...first, grid, bank, cells: new Map([[10, solid(0, 248, 0)]]) });
    const px = RS64.terrainTexRGBA(bank, new DataView(second.paint.buffer), field(second.tex, cellTex(second.hmp, 11)));

    assert.equal(second.paint.length, first.paint.length + 2080);
    assert.deepEqual([...px.subarray(0, 4)], [round5551(248), 0, 0, 255]);
  });

  await check('sculpt after paint', async () => {
    const lv = await level(1);
    const out = RS64.paintApply({ ...lv, cells: new Map([[10, red]]) });
    const grid = RS64.hmpGrid(RS64.parseHMP(new DataView(out.hmp.buffer)));

    grid.hts[0] += 5;

    const again = RS64.writeHMP(out.hmp, grid);

    assert.equal(cellTex(again, 10), cellTex(out.hmp, 10));
    assert.equal(plain(again), plain(out.hmp));
  });

  await check('animated blocks kept', async () => {
    const lv = await level(2);
    const n = plain(lv.hmp);
    const out = RS64.paintApply({ ...lv, cells: new Map([[0, red], [1, solid(0, 0, 248)]]) });

    assert.deepEqual([...out.tex.subarray((n + 2) * 4)], [...lv.tex.subarray(n * 4)]);
  });

  // The game's own 0x8000 + id fields borrow another id's texture: never paint, never a reuse target.
  await check('alias fields are not paint', async () => {
    const lv = await level(1);
    const n = plain(lv.hmp);

    new DataView(lv.tex.buffer).setUint16(5 * 4, 0x8001);

    const out = RS64.paintApply({ ...lv, cells: new Map([[3, red]]) });

    assert.equal(field(out.tex, 5), 0x8001);
    assert.equal(field(out.tex, n), 0x2000);
    assert.equal(out.paint.length, 2080);
  });

  await check('limits', async () => {
    const lv = await level(1);
    const paint = new Uint8Array(0x2000 * 2080);
    const hmp = lv.hmp.slice();
    const tex = lv.tex.slice();
    const texIds = lv.grid.tex.slice();

    assert.throws(() => RS64.paintApply({ ...lv, paint, cells: new Map([[3, red]]) }), /8192 painted textures/);
    assert.deepEqual(lv.hmp, hmp);
    assert.deepEqual(lv.tex, tex);
    assert.deepEqual(lv.grid.tex, texIds);
  });
}

if (failed)
  process.exitCode = 1;
