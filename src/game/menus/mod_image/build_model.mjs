// Builds the MODS page's image model from the player's own ROM (no game data is committed):
//   node src/game/menus/mod_image/build_model.mjs <rom.z64>
// mod_image_HOB.bps: the N64 controller's HOB (its node names, which the menu's model code looks up, kept) with its body a flat 2x3 grid of textured
// squares on top, the other parts empty, and its object renamed mod_image. mod_image_HMT: six materials, each on one 64x32 RGBA16 texture (one TMEM
// load), blank: the game writes each mod's thumb.png into them. The exe builds both in and serves them as frontend/controller/mod_image.
import { readFileSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
createRequire(import.meta.url)(join(here, '../../../../tools/web/rs64-explorer/rs64assets.js'));
const { RS64 } = globalThis;
const [romPath] = process.argv.slice(2);

if (!romPath) throw new Error('usage: build_model.mjs <rom.z64>');

const SRC_HOB = 'data/frontend/controller/controller_HOB';
const BODY_NODE = 2;
const COLS = 2, ROWS = 3, TILE_W = 64, TILE_H = 32;
const buf = readFileSync(romPath);
const rom = RS64.openRom(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.length));
const raw = await rom.read(SRC_HOB);
const orig = new Uint8Array(raw.buffer ?? raw);
const hob = RS64.parseHOB(new DataView(orig.buffer, orig.byteOffset, orig.length));
const lod = hob.objects[0].lods[0];

// The controller lies flat (x width, z depth toward the screen's top, y up toward the camera): the image covers the middle of its top, 4:3.
const mn = [Infinity, Infinity, Infinity], mx = [-Infinity, -Infinity, -Infinity];

for (const list of lod) for (const p of list) for (let i = 0; i < p.verts.length; i += 3) for (let a = 0; a < 3; a++) {
  mn[a] = Math.min(mn[a], p.verts[i + a]);
  mx[a] = Math.max(mx[a], p.verts[i + a]);
}

const width = Math.min(mx[0] - mn[0], ((mx[2] - mn[2]) * (COLS * TILE_W)) / (ROWS * TILE_H));
const depth = (width * ROWS * TILE_H) / (COLS * TILE_W);
const cx = (mn[0] + mx[0]) / 2, cz = (mn[2] + mx[2]) / 2, top = mx[1];
const geo = { verts: [], faces: [] };
// The page's text is drawn over the image, which the game lights only by its vertex colours: half brightness, as the controls mod's models.
const DIM = 0x808080ff;

for (let r = 0; r < ROWS; r++) for (let c = 0; c < COLS; c++) {
  const x0 = Math.round(cx - width / 2 + (c * width) / COLS), x1 = Math.round(cx - width / 2 + ((c + 1) * width) / COLS);
  const z0 = Math.round(cz + depth / 2 - (r * depth) / ROWS), z1 = Math.round(cz + depth / 2 - ((r + 1) * depth) / ROWS);
  const at = geo.verts.length;

  geo.verts.push([x0, top, z0], [x1, top, z0], [x1, top, z1], [x0, top, z1]);

  // Both windings, so the square shows whichever way the menu culls.
  const uvs = [[0, 0], [1, 0], [1, 1], [0, 1]];
  const mat = r * COLS + c;

  // One colour per corner (the writer always stores them per vertex): a missing one is black and shades the square toward it.
  const colors = [DIM, DIM, DIM, DIM];

  geo.faces.push({ idx: [at, at + 1, at + 2, at + 3], mat, colors, uvs });
  geo.faces.push({ idx: [at + 3, at + 2, at + 1, at], mat, colors, uvs: [...uvs].reverse() });
}

let out = orig;

lod.forEach((list, n) => list.forEach((p) => {
  out = RS64.hobSetPartGeometry(out, p.offset, n === BODY_NODE ? geo : { verts: [], faces: [] });
}));

// The object's 16-byte name (+0x08) is what the game finds the mesh by.
out.fill(0, 0x08, 0x18);
out.set(new TextEncoder().encode('mod_image'), 0x08);
writeFileSync(join(here, 'mod_image_HOB.bps'), RS64.encodeBPS(orig, out));

// HMT: material count, texture table offset; materials (textured type 1, its texture, the two 1.0 factors and 0x0A000000 every craft material
// has, name); texture count and entries (pixels, name, W, H, one level, flags_type 0x0302 = RGBA5551 as the game's own RGBA16 texture has);
// names; pixels.
const N = COLS * ROWS;
const texOff = 8 + N * 0x24;
const namesOff = texOff + 4 + N * 0x34;
const pixOff = (namesOff + N * 16 + 7) & ~7;
const hmt = new Uint8Array(pixOff + N * TILE_W * TILE_H * 2);
const dv = new DataView(hmt.buffer);
const enc = new TextEncoder();

dv.setUint16(0, N);
dv.setUint32(4, texOff);

for (let i = 0; i < N; i++) {
  const m = 8 + i * 0x24, t = texOff + 4 + i * 0x34, name = `modimg_${i}`;

  dv.setUint16(m, 1);
  dv.setUint16(m + 2, i);
  dv.setFloat32(m + 4, 1);
  dv.setFloat32(m + 8, 1);
  dv.setUint32(m + 0x10, 0x0A000000);
  hmt.set(enc.encode(name), m + 0x14);

  dv.setUint32(t, pixOff + i * TILE_W * TILE_H * 2);
  dv.setUint32(t + 0x24, namesOff + i * 16);
  dv.setUint16(t + 0x28, TILE_W);
  dv.setUint16(t + 0x2A, TILE_H);
  dv.setUint8(t + 0x2C, 1);
  dv.setUint8(t + 0x2D, 1);
  dv.setUint16(t + 0x2E, 0x0302);
  hmt.set(enc.encode(name), namesOff + i * 16);
}

dv.setUint16(texOff, N);
writeFileSync(join(here, 'mod_image_HMT'), hmt);

console.log(`mod_image: ${geo.verts.length} verts, ${geo.faces.length} faces, ${Math.round(width)} x ${Math.round(depth)} at y ${top}; HMT ${hmt.length} bytes`);
