// Rebuilds the mod's model assets from the .blend sources and the player's own ROM (the mod ships no game data):
//   blender -b -P tools/make_models.py -- <preview dir> source                   (regenerates the .blend files from scratch, overwriting edits; optional)
//   blender -b source/<model>.blend -P tools/dump_tris.py -- <model>.json       (for gamepad, keyboard and joystick)
//   node tools/build_models.mjs <rom.z64> <dir with the three .json files>
// Each model becomes a copy of the N64 controller's HOB (listed in assets/from.json) with its geometry replaced and its object renamed to <name>
// (<name>_HOB.bps), sharing its HMT. The game loads them as frontend/controller/<name>.
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
createRequire(import.meta.url)(join(here, '../../../tools/web/rs64-explorer/rs64assets.js'));
const { RS64 } = globalThis;
const [romPath, jsonDir] = process.argv.slice(2);

if (!romPath || !jsonDir) throw new Error('usage: build_models.mjs <rom.z64> <json dir>');

const SRC_HOB = 'frontend/controller/controller_HOB', SRC_HMT = 'frontend/controller/controller_HMT';
const BODY_NODE = 2;
const buf = readFileSync(romPath);
const rom = RS64.openRom(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.length));
const raw = await rom.read('data/' + SRC_HOB);
const orig = new Uint8Array(raw.buffer ?? raw);
const hob = RS64.parseHOB(new DataView(orig.buffer, orig.byteOffset, orig.length));
const lod = hob.objects[0].lods[0];

// The N64 model's extent: its geometry lies flat (x width, z depth, y thickness), the Z trigger underneath toward -y.
const mn = [Infinity, Infinity, Infinity], mx = [-Infinity, -Infinity, -Infinity];

for (const list of lod) for (const p of list) for (let i = 0; i < p.verts.length; i += 3) for (let a = 0; a < 3; a++) {
  mn[a] = Math.min(mn[a], p.verts[i + a]);
  mx[a] = Math.max(mx[a], p.verts[i + a]);
}

const outDir = join(here, '../assets/frontend/controller');
mkdirSync(outDir, { recursive: true });

// The list is drawn over the model, which the game lights only by its vertex colours: every model ships at this brightness.
const DIM = 0.5;
const dimRgba = (c) => ((Math.round(((c >>> 24) & 0xff) * DIM) << 24) | (Math.round(((c >>> 16) & 0xff) * DIM) << 16) | (Math.round(((c >>> 8) & 0xff) * DIM) << 8) | (c & 0xff)) >>> 0;

// The object's 16-byte name (+0x08) is what the game looks the mesh up by, so a copy keeps the original findable.
const rename = (bytes, name) => {
  bytes.fill(0, 0x08, 0x18);
  bytes.set(new TextEncoder().encode(name), 0x08);
};

// assets/from.json maps each new asset to the game asset it copies (the HMT unchanged, the HOB patched by its .bps).
const fromMap = {};
const writeModel = (name, bytes) => {
  writeFileSync(join(outDir, name + '_HOB.bps'), RS64.encodeBPS(orig, bytes));
  fromMap[`frontend/controller/${name}_HOB`] = SRC_HOB;
  fromMap[`frontend/controller/${name}_HMT`] = SRC_HMT;
};

// The menu looks straight down on its model; a flight stick only reads as one from the front, so it is turned and stood up to face the viewer
// (tip -90 would be a level front view; -60 looks down on it a little).
const POSE = { joystick: { yaw: 30, tip: -60 } };

const posed = ([x, y, z], { yaw, tip }) => {
  const a = (yaw * Math.PI) / 180, t = (tip * Math.PI) / 180;
  const rx = x * Math.cos(a) - y * Math.sin(a), ry = x * Math.sin(a) + y * Math.cos(a);

  return [rx, ry * Math.cos(t) - z * Math.sin(t), ry * Math.sin(t) + z * Math.cos(t)];
};

for (const name of ['gamepad', 'keyboard', 'joystick']) {
  const tris = JSON.parse(readFileSync(join(jsonDir, name + '.json'), 'utf8'));

  if (POSE[name]) for (const t of tris) t.p = t.p.map((v) => posed(v, POSE[name]));

  // Blender is z-up with the far edge at +y; the menu views the HOB from +y with the far edge at +z.
  const toHob = ([x, y, z]) => [x, z, y];
  const bmn = [Infinity, Infinity, Infinity], bmx = [-Infinity, -Infinity, -Infinity];

  for (const t of tris) for (const v of t.p) toHob(v).forEach((c, a) => { bmn[a] = Math.min(bmn[a], c); bmx[a] = Math.max(bmx[a], c); });

  // Fit inside the N64 model's width and depth, centred on it.
  const s = Math.min((mx[0] - mn[0]) / (bmx[0] - bmn[0]), (mx[2] - mn[2]) / (bmx[2] - bmn[2]));
  // The menu camera sits close above the model, so height is capped at twice the N64 controller's or a tall stick swells in perspective.
  const sy = Math.min(s, (2 * (mx[1] - mn[1])) / (bmx[1] - bmn[1]));
  const place = (v) => toHob(v).map((c, a) => (c - (bmn[a] + bmx[a]) / 2) * (a === 1 ? sy : s) + (mn[a] + mx[a]) / 2);
  const geo = { verts: [], faces: [] }, at = new Map();

  // The menu draws the model without a depth test, last face first: the file lists the highest faces first so lower ones are painted over.
  const height = (t) => t.p.reduce((s, v) => s + v[2], 0) / t.p.length;

  tris.sort((a, b) => height(a) - height(b));

  for (const t of tris) {
    const idx = t.p.map((v) => {
      const h = place(v).map(Math.round), k = h.join(',');

      if (!at.has(k)) { at.set(k, geo.verts.length); geo.verts.push(h); }

      return at.get(k);
    });
    const colors = t.c.map(([r, g, b, a]) => dimRgba(((r << 24) | (g << 16) | (b << 8) | a) >>> 0));

    geo.faces.push({ idx, mat: 0, colors, uvs: null });
  }

  geo.faces.reverse();

  let out = orig;

  lod.forEach((list, n) => list.forEach((p) => {
    out = RS64.hobSetPartGeometry(out, p.offset, n === BODY_NODE ? geo : { verts: [], faces: [] });
  }));

  rename(out, name);
  writeModel(name, out);
  console.log(`${name}: ${geo.verts.length} verts, ${geo.faces.length} faces, scale ${s.toFixed(1)}`);
}

writeFileSync(join(here, '../assets/from.json'), JSON.stringify(fromMap, null, 2) + '\n');
