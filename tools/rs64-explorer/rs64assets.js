// Rogue Squadron 64 asset parsers (data blob, HOB, HMT, HMP, DAT). Plain script so index.html works from file://; Node can import it too.
// Formats: rogue_squadron64/docs/{data_blob,hob_files,hmt_files,hmp_files,dat_files}.
// Wrapped in one function so the page can start Web Workers from its source (rs64Lib.toString()) without a server.
function rs64Lib() {

const BLOB_START = 0x144340;
const BLOB_DATA = BLOB_START + 0x40;

const name16 = (dv, off) => {
  let s = '';
  for (let i = 0; i < 16; i++) {
    const c = dv.getUint8(off + i);
    if (!c) break;
    s += String.fromCharCode(c);
  }
  return s;
};

const cstr = (dv, off, max = 64) => {
  let s = '';
  for (let i = 0; i < max && off + i < dv.byteLength; i++) {
    const c = dv.getUint8(off + i);
    if (!c) break;
    s += String.fromCharCode(c);
  }
  return s;
};

async function inflate(bytes) {
  const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('deflate'));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

// Returns { assets: Map(path -> entry), read(path) -> Promise<DataView> }.
function openRom(buf) {
  const rom = new Uint8Array(buf);
  const dv = new DataView(buf);
  // .v64 swaps byte pairs, .n64 reverses each word; both are put back in place, like librecomp's check_rom_start.
  const swap = { 0x37804012: 1, 0x40123780: 3 }[dv.getUint32(0)];
  if (swap) for (let i = 0; i + 3 < rom.length; i += 4) {
    const w = rom.slice(i, i + 4);
    for (let k = 0; k < 4; k++) rom[i + (k ^ swap)] = w[k];
  }
  if (dv.getUint32(0) !== 0x80371240) throw new Error('Not a Rogue Squadron N64 ROM (.z64, .v64 or .n64)');
  const assets = new Map();
  for (let s = 0; s < 2; s++) {
    const seg = BLOB_START + s * 0x20;
    const segName = name16(dv, seg);
    const base = BLOB_DATA + dv.getUint32(seg + 0x1C);
    const manifest = base + dv.getUint32(base);
    const count = dv.getUint16(base + 6) / 0x20;
    const walk = (i, n, dir) => {
      while (n > 0) {
        const e = manifest + i * 0x20;
        const flags = dv.getUint8(e + 0x0C);
        const nm = name16(dv, e + 0x10);
        if (flags & 0x80) {
          const sub = dv.getUint16(e + 0x0E) / 0x20;
          walk(i + 1, sub - 1, dir + nm + '/');
          i += sub; n -= sub;
        } else {
          const csize = dv.getUint32(e + 8);
          assets.set(dir + nm, {
            path: dir + nm, name: nm, dir: dir.slice(0, -1),
            offset: base + dv.getUint32(e), size: dv.getUint32(e + 4),
            csize: csize === 0xFFFFFFFF ? 0 : csize - 10,
          });
          i++; n--;
        }
      }
    };
    walk(0, count, segName + '/');
  }
  const cache = new Map();
  const read = async (path) => {
    if (cache.has(path)) return cache.get(path);
    const a = assets.get(path);
    if (!a) throw new Error('No asset ' + path);
    let raw = a.csize ? await inflate(rom.subarray(a.offset, a.offset + a.csize)) : rom.slice(a.offset, a.offset + a.size);
    // The hidden Naboo Starfighter's textures are encrypted (docs/cheat_codes/ns_hmt_decode.c): rolling XOR, key 0x050FE09C.
    if (path.endsWith('/koelsch_HMT')) {
      let key = 0x050FE09C;
      for (let i = 0; i < raw.length; i++) {
        const c = raw[i];
        raw[i] = c ^ (key & 0xFF);
        key = ((((key >>> 8) | ((key & 0xFF) << 24)) >>> 0) + c) >>> 0;
      }
      if (raw[0] === 0x78) raw = await inflate(raw);
    }
    const view = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
    cache.set(path, view);
    return view;
  };
  // Replaces an asset's contents for later reads (an edit that changes its size).
  const write = (path, bytes) => cache.set(path, new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength));
  // A new path that reads as an existing asset (a mod level's copies of its template's files); edits to it stay its own.
  const aliases = new Map();
  const alias = (path, from) => {
    const a = assets.get(from);
    if (!a || assets.has(path) && !aliases.has(path)) return;
    const name = path.slice(path.lastIndexOf('/') + 1);
    assets.set(path, { ...a, path, name, dir: path.slice(0, path.lastIndexOf('/')) });
    aliases.set(path, from);
  };
  const unalias = (path) => {
    if (!aliases.delete(path)) return;
    assets.delete(path);
    cache.delete(path);
  };
  return { assets, read, write, alias, unalias, aliases };
}

// ---- Mod levels: a mod's levels.json (new levels that play as a stock template level, see src/game/mod_levels.h) ----
function modLevelsParse(text) {
  try {
    const doc = JSON.parse(text || '{}');
    return Array.isArray(doc.levels) ? doc.levels : [];
  } catch {
    return [];
  }
}
const modLevelsWrite = (levels) => JSON.stringify({ levels }, null, 1) + '\n';
// A new level from a template: id and folder from the title, made unique and kept to the game's 48-character folder limit.
// data/medal_info: per level, bronze/silver/gold records of f32 minutes, then u16 kills, accuracy %, friendlies saved, bonus (bonus is not a mod level's).
function medalInfo(dv, level) {
  return Object.fromEntries(['bronze', 'silver', 'gold'].map((t, i) => {
    const o = (level * 3 + i) * 0xC;
    return [t, { time: Math.round(dv.getFloat32(o) * 100) / 100, kills: dv.getUint16(o + 4), accuracy: dv.getUint16(o + 6), friendlies: dv.getUint16(o + 8) }];
  }));
}
// Thresholds for a level not made from a stock one.
const MEDALS_SIMPLE = {
  bronze: { time: 10, kills: 10, accuracy: 20, friendlies: 0 },
  silver: { time: 8, kills: 20, accuracy: 35, friendlies: 0 },
  gold: { time: 6, kills: 30, accuracy: 50, friendlies: 0 },
};
function modLevelNew(levels, title, template, medals = MEDALS_SIMPLE) {
  const base = title.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '').slice(0, 44) || 'level';
  let folder = base;
  for (let k = 2; levels.some((l) => l.folder === folder || l.id === folder); k++) folder = `${base}_${k}`;
  return { id: folder, title, description: '', template, folder, medals: structuredClone(medals) };
}
// [new path, template path] for a mod level: every file of its template's folder, and the template's intro/extro (levels 0-15 have them).
function modLevelAliases(level, assetKeys) {
  const dir = 'data/level/lv_' + level.template.toString(36) + '/', out = [];
  for (const k of assetKeys) if (k.startsWith(dir)) out.push(['data/level/' + level.folder + '/' + k.slice(dir.length), k]);
  const keys = new Set(out.map(([, k]) => k));
  for (const kind of ['intro', 'extro']) {
    const src = `data/cuts/id${level.template}_${kind}`;
    if (level.template <= 15) out.push([`data/cuts/${level.folder}_${kind}`, src]);
  }
  return keys.size ? out : [];
}

// ---- HMT: materials + textures decoded to RGBA8 ----

const c5 = (v) => (v << 3) | (v >> 2);

function decodeTexture(dv, t) {
  const { w, h, type, depth, pix, pal, flags, trans } = t;
  const out = new Uint8Array(w * h * 4);
  const put = (i, r, g, b, a) => { out[i * 4] = r; out[i * 4 + 1] = g; out[i * 4 + 2] = b; out[i * 4 + 3] = a; };
  const palRGB = (n, k) => {
    const o = pal + k * 3;
    return [dv.getUint8(o), dv.getUint8(o + 1), dv.getUint8(o + 2)];
  };
  // Compared at RGBA5551 precision: sprite anims store the key already cut to 5 bits (0x403018 for HMT key 0x47361A).
  const keyed = (r, g, b) => (flags & 0x400) && (((r << 24) | (g << 16) | (b << 8)) & 0xF8F8F800) >>> 0 === ((trans & 0xF8F8F800) >>> 0) ? 0 : 255;
  const n = w * h;
  try {
    // t.bpp (effect frames) is measured from the pixel block's size; HMT textures go by type and depth.
    // Gray effect frames at 8 (with the alpha flag) or 16 bits per pixel are intensity+alpha: IA8 (4+4) and IA16 (8+8).
    if ((type === 4 || type === 5) && (t.bpp === 16 || (t.bpp === 8 && flags & 0x800))) {
      for (let i = 0; i < n; i++) {
        if (t.bpp === 16) {
          const v = dv.getUint8(pix + i * 2);
          put(i, v, v, v, dv.getUint8(pix + i * 2 + 1));
        } else {
          const byte = dv.getUint8(pix + i), v = (byte >> 4) * 17;
          put(i, v, v, v, (byte & 15) * 17);
        }
      }
    } else if (type === 0 || type === 1 || type === 4 || (type === 5 && t.bpp === 4)) {
      const four = t.bpp ? t.bpp === 4 : type === 0 || (type === 4 && depth === 1);
      for (let i = 0; i < n; i++) {
        const byte = dv.getUint8(pix + (four ? i >> 1 : i));
        const k = four ? (i & 1 ? byte & 15 : byte >> 4) : byte;
        if (type === 4 || type === 5) {
          const v = four ? k * 17 : k;
          put(i, v, v, v, flags & 0x800 ? v : keyed(v, v, v));
        } else {
          const [r, g, b] = palRGB(four ? 16 : 256, k);
          put(i, r, g, b, keyed(r, g, b));
        }
      }
    } else if (type === 2) {
      for (let i = 0; i < n; i++) {
        const v = dv.getUint16(pix + i * 2);
        put(i, c5((v >> 10) & 31), c5((v >> 5) & 31), c5(v & 31), v & 0x8000 ? 255 : 0);
      }
    } else if (type === 3) {
      const bpp = flags & 0x800 ? 4 : 3;
      for (let i = 0; i < n; i++) {
        const o = pix + i * bpp;
        put(i, dv.getUint8(o), dv.getUint8(o + 1), dv.getUint8(o + 2), bpp === 4 ? dv.getUint8(o + 3) : 255);
      }
    } else if (type === 5) {
      for (let i = 0; i < n; i++) {
        const v = dv.getUint8(pix + i);
        put(i, v, v, v, flags & 0x800 ? v : keyed(v, v, v));
      }
    } else {
      return null;
    }
  } catch {
    // Rows are padded to even sizes, so the last texture in a file can run a few bytes short.
  }
  // Effect frames (explosions, glows) keep their alpha in a separate block (entry +4) ending at the name, read as a second texture:
  // 8-bit or 4-bit at full size, or 8-bit at half size (explo2's 20x21 mask on a 40x42 frame). The colour block's alpha bit is unused.
  const maskBytes = t.mask && t.maskEnd > t.mask ? t.maskEnd - t.mask : 0;
  if (maskBytes) {
    const half = maskBytes < n / 2, bits = maskBytes >= n ? 8 : half ? 8 : 4;
    const mw = half ? Math.ceil(w / 2) : w, max = (1 << bits) - 1, per = 8 / bits;
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      const mi = half ? (y >> 1) * mw + (x >> 1) : y * w + x;
      const byte = dv.getUint8(t.mask + Math.floor(mi / per));
      out[(y * w + x) * 4 + 3] = Math.round((((byte >> (8 - bits * (1 + (mi % per)))) & max) * 255) / max);
    }
  }
  return out;
}

// data/pl_crafts/shadows_TM (loadCraftShadowTextures 0x8006B710): records of u32 size + a 0x34-byte HMT texture header (pixel offset +0,
// name offset +0x24, w +0x28, h +0x2A); the next record starts at (rec + size + 7) & ~3. Pixels are I4, high nibble first; the intensity
// is the shadow's alpha. Index order: awng vwng xwng ywng spdr falcon t16 tiein koelsch tie circ sqr destr2 bomber.
function parseShadowTextures(dv) {
  const out = [];
  for (let o = 0; o + 0x38 <= dv.byteLength; o = (o + dv.getUint32(o) + 7) & ~3) {
    const t = o + 4, w = dv.getUint16(t + 0x28), h = dv.getUint16(t + 0x2A), px = t + dv.getUint32(t);
    if (!w || !h || px + ((w * h) >> 1) > dv.byteLength) break;
    const alpha = new Uint8Array(w * h);
    for (let i = 0; i < w * h; i++) {
      const b = dv.getUint8(px + (i >> 1));
      alpha[i] = ((i & 1 ? b & 15 : b >> 4) * 255) / 15;
    }
    out.push({ name: cstr(dv, t + dv.getUint32(t + 0x24), 16), w, h, alpha });
    if (!dv.getUint32(o)) break;
  }
  return out;
}

function parseHMT(dv) {
  const matCount = dv.getUint16(0);
  const texOff = dv.getUint32(4);
  const materials = [];
  for (let i = 0; i < matCount; i++) {
    const o = 8 + i * 0x24;
    // +4 is 0 on glow/glass/exhaust materials (emissive) and 0.8-1.0 on ordinary ones.
    materials.push({ type: dv.getUint16(o), tex: dv.getUint16(o + 2), name: name16(dv, o + 0x14), emissive: dv.getFloat32(o + 4) === 0, diffuse: dv.getFloat32(o + 4) });
  }
  const texCount = dv.getUint16(texOff);
  const textures = [];
  for (let i = 0; i < texCount; i++) {
    const o = texOff + 4 + i * 0x34;
    const ft = dv.getUint16(o + 0x2E);
    const t = {
      pix: dv.getUint32(o), pal: dv.getUint32(o + 0x20), name: cstr(dv, dv.getUint32(o + 0x24), 16),
      w: (dv.getUint16(o + 0x28) + 1) & ~1, h: (dv.getUint16(o + 0x2A) + 1) & ~1, rows: dv.getUint16(o + 0x2A), depth: dv.getUint8(o + 0x2D),
      type: ft & 0xF, flags: ft >> 4, trans: dv.getUint32(o + 0x30),
    };
    t.rgba = decodeTexture(dv, t);
    textures.push(t);
  }
  return { materials, textures };
}

// ---- HOB: objects -> meshdef0 node tree -> meshdef1 parts -> faces ----

function parseFaces(dv, fg) {
  const faceOff = dv.getUint32(fg + 8), faceCount = dv.getUint32(fg + 12);
  const faces = [];
  let o = faceOff;
  for (let i = 0; i < faceCount; i++) {
    const flags = dv.getUint32(o);
    const size = (dv.getUint8(o + 4) & 0x3F) * 4;
    const quad = !!(flags & 8);
    const nv = quad ? 4 : 3;
    const f = { flags, mat: dv.getUint16(o + 8), idx: [], colors: null, uvs: null };
    for (let k = 0; k < nv; k++) f.idx.push(dv.getUint16(o + 12 + k * 2));
    let p = o + 20;
    // Normal indices are present when bit 13 of the second header word is set (matches all 21539 ROM faces); flags 0x02/0x40 do not decide it.
    if (dv.getUint32(o + 4) & 0x2000) {
      f.nidx = f.idx.map((_, k) => dv.getUint16(p + k * 2));
      p += 8;
    }
    if (flags & 0x20) {
      const nc = flags & 0x10 ? nv : 1;
      f.colors = [];
      for (let k = 0; k < nc; k++) f.colors.push(dv.getUint32(p + k * 4));
      p += nc * 4;
    }
    if (flags & 4) {
      f.uvs = [];
      for (let k = 0; k < nv; k++) f.uvs.push([dv.getInt16(p + k * 4) / 4096, dv.getInt16(p + k * 4 + 2) / 4096]);
      p += nv * 4;
    }
    faces.push(f);
    o += size || p - o;
  }
  return faces;
}

function parseMeshdef1(dv, off) {
  const count = dv.getUint32(off + 0x14);
  const vOff = dv.getUint32(off + 0x24);
  const verts = new Float32Array(count * 3);
  for (let i = 0; i < count * 3; i++) verts[i] = dv.getInt16(vOff + (i / 3 | 0) * 8 + (i % 3) * 2);
  const faces = parseFaces(dv, dv.getUint32(off + 0x20));
  // Normals (int16 x, y, z, pad) at +0x28, indexed per face corner; the game reads them only to env-map the medals.
  const nCount = Math.max(0, ...faces.flatMap((f) => f.nidx ?? [])) + 1, nOff = dv.getUint32(off + 0x28);
  const normals = nOff && nOff + nCount * 8 <= dv.byteLength ? Float32Array.from({ length: nCount * 3 }, (_, i) => dv.getInt16(nOff + (i / 3 | 0) * 8 + (i % 3) * 2)) : null;
  return { offset: off, verts, normals, faces };
}

function parseHOB(dv) {
  const objCount = dv.getUint16(0);
  const objects = [];
  for (let oi = 0; oi < objCount; oi++) {
    const ob = 8 + oi * 0x74;
    // +0x34: NPC spawn code scales the placement by item.scale / this (npcAtStUpdate 0x800D001C; AT-ST 0.1, TIE 1.125).
    const obj = { name: name16(dv, ob), nodes: [], partNames: new Map(), scaleDiv: dv.getFloat32(ob + 0x34) || 1 };
    // Named nodes (+0x28): 8-char name + u16 node index, e.g. the X-wing S-foil hinges wngrot_a/wngrot_b.
    obj.nodeNames = new Map();
    const n0 = dv.getUint32(ob + 0x28);
    if (n0) for (let p = n0; p + 10 <= dv.byteLength && dv.getUint32(p); p += 10) obj.nodeNames.set(cstr(dv, p, 8), dv.getUint16(p + 8));
    const n1 = dv.getUint32(ob + 0x2C);
    // Named parts (+0x2C): 12-byte entries of part pointer, 6-byte name (eng_a, wnd_a, glw_b), u16 node index; zero-terminated.
    if (n1) for (let p = n1; p + 12 <= dv.byteLength && dv.getUint32(p); p += 12) obj.partNames.set(dv.getUint32(p), cstr(dv, p + 4, 6));
    // Hardpoints (+0x30): name[6], u8 node, u8 count, then count x 36 bytes (position, direction, up; node-local vertex units). Weapons
    // fire from a / b / a_b / c_d (lookupHmtTextureByName 0x80058360); zf/zl/... and eng/glw/trl mark other attachment points.
    obj.hardpoints = [];
    for (let p = dv.getUint32(ob + 0x30); p && p + 8 <= dv.byteLength && dv.getUint8(p); ) {
      const n = dv.getUint8(p + 7), rec = (k, o) => [0, 1, 2].map((j) => dv.getFloat32(p + 8 + k * 36 + o + j * 4));
      obj.hardpoints.push({ name: cstr(dv, p, 6), node: dv.getUint8(p + 6), points: Array.from({ length: n }, (_, k) => ({ pos: rec(k, 0), dir: rec(k, 12), up: rec(k, 24) })) });
      p += 8 + n * 36;
    }
    // Nodes come from the meshdef0 prelude; +0x08 of each node is its parent node.
    const pre0 = dv.getUint32(ob + 0x14);
    const offs = [];
    for (let i = 0; i < dv.getUint16(pre0); i++) offs.push(dv.getUint32(pre0 + 8 + i * 8));
    for (const off of offs) {
      const m = [];
      for (let i = 0; i < 12; i++) m.push(dv.getFloat32(off + 0x1C + i * 4));
      const f = (o, n) => Array.from({ length: n }, (_, i) => dv.getFloat32(off + o + i * 4));
      // flags 1 = rotate about pivot at +0x50, 4 = rotation quaternion (w,x,y,z) at +0x68, 8 = translation at +0x78 (world units, 256 per vertex unit).
      obj.nodes.push({
        offset: off, parent: offs.indexOf(dv.getUint32(off + 8)), matrix: m, flags: dv.getUint32(off + 0x4C),
        pivot: f(0x50, 3), scale: f(0x5C, 3), quat: f(0x68, 4), pos: f(0x78, 3),
      });
    }
    // Each meshdef1 prelude is one LOD level (0 = most detailed): a zero-terminated part list per node, in node order.
    const parts = new Map();
    const part = (p) => {
      if (!parts.has(p)) parts.set(p, Object.assign(parseMeshdef1(dv, p), { name: obj.partNames.get(p) || '' }));
      return parts.get(p);
    };
    obj.lods = [];
    for (let k = 0; k < 4; k++) {
      const pre = dv.getUint32(ob + 0x18 + k * 4);
      if (!pre) continue;
      const total = dv.getUint16(pre + 2);
      const lists = offs.map(() => []);
      let p = pre + 4, node = 0, found = 0;
      while (found < total && node < offs.length) {
        const v = dv.getUint32(p);
        p += 4;
        if (v) { lists[node].push(part(v)); found++; } else node++;
      }
      obj.lods.push(lists);
    }
    objects.push(obj);
  }
  return { objects };
}

// ---- HMP: terrain heightmap tiles ----

function parseHMP(dv) {
  const heightScale = dv.getFloat32(0x10);
  const tileCount = dv.getUint16(0x18);
  const tileOff = dv.getUint32(0x1C);
  const w = dv.getUint16(0x24), h = dv.getUint16(0x26);
  const map = new Uint16Array(w * h);
  for (let i = 0; i < w * h; i++) map[i] = dv.getUint16(0x28 + i * 2) & 0x1FFF;
  const tiles = [];
  for (let i = 0; i < tileCount; i++) {
    const o = tileOff + i * 0x1E;
    const hts = new Int8Array(25);
    for (let k = 0; k < 25; k++) hts[k] = dv.getInt8(o + 5 + k);
    tiles.push({ tex: dv.getUint16(o), flags: dv.getUint8(o + 2), hts });
  }
  // Lighting block: shadow (ambient) and world colours, then light objects; the first is the sun (direction, game Y-down).
  const L = dv.getUint32(0x20);
  const rgb = (o) => [dv.getUint8(o), dv.getUint8(o + 1), dv.getUint8(o + 2)];
  const sun = L + 12;
  const light = {
    shadow: rgb(L + 4), world: rgb(L + 8),
    sunColor: [0, 4, 8].map((k) => dv.getFloat32(sun + k)),
    sunDir: [16, 20, 24].map((k) => dv.getFloat32(sun + k)),
  };
  return { heightScale, w, h, map, tiles, light };
}

// ---- HOB geometry writing ----
// A part (meshdef1) reaches its geometry only through file offsets: +0x14 vertex count, +0x20 facegroup, +0x24 vertex list, and the
// word at -4 (end of its face data). New geometry is appended to the file and the part repointed, so nothing else moves.
// geo = { verts: [[x, y, z], ...] in HOB vertex units, faces: [{ idx: [a, b, c] or [a, b, c, d], mat, colors: [rgba u32 per vertex],
// uvs: [[u, v] per vertex] or null }] }. Faces are written like the game's own textured, vertex-coloured faces without normals.
function hobGeometryBlock(base, geo) {
  const faceSize = (f) => 20 + f.idx.length * 4 + (f.uvs ? f.idx.length * 4 : 0);
  const vbytes = geo.verts.length * 8, fgAt = base + vbytes, facesAt = fgAt + 16;
  const total = vbytes + 16 + geo.faces.reduce((s, f) => s + faceSize(f), 0);
  const out = new Uint8Array(total), o = new DataView(out.buffer);
  geo.verts.forEach((v, i) => v.forEach((c, a) => o.setInt16(i * 8 + a * 2, Math.max(-32768, Math.min(32767, Math.round(c))))));
  o.setUint32(vbytes + 8, facesAt);
  o.setUint32(vbytes + 12, geo.faces.length);
  let p = vbytes + 16;
  for (const f of geo.faces) {
    const n = f.idx.length, size = faceSize(f) / 4, col = 5, uv = col + n;
    o.setUint32(p, 0x200 | 0x20 | 0x10 | (f.uvs ? 0x4 : 0) | (n === 4 ? 0x8 : 0));
    o.setUint32(p + 4, (size << 24) | (3 << 18) | (col << 12) | (col << 6) | uv);
    o.setUint16(p + 8, f.mat);
    f.idx.forEach((v, k) => o.setUint16(p + 12 + k * 2, v));
    f.colors.forEach((c, k) => o.setUint32(p + 20 + k * 4, c >>> 0));
    if (f.uvs) f.uvs.forEach(([u, v], k) => { o.setInt16(p + 20 + n * 4 + k * 4, Math.round(u * 4096)); o.setInt16(p + 22 + n * 4 + k * 4, Math.round(v * 4096)); });
    p += size * 4;
  }
  return { bytes: out, vertsAt: base, fgAt, end: base + total };
}
function hobAppend(orig, chunks) {
  const len = chunks.reduce((s, c) => s + c.length, orig.length), out = new Uint8Array(len);
  out.set(orig);
  let p = orig.length;
  for (const c of chunks) { out.set(c, p); p += c.length; }
  return out;
}
// Replaces one part's geometry. Only unnamed parts: named ones (eng_*, glw_*, wnd_*) keep their geometry inside the header region the game copies per craft, so appended geometry would be addressed wrongly.
function hobSetPartGeometry(orig, part, geo) {
  const base = (orig.length + 3) & ~3, g = hobGeometryBlock(base, geo);
  const out = hobAppend(orig, [new Uint8Array(base - orig.length), g.bytes]), o = new DataView(out.buffer);
  o.setUint32(part - 4, g.end);
  o.setUint32(part + 0x14, geo.verts.length);
  o.setUint32(part + 0x20, g.fgAt);
  o.setUint32(part + 0x24, g.vertsAt);
  return out;
}
// Craft model import: the texture slots an imported model may take, largest first. One textured material per distinct texture, and
// never a texture that a named part (exhaust, glow, canopy) draws, since those parts keep their original faces.
function craftImportSlots(hob, hmt) {
  const named = new Set();
  for (const lod of hob.objects[0].lods) for (const list of lod) for (const p of list) if (p.name) p.faces.forEach((f) => named.add(hmt.materials[f.mat]?.tex));
  const slots = [], seen = new Set();
  hmt.materials.forEach((m, i) => {
    const t = m.type & 1 ? hmt.textures[m.tex] : null;
    if (t && !named.has(m.tex) && !seen.has(m.tex)) { seen.add(m.tex); slots.push({ mat: i, tex: t }); }
  });
  return slots.sort((a, b) => b.tex.w * b.tex.h - a.tex.w * a.tex.h);
}
// tris: [{ p: [[x, y, z] x3] in three.js display space (Y up), uv: [[u, v] x3] or null, rgba: [u32 x3], node: name, mat (optional, else opts.mat) }].
// Display maps to HOB as (x, -y, -z); with plusZ the model's nose is +Z (glTF habit) and is turned to face the craft's way.
// The model is scaled and centred onto the original craft's extent. A triangle goes to the unnamed part of the node it names
// (model-space nodes only, flags 0/1), otherwise to the body; unnamed parts left without triangles are emptied.
// shortcut: first object and LOD 0 only (every player craft has one of each); nodes with their own translation (flags 4/8) are not targeted.
function hobImportCraft(orig, hob, tris, { mat, plusZ = true }) {
  const obj = hob.objects[0], lod = obj.lods[0];
  const box = () => ({ mn: [Infinity, Infinity, Infinity], mx: [-Infinity, -Infinity, -Infinity] });
  const grow = (b, v) => v.forEach((c, a) => { b.mn[a] = Math.min(b.mn[a], c); b.mx[a] = Math.max(b.mx[a], c); });
  const ob = box(), ib = box();
  for (const list of lod) for (const p of list) for (let i = 0; i < p.verts.length; i += 3) grow(ob, [p.verts[i], p.verts[i + 1], p.verts[i + 2]]);
  const toHob = ([x, y, z]) => (plusZ ? [-x, -y, z] : [x, -y, -z]);
  for (const t of tris) t.p.forEach((v) => grow(ib, toHob(v)));
  const size = (b) => Math.max(...b.mx.map((v, a) => v - b.mn[a])) || 1;
  const s = size(ob) / size(ib), oc = ob.mn.map((v, a) => (v + ob.mx[a]) / 2), ic = ib.mn.map((v, a) => (v + ib.mx[a]) / 2);
  const place = (v) => toHob(v).map((c, a) => (c - ic[a]) * s + oc[a]);
  const body = lod[0].find((p) => !p.name);
  const target = (name) => {
    const n = obj.nodeNames.get(name);
    const p = n !== undefined && !(obj.nodes[n].flags & 12) && lod[n]?.find((q) => !q.name);
    return p || body;
  };
  const geos = new Map();
  for (const t of tris) {
    const part = target(t.node);
    if (!geos.has(part)) geos.set(part, { verts: [], faces: [], at: new Map() });
    const g = geos.get(part);
    const idx = t.p.map((v) => {
      const h = place(v).map(Math.round), k = h.join(',');
      if (!g.at.has(k)) { g.at.set(k, g.verts.length); g.verts.push(h); }
      return g.at.get(k);
    });
    g.faces.push({ idx, mat: t.mat ?? mat, colors: t.rgba, uvs: t.uv });
  }
  let out = orig;
  for (const list of lod) for (const p of list) {
    if (p.name) continue;
    const g = geos.get(p) || { verts: [], faces: [] };
    out = hobSetPartGeometry(out, p.offset, g);
  }
  return out;
}

// ---- DAT editing: add / remove items ----
// Header: (count u32, list offset u32) per type 0..7 at 0x00..0x3C, then (size, offset) of a trailing block at 0x40/0x44.
// Each type's section is its list of u32 offsets (relative to the list) followed by its records, sections back to back in type
// order; record fields are relative to the record. So one section can grow or shrink if the later sections' offsets shift with it.
function datSections(dv) {
  const ends = [dv.byteLength, dv.getUint32(0x44)];
  for (let k = 0; k < 8; k++) if (dv.getUint32(k * 8 + 4)) ends.push(dv.getUint32(k * 8 + 4));
  return (type) => {
    const list = dv.getUint32(type * 8 + 4);
    return { count: dv.getUint32(type * 8), list, end: Math.min(...ends.filter((e) => e > list)) };
  };
}
function datRebuild(orig, type, entries, region, extra) {
  const dv = new DataView(orig.buffer, orig.byteOffset, orig.byteLength), s = datSections(dv)(type);
  const listBytes = entries.length * 4;
  const section = new Uint8Array(listBytes + region.length + extra.length);
  const sdv = new DataView(section.buffer);
  entries.forEach((e, i) => sdv.setUint32(i * 4, e));
  section.set(region, listBytes);
  section.set(extra, listBytes + region.length);
  const delta = section.length - (s.end - s.list);
  const out = new Uint8Array(orig.length + delta);
  out.set(orig.subarray(0, s.list));
  out.set(section, s.list);
  out.set(orig.subarray(s.end), s.list + section.length);
  const o = new DataView(out.buffer);
  o.setUint32(type * 8, entries.length);
  for (let k = 0; k < 8; k++) if (k !== type && o.getUint32(k * 8 + 4) >= s.end) o.setUint32(k * 8 + 4, o.getUint32(k * 8 + 4) + delta);
  if (o.getUint32(0x44) >= s.end) o.setUint32(0x44, o.getUint32(0x44) + delta);
  return datRebuildNameMap(out);
}
// The name map (last in the file; size +0x40, offset +0x44) is how the game finds items by name (getLevelDatItemByName): 256 bucket
// offsets relative to the map, each to { type, index } pairs ending in { -1, -1 }, sorted by type then index. Bucket = this hash.
const datNameHash = (name) => {
  let h = 0xffff;
  for (let i = 0; i < name.length; i++) h = (name.charCodeAt(i) + (h & 0xffff) * 0x21) >>> 0;
  return h & 0xff;
};
function datLookup(bytes, name) {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength), map = dv.getUint32(0x44), rel = dv.getUint32(map + datNameHash(name) * 4);
  if (!rel) return null;
  const items = parseDAT(dv).items;
  for (let e = map + rel; dv.getInt32(e) !== -1; e += 8) {
    const it = items.find((x) => x.type === dv.getUint32(e) && x.index === dv.getUint32(e + 4));
    if (it?.name === name) return { type: it.type, index: it.index };
  }
  return null;
}
// Rewrites the name map from the items, so added, removed or moved items are found by name.
function datRebuildNameMap(bytes) {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength), map = dv.getUint32(0x44);
  const buckets = Array.from({ length: 256 }, () => []);
  for (const it of parseDAT(dv).items.filter((x) => x.name).sort((a, b) => a.type - b.type || a.index - b.index)) buckets[datNameHash(it.name)].push(it);
  const size = 0x400 + buckets.reduce((n, b) => n + (b.length ? (b.length + 1) * 8 : 0), 0);
  const out = new Uint8Array(map + size), o = new DataView(out.buffer);
  out.set(bytes.subarray(0, map));
  o.setUint32(0x40, size);
  let at = 0x400;
  buckets.forEach((b, k) => {
    if (!b.length) return;
    o.setUint32(map + k * 4, at);
    for (const it of b) {
      o.setUint32(map + at, it.type);
      o.setUint32(map + at + 4, it.index);
      at += 8;
    }
    o.setInt32(map + at, -1);
    o.setInt32(map + at + 4, -1);
    at += 8;
  });
  return out;
}
// Appends a record (a copy of an existing item, whose offsets are all record-relative) to a type's list; it becomes the last index.
function datAddItem(orig, type, record) {
  const dv = new DataView(orig.buffer, orig.byteOffset, orig.byteLength), s = datSections(dv)(type);
  const region = orig.subarray(s.list + s.count * 4, s.end), pad = (4 - (region.length % 4)) % 4;
  const entries = [];
  for (let i = 0; i < s.count; i++) entries.push(dv.getUint32(s.list + i * 4) + 4);
  const padded = new Uint8Array(region.length + pad);
  padded.set(region);
  entries.push((s.count + 1) * 4 + padded.length);
  return datRebuild(orig, type, entries, padded, record);
}
// Points a type's list entry at a new record appended to the section, so the item keeps its index; the old bytes stay, unreferenced.
function datReplaceItem(orig, type, index, record) {
  const dv = new DataView(orig.buffer, orig.byteOffset, orig.byteLength), s = datSections(dv)(type);
  const region = orig.subarray(s.list + s.count * 4, s.end), pad = (4 - (region.length % 4)) % 4;
  const entries = [];
  for (let i = 0; i < s.count; i++) entries.push(dv.getUint32(s.list + i * 4));
  const padded = new Uint8Array(region.length + pad);
  padded.set(region);
  entries[index] = s.count * 4 + padded.length;
  return datRebuild(orig, type, entries, padded, record);
}
// Enemies and NPCs that can wait for a wave: the type-0 subtypes the game defers (+0x88 == 0, parseDatSpawnPositions) and spawns once
// flag +0x60 is set and counter +0x64 >= +0x68 (isDatItemConditionMet; 0 = no condition), flying the path named at +0x8C (record-relative
// offset to the record's last string; "-" none, "+name" a roam box). Only subtypes stock levels defer this way; World Devastators (0x37)
// are left out because data follows their path string.
const WAVE_LAYOUT = { flag: 0x60, counter: 0x64, atLeast: 0x68, gridSpawn: 0x88, path: 0x8C };
const WAVE_FIELDS = Object.fromEntries([0x3B, 0x3D, 0x12, 0x0C, 0x13, 0x1F, 0x3C, 0x25, 0x47, 0x21, 0x24, 0x0F, 0x2E, 0x2B, 0x2C, 0x3E, 0x29, 0x35, 0x2D, 0x30, 0x31, 0x2F, 0x34, 0x48].map((s) => [s, WAVE_LAYOUT]));
function waveGet(dv, item) {
  const L = item.type === 0 && WAVE_FIELDS[item.sub], o = item.offset;
  if (!L) return null;
  const size = dv.getUint32(o + 8), po = dv.getUint32(o + L.path);
  let path = '';
  for (let i = po; i < size && dv.getUint8(o + i); i++) path += String.fromCharCode(dv.getUint8(o + i));
  return { flag: dv.getUint32(o + L.flag), counter: dv.getUint32(o + L.counter), atLeast: dv.getUint32(o + L.atLeast), gridSpawn: dv.getUint32(o + L.gridSpawn), path };
}
// Path (type 3 sub 3) geometry: point count +0x1C, offset +0x20 (XYZ relative to the item position +0x10), arc length +0x24, then a
// table of n-2 cumulative fractions after the points. Length and table are the Catmull-Rom arc length over the interior segments.
function splinePoints(dv, item) {
  const o = item.offset, n = dv.getUint32(o + 0x1C), po = o + dv.getUint32(o + 0x20), base = [0x10, 0x14, 0x18].map((c) => dv.getFloat32(o + c));
  return Array.from({ length: n }, (_, k) => [0, 1, 2].map((a) => dv.getFloat32(po + k * 12 + a * 4) + base[a]));
}
function splineSetPoints(bytes, item, points) {
  const out = bytes.slice(), dv = new DataView(out.buffer), o = item.offset, n = points.length, po = o + dv.getUint32(o + 0x20);
  const base = [0x10, 0x14, 0x18].map((c) => dv.getFloat32(o + c)), rel = points.map((p) => p.map((v, a) => Math.fround(v - base[a])));
  rel.forEach((p, k) => p.forEach((v, a) => dv.setFloat32(po + k * 12 + a * 4, v)));
  const cr = (p0, p1, p2, p3, t) => [0, 1, 2].map((a) => 0.5 * (2 * p1[a] + (p2[a] - p0[a]) * t + (2 * p0[a] - 5 * p1[a] + 4 * p2[a] - p3[a]) * t * t + (3 * p1[a] - p0[a] - 3 * p2[a] + p3[a]) * t * t * t));
  const cum = [0];
  for (let k = 1; k < n - 2; k++) {
    let l = 0, prev = rel[k];
    for (let i = 1; i <= 16; i++) {
      const q = cr(rel[k - 1], rel[k], rel[k + 1], rel[k + 2], i / 16);
      l += Math.hypot(q[0] - prev[0], q[1] - prev[1], q[2] - prev[2]);
      prev = q;
    }
    cum.push(cum.at(-1) + l);
  }
  const total = cum.at(-1);
  dv.setFloat32(o + 0x24, total);
  cum.forEach((c, k) => dv.setFloat32(po + n * 12 + k * 4, total > 0 ? c / total : k / (cum.length - 1)));
  return out;
}
// Path events: nodes (count +0x28, offset +0x2C; f32 fraction, u16 0, u16 k) index a reference table (count +0xD4, offset +0xD8 relative
// to +0xD4; u16 type, u16 index). Eight 16-byte blocks at +0x60 start with an offset to a lead-count list of continuation path names.
function splineEvents(dv, item) {
  const o = item.offset, nn = dv.getUint32(o + 0x28), no = o + dv.getUint32(o + 0x2C), nr = dv.getUint32(o + 0xD4), ro = o + 0xD4 + dv.getUint32(o + 0xD8);
  return {
    nodes: Array.from({ length: nn }, (_, k) => ({ at: dv.getFloat32(no + k * 8), k: dv.getUint16(no + k * 8 + 6) })),
    refs: Array.from({ length: nr }, (_, k) => ({ type: dv.getUint16(ro + k * 4), index: dv.getUint16(ro + k * 4 + 2) })),
  };
}
// The record with new events (and optionally a new name and its continuation links cleared), all appended at its end.
function splineRecord(dv, item, { name, nodes, refs, clearLinks }) {
  const o = item.offset, size = dv.getUint32(o + 8), text = name ? new TextEncoder().encode(name) : new Uint8Array(0);
  const nameAt = size, nodesAt = (nameAt + (name ? text.length + 1 : 0) + 3) & ~3, refsAt = nodesAt + nodes.length * 8;
  const rec = new Uint8Array(refsAt + refs.length * 4), r = new DataView(rec.buffer);
  rec.set(new Uint8Array(dv.buffer, dv.byteOffset + o, size));
  if (name) {
    rec.set(text, nameAt);
    r.setUint32(0x0C, nameAt);
  }
  if (clearLinks) for (let k = 0; k < 8; k++) r.setUint32(0x60 + k * 16, nameAt + text.length);
  nodes.forEach((n, k) => {
    r.setFloat32(nodesAt + k * 8, n.at);
    r.setUint16(nodesAt + k * 8 + 6, n.k);
  });
  refs.forEach((x, k) => {
    r.setUint16(refsAt + k * 4, x.type);
    r.setUint16(refsAt + k * 4 + 2, x.index);
  });
  r.setUint32(8, rec.length);
  r.setUint32(0x28, nodes.length);
  r.setUint32(0x2C, nodes.length ? nodesAt : 0);
  r.setUint32(0xD4, refs.length);
  r.setUint32(0xD8, refsAt - 0xD4);
  return rec;
}
const uniqueName = (bytes, base) => {
  let name = base;
  for (let k = 2; datLookup(bytes, name); k++) name = `${base}_${k}`;
  return name;
};
// Events a copied path keeps: craft action, speed, weave, landing pitch, and fly-to the player. Others name their old level's items or
// flags (attack targets, holds, named fly-to) and are dropped.
const PATH_EVENT_KEEP = new Set([0x1A, 0x1B, 0x1C, 0x23, 0x4E]);
const keepsEvent = (t) => t && PATH_EVENT_KEEP.has(t.sub) && (t.sub !== 0x4E || !t.pathEvent?.target);
// A craft that reaches the end of a path with no continuation reads a NULL next path (crash), and an event at exactly 1.0 never fires
// first; the end attack sits just before the end.
const PATH_ATTACK_AT = 0.95;
const isEndAttack = (n, refs, t7) => n.at >= 0.9 && refs[n.k]?.type === 7 && t7.some((x) => x.index === refs[n.k].index && x.sub === 0x4E && !x.pathEvent?.target);
// Copies a path into another DAT under name, moved so its first interior point (where an NPC starts) is at `at`. Its continuation
// links are cleared, so unless it keeps a fly-to-the-player event it gets one from `attack` (a stock 0x4E "+" record).
function splineCopy(fromBytes, spline, toBytes, { name, at, attack }) {
  const fdv = new DataView(fromBytes.buffer, fromBytes.byteOffset, fromBytes.byteLength), t7 = parseDAT(fdv).items.filter((x) => x.type === 7);
  const { nodes, refs } = splineEvents(fdv, spline), keepNodes = [], keepRefs = [];
  let out = toBytes, dropped = 0;
  for (const n of nodes) {
    const ref = refs[n.k], t = ref?.type === 7 && t7.find((x) => x.index === ref.index);
    if (!keepsEvent(t)) {
      dropped++;
      continue;
    }
    keepRefs.push({ type: 7, index: new DataView(out.buffer, out.byteOffset, out.byteLength).getUint32(7 * 8) });
    out = datAddItem(out, 7, fromBytes.slice(t.offset, t.offset + fdv.getUint32(t.offset + 8)));
    keepNodes.push({ at: n.at, k: keepRefs.length - 1 });
  }
  if (attack && !nodes.some((n) => isEndAttack(n, refs, t7))) {
    keepRefs.push({ type: 7, index: new DataView(out.buffer, out.byteOffset, out.byteLength).getUint32(7 * 8) });
    out = datAddItem(out, 7, attack);
    keepNodes.push({ at: PATH_ATTACK_AT, k: keepRefs.length - 1 });
  }
  const rec = splineRecord(fdv, spline, { name, nodes: keepNodes, refs: keepRefs, clearLinks: true }), r = new DataView(rec.buffer);
  const start = splinePoints(fdv, spline)[1];
  for (let a = 0; a < 3; a++) r.setFloat32(0x10 + a * 4, r.getFloat32(0x10 + a * 4) + at[a] - start[a]);
  return { bytes: datAddItem(out, 3, rec), dropped };
}
// True when the path ends by sending its NPC at the player (a fly-to event with no target at the end).
function splineAttackEnd(bytes, item) {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength), t7 = parseDAT(dv).items.filter((x) => x.type === 7), { nodes, refs } = splineEvents(dv, item);
  return nodes.some((n) => isEndAttack(n, refs, t7));
}
// Adds (copying template, a stock fly-to-player record) or removes the end attack; the path keeps its index.
function splineSetAttackEnd(bytes, item, on, template) {
  let out = bytes;
  const dv0 = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength), t7 = parseDAT(dv0).items.filter((x) => x.type === 7);
  let { nodes, refs } = splineEvents(dv0, item);
  if (on) {
    refs = [...refs, { type: 7, index: dv0.getUint32(7 * 8) }];
    nodes = [...nodes, { at: PATH_ATTACK_AT, k: refs.length - 1 }];
    out = datAddItem(out, 7, template);
  } else {
    nodes = nodes.filter((n) => !isEndAttack(n, refs, t7));
  }
  const dv = new DataView(out.buffer, out.byteOffset, out.byteLength), now = parseDAT(dv).items.find((x) => x.type === 3 && x.index === item.index);
  return datReplaceItem(out, 3, item.index, splineRecord(dv, now, { nodes, refs }));
}
// Mission flags (1-based bytes at 0x801388A0; stock levels use up to 128) the level's DAT reads or writes: wave gates, trigger
// conditions and effects, HUD arrow conditions. Level scripts set more that the DAT never names (mostly low numbers).
function levelUsedFlags(dv) {
  const used = new Set();
  for (const it of parseDAT(dv).items) {
    const w = waveGet(dv, it);
    for (const f of [w?.flag, it.requires?.flag, it.onEnter?.flag, it.onExit?.flag, it.waypoint?.flag, it.waypoint?.hideFlag]) if (f) used.add(f);
    const d = isUnit(it) && unitDeath(dv, it).flag;
    if (d > 0 && d <= 128) used.add(d);
  }
  return used;
}
// A type-0 unit's death effect (datItemSetObjectiveBooleanCount, record-relative): counter +0x70 += +0x74, flag +0x78 = +0x7C,
// counter2 +0x80 += +0x84. All 1-based; 0 = none. Only units the game can destroy run it.
const UNIT_DEATH = { counter: 0x70, add: 0x74, flag: 0x78, value: 0x7C, counter2: 0x80, add2: 0x84 };
const isUnit = (it) => it.type === 0 && ![0x01, 0x02, 0x26, 0x27].includes(it.sub);
const unitDeath = (dv, it) => Object.fromEntries(Object.entries(UNIT_DEATH).map(([k, o]) => [k, dv.getInt32(it.offset + o)]));
// Objective counters and flags the level's DAT already uses (so objectives take others).
function levelUsedObjectiveSlots(dv) {
  const counters = new Set(), flags = levelUsedFlags(dv);
  for (const it of parseDAT(dv).items) {
    for (const b of [it.onEnter, it.onExit]) if (b?.counter) counters.add(b.counter);
    if (it.requires?.counter) counters.add(it.requires.counter);
    if (it.pathEvent?.addCounter) counters.add(it.pathEvent.addCounter);
    if (isUnit(it)) {
      const d = unitDeath(dv, it);
      for (const c of [d.counter, d.counter2]) if (c > 0 && c <= 128) counters.add(c);
      if (d.flag > 0 && d.flag <= 128) flags.add(d.flag);
    }
  }
  return { counters, flags };
}
// The DAT items an objective uses: destroy = units adding to its counter (counter2), protect = the unit setting its flag on death,
// reach = the area triggers setting its flag on enter.
function objectiveTargets(dv, obj) {
  const items = parseDAT(dv).items;
  if (obj.goal === 'destroy') return items.filter((it) => isUnit(it) && unitDeath(dv, it).counter2 === obj.counter);
  if (obj.goal === 'protect') return items.filter((it) => isUnit(it) && unitDeath(dv, it).flag === obj.flag);
  if (obj.goal === 'reach') return items.filter((it) => it.type === 7 && it.onEnter && it.onEnter.flag === obj.flag);
  return [];
}
// Writes objectives into a level DAT. objs: [{ text, goal, seconds, targets: [DAT indices] }] (destroy/protect: type-0 units, reach:
// type-7 area triggers). Old objective wiring (prev, as returned before) is cleared first. Returns { bytes, objectives } with the
// counters/flags picked (top down from 128, away from what the level uses) and the levels.json objectives.
function objectivesApply(orig, prev, objs) {
  const bytes = orig.slice(), dv = new DataView(bytes.buffer), items = parseDAT(dv).items;
  const put = (it, o, v) => dv.setInt32(it.offset + o, v);
  for (const p of prev ?? []) {
    for (const it of objectiveTargets(dv, p)) {
      if (p.goal === 'destroy') put(it, UNIT_DEATH.counter2, 0), put(it, UNIT_DEATH.add2, 0);
      else if (p.goal === 'protect') put(it, UNIT_DEATH.flag, 0), put(it, UNIT_DEATH.value, 0);
      else if (p.goal === 'reach') dv.setUint32(it.offset + (it.sub === 0x18 ? 0x38 : 0x48) + 8, 0);
    }
  }
  const used = levelUsedObjectiveSlots(dv), pick = (set) => {
    for (let i = 128; i >= 1; i--) if (!set.has(i)) return set.add(i), i;
    throw new Error('No free objective counter or flag');
  };
  const out = objs.map((o) => {
    const targets = o.targets.map((i) => items[i]);
    if (o.goal === 'destroy') {
      const counter = pick(used.counters);
      for (const it of targets) put(it, UNIT_DEATH.counter2, counter), put(it, UNIT_DEATH.add2, 1);
      return { text: o.text, goal: o.goal, counter, count: o.count ?? targets.length };
    }
    if (o.goal === 'protect' || o.goal === 'reach') {
      const flag = pick(used.flags);
      for (const it of targets) {
        if (o.goal === 'protect') put(it, UNIT_DEATH.flag, flag), put(it, UNIT_DEATH.value, 1);
        else {
          const b = it.offset + (it.sub === 0x18 ? 0x38 : 0x48);
          dv.setUint32(b + 8, flag);
          dv.setUint8(b + 0xF, 1);
        }
      }
      return { text: o.text, goal: o.goal, flag };
    }
    return { text: o.text, goal: o.goal, seconds: o.seconds };
  });
  return { bytes, objectives: out };
}
// A blank level's DAT: every item removed but the player start (the only one a level needs; the game ran one like this to its debrief).
function datBlank(orig) {
  let bytes = orig;
  for (const type of Object.keys(DAT_TYPES).map(Number)) {
    for (;;) {
      const left = parseDAT(new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)).items.filter((it) => it.type === type && !(type === 0 && it.sub === 0x01));
      if (!left.length) break;
      bytes = datRemoveItem(bytes, type, left[left.length - 1].index);
    }
  }
  return bytes;
}
// Flat ground at the terrain's median height (cells keep their textures, so the ground still looks like the level's).
function hmpFlat(orig) {
  const hmp = parseHMP(new DataView(orig.buffer, orig.byteOffset, orig.byteLength)), g = hmpGrid(hmp);
  const h = [...g.hts].sort((a, b) => a - b)[g.hts.length >> 1];
  g.hts.fill(h);
  return writeHMP(orig, g);
}
// A record copied from another level starts inert: an area trigger's condition and enter/exit effects are cleared, a wingman's path dropped.
function recordInert(rec, it) {
  const dv = new DataView(rec.buffer, rec.byteOffset, rec.byteLength);
  // A wingman spawn names its path right after its own name; without one it stays put (like lv_3's Han_Solo_Stationary).
  if (it.type === 0 && it.sub === 0x27 && it.name) rec.fill(0, dv.getUint32(0x0C) + it.name.length + 1, dv.getUint32(0x0C) + it.name.length + 2);
  if (it.type === 7 && it.onEnter) {
    const base = it.sub === 0x18 ? 0x38 : 0x48;
    for (let o = base; o < base + 0x20; o += 4) dv.setUint32(o, 0);
    if (it.sub === 0x18) for (const o of [0x28, 0x2C, 0x30]) dv.setUint32(o, 0);
  }
  return rec;
}
// A wave's flag: its own if named before, else the highest flag up to 128 that neither the level nor another wave uses (top down, away
// from the low numbers level scripts use).
function waveFlagFor(waves, name, used) {
  if (waves[name]) return waves[name];
  const taken = new Set(Object.values(waves));
  for (let f = 128; f >= 1; f--) if (!used.has(f) && !taken.has(f)) return f;
  return 0;
}
// A trigger's enter block for waves: 'free' (no flag), 'wave' (raises a wave's flag), 'stock' (sets or clears a flag the mission uses).
const triggerWave = (onEnter, waves) => (!onEnter.flag ? 'free' : onEnter.value && Object.values(waves).includes(onEnter.flag) ? 'wave' : 'stock');
// Subtypes proven to spawn with their model when copied into a level that has none (TIE fighter, TIE interceptor).
const WAVE_FOREIGN = new Set([0x0C, 0x12]);
// Appends a copied enemy record at pos with no wave gate and no path: a template's gate and spline name belong to its own level.
function datAddEnemy(bytes, record, pos) {
  const rec = record.slice();
  pos.forEach((v, a) => new DataView(rec.buffer).setFloat32(0x10 + a * 4, v));
  const out = datAddItem(bytes, 0, rec), it = parseDAT(new DataView(out.buffer)).items.filter((x) => x.type === 0).at(-1);
  return waveSet(out, it, { flag: 0, counter: 0, atLeast: 0, gridSpawn: 0, path: '-' });
}
// Copy of the DAT with the given wave fields of one item changed. A path that does not fit where the old one was grows the record
// (datReplaceItem, same index), so re-parse the result for the item's offset.
function waveSet(bytes, item, fields) {
  const L = WAVE_FIELDS[item.sub], o = item.offset, out = bytes.slice(), dv = new DataView(out.buffer);
  for (const k of ['flag', 'counter', 'atLeast', 'gridSpawn']) if (fields[k] !== undefined) dv.setUint32(o + L[k], fields[k]);
  return fields.path === undefined ? out : npcSetPath(out, item, fields.path);
}
// The field holding the record offset of an NPC's path (or roam area) name, the record's last string: enemies +0x8C, wingman spawns +0x94
// (all 139 stock ones). 0 for items that fly no path.
const npcPathField = (it) => (it.type !== 0 ? 0 : WAVE_FIELDS[it.sub] ? WAVE_LAYOUT.path : it.sub === 0x27 ? 0x94 : 0);
function npcPathGet(dv, it) {
  const o = it.offset, size = dv.getUint32(o + 8), po = dv.getUint32(o + npcPathField(it));
  let path = '';
  for (let i = po; i < size && dv.getUint8(o + i); i++) path += String.fromCharCode(dv.getUint8(o + i));
  return path;
}
function npcSetPath(bytes, item, path) {
  const o = item.offset, out = bytes.slice(), dv = new DataView(out.buffer);
  const size = dv.getUint32(o + 8), po = dv.getUint32(o + npcPathField(item)), text = new TextEncoder().encode(path);
  if (text.length < size - po) {
    out.fill(0, o + po, o + size);
    out.set(text, o + po);
    return out;
  }
  const grown = new Uint8Array((po + text.length + 1 + 3) & ~3);
  grown.set(out.subarray(o, o + po));
  grown.set(text, po);
  new DataView(grown.buffer).setUint32(8, grown.length);
  return datReplaceItem(out, item.type, item.index, grown);
}
// Removes a zone (type 6) or trigger (type 7) and renumbers what refers to them by index: paths' reference tables (4-byte entries,
// u16 type + u16 index, at item + [+0xD8] + 0xD4; count +0xD4) and zones' link lists (count +0xA4, u16s at item + [+0xA8] + 0xA6).
// A trigger a path fires cannot go (throws with the path's name); links to a removed zone are dropped.
function datRemoveIndexed(orig, type, index) {
  const before = parseDAT(new DataView(orig.buffer, orig.byteOffset, orig.byteLength)).items;
  const user = before.find((it) => it.type === 3 && it.events?.some((e) => e.trigger === index) && type === 7);
  if (user) throw new Error(`The path "${user.name}" fires it`);
  const out = datRemoveItem(orig, type, index), dv = new DataView(out.buffer, out.byteOffset, out.byteLength);
  for (const it of parseDAT(dv).items) {
    const o = it.offset, size = dv.getUint32(o + 8);
    if (type === 7 && it.type === 3 && size > 0xDC) {
      const n = dv.getUint32(o + 0xD4), ref = o + 0xD4 + dv.getUint32(o + 0xD8);
      for (let k = 0; k < n && n < 256; k++) {
        const i = dv.getUint16(ref + k * 4 + 2);
        if (dv.getUint16(ref + k * 4) === 7 && i > index) dv.setUint16(ref + k * 4 + 2, i - 1);
      }
    }
    if (type === 6 && it.type === 6 && size >= 0xAC) {
      const n = dv.getUint32(o + 0xA4), lo = o + dv.getUint32(o + 0xA8) + 0xA6;
      if (!n || n >= 64) continue;
      const links = Array.from({ length: n }, (_, k) => dv.getUint16(lo + k * 2)).filter((i) => i !== index).map((i) => (i > index ? i - 1 : i));
      links.forEach((i, k) => dv.setUint16(lo + k * 2, i));
      for (let k = links.length; k < n; k++) dv.setUint16(lo + k * 2, 0);
      dv.setUint32(o + 0xA4, links.length);
    }
  }
  return out;
}
// Drops one entry from a type's list; later items of that type move down one index. The record's bytes stay, unreferenced.
function datRemoveItem(orig, type, index) {
  const dv = new DataView(orig.buffer, orig.byteOffset, orig.byteLength), s = datSections(dv)(type);
  const entries = [];
  for (let i = 0; i < s.count; i++) if (i !== index) entries.push(dv.getUint32(s.list + i * 4) - 4);
  return datRebuild(orig, type, entries, orig.subarray(s.list + s.count * 4, s.end), new Uint8Array(0));
}

// Continuous terrain grid for editing: (4w+1) x (4h+1) heights in stored s8 units (neighbouring tiles share their edge row),
// plus each cell's texmap id. Grid point (x, y) is world (x * 0.5, height * heightScale, y * 0.5), game Y-down.
function hmpGrid(hmp) {
  const W = hmp.w * 4 + 1, H = hmp.h * 4 + 1, hts = new Float32Array(W * H), tex = new Uint16Array(hmp.w * hmp.h);
  for (let ty = 0; ty < hmp.h; ty++) for (let tx = 0; tx < hmp.w; tx++) {
    const tile = hmp.tiles[hmp.map[ty * hmp.w + tx]];
    if (!tile) continue;
    tex[ty * hmp.w + tx] = tile.tex;
    for (let py = 0; py < 5; py++) for (let px = 0; px < 5; px++) hts[(ty * 4 + py) * W + tx * 4 + px] = tile.hts[py * 5 + px];
  }
  return { w: hmp.w, h: hmp.h, W, H, hts, tex };
}

// Rewrites an HMP from a grid of the same map size. The file is header, tile map (+0x28), lighting, then the tile table last,
// so only the map, the tile count (+0x18) and the table's tail change. The original tiles stay where they are (a cell reuses one
// when its heights and texture match) and new tiles are appended, so a BPS patch carries only the changed cells and new tiles.
// Tile record (0x1E): texmap u16, flags u8 (0 in every level), s8 min and max of its heights, 25 s8 heights.
function writeHMP(orig, grid) {
  const dv = new DataView(orig.buffer, orig.byteOffset, orig.byteLength);
  const tileOff = dv.getUint32(0x1C), count = dv.getUint16(0x18);
  if (dv.getUint16(0x24) !== grid.w || dv.getUint16(0x26) !== grid.h) throw new Error('Terrain map size changed');
  const keys = new Map(), added = [], map = new Uint16Array(grid.w * grid.h);
  for (let i = count - 1; i >= 0; i--) {
    const p = tileOff + i * 0x1E, hts = [];
    for (let k = 0; k < 25; k++) hts.push(dv.getInt8(p + 5 + k));
    keys.set(dv.getUint16(p) + ':' + hts.join(','), i);
  }
  for (let ty = 0; ty < grid.h; ty++) for (let tx = 0; tx < grid.w; tx++) {
    const hts = new Int8Array(25);
    for (let py = 0; py < 5; py++) for (let px = 0; px < 5; px++) {
      hts[py * 5 + px] = Math.max(-128, Math.min(127, Math.round(grid.hts[(ty * 4 + py) * grid.W + tx * 4 + px])));
    }
    const tex = grid.tex[ty * grid.w + tx], key = tex + ':' + hts.join(',');
    if (!keys.has(key)) { keys.set(key, count + added.length); added.push({ tex, hts }); }
    map[ty * grid.w + tx] = keys.get(key);
  }
  const total = count + added.length;
  if (total > 0x1FFF) throw new Error(`Terrain needs ${total} tiles; the map index holds ${0x1FFF}`);
  const out = new Uint8Array((tileOff + total * 0x1E + 3) & ~3);
  out.set(orig.subarray(0, Math.min(orig.length, tileOff + count * 0x1E)));
  const o = new DataView(out.buffer);
  o.setUint16(0x18, total);
  map.forEach((v, i) => o.setUint16(0x28 + i * 2, v));
  added.forEach((t, i) => {
    const p = tileOff + (count + i) * 0x1E;
    o.setUint16(p, t.tex);
    o.setInt8(p + 3, Math.min(...t.hts));
    o.setInt8(p + 4, Math.max(...t.hts));
    t.hts.forEach((v, k) => o.setInt8(p + 5 + k, v));
  });
  return out;
}

// ---- Terrain textures: lv_X_TEX maps a tile's texmap id to an entry in the shared data/level/tex bank ----

function parseTexTable(dv) {
  const ids = [];
  for (let o = 0; o + 4 <= dv.byteLength && dv.getUint16(o) !== 0xFFFF; o += 4) ids.push(dv.getUint16(o));
  return ids;
}

// Bank entries are 2080 bytes: 64x64 CI4 then a 16-entry RGBA5551 palette. Rows are stored TMEM-style (odd rows have 4-byte words swapped).
function decodeTerrainTex(dv, index) {
  const base = index * 2080;
  const out = new Uint8Array(64 * 64 * 4);
  for (let i = 0; i < 4096; i++) {
    const src = (i >> 6) & 1 ? i ^ 8 : i;
    const byte = dv.getUint8(base + (src >> 1));
    const v = dv.getUint16(base + 2048 + (src & 1 ? byte & 15 : byte >> 4) * 2);
    out.set([c5((v >> 11) & 31), c5((v >> 6) & 31), c5((v >> 1) & 31), v & 1 ? 255 : 0], i * 4);
  }
  return out;
}

// ---- DAT: level items (only the common header is understood) ----

const DAT_TYPES = { 0: 0x04, 2: 0x14, 3: 0x1C, 6: 0x34, 7: 0x3C };

function parseDAT(dv) {
  const items = [];
  for (const [type, offField] of Object.entries(DAT_TYPES)) {
    const count = dv.getUint32(offField - 4);
    const list = dv.getUint32(offField);
    for (let i = 0; i < count; i++) {
      const it = list + dv.getUint32(list + i * 4);
      const nameOff = dv.getUint32(it + 0x0C);
      const name = nameOff ? cstr(dv, it + nameOff, 255) : '';
      const f = (o) => dv.getFloat32(it + o);
      const item = { type: +type, index: i, offset: it, sub: dv.getUint16(it), name, pos: [f(0x10), f(0x14), f(0x18)] };
      if (+type === 0) {
        item.rot = [f(0x1C), f(0x20), f(0x24)];
        item.scale = [f(0x28), f(0x2C), f(0x30)];
        // Placed props store their model's name right after the item name; NPCs store their spline or roam-area name there (full length).
        const after = it + nameOff + name.length + 1;
        if (nameOff && after < it + dv.getUint32(it + 8)) item.model = cstr(dv, after, Math.min(48, it + dv.getUint32(it + 8) - after));
        // Unnamed props keep their model name only via +0x5C, an item-relative offset (matches the after-name string on 1605 named props; 0 on NPCs).
        const mo = dv.getUint32(it + 0x5C), size = dv.getUint32(it + 8);
        if (!item.model && mo >= 0x34 && mo < size) item.model = cstr(dv, it + mo, size - mo) || undefined;
      }
      if (+type === 3) {
        // Spline control points: count +0x1C, item-relative offset +0x20, XYZ floats relative to the item position.
        const n = dv.getUint32(it + 0x1C), po = it + dv.getUint32(it + 0x20);
        item.points = [];
        for (let k = 0; k < n && po + k * 12 + 12 <= dv.byteLength; k++) item.points.push([0, 4, 8].map((c, a) => dv.getFloat32(po + k * 12 + c) + item.pos[a]));
        item.length = f(0x24);
        // Event nodes (func_800F410C): count +0x28, 8-byte nodes at item + [+0x2C] = { f32 fraction along the path, u16, u16 k };
        // k indexes the spline's reference table at item + [+0xD8] + 0xD4 (4-byte entries), whose u16 at +2 is the type-7 item fired.
        const ne = dv.getUint32(it + 0x28), no = dv.getUint32(it + 0x2C), size = dv.getUint32(it + 8);
        if (ne && ne < 256 && no + ne * 8 <= size && size > 0xDC) {
          const ref = it + dv.getUint32(it + 0xD8) + 0xD4;
          item.events = Array.from({ length: ne }, (_, k) => ({ at: dv.getFloat32(it + no + k * 8), trigger: dv.getUint16(ref + dv.getUint16(it + no + k * 8 + 6) * 4 + 2) }));
        }
      }
      if (+type === 6) {
        // Zone boxes: Euler rotation (degrees) +0x1C, full box size +0x28 (MUSICRNG_Eisley's 100x75 box covers the town).
        item.rot = [f(0x1C), f(0x20), f(0x24)];
        item.size = [f(0x28), f(0x2C), f(0x30)];
        // +0x34 flags: 0x80 = music zone with four song ids at +0x3C (func_800EEA54).
        item.flags = dv.getUint32(it + 0x34);
        const recSize = dv.getUint32(it + 8);
        if (item.flags & 0x80) item.songs = [0x3C, 0x40, 0x44, 0x48].map((o) => dv.getUint32(it + o));
        // Linked zones: count +0xA4, list at item + [+0xA8] + 0xA6 of u16 zone indices. Music zones link their 0x39
        // trigger area this way; func_800ECC28 copies the linked area into its live check.
        if (recSize >= 0xAC) {
          const n = dv.getUint32(it + 0xA4), lo = it + dv.getUint32(it + 0xA8) + 0xA6;
          if (n && n < 64 && lo + n * 2 <= dv.byteLength) item.zoneLinks = Array.from({ length: n }, (_, k) => dv.getUint16(lo + k * 2));
        }
      }
      // NPC roam areas (0x11 For_*, 0x17 *_SPHERE): centre = pos, radius +0x1C (npcProbeDriod 0x800CAE24, roam update func_800F4E74).
      if (+type === 7 && (item.sub === 0x11 || item.sub === 0x17)) item.radius = f(0x1C);
      // Path events (0x1A-0x23, 0x4E): fired by spline event nodes when an NPC passes them (func_800F3CB0, jtbl_800A8AC8).
      // Common gate/actions: flag +0x28 set and counter +0x2C >= +0x30, then counter +0x34 += +0x38 and flag +0x3C = byte +0x43. Body from +0x44.
      if (+type === 7 && [0x1A, 0x1B, 0x1C, 0x20, 0x22, 0x23, 0x4E].includes(item.sub)) {
        const u = (o) => dv.getUint32(it + o), str = (o) => (u(o) && u(o) < dv.getUint32(it + 8) ? cstr(dv, it + u(o), 32) : '');
        item.pathEvent = {
          flag: u(0x28), counter: u(0x2C), atLeast: dv.getInt32(it + 0x30), addCounter: u(0x34), amount: dv.getInt32(it + 0x38), setFlag: u(0x3C), value: dv.getUint8(it + 0x43),
          a: f(0x44), b: f(0x48), c: f(0x4C), ia: u(0x44), ib: u(0x48),
          // Body from +0x44 as words and as floats (meaning depends on the subtype).
          w: [0x44, 0x48, 0x4C, 0x50, 0x54, 0x58].map((o) => (o + 4 <= dv.getUint32(it + 8) ? dv.getInt32(it + o) : 0)),
          fl: [0x44, 0x48, 0x4C, 0x50, 0x54, 0x58].map((o) => (o + 4 <= dv.getUint32(it + 8) ? f(o) : 0)),
          // 0x20 attack target; 0x4E fly-to target, '+' then a DAT item name (empty = the player; func_800F5A30).
          target: item.sub === 0x20 ? str(0x4C) : item.sub === 0x4E ? str(0x44).replace(/^\+/, '') : '',
        };
      }
      // HUD objective-arrow targets (0x3A; func_800C6340 collects them, func_800C5580 points the arrow at the nearest active one).
      // Active while flag +0x30 is set and counter +0x34 >= +0x38, and while flag +0x3C is clear and counter +0x40 < +0x44 (1-based, 0 = no test).
      // +0x2C is an offset to the name of an item whose position the target follows ("-" = fixed).
      if (+type === 7 && item.sub === 0x3A) {
        const u = (o) => dv.getUint32(it + o), lo = u(0x2C);
        const follow = lo && dv.getUint8(it + lo) !== 0x2D ? cstr(dv, it + lo, 32) : '';
        item.waypoint = { follow, flag: u(0x30), counter: u(0x34), atLeast: dv.getInt32(it + 0x38), hideFlag: u(0x3C), hideCounter: u(0x40), below: dv.getInt32(it + 0x44) };
      }
      if (+type === 7 && (item.sub === 0x18 || item.sub === 0x19)) {
        // Activators (func_800AA870): offset at +0x58 (0x18) / +0x68 (0x19) -> lead byte, then "a|b|c"; "ply" = player.
        const rel = dv.getUint32(it + (item.sub === 0x18 ? 0x58 : 0x68));
        const end = it + dv.getUint32(it + 8);
        if (rel && it + rel < end && dv.getUint8(it + rel)) item.activators = cstr(dv, it + rel + 1, end - it - rel - 1).split('|').filter(Boolean);
        // Area (func_800AA6D0): radius +0x1C; a non-zero +0x34 makes it a vertical cylinder (height ignored), else a sphere.
        item.radius = f(0x1C);
        item.cylinder = dv.getUint32(it + 0x34) !== 0;
        // Actions (func_80065980): 0x18 needs objective flag +0x28 set and counter +0x2C >= +0x30 first.
        // Blocks of 16 bytes: counter index (+0, 1-based) += amount (+4); flag index (+8, 1-based) = byte (+0xF).
        if (item.sub === 0x18) item.requires = { flag: dv.getUint32(it + 0x28), counter: dv.getUint32(it + 0x2C), atLeast: dv.getInt32(it + 0x30) };
        const block = (o) => ({ counter: dv.getUint32(it + o), add: dv.getInt32(it + o + 4), flag: dv.getUint32(it + o + 8), value: dv.getUint8(it + o + 0xF) });
        const base = item.sub === 0x18 ? 0x38 : 0x48;
        item.onEnter = block(base);
        item.onExit = block(base + 0x10);
      }
      if (+type === 7) {
        // Every printable run in the record; names of other items among them are what the trigger refers to.
        const end = it + dv.getUint32(it + 8);
        item.strings = [];
        for (let o = it + 0x10, s = ''; o <= end; o++) {
          const c = o < end ? dv.getUint8(o) : 0;
          if (c >= 32 && c < 127) s += String.fromCharCode(c);
          else { if (s.length >= 3) item.strings.push(s); s = ''; }
        }
      }
      items.push(item);
    }
  }
  return { items };
}

// ---- Skeletal animations (atat_walk, atst_start, ...): rogue_squadron64/docs/model_animations ----
// One track per model node after the root (track i drives node i + 1). Key times run 0..4096 over one cycle.
// Track flags use the node bits: 4 = rotation keys (quaternion x,y,z,w x4096), 8 = translation keys (vertex units).
// Header +0x0C looks like the cycle length in ms (AT-AT walk 6477); it is 0 for the AT-ST.
function parseAnim(dv) {
  if (dv.getUint32(4) !== 0xC) return null;
  const t1 = 0xC, t2 = dv.getUint32(t1), count = dv.getUint16(t1 + 0xE);
  const s16 = (o) => dv.getInt16(o);
  const tracks = [];
  for (let i = 0; i < count; i++) {
    const e = t2 + i * 0x1C, n = dv.getUint32(e), flags = dv.getUint32(e + 4);
    if (!n) { tracks.push(null); continue; }
    const times = [], times0 = dv.getUint32(e + 8), quads = dv.getUint32(e + 20), trans = dv.getUint32(e + 24);
    for (let k = 0; k < n; k++) times.push(s16(times0 + k * 2) / 4096);
    const rot = flags & 4 ? times.map((_, k) => [0, 2, 4, 6].map((c) => s16(quads + k * 8 + c) / 4096)) : null;
    const pos = flags & 8 ? times.map((_, k) => [0, 2, 4].map((c) => s16(trans + k * 6 + c))) : null;
    tracks.push({ times, rot, pos });
  }
  return { lengthMs: dv.getUint16(t1 + 0xC), tracks };
}

// ---- Voice lines: voice id -> text id (u16 table at 0x8009FFE0 in boot code) -> XOR-chained English string in data/voice_TXT ----

function parseVoiceTable(romBytes, txt) {
  const strings = [];
  const n = txt.getUint16(2), lang = txt.getUint32(4);
  for (let i = 0; i < n; i++) {
    let o = lang + txt.getUint16(lang + i * 2), prev = 0xF5, s = '';
    for (let k = 0; k < 1024 && o + k < txt.byteLength; k++) {
      const c = txt.getUint8(o + k) ^ prev;
      prev ^= c;
      if (!c) break;
      s += String.fromCharCode(c);
    }
    strings.push(s);
  }
  const map = 0x8009FFE0 - 0x80000400 + 0x1000;
  const voiceMap = new Uint16Array(0x2F4);
  for (let v = 0; v < 0x2F4; v++) voiceMap[v] = (romBytes[map + v * 2] << 8) | romBytes[map + v * 2 + 1];
  return { strings, voiceMap };
}

// front_TXT / game_TXT: unlike voice_TXT, the XOR chain runs across the whole string block. front_TXT 0-18 = mission names by level.
function parseFrontText(txt) {
  const n = txt.getUint16(2), lang = txt.getUint32(4);
  const offs = Array.from({ length: n }, (_, i) => lang + txt.getUint16(lang + i * 2));
  const start = Math.min(...offs), plain = new Uint8Array(txt.byteLength - start);
  let prev = 0xF5;
  for (let i = 0; i < plain.length; i++) {
    const c = txt.getUint8(start + i) ^ prev;
    prev ^= c;
    plain[i] = c;
  }
  return offs.map((o) => {
    let s = '';
    for (let k = o - start; k < plain.length && plain[k]; k++) s += String.fromCharCode(plain[k]);
    return s;
  });
}

function parseVoiceLines(romBytes, txt) {
  const { strings, voiceMap } = parseVoiceTable(romBytes, txt);
  return (voiceId) => (voiceId < 0x2F4 ? strings[voiceMap[voiceId]] : undefined);
}

// Voice-id lists the menus and pause screen play (US v1.0; both overlays load at VRAM 0x800A5130: menu ROM 0x10C2D0, mission ROM 0xA5D30).
function parseMenuVoice(rom) {
  const be16 = (o) => (rom[o] << 8) | rom[o + 1], be32 = (o) => be16(o) * 0x10000 + be16(o + 2);
  const menu = (v) => v - 0x800A5130 + 0x10C2D0, mission = (v) => v - 0x800A5130 + 0xA5D30;
  const list = (o) => {
    const ids = [];
    for (let v; ids.length < 8 && (v = be16(o + ids.length * 2)) !== 0xFFFF;) ids.push(v);
    return ids;
  };
  return {
    // craftSelectionVoiceLineHelper: D_menu_overlay_800CC4F8[bay - 1] -> 0xFFFF-terminated list, parts played with a 0.7 s gap.
    crafts: Array.from({ length: 7 }, (_, i) => list(menu(be32(menu(0x800CC4F8) + i * 4)))),
    // menuControllerInput: 0x24-byte pilot records at D_menu_overlay_800CC89C, list at +0x18.
    roster: Array.from({ length: 6 }, (_, i) => list(menu(0x800CC89C) + i * 0x24 + 0x18)),
    // startSlotLoopAnimWithVoiceSync: mission-select briefing = level + 0x279.
    briefings: Array.from({ length: 19 }, (_, l) => l + 0x279),
    // Pause screen (mission 0C0390): D_mission_overlay_80109D80[level * 4 + i] + 0x2C0, 0xFF = none.
    objectives: Array.from({ length: 16 }, (_, l) => [...rom.subarray(mission(0x80109D80) + l * 4, mission(0x80109D80) + l * 4 + 4)].filter((b) => b !== 0xFF).map((b) => b + 0x2C0)),
  };
}

// data/cuts/idN_{intro,extro,special}: length in seconds f32 +0x50, time units per second f32 +0x54 (20), u16 event count +0x4E,
// 0x18-byte events at +0x58 {type, time, a, ...}; type 0 plays voice id a (spawnCutsceneEffectsAndCues).
function parseCutsceneVoice(dv) {
  const rate = dv.getFloat32(0x54) || 20, events = [];
  for (let i = 0, n = dv.getUint16(0x4E); i < n && 0x58 + i * 0x18 + 0xC <= dv.byteLength; i++) {
    const o = 0x58 + i * 0x18;
    if (dv.getInt32(o) === 0) events.push({ voiceId: dv.getInt32(o + 8), at: dv.getUint32(o + 4) / rate });
  }
  return { length: dv.getFloat32(0x50), events };
}

// Voice ids a DAT trigger leads to: a LINE_n name is a voice id; counters and flags it sets run the handlers the level script registered
// for them (c, b) or that its checks poll (pc, pb: flag -> value written -> ids). tv = one level of RS64_TRIGGER_VOICES (gen_voice_cats.mjs).
function triggerVoiceIds(it, tv) {
  const ids = it.voiceId != null ? [it.voiceId] : [];
  const p = it.pathEvent, acts = [it.onEnter, it.onExit, p && { counter: p.addCounter, flag: p.setFlag, value: p.value }];
  if (tv) {
    for (const a of acts.filter(Boolean)) {
      if (a.counter) ids.push(...(tv.c[a.counter] ?? []), ...(tv.pc?.[a.counter] ?? []));
      if (a.flag) ids.push(...(a.value ? (tv.b[a.flag] ?? []) : []), ...(tv.pb?.[a.flag]?.[a.value] ?? []));
    }
  }
  return [...new Set(ids)];
}

// data/speech: u32 count, then count x u32 (low 24 bits = offset of a MORT stream: "MORT", flag byte, u8, u16 freq, u32 word count).
function parseSpeech(dv) {
  const out = [], n = dv.getUint32(0);
  for (let i = 0; i < n; i++) {
    const o = dv.getUint32(4 + i * 4) & 0xFFFFFF;
    if (o + 12 > dv.byteLength || dv.getUint32(o) !== 0x4D4F5254) continue;
    out.push({ index: i, offset: o, flag: dv.getUint8(o + 4), freq: dv.getUint16(o + 6), size: dv.getUint32(o + 8) * 4 });
  }
  return out;
}

// ---- MORT voice codec: port of tools/mort_decode.py (method names kept for side-by-side diffing) ----
// Its intermediate/output buffers are write-only for PCM extraction and are left out.
function decodeMort(speech, offset, size) {
  const s16 = (x) => (x << 16) >> 16, s32 = (x) => x | 0, u32 = (x) => x >>> 0;
  const V1T = [-4, -3, -2, -2, -1, -1, -1, -1], V2T = [7, 7, 3, 7, 1, 3, 5, 7];
  const T738 = [0x0CCD, 0x2CCD, 0x5333, 0x7FFF, 0x852A, 0x0000];
  const RAW = 0x1000, PRED = 0xA0, S5 = 0x8A0, S6 = 0xA00;
  const raw = new Uint8Array(RAW);
  const rd32 = (o) => ((raw[o] << 24) | (raw[o + 1] << 16) | (raw[o + 2] << 8) | raw[o + 3]) >>> 0;
  const st = {
    predictor: new Array(PRED).fill(0), skipReset: 0, resetPred: 0, lastBase: 0, lastSample: 0, sampleBuffer: new Array(8).fill(0),
    curSmoother: 0, spA: new Array(8).fill(0), spB: new Array(8).fill(0),
    amountSoundLeft: 0, unk3E80: 0, compWordsLeft: 0, bitPos: 0, chunkUsed: 0, chunkLeft: 0, curF08: 0, curF0C: 0,
    status1: 0, status2: 4, status3: 2, romAddr: offset, romMortAddr: offset,
    volume: 0x7F, play1: 0xFFFFFFFF, play2: 0xFFFFFFFF, inc2: 0x1F8, mortStatus2: 0, eee8: 0xB8, eeec: 0x194, ced0: 0, cf04: 0, cf20: 1, started: false,
  };
  const pcm = [];

  const fn800455DC = (src, dst, n) => {
    for (let i = 0; i < n; i++) raw[(dst + i) % RAW] = src + i < speech.length ? speech[src + i] : 0;
  };
  const fn800456D0 = () => {
    st.predictor.fill(0); st.skipReset = 0; st.resetPred = 0; st.lastBase = 0x28; st.lastSample = 0;
    st.sampleBuffer.fill(0); st.curSmoother = 0; st.spA.fill(0); st.spB.fill(0);
    st.romAddr = st.romMortAddr;
    st.status2 = 1; st.status1 = 0; st.amountSoundLeft = 0; st.unk3E80 = 0; st.bitPos = 0; st.chunkLeft = 0; st.chunkUsed = 0;
    st.curF0C = 0; st.curF08 = 0; st.status3 = 2;
  };
  // state = [currentInputData (u32), bitsleft, currentOverallBitPosition (u32)]
  const readBits = (n, s) => {
    let [cur, left, pos] = s;
    if (s32(left) >= n + 1) {
      const r = cur & ((1 << n) - 1);
      s[0] = cur >>> n; s[1] = left - n; s[2] = pos;
      return r & 0xFFFF;
    }
    const t9 = (1 << left) - 1;
    pos = u32(pos + 0x20);
    const t7 = t9 & cur;
    let r = t7 & 0xFFFF;
    cur = rd32(((pos >>> 5) & 0x3FF) << 2 & 0xFFFF);
    if (left !== n) r = (t7 | ((cur & ((1 << (n - left)) - 1)) << left)) & 0xFFFF;
    s[0] = cur >>> (n - left); s[1] = u32(left + 0x20 - n); s[2] = pos;
    return r;
  };
  const fn80048590 = (spc8, off, sb2, row) => {
    spc8 = s16(spc8); off = s16(off);
    let T0, T1;
    if (spc8 < 8 && spc8 >= 0) { T0 = V1T[spc8]; T1 = V2T[spc8]; } else { T0 = (spc8 - 8) >> 3; T1 = spc8 & 7; }
    T0 = 6 - T0;
    const T4 = T1 * 0x800 + 0x47FF;
    sb2.fill(0);
    for (let x = 0; x < 0xD; x++) {
      const v = s16(row[x]);
      let t5 = s32((v * 0x2000 - 0x7000) * T4 + 0x4000) >> 0xF;
      // MIPS masks shift amounts to 5 bits: T0 == 0 makes the bias 1 << 31.
      const bias = 1 << ((T0 - 1) & 0x1F);
      t5 = s32(t5 + bias) >> (T0 & 0x1F);
      row[x] = t5 & 0xFFFF;
      sb2[off + x * 3] = s16(t5);
    }
  };
  const fn80048684 = (spe0, spd0, sb2) => {
    spe0 = s16(spe0);
    if (spe0 >= 0x28 && spe0 <= 0x78) st.lastBase = spe0 & 0xFFFF;
    const p = st.predictor;
    for (let x = 0; x < 0x78; x++) p[x] = p[x + 0x28];
    const coef = T738[spd0 & 0xFFFF];
    for (let x = 0; x < 0x28; x++) {
      const idx = 0x78 - st.lastBase + x;
      let T1 = idx >= 0 && idx < PRED ? s16(p[idx]) : 0;
      T1 = s32(T1 * coef + 0x4000) >> 0xF;
      p[0x78 + x] = (T1 + s16(sb2[x])) & 0xFFFF;
    }
  };
  const fn80048B3C = (T2) => {
    T2 = s32(T2);
    const neg = T2 < 0;
    if (neg) T2 = -T2;
    if (T2 < 0x2B33) T2 *= 2;
    else if (T2 < 0x4E66) T2 += 0x2B33;
    else T2 = (T2 >> 2) + 0x6600;
    if (T2 > 0x7FFF) T2 = 0x7FFF;
    if (neg) T2 = -T2;
    return T2 & 0xFFFF;
  };
  const BLEND = [
    (a, b) => fn80048B3C((a >> 2) + (b >> 1) + (b >> 2)),
    (a, b) => fn80048B3C((a >> 1) + (b >> 1)),
    (a, b) => fn80048B3C((a >> 1) + (b >> 2) + (a >> 2)),
    (a) => fn80048B3C(a),
  ];
  const fn80048A58 = (alg) => {
    const [sp1, sp2] = st.curSmoother ? [st.spB, st.spA] : [st.spA, st.spB];
    return sp1.map((v, x) => BLEND[alg](s16(v), s16(sp2[x])));
  };
  const fn80048740 = (pOff, count, adjusters, S, A3) => {
    const adj = adjusters.map(s16);
    for (let x = 0; x < count; x++) {
      const pidx = pOff + x;
      let T2 = pidx >= 0 && pidx < PRED ? s16(st.predictor[pidx]) : 0;
      T2 = s32(T2 - (s32(s32(S[7]) * adj[7] + 0x4000) >> 0xF));
      // Each stage updates S[i + 1] with the current tap's adjuster (one stage later than it subtracts).
      for (let i = 6; i >= 0; i--) {
        T2 = s32(T2 - (s32(s32(S[i]) * adj[i] + 0x4000) >> 0xF));
        S[i + 1] = s32((s32(adj[i] * T2 + 0x4000) >> 0xF) + s32(S[i]));
      }
      S[0] = T2;
      T2 = s32(T2 + (s32(0x6E14 * A3[0] + 0x4000) >> 0xF));
      A3[0] = s16(T2);
      const sh = s32(T2 << 1);
      const AT = ((sh >> 0xF) + 1) >> 1;
      const out = (AT !== 0 ? (AT < 0 ? -0x8000 : 0x7FFF) : sh) & 0xFFF8 & 0xFFFF;
      pcm.push(out);
    }
  };
  const fn80048904 = () => {
    const S = st.sampleBuffer.slice(), A3 = [s16(st.lastSample)];
    fn80048740(0, 0xD, fn80048A58(0), S, A3);
    fn80048740(0xD, 0xE, fn80048A58(1), S, A3);
    fn80048740(0x1B, 0xD, fn80048A58(2), S, A3);
    fn80048740(0x28, 0x78, fn80048A58(3), S, A3);
    st.sampleBuffer = S;
    st.lastSample = s16(A3[0]);
  };
  const fn80045A80 = (spe8) => {
    const sp = st.curSmoother ? st.spB : st.spA;
    const calc = (i, k, coef, bias) => ((s32((s16(spe8[i]) - k) * 0x400 * coef + bias) >> 0xF) << 1) & 0xFFFF;
    sp[0] = calc(0, 0x20, 0x3333, 0x4000);
    sp[1] = calc(1, 0x20, 0x3333, 0x4000);
    sp[2] = calc(2, 0x10, 0x3333, -0x332F000);
    sp[3] = calc(3, 0x10, 0x3333, 0x04003C00);
    sp[4] = calc(4, 8, 0x4B17, s32(0xFFC91B1C));
    sp[5] = calc(5, 8, 0x4444, 0x03BBF800);
    sp[6] = calc(6, 4, 0x7ADE, 0x0147936C);
    sp[7] = calc(7, 4, 0x740C, 0x040D6B40);
    fn80048904();
    st.curSmoother = st.curSmoother ? 0 : 1;
  };
  const fn80045FF0 = () => {
    const pos0 = st.bitPos;
    const word = rd32((((s32(pos0) >> 5) & 0x3FF) << 2) & 0xFFFF);
    const used = pos0 & 0x1F;
    const s = [word >>> used, u32(0x20 - used), pos0];
    if (st.skipReset === 0 && st.resetPred === 0) {
      if (readBits(1, s)) st.resetPred = readBits(4, s) + 1;
      else st.skipReset = readBits(7, s) + 1;
    }
    if (st.resetPred !== 0) {
      st.resetPred--;
      // A silence frame takes real time in the game's wall-clock mixer; offline it must emit its 158 samples.
      for (let i = 0; i < 0x9E; i++) pcm.push(0);
    } else {
      const spe8 = [6, 6, 5, 5, 4, 4, 3, 3].map((n) => readBits(n, s));
      const spe0 = [], spd0 = [], sbOff = [], spc8 = [], sp60 = [];
      for (let x = 0; x < 4; x++) {
        spe0.push(readBits(7, s)); spd0.push(readBits(2, s)); sbOff.push(readBits(2, s)); spc8.push(readBits(6, s));
        const row = [];
        for (let y = 0; y < 0xD; y++) row.push(readBits(3, s));
        sp60.push(row);
      }
      st.skipReset--;
      const sb2 = new Array(0x28).fill(0);
      for (let x = 0; x < 4; x++) {
        fn80048590(spc8[x], sbOff[x], sb2, sp60[x]);
        fn80048684(spe0[x], spd0[x], sb2);
      }
      fn80045A80(spe8);
    }
    st.bitPos = u32(u32(s[2] & 0xFFFFFFE0) - s[1] + 0x20);
  };
  const tail = (V1) => {
    if (st.status2 === 1 && s32(V1) >= 0x8C0) { st.status2 = 2; st.started = true; }
  };
  const fn80045780 = () => {
    if (st.status1 !== 0) {
      let V1 = st.chunkLeft;
      if (V1 === 0) {
        const T9 = rd32(4), T0 = rd32(8);
        st.bitPos = 0x60;
        st.amountSoundLeft = (T9 >>> 16) & 0xFFFF;
        st.compWordsLeft = T0 & 0xFFFFFF;
        st.unk3E80 = T9 & 0xFFFF;
        V1 = st.chunkLeft;
      }
      const V0 = st.compWordsLeft;
      st.chunkLeft = u32(V1 + 0x400);
      st.compWordsLeft = V0 < 0x100 ? 0 : u32(V0 - 0x100);
      st.status1 = 0;
    }
    let S2 = st.status3;
    if (st.status1 === 0) {
      if (st.status2 >= 1 && st.status2 <= 3) {
        const V1 = st.chunkLeft;
        if (s32(u32(V1 - st.chunkUsed)) < 0xC01) {
          const A2 = V1 !== 0 ? (st.compWordsLeft < 0x101 ? st.compWordsLeft << 2 : 0x400) : 0x400;
          if (s32(A2) > 0) {
            st.status1 = 1;
            fn800455DC(u32(st.romAddr + V1), ((V1 & 0xFFF) >> 3) << 3, A2);
          }
        }
      }
      let S1 = st.curF0C;
      let V1 = u32(S1 - st.curF08);
      if (s32(V1) < 0x960) {
        let T5;
        if (st.status2 === 1) T5 = st.chunkLeft;
        else {
          if (s16(st.amountSoundLeft) <= 0) return tail(V1);
          T5 = st.chunkLeft;
        }
        if (s32(u32(T5 - st.chunkUsed)) <= 0 || s32(S2) <= 0) return tail(V1);
        for (;;) {
          fn80045FF0();
          S1 = u32(st.curF0C + 0xA0);
          V1 = u32(S1 - st.curF08);
          const T7 = Math.max(0, st.amountSoundLeft - 1);
          st.chunkUsed = (st.bitPos >>> 3) & 0xFFFFFFFC;
          st.curF0C = S1;
          st.amountSoundLeft = T7 & 0xFFFF;
          S2 = u32(S2 - 1);
          if (s32(V1) >= 0x960) break;
          if (st.status2 !== 1 && s16(T7 & 0xFFFF) <= 0) break;
          if (s32(u32(st.chunkLeft - st.chunkUsed)) <= 0) break;
          if (s32(S2) <= 0) break;
        }
        return tail(V1);
      }
    }
    tail(0);
  };
  const fn800459E0 = (A1) => {
    if (st.status2 !== 3) return 0;
    const V0 = u32(st.curF0C - st.curF08);
    let A2;
    if (A1 < V0) A2 = A1;
    else {
      A2 = V0;
      if (s16(st.amountSoundLeft) <= 0) st.status2 = 4;
    }
    st.curF08 = u32(st.curF08 + A2);
    return A2;
  };

  let ended = -1, loops = 0;
  for (;;) {
    if (st.play1 === 0xFFFFFFFF) { st.play1 = 0xB8; st.play2 = 0xB8; } else {
      for (let i = 0; i < 3; i++) {
        if (st.play1 >= 0x8A0) { st.play1 = 0xB8; st.play2 = 0xB8; } else { st.play1 = u32(st.play1 + 0xB8); st.play2 = u32(st.play2 + 0xB8); }
      }
    }
    if (st.mortStatus2 !== 2) {
      const A0 = st.inc2, V1 = st.play2;
      const S7 = V1 < A0 ? (A0 - V1 < st.eeec ? st.eeec : st.eee8) : (S5 - st.eeec < V1 - A0 ? st.eeec : st.eee8);
      fn80045780();
      if (st.status2 === 2) { st.status2 = 3; st.ced0 = 1; st.cf20 = 0; }
      fn800459E0(S7);
      if (st.mortStatus2 === 0) { fn800456D0(); st.mortStatus2 = 4; st.ced0 = 0; st.cf04 = st.volume; }
    }
    loops++;
    if (!st.started) {
      if (pcm.length) st.started = true;
    } else if (st.status2 === 4 || loops >= 2000) {
      if (ended === -1) ended = loops + 1000;
      else if (loops > ended) break;
    }
    if (loops > 50000) break;
  }
  return Int16Array.from(pcm);
}

// 8 kHz mono PCM -> one MORT stream: a GSM 06.10 full-rate coder (port of libgsm's fixed-point encoder, the decoder above is its
// inverse). Header "MORT", u16 frames, u16 rate, u32 words with the header; from bit 96, runs of 1-128 coded frames (bit 0, 7 bits
// count - 1) of 260 bits each, packed LSB-first into big-endian words.
function encodeMort(pcm) {
  const sat = (x) => (x < -32768 ? -32768 : x > 32767 ? 32767 : x);
  const lsat = (x) => (x < -2147483648 ? -2147483648 : x > 2147483647 ? 2147483647 : x);
  const add = (a, b) => sat(a + b), sub = (a, b) => sat(a - b);
  const multR = (a, b) => (a === -32768 && b === -32768 ? 32767 : Math.floor((a * b + 16384) / 32768));
  const mult = (a, b) => (a === -32768 && b === -32768 ? 32767 : Math.floor((a * b) / 32768));
  const asr = (a, n) => Math.floor(a / 2 ** n);
  const abs = (a) => (a === -32768 ? 32767 : Math.abs(a));
  const norm = (a) => Math.clz32(a < 0 ? (a <= -1073741824 ? 0 : ~a) : a) - 1;
  const div = (num, den) => {
    let r = 0, L_num = num;
    for (let k = 15; k--;) {
      r <<= 1;
      L_num <<= 1;
      if (L_num >= den) {
        L_num -= den;
        r++;
      }
    }
    return r;
  };
  const STEPS = [[20480, 0, 31, -32], [20480, 0, 31, -32], [20480, 2048, 15, -16], [20480, -2560, 15, -16], [13964, 94, 7, -8], [15360, -1792, 7, -8], [8534, -341, 3, -4], [9036, -1144, 3, -4]];
  const INVA = [13107, 13107, 13107, 13107, 19223, 17476, 31454, 29708];
  const H = [-134, -374, 0, 2054, 5741, 8192, 5741, 2054, 0, -374, -134];
  const DLB = [6554, 16384, 26214, 32767], QLB = [3277, 11469, 21299, 32767];
  const NRFAC = [29128, 26215, 23832, 21846, 20165, 18725, 17476, 16384], FAC = [18431, 20479, 22527, 24575, 26623, 28671, 30719, 32767];
  const st = { z1: 0, L_z2: 0, mp: 0, u: new Array(8).fill(0), LARpp: [new Array(8).fill(0), new Array(8).fill(0)], j: 0, dp0: new Array(280).fill(0) };

  const frame = (s) => {
    // Preprocessing: downscale to 13 bits, offset compensation, pre-emphasis.
    const so = new Array(160);
    for (let k = 0; k < 160; k++) {
      const SO = asr(s[k], 3) * 4;
      const s1 = SO - st.z1;
      st.z1 = SO;
      let L_s2 = s1 * 32768;
      const msp = asr(st.L_z2, 15), lsp = st.L_z2 - msp * 32768;
      L_s2 += multR(lsp, 32735);
      st.L_z2 = lsat(msp * 32735 + L_s2);
      const L_temp = lsat(st.L_z2 + 16384);
      const m = multR(st.mp, -28180);
      st.mp = asr(L_temp, 15);
      so[k] = add(st.mp, m);
    }
    // LPC analysis: autocorrelation, Schur reflection coefficients, log-area ratios, quantization.
    let smax = 0;
    for (const v of so) smax = Math.max(smax, abs(v));
    const scal = smax === 0 ? 0 : 4 - norm(smax << 16);
    const ss = scal > 0 ? so.map((v) => multR(v, 16384 >> (scal - 1))) : so.slice();
    const L_ACF = new Array(9).fill(0);
    for (let k = 0; k < 9; k++) {
      let acc = 0;
      for (let i = k; i < 160; i++) acc += ss[i] * ss[i - k];
      L_ACF[k] = acc * 2;
    }
    const r = new Array(8).fill(0);
    if (L_ACF[0] !== 0) {
      const t = norm(L_ACF[0]);
      const ACF = L_ACF.map((v) => asr(v * 2 ** t, 16));
      const P = ACF.slice(), K = ACF.slice();
      for (let n = 1; n <= 8; n++) {
        const temp = abs(P[1]);
        if (P[0] < temp) break;
        let rn = div(temp, P[0]);
        if (P[1] > 0) rn = -rn;
        r[n - 1] = rn;
        if (n === 8) break;
        P[0] = add(P[0], multR(P[1], rn));
        for (let m = 1; m <= 8 - n; m++) {
          P[m] = add(P[m + 1], multR(K[m], rn));
          K[m] = add(K[m], multR(P[m + 1], rn));
        }
      }
    }
    const LARc = r.map((v, i) => {
      let t = abs(v);
      t = t < 22118 ? t >> 1 : t < 31130 ? t - 11059 : (t - 26112) << 2;
      const lar = v < 0 ? -t : t;
      const [A, B, MAC, MIC] = STEPS[i];
      const q = asr(add(add(mult(A, lar), B), 256), 9);
      return q > MAC ? MAC - MIC : q < MIC ? 0 : q - MIC;
    });
    // Short-term analysis filter with the decoded, interpolated LARs (as the decoder will see them).
    const cur = st.LARpp[st.j], prev = st.LARpp[st.j ^ 1];
    st.j ^= 1;
    LARc.forEach((c, i) => {
      const [, B, , MIC] = STEPS[i];
      const t = multR(INVA[i], sub(add(c, MIC) * 1024, B * 2));
      cur[i] = add(t, t);
    });
    const toRp = (v) => {
      let t = abs(v);
      t = t < 11059 ? t << 1 : t < 20070 ? t + 11059 : (t >> 2) + 26112;
      return v < 0 ? -t : t;
    };
    const filt = (lar, from, n) => {
      const rp = lar.map(toRp);
      for (let k = from; k < from + n; k++) {
        let di = so[k], sav = di;
        for (let i = 0; i < 8; i++) {
          const ui = st.u[i];
          st.u[i] = sav;
          sav = add(ui, multR(rp[i], di));
          di = add(di, multR(rp[i], ui));
        }
        so[k] = di;
      }
    };
    filt(cur.map((c, i) => (prev[i] >> 2) + (c >> 2) + (prev[i] >> 1)), 0, 13);
    filt(cur.map((c, i) => (prev[i] >> 1) + (c >> 1)), 13, 14);
    filt(cur.map((c, i) => (prev[i] >> 2) + (c >> 2) + (c >> 1)), 27, 13);
    filt(cur, 40, 120);
    // Four subframes: long-term predictor, then RPE.
    const out = [LARc];
    const dp = st.dp0;
    for (let sf = 0; sf < 4; sf++) {
      const base = 120 + sf * 40, d = so.slice(sf * 40, sf * 40 + 40);
      let dmax = 0;
      for (const v of d) dmax = Math.max(dmax, abs(v));
      const tn = dmax === 0 ? 0 : norm(dmax << 16), sc = tn > 6 ? 0 : 6 - tn;
      const wt = d.map((v) => asr(v, sc));
      let Nc = 40, L_max = 0;
      for (let lambda = 40; lambda <= 120; lambda++) {
        let acc = 0;
        for (let k = 0; k < 40; k++) acc += wt[k] * dp[base + k - lambda];
        if (acc > L_max) {
          Nc = lambda;
          L_max = acc;
        }
      }
      L_max = asr(L_max * 2, 6 - sc);
      let L_power = 0;
      for (let k = 0; k < 40; k++) L_power += asr(dp[base + k - Nc], 3) ** 2;
      L_power *= 2;
      let bc = 0;
      if (L_max <= 0) bc = 0;
      else if (L_max >= L_power) bc = 3;
      else {
        const t = norm(L_power), R = asr(L_max * 2 ** t, 16), S = asr(L_power * 2 ** t, 16);
        for (bc = 0; bc <= 2; bc++) if (R <= mult(S, DLB[bc])) break;
      }
      const dpp = new Array(40), e = new Array(50).fill(0);
      for (let k = 0; k < 40; k++) {
        dpp[k] = multR(QLB[bc], dp[base + k - Nc]);
        e[k + 5] = sub(d[k], dpp[k]);
      }
      const x = new Array(40);
      for (let k = 0; k < 40; k++) {
        let acc = 4096;
        for (let i = 0; i < 11; i++) acc += e[k + i] * H[i];
        x[k] = sat(asr(acc, 13));
      }
      let Mc = 0, EM = -1;
      for (let m = 0; m < 4; m++) {
        let acc = 0;
        for (let i = 0; i < 13; i++) acc += asr(x[m + 3 * i], 2) ** 2;
        if (acc * 2 > EM) {
          Mc = m;
          EM = acc * 2;
        }
      }
      const xM = Array.from({ length: 13 }, (_, i) => x[Mc + 3 * i]);
      let xmax = 0;
      for (const v of xM) xmax = Math.max(xmax, abs(v));
      let exp = 0, temp = asr(xmax, 9), itest = 0;
      for (let i = 0; i <= 5; i++) {
        itest |= temp <= 0;
        temp = asr(temp, 1);
        if (!itest) exp++;
      }
      const xmaxc = add(asr(xmax, exp + 5), exp << 3);
      let mexp = 0, mant;
      if (xmaxc > 15) mexp = (xmaxc >> 3) - 1;
      mant = xmaxc - (mexp << 3);
      if (mant === 0) {
        mexp = -4;
        mant = 7;
      } else {
        while (mant <= 7) {
          mant = (mant << 1) | 1;
          mexp--;
        }
        mant -= 8;
      }
      const xMc = xM.map((v) => asr(mult(v * 2 ** (6 - mexp), NRFAC[mant]), 12) + 4);
      // Reconstruct exactly as the decoder does, so the predictor history matches.
      const t2 = 6 - mexp, t3 = t2 - 1 >= 0 ? 1 << (t2 - 1) : 0;
      const ep = new Array(40).fill(0);
      xMc.forEach((c, i) => {
        const t = add(multR(FAC[mant], ((c << 1) - 7) << 12), t3);
        ep[Mc + 3 * i] = t2 >= 0 ? asr(t, t2) : t << -t2;
      });
      for (let k = 0; k < 40; k++) dp[base + k] = add(ep[k], dpp[k]);
      out.push([Nc, bc, Mc, xmaxc, ...xMc]);
    }
    for (let k = 0; k < 120; k++) dp[k] = dp[k + 160];
    return out;
  };

  const frames = Math.ceil(pcm.length / 160);
  const words = [0, 0, 0];
  let bit = 96;
  const put = (v, n) => {
    for (let i = 0; i < n; i++, bit++) {
      if ((bit >> 5) >= words.length) words.push(0);
      if ((v >> i) & 1) words[bit >> 5] |= 1 << (bit & 31);
    }
  };
  const LAR_BITS = [6, 6, 5, 5, 4, 4, 3, 3];
  const block = (f) => Array.from({ length: 160 }, (_, k) => pcm[f * 160 + k] ?? 0);
  // Frames below about -54 dBFS RMS are stored as silence runs (1-16 frames in 5 bits) instead of 260 coded bits.
  const silent = Array.from({ length: frames }, (_, f) => block(f).reduce((a, v) => a + v * v, 0) < 64 * 64 * 160);
  for (let f = 0; f < frames;) {
    let n = 1;
    const max = silent[f] ? 16 : 128;
    while (n < max && f + n < frames && silent[f + n] === silent[f]) n++;
    put(silent[f] ? 1 : 0, 1);
    put(n - 1, silent[f] ? 4 : 7);
    if (silent[f]) {
      f += n;
      continue;
    }
    for (const end = f + n; f < end; f++) coded(block(f));
  }
  function coded(s) {
    const [LARc, ...subs] = frame(s);
    LARc.forEach((c, i) => put(c, LAR_BITS[i]));
    for (const [Nc, bc, Mc, xmaxc, ...xMc] of subs) {
      put(Nc, 7);
      put(bc, 2);
      put(Mc, 2);
      put(xmaxc, 6);
      for (const c of xMc) put(c, 3);
    }
  }
  // The player prebuffers ~15 frames before it checks the frame count: an uncounted silence run makes reads past a short line silent.
  put(1, 1);
  put(15, 4);
  const out = new Uint8Array(words.length * 4), dv = new DataView(out.buffer);
  words.forEach((w, i) => dv.setUint32(i * 4, w >>> 0));
  dv.setUint32(0, 0x4D4F5254);
  dv.setUint16(4, frames);
  dv.setUint16(6, 8000);
  dv.setUint32(8, words.length);
  return out;
}

// Sprite animations (civ_anm_run, trp_anm_run, ...): frames of u32 length + HMT texture entry (offsets frame-relative) + palette + pixels + name, padded to 4.
// The game keys frames with the colour key of the model texture they replace (trp_anm_sht stores 0 but st_sht is keyed 0x969696); pass it as trans.
function parseSpriteAnim(dv, trans) {
  const frames = [];
  for (let o = 0, base = 4; base + 0x34 <= dv.byteLength; o = base + ((dv.getUint32(o) + 3) & ~3), base = o + 4) {
    const ft = dv.getUint16(base + 0x2E);
    const t = {
      pix: base + dv.getUint32(base), pal: base + dv.getUint32(base + 0x20), w: dv.getUint16(base + 0x28), h: dv.getUint16(base + 0x2A),
      depth: dv.getUint8(base + 0x2D), type: ft & 0xF, flags: ft >> 4, trans: dv.getUint32(base + 0x30),
      mask: dv.getUint32(base + 4) ? base + dv.getUint32(base + 4) : 0, maskEnd: base + dv.getUint32(base + 0x24),
      name: cstr(dv, base + dv.getUint32(base + 0x24), 16),
    };
    if (!t.w || t.w > 256 || !t.h || t.h > 256) return null;
    if (trans != null) t.trans = trans;
    // Bit depth from the pixel block's size (it ends at the palette, mask or name, whichever follows it): part_spr's gray
    // frames are 4bpp despite depth 0, so the HMT type/depth rule does not hold for effect frames.
    const end = Math.min(...[t.pal, t.mask, t.maskEnd].filter((x) => x > t.pix));
    const bpp = Math.round(((end - t.pix) * 8) / (t.w * t.h));
    if ([0, 1, 4, 5].includes(t.type) && (bpp === 4 || bpp === 8 || (bpp === 16 && t.type >= 4))) t.bpp = bpp;
    t.rgba = decodeTexture(dv, t);
    frames.push(t);
  }
  return frames.length ? frames : null;
}

// ---- Audio: MusyX samples (sdir_SND + samp_SND): N64 MusyX ADPCM ----

function parseSdir(dv) {
  const out = [];
  for (let o = 0; o + 0x18 <= dv.byteLength && dv.getUint16(o) !== 0xFFFF; o += 0x18) {
    out.push({ index: out.length, id: dv.getUint16(o), off: dv.getUint32(o + 4), root: dv.getUint8(o + 8), rate: dv.getUint16(o + 0xA),
      num: dv.getUint32(o + 0xC) & 0xFFFFFF, loopStart: dv.getUint32(o + 0x10), loopLen: dv.getUint32(o + 0x14) });
  }
  return out;
}

// Each sample: 8x2x8 s16 coefficient book (256 bytes), then 0x28-byte frames of two self-contained 32-sample halves.
function decodeMusyxAdpcm(samp, off, num) {
  const s16 = (v) => (v << 16) >> 16, clamp = (v) => (v < -32768 ? -32768 : v > 32767 ? 32767 : v);
  const dv = new DataView(samp.buffer, samp.byteOffset, samp.byteLength);
  const out = new Int16Array(num);
  if (!num) return out;
  const book = [];
  for (let k = 0; k < 16; k++) book.push(Array.from({ length: 8 }, (_, j) => dv.getInt16(off + k * 16 + j * 2)));
  let n = 0;
  for (let fp = off + 256; n < num && fp + 0x28 <= samp.length; fp += 0x28) {
    for (const [rawo, nibo] of [[0, 8], [4, 0x18]]) {
      const c2 = samp[fp + nibo] % 0x80, bi = (c2 & 0xF0) >> 4, rshift = c2 & 0xF;
      const b1 = book[bi * 2], b2 = book[bi * 2 + 1];
      const f = new Array(32);
      f[0] = s16((samp[fp + rawo] << 8) | samp[fp + rawo + 1]);
      f[1] = s16((samp[fp + rawo + 2] << 8) | samp[fp + rawo + 3]);
      for (let i = 1; i < 16; i++) {
        const b = samp[fp + nibo + i];
        f[2 * i] = s16((b & 0xF0) << 8) >> rshift;
        f[2 * i + 1] = s16((b & 0x0F) << 12) >> rshift;
      }
      const base = n;
      for (let k = 0; k < 2 && n < num; k++) out[n++] = f[k];
      for (const [o, hoff, size] of [[2, 0, 6], [8, 6, 8], [16, 14, 8], [24, 22, 8]]) {
        const l1 = out[base + hoff], l2 = out[base + hoff + 1];
        for (let i = 0; i < size && n < num; i++) {
          let acc = f[o + i] * 2048 + b1[i] * l1 + b2[i] * l2;
          for (let k = 0; k < i; k++) acc += b2[k] * f[o + i - 1 - k];
          out[n++] = clamp(Math.floor(acc / 2048));
        }
      }
    }
  }
  return n < num ? out.slice(0, n) : out;
}

// pool_SND: 4 sections (offsets in the first 4 words) of [u32 size, u16 id, pad, data]; 0 macros, 1 ADSR, 2 keymaps, 3 layers.
function parsePool(dv) {
  const bytes = new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength);
  return [0, 1, 2, 3].map((s) => {
    const m = new Map();
    for (let o = dv.getUint32(s * 4); o + 8 <= dv.byteLength;) {
      const size = dv.getUint32(o);
      if (size === 0xFFFFFFFF || !size) break;
      m.set(dv.getUint16(o + 4), bytes.subarray(o + 8, o + size));
      o += size;
    }
    return m;
  });
}

// proj_SND type-1 section (the SFX group), sub-segment 5: u16 count, pad, 0xC-byte {id, macro (|0x8000 = layer), 2 bytes voices/priority, vel, pan, key}.
function parseSfxTable(dv) {
  let off = 0;
  while (off + 8 <= dv.byteLength && dv.getUint16(off + 6) !== 1) off += dv.getUint32(off);
  const p = off + 8 + dv.getUint32(off + 8 + 20), out = [];
  for (let i = 0, n = dv.getUint16(p); i < n; i++) {
    const e = p + 4 + i * 12;
    out.push({ id: dv.getUint16(e), macro: dv.getUint16(e + 2), vel: dv.getUint8(e + 6), pan: dv.getUint8(e + 7), key: dv.getUint8(e + 8) });
  }
  return out;
}

// ---- Music: song table, song -> group binding, _SNG sequences (format: startSongSequence / advanceSongRegion) ----

// D_main_8009FCD4: {u32 songId, char *name} until id == -1; names are sound/<name>_SNG.
function parseSongTable(rom) {
  const be32 = (o) => ((rom[o] << 24) | (rom[o + 1] << 16) | (rom[o + 2] << 8) | rom[o + 3]) >>> 0, R = (v) => v - 0x80000400 + 0x1000;
  const out = [];
  for (let o = R(0x8009FCD4); be32(o) !== 0xFFFFFFFF; o += 8) {
    let p = R(be32(o + 4)), name = '';
    while (rom[p]) name += String.fromCharCode(rom[p++]);
    out.push({ id: be32(o), name });
  }
  return out;
}

// proj_SND type-0 sections: playSongById(group, songId) searches the MIDISetup list (+8 + [+0x24]: 0x84-byte records
// {u16 songId, u16, 16 x {program, CC7, CC10, CC91, CC93, ...}}); program tables at +8 + [+0x1C] (normal) / [+0x20] (drums,
// channel 9): 8-byte {u16 target, x2, x3, defaultChannel, programNumber, pad} until programNumber 0xFF.
function parseSongGroups(dv) {
  const out = new Map();
  for (let off = 0; off + 8 <= dv.byteLength;) {
    const size = dv.getUint32(off);
    if (!size || size === 0xFFFFFFFF) break;
    if (dv.getUint16(off + 6) === 0) {
      const group = dv.getUint16(off + 4), sub = (k) => off + 8 + dv.getUint32(off + k);
      const progTable = (p) => {
        const m = new Map();
        for (; p + 8 <= dv.byteLength && dv.getUint8(p + 5) !== 0xFF; p += 8) {
          m.set(dv.getUint8(p + 5), { target: dv.getUint16(p), x2: dv.getUint8(p + 2), x3: dv.getUint8(p + 3), defaultChannel: dv.getUint8(p + 4) });
        }
        return m;
      };
      const progs = progTable(sub(0x1C)), drums = progTable(sub(0x20));
      for (let p = sub(0x24); dv.getUint16(p) !== 0xFFFF; p += 0x84) {
        const setup = Array.from({ length: 16 }, (_, c) => {
          const e = p + 4 + c * 8;
          return { program: dv.getUint8(e), vol: dv.getUint8(e + 1), pan: dv.getUint8(e + 2), reverb: dv.getUint8(e + 3), chorus: dv.getUint8(e + 4) };
        });
        out.set(dv.getUint16(p), { group, setup, progs, drums });
      }
    }
    off += size;
  }
  return out;
}

// _SNG: header {trackTbl, regionTbl, chanMap u8[64], tempoTbl, bpm, loopTick}; TrackRegion 12 B {start, program, CC7, -, regionIdx
// (0xFFFF end, 0xFFFE loop -> u16 loopTo at +0xA), transpose, velAdd}; region {+4 bend lane, +8 mod lane, events from +0xC:
// {u32 tick, u16 dur, a, b} until a == b == 0xFF}; lanes are (dt, dv) varints ending at 0x80 0x00.
function parseSong(dv) {
  const L = dv.byteLength, trackTbl = dv.getUint32(0), regionTbl = dv.getUint32(4), chanMap = dv.getUint32(8), tempoTbl = dv.getUint32(0xC);
  const tempo = [];
  for (let o = tempoTbl; tempoTbl && o + 8 <= L && dv.getUint32(o) !== 0xFFFFFFFF; o += 8) tempo.push({ tick: dv.getUint32(o), bpm: dv.getUint32(o + 4) });
  const lane = (o) => {
    const out = [];
    if (!o) return out;
    for (let n = 0; o + 2 <= L && n < 100000; n++) {
      const b0 = dv.getUint8(o);
      if (b0 === 0x80 && dv.getUint8(o + 1) === 0) break;
      const dt = b0 & 0x80 ? ((b0 & 0x7F) << 8) | dv.getUint8(o + 1) : b0;
      o += b0 & 0x80 ? 2 : 1;
      const v0 = dv.getUint8(o);
      const dvv = v0 & 0x80 ? ((((v0 & 0x7F) << 8) | dv.getUint8(o + 1)) << 17) >> 17 : (v0 << 25) >> 25;
      o += v0 & 0x80 ? 2 : 1;
      out.push({ dt, dv: dvv });
    }
    return out;
  };
  const regions = new Map(), tracks = [];
  for (let t = 0; t < 64; t++) {
    const to = dv.getUint32(trackTbl + t * 4);
    if (!to) continue;
    const track = { index: t, channel: dv.getUint8(chanMap + t), regions: [] };
    for (let r = to; r + 12 <= L; r += 12) {
      const idx = dv.getUint16(r + 8);
      if (idx === 0xFFFF) break;
      if (idx === 0xFFFE) {
        track.regions.push({ start: dv.getUint32(r), loopTo: dv.getUint16(r + 0xA) });
        break;
      }
      track.regions.push({ start: dv.getUint32(r), program: dv.getUint8(r + 4), vol: dv.getUint8(r + 5), region: idx,
        transpose: dv.getInt8(r + 0xA), velAdd: dv.getInt8(r + 0xB) });
      if (!regions.has(idx)) {
        const ro = dv.getUint32(regionTbl + idx * 4), events = [];
        for (let e = ro + 0xC; e + 8 <= L; e += 8) {
          const a = dv.getUint8(e + 6), b = dv.getUint8(e + 7);
          if (a === 0xFF && b === 0xFF) break;
          events.push({ tick: dv.getUint32(e), dur: dv.getUint16(e + 4), a, b });
        }
        regions.set(idx, { events, bend: lane(dv.getUint32(ro + 4)), mod: lane(dv.getUint32(ro + 8)) });
      }
    }
    tracks.push(track);
  }
  return { bpm: dv.getUint32(0x10), loopTick: dv.getUint32(0x14), tempo, tracks, regions };
}

// Program target -> voices (dispatchSequencerNoteEvent): top bits 0 = macro, 0x4000 keymap, 0x8000 layer, 0xC000 none.
// Keymap (pool 2): 128 x 8 B by note {u16 target (0xFFFF silent), s8 transpose, pan, s8 prio}; no volume field.
// Layer (pool 3, applyNoteEventToGroup): u32 count, 12 B {u16 macro, keyLo, keyHi, s8 transpose, vol, s8 prio, -, pan};
// every entry covering the key plays, entries with 0xC000 bits are skipped, vel' = vel * vol / 127.
function resolveNote(pool, target, note, vel, pan, prio) {
  const clamp = (v, hi) => Math.max(0, Math.min(hi, v));
  const panOf = (p) => (p & 0x80 ? 0x80 : clamp(pan + p - 0x40, 127));
  const layer = (id, note, vel, pan, prio) => {
    const L = pool[3].get(id), out = [];
    const n = L ? ((L[0] << 24) | (L[1] << 16) | (L[2] << 8) | L[3]) >>> 0 : 0;
    for (let i = 0; i < n && 4 + i * 12 + 12 <= L.length; i++) {
      const e = 4 + i * 12, m = (L[e] << 8) | L[e + 1];
      if (m & 0xC000 || note < L[e + 2] || note > L[e + 3]) continue;
      out.push({ macro: m, note: clamp(note + ((L[e + 4] << 24) >> 24), 127), vel: Math.floor((vel * L[e + 5]) / 127),
        pan: L[e + 8] & 0x80 ? 0x80 : clamp(pan + L[e + 8] - 0x40, 127), prio: clamp(prio + ((L[e + 6] << 24) >> 24), 255) });
    }
    return out;
  };
  const kind = target & 0xC000;
  if (kind === 0) return [{ macro: target, note, vel, pan, prio }];
  if (kind === 0x8000) return layer(target, note, vel, pan, prio);
  if (kind !== 0x4000) return [];
  const K = pool[2].get(target);
  if (!K) return [];
  const e = (note & 0x7F) * 8, t = (K[e] << 8) | K[e + 1];
  if (t === 0xFFFF) return [];
  const n2 = clamp(note + ((K[e + 2] << 24) >> 24), 127), p2 = panOf(K[e + 3]), pr2 = clamp(prio + ((K[e + 4] << 24) >> 24), 255);
  if ((t & 0xC000) === 0) return [{ macro: t, note: n2, vel, pan: p2, prio: pr2 }];
  return (t & 0xC000) === 0x8000 ? layer(t, n2, vel, p2, pr2) : [];
}

// ---- MusyX voice engine (docs/musyx-voice-mixing.md): one macro voice simulated per audio tick, shared by SFX and songs ----

// Mixer tables read from the ROM (main code): volume curve, mix constants, pan law, semitone and 1/64-semitone rate ratios
// (12.20), quarter-wave sine.
function parseMixTables(rom) {
  const dv = new DataView(rom.buffer, rom.byteOffset, rom.byteLength), R = (v) => v - 0x80000400 + 0x1000;
  const f32 = (v, n) => Float32Array.from({ length: n }, (_, i) => dv.getFloat32(R(v) + i * 4));
  const u32 = (v, n) => Float64Array.from({ length: n }, (_, i) => dv.getUint32(R(v) + i * 4) / 0x100000);
  return { vol: f32(0x800A4BD8, 128), k: f32(0x800A4DD8, 4), pan: f32(0x800A4DE8, 3), semiUp: u32(0x800A2FD0, 128), semiDown: u32(0x800A31D0, 128),
    fineUp: u32(0x800A33D0, 769), fineDown: u32(0x800A3FD4, 769), sine: Int16Array.from({ length: 1024 }, (_, i) => dv.getInt16(R(0x800A0F58) + i * 2)) };
}

const MIX_RATE = 22050, TICK_SAMPLES = 384, TICK_DT = 4456;
const BIPOLAR_SRC = new Set([1, 0xA, 0x80, 0x83, 0x84, 0xA0, 0xA1]);

// computeVoiceVolumePanCoefficients (94A80.c hand asm): vol/pan/span/aux in 16.16 (0..0x7F0000) -> s16 L, R, aux gains.
function voiceGains(T, vol, pan, aux) {
  const curve = (x) => {
    x = Math.min(x, 0x7F0000);
    const i = x >>> 16, a = T.vol[i], b = T.vol[Math.min(i + 1, 127)];
    return a + (b - a) * (x & 0xFFFF) * T.k[0];
  };
  const panT = (x) => {
    const i = x >>> 22;
    return T.pan[i] + (T.pan[Math.min(i + 1, 2)] - T.pan[i]) * (x & 0x3FFFFF) * T.k[3];
  };
  const V = curve(vol);
  let L, Rg;
  if (pan === 0x800000) [Rg, L] = [V * T.k[1] * T.k[2], -V * T.k[1] * T.k[2]];
  else [Rg, L] = [V * panT(pan) * T.k[2], V * panT(0x7FFFFF - pan) * T.k[2]];
  return { L, R: Rg, aux: curve(aux) * T.k[1] * T.k[2] };
}

// Renders one macro voice into out.L / out.R / out.aux (Float32Array, MIX_RATE) starting at sample `at`.
// o = { macro, note, vel, pan (0..127, 0x80 = anti-phase), keyOff (s, Infinity = never), tickRate R (ticks/s), song (sequencer
// channel: CC stages and reverb send active), ctl (t -> {cc: Uint8Array(0x86), bend: 14-bit}), maxT (s) }.
// Returns { pitch: [[t, note]], end (s), unresolved: Set }.
function renderVoice(ctx, o, out, at = 0) {
  ctx.pcm ??= ((cache) => (sd) => cache.get(sd) ?? cache.set(sd, decodeMusyxAdpcm(ctx.samp, sd.off, sd.num)).get(sd))(new Map());
  const T = ctx.tables, pool = ctx.pool, unresolved = new Set(), pitchLog = [];
  const s8 = (v) => (v << 24) >> 24, clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
  // Channel state (offsets in docs/musyx-voice-mixing.md).
  let vol = o.vel << 16, vol2 = vol, pan = (o.pan & 0x80 ? 0x80 : o.pan) << 16, note = o.note, detune = 0, keyedOff = false;
  let ramp = null, panSweep = null, vib = null, bendRange = [2, 2];
  const lfo = [{ period: 0, phase: 0 }, { period: 0, phase: 0 }];
  const recs = { vol: [{ src: 7, op: 0, scale: 0x100 }], bend: [{ src: 0x80, op: 0, scale: 0x100 }] }, pushed = new Set();
  let smp = null, adsr = null, envStage = 'sustain', env = 1, envFrom = 1, envT = 0, stopReq = false;
  let pc = [o.macro, 0], stack = [], waiting = null, stopped = false, steps = 0, seed = o.seed;
  // Seeded so a re-render of the same note (one track at a time) draws the same random waits.
  const rand = seed == null ? Math.random : () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 0x100000000);
  // Tick-unit wait: N * 32000 / R << 3 (computeChannelWaitDuration), in ms*256 units.
  const ticksToUnits = (n) => Math.floor((n * 32000) / o.tickRate) * 8;
  const startWait = (w0, w1) => {
    const b0 = w0 >>> 24, b1 = (w0 >>> 16) & 0xFF, b2 = (w0 >>> 8) & 0xFF;
    if (b2 & 1 && keyedOff) return;
    if (b0 & 1 && !(smp && smp.playing)) return;
    let n = w1 >>> 16;
    if (b1 & 1) n = (rand() * 0x10000 >>> 0) % Math.max(1, n);
    waiting = { units: n === 0xFFFF ? Infinity : (w1 >>> 8) & 1 ? n * 256 : ticksToUnits(n), keyOff: !!(b2 & 1), sampleEnd: !!(b0 & 1) };
    if (waiting.units === 0) waiting = null;
  };
  const tableRate = (n) => {
    if (!smp) return 0;
    const d = n - smp.sd.root;
    return (smp.sd.rate * (d >= 0 ? T.semiUp[Math.min(127, d)] : T.semiDown[Math.min(127, -d)]));
  };
  const fineRatio = (steps64) => {
    const up = steps64 >= 0, s = Math.abs(steps64), oct = Math.floor(s / 768), i = Math.floor(s % 768), f = s - Math.floor(s);
    const tb = up ? T.fineUp : T.fineDown, r = tb[i] + (tb[i + 1] - tb[i]) * f;
    return up ? r * 2 ** oct : r / 2 ** oct;
  };
  const ctlVal = (c, cc, bend) => {
    if (c === 0x80) return bend;
    if (c === 0xA0 || c === 0xA1) return 0x2000 + 2 * lfoOut[c - 0xA0];
    if (c < 0x40) return (cc[c & 0x1F] << 7) | cc[(c & 0x1F) + 0x20];
    if (c >= 0x40 && c <= 0x45) return cc[c] >= 0x40 ? 0x3FFF : 0;
    return cc[c] << 7;
  };
  const evalRec = (rec, cc, bend) => {
    let acc = 0;
    const bip = BIPOLAR_SRC.has(rec[0].src);
    for (const e of rec) {
      const v = ctlVal(e.src, cc, bend);
      const m = BIPOLAR_SRC.has(e.src) ? clamp(((v - 0x2000) * e.scale) >> 8, -0x2000, 0x2000) : clamp((v * e.scale) >> 8, 0, 0x3FFF);
      if (e.op === 0) acc = m;
      else if (e.op === 1) acc = bip ? clamp(acc + m, -0x2000, 0x2000) : clamp(acc + m, 0, 0x3FFF);
      else acc = bip ? (acc * m) >> 13 : (acc * m) >> 14;
    }
    return bip ? acc + 0x2000 : acc;
  };
  const sine = (ang) => {
    const q = ang >> 10, i = ang & 1023, t = T.sine;
    return q === 0 ? t[i] : q === 1 ? t[1023 - i] : q === 2 ? -t[i] : -t[1023 - i];
  };
  const lfoOut = [0, 0];
  const exec = () => {
    while (!waiting && !stopped && steps++ < 20000) {
      const code = pool[0].get(pc[0]);
      if (!code) { stopped = true; break; }
      const p = pc[1] * 8;
      if (p + 8 > code.length) { stopped = true; break; }
      const w0 = ((code[p] << 24) | (code[p + 1] << 16) | (code[p + 2] << 8) | code[p + 3]) >>> 0;
      const w1 = ((code[p + 4] << 24) | (code[p + 5] << 16) | (code[p + 6] << 8) | code[p + 7]) >>> 0;
      const op = w0 & 0x7F, b0 = w0 >>> 24, b1 = (w0 >>> 16) & 0xFF, b2 = (w0 >>> 8) & 0xFF;
      pc = [pc[0], pc[1] + 1];
      switch (op) {
        case 0x00: if (stack.length) { pc = stack.pop(); break; } stopped = true; break;
        case 0x01: if (b2 && stack.length) { pc = stack.pop(); break; } stopped = true; break;
        case 0x03: if (b2 <= vol >>> 16) pc = [w0 >>> 16, w1 & 0xFFFF]; break;
        case 0x04: case 0x07: startWait(w0, op === 0x07 ? (w1 | 0x100) >>> 0 : w1); break;
        case 0x06: if (pool[0].has(w0 >>> 16) && stack.length < 16) { stack.push(pc); pc = [w0 >>> 16, w1 & 0xFFFF]; } break;
        case 0x0C: {
          const d = pool[1].get((w0 >>> 8) & 0xFFFF);
          if (d) adsr = [0, 2, 4, 6].map((i) => d[i] | (d[i + 1] << 8));
          break;
        }
        case 0x0D: vol = clamp(((((w1 >>> 8) & 0xFF ? vol2 : vol) * b2) >>> 7) + (b1 << 16), 0, 0x7F0000); break;
        case 0x0E: {
          const t = ((w0 >>> 16) & 0xFFFF) * 256, a = b2 << 16, d = s8(w1 & 0xFF) << 16;
          pan = a;
          panSweep = t ? { from: a, to: a + d, total: t, left: t } : null;
          if (!t) pan = clamp(a + d, 0, 0x7F0000);
          break;
        }
        case 0x0F: {
          const t = (w1 >>> 8) & 1 ? (w1 >>> 16) * 256 : ticksToUnits(w1 >>> 16);
          const target = clamp(((vol * b2) >>> 7) + (b1 << 16), 0, 0x7F0000);
          ramp = { from: vol, to: target, total: Math.max(256, t), left: Math.max(256, t) };
          break;
        }
        case 0x10: {
          const sd = ctx.sdir.get((w0 >>> 8) & 0xFFFF);
          if (sd) {
            let off = w1 < sd.num ? w1 : 0;
            if (b0) off = Math.min(sd.num - 1, Math.floor(((127 - (vol >>> 16)) * w1) / 127));
            smp = { sd, pcm: ctx.pcm(sd), pos: off, playing: true };
            envStage = adsr ? 'attack' : 'sustain'; env = adsr ? 0 : 1; envT = 0;
            pitchLog.push([at / MIX_RATE + tNow, note + detune / 100]);
          }
          break;
        }
        case 0x11: if (smp) { envStage = 'release'; envFrom = env; envT = 0; } break;
        // Sets ch+0x24 bit 0x80; tickAllAudioChannels then cuts the voice (musyxStopVoice, no release) unless the sustain pedal holds it.
        case 0x12: stopReq = true; break;
        case 0x18: case 0x19: case 0x1A: {
          const base = op === 0x19 ? 0 : op === 0x18 && !b0 ? note : o.note;
          note = clamp((base + b2) & 0xFF, 0, 127);
          detune = s8(b1);
          pitchLog.push([at / MIX_RATE + tNow, note + detune / 100]);
          startWait(0, w1);
          break;
        }
        case 0x17: {
          // soundMacroPlayRandomKeyNote 0x8008B0A8: random key in [b0, b2] (or [note - b2, note + b0] when w1 byte 2 is set), detune b1 (or random -100..100
          // when w1 byte 3 is set), then the command rewrites itself in the macro as op 0x19 with that key, so the first pick sticks.
          const picks = (ctx.randomKeys ??= new Map()), k = pc[0] + ':' + p;
          if (!picks.has(k)) {
            const [lo, hi] = (w1 >>> 8) & 0xFF ? [clamp(note - b2, 0, 127), clamp(note + b0, 0, 127)] : b0 < b2 ? [b0, b2] : [b2, b0];
            // shortcut: the game's RNG state at the first play is unknowable, so the pick is seeded per command (same in every render).
            let sd = (pc[0] << 12) + p + 1;
            const r16 = () => (sd = (Math.imul(sd, 1103515245) + 12345) >>> 0) >>> 16, det = w1 & 0xFF ? (r16() % 201) - 100 : s8(b1);
            picks.set(k, { note: lo + (r16() % (hi - lo + 1)), detune: det });
          }
          ({ note, detune } = picks.get(k));
          pitchLog.push([at / MIX_RATE + tNow, note + detune / 100]);
          break;
        }
        case 0x1C: {
          const t = (w1 >>> 8) & 1 ? (w1 >>> 16) * 256 : ticksToUnits(w1 >>> 16);
          if (t) vib = { period: t, depth: s8(b2) < 0 ? -s8(b2) : b2, fine: b1, phase: s8(b2) < 0 ? t >> 1 : 0, wheel: !!(b0 & 3) };
          else vib = null;
          break;
        }
        case 0x33: bendRange = [b2, b1]; break;
        case 0x40: case 0x42: {
          const key = op === 0x40 ? 'vol' : 'bend';
          const remap = { 0x81: 0x82, 0x82: 0xA0, 0x83: 0xA1, 0x84: 0x83, 0x85: 0x84 };
          const e = { src: remap[b2] ?? b2, op: w1 >>> 24, scale: Math.trunc(((((w0 >>> 16) << 16) >> 16) * 256) / 100) };
          if (!pushed.has(key) || e.op === 0) { recs[key] = []; pushed.add(key); }
          if (recs[key].length < 4) recs[key].push(e);
          break;
        }
        case 0x50: {
          const n = b2 & 1;
          if (lfo[n].period) lfo[n].phase = 0;
          lfo[n].period = (w0 >>> 16) * 256;
          break;
        }
        case 0x30: case 0x31: case 0x38: case 0x36: case 0x37: break;
        default: unresolved.add('macro op 0x' + op.toString(16));
      }
    }
  };
  let tNow = 0;
  const maxT = o.maxT ?? 60, total = Math.min(out.L.length - at, Math.ceil(maxT * MIX_RATE));
  let prev = null;
  for (let s = 0; s < total; s += TICK_SAMPLES) {
    tNow = s / MIX_RATE;
    if (!keyedOff && tNow >= o.keyOff) {
      keyedOff = true;
      if (waiting?.keyOff) waiting = null;
    }
    if (waiting) {
      if ((waiting.sampleEnd && !(smp && smp.playing))) waiting = null;
      else if (waiting.units !== Infinity && (waiting.units -= TICK_DT) <= 0) waiting = null;
    }
    if (!stopped) exec();
    // A voice ends when its macro stops (soundMacroOpStopVoice frees it), except to finish a release; or when it waits forever on silence.
    if (stopped && !(envStage === 'release' && smp?.playing)) break;
    if (waiting?.units === Infinity && !(smp && smp.playing) && !(waiting.keyOff && o.keyOff !== Infinity)) break;
    // Modulators (tickAllAudioChannels).
    if (ramp) {
      ramp.left -= TICK_DT;
      vol = ramp.left <= 0 ? ramp.to : Math.round(ramp.to - ((ramp.to - ramp.from) * ramp.left) / ramp.total);
      if (ramp.left <= 0) ramp = null;
    }
    if (panSweep) {
      panSweep.left -= TICK_DT;
      pan = clamp(panSweep.left <= 0 ? panSweep.to : Math.round(panSweep.to - ((panSweep.to - panSweep.from) * panSweep.left) / panSweep.total), 0, 0x7F0000);
      if (panSweep.left <= 0) panSweep = null;
    }
    for (let n = 0; n < 2; n++) {
      if (!lfo[n].period) continue;
      lfo[n].phase += TICK_DT;
      lfoOut[n] = sine(Math.floor(((lfo[n].phase % lfo[n].period) * 16) / (lfo[n].period >> 8)) & 4095);
    }
    const c = o.ctl ? o.ctl(tNow + at / MIX_RATE) : null;
    const cc = c?.cc ?? DEFAULT_CC, bendCtl = c?.bend ?? 0x2000;
    if (stopReq && !(o.song && cc[0x40] >= 0x40)) { stopReq = false; if (smp) smp.playing = false; }
    // Pitch: note rate x detune, vibrato, pitch bend record.
    let rate = tableRate(note);
    if (detune > 0) rate += (rate * (0x10D390 / 0x100000) - rate) * detune / 100;
    else if (detune < 0) rate += (rate - rate * (0xF1A1C / 0x100000)) * detune / 100;
    if (vib) {
      vib.phase += TICK_DT;
      const sv = sine(Math.floor(((vib.phase % vib.period) * 16) / (vib.period >> 8)) & 4095);
      let p = rate * (sv >= 0 ? T.semiUp[vib.depth] : T.semiDown[vib.depth]);
      if (vib.fine) p += (p * (sv >= 0 ? 0x10D390 : 0xF1A1C) / 0x100000 - p) * vib.fine / 100;
      const a0 = vib.wheel && o.song ? ctlVal(1, cc, bendCtl) >> 7 : 0x40;
      rate += ((sv * (p - rate)) / 4096) * (a0 / 64);
    }
    const b = evalRec(recs.bend, cc, bendCtl) - 0x2000;
    if (b) rate *= fineRatio(((b < 0 ? -1 : 1) * Math.abs(b) * bendRange[b < 0 ? 0 : 1]) / 128);
    // Volume chain: faders (0x7F, runtime defaults) and CC7 when on a sequencer channel; pan with CC10; aux = CC91 x volume.
    let V = vol;
    for (let k = 0; k < 4; k++) V = (V >>> 7) * 0x7F;
    if (o.song) V = (V >>> 7) * (evalRec(recs.vol, cc, bendCtl) >>> 7);
    let P = pan;
    if (o.song && P !== 0x800000) P = clamp(P + (((ctlVal(0xA, cc, bendCtl) >> 7) - 0x40) << 16), 0, 0x7F0000);
    const aux = o.song ? Math.min(0x7F0000, (((cc[0x5B] << 7) >> 7) * V) >>> 7) : 0;
    const g = voiceGains(T, V, P, aux);
    // ADSR (advanceVolumeFadeEnvelope): linear stages in ms.
    const ms = (TICK_SAMPLES / MIX_RATE) * 1000;
    if (adsr && envStage !== 'sustain') {
      envT += ms;
      const [A, D, S, Rl] = adsr, sus = S / 4096;
      if (envStage === 'attack') { env = A ? Math.min(1, envT / A) : 1; if (env >= 1) { envStage = 'decay'; envT = 0; } }
      else if (envStage === 'decay') { env = D ? 1 + (sus - 1) * Math.min(1, envT / D) : sus; if (envT >= D) { envStage = 'hold'; env = sus; } }
      else if (envStage === 'release') { const r = Rl || 20; env = Math.max(0, envFrom * (1 - envT / r)); if (env <= 0 && smp) smp.playing = false; }
    } else if (!adsr && envStage === 'release') {
      envT += ms;
      env = Math.max(0, envFrom * (1 - envT / 20));
      if (env <= 0 && smp) smp.playing = false;
    }
    // Render this tick's samples (gains ramp across the first 192-sample block, as the RSP does).
    if (smp && smp.playing) {
      const step = rate / MIX_RATE, sd = smp.sd, pcm = smp.pcm, loopLen = sd.loopLen, loopStart = sd.loopStart;
      const tgt = { L: g.L * env, R: g.R * env, aux: g.aux * env }, from = prev ?? tgt;
      const loopEnd = loopLen ? Math.min(pcm.length, loopStart + loopLen) : pcm.length, endAt = loopEnd - (loopLen ? 0 : 1);
      const fL = from.L, dL = tgt.L - fL, fR = from.R, dR = tgt.R - fR, fA = from.aux, dA = tgt.aux - fA;
      const oL = out.L, oR = out.R, oA = out.aux, base = at + s, n = Math.min(TICK_SAMPLES, total - s);
      let pos = smp.pos;
      // Most voice ticks are fully silent (volume 0 after key-off): only advance the sample position, the same steps as below.
      const silent = !fL && !dL && !fR && !dR && !fA && !dA;
      if (silent) {
        for (let i = 0; i < n; i++) {
          if (pos >= endAt) {
            if (!loopLen) { smp.playing = false; break; }
            pos = loopStart + ((pos - loopStart) % loopLen);
          }
          pos += step;
        }
      } else for (let i = 0; i < n; i++) {
        if (pos >= endAt) {
          if (!loopLen) { smp.playing = false; break; }
          pos = loopStart + ((pos - loopStart) % loopLen);
        }
        const k = Math.floor(pos), a = pcm[k], next = k + 1 < loopEnd ? pcm[k + 1] : loopLen ? pcm[loopStart] : a;
        const x = (a + (next - a) * (pos - k)) / 32768, f = i < 192 ? i / 192 : 1;
        oL[base + i] += x * (fL + dL * f);
        oR[base + i] += x * (fR + dR * f);
        if (oA) oA[base + i] += x * (fA + dA * f);
        pos += step;
      }
      smp.pos = pos;
      prev = tgt;
    } else if (stopped) break;
  }
  return { pitch: pitchLog, end: tNow, unresolved };
}
// Menu reverb presets (syncMusyXIfActive callers: cueMenuScreenAudioA/B/C, runModelViewerSoundDemo), by song id. Reverb is off elsewhere.
const SONG_REVERB = (() => {
  const ab = { name: 'menu A/B', lenMs: 160, delayMs: 155, tapGain: 0x7800, firGain: 0x4100, fir: [0x051E, 0x0F5C, 0x1999, 0x0800] };
  const c = { ...ab, name: 'menu C / model viewer', delayMs: 160, firGain: 0x3200 };
  return new Map([[1, ab], [0x3B, ab], [0x5E, c], [2, c]]);
})();

// MusyX v1 effect stage per 192-sample block (sfx_stage; block built by buildSongTempoSchedule): taps from the circular buffer -> L/R,
// 4-tap FIR of the tap mix + the voices' aux bus written back into the buffer.
function applyMusyxReverb(L, R, aux, p) {
  const B = 192, sz = Math.floor((MIX_RATE * p.lenMs) / 1000 / B) * B + B, delay = ((Math.floor((MIX_RATE * p.delayMs) / 1000) + 3) & ~3);
  const buf = new Int16Array(sz), h = p.fir.map((c) => (c * p.firGain) >> 15), c16 = (v) => Math.max(-32768, Math.min(32767, v));
  let pos = 0, last4 = [0, 0, 0, 0];
  for (let s = 0; s + B <= L.length; s += B, pos = (pos + B) % sz) {
    const sub = new Int32Array(B);
    let d = pos - delay;
    if (d <= 0) d += sz;
    for (let j = 0; j < B; j++) sub[j] = c16(sub[j] + ((buf[(d + j) % sz] * p.tapGain + 0x4000) >> 15));
    const x = [...last4, ...sub];
    last4 = [...sub.slice(B - 4)];
    for (let j = 0; j < B; j++) {
      L[s + j] += sub[j];
      R[s + j] += sub[j];
      const e = c16(Math.round(aux[s + j]) + ((h[0] * x[j + 1] + h[1] * x[j + 2] + h[2] * x[j + 3] + h[3] * x[j + 4]) >> 15));
      buf[(pos + j) % sz] = e;
    }
  }
}

// One pass of a song (until its first track loop), stereo, with per-note timeline data.
// ctx = { pool, sdir, samp, tables, groups: parseSongGroups() }; opts = { skipTracks: Set(track index), maxSeconds, dry: keep the reverb send (aux) and skip the reverb }.
function* renderSongSteps(ctx, song, songId, opts = {}) {
  const g = ctx.groups.get(songId), unresolved = new Set(), notes = [], ctl = [];
  if (!g) unresolved.add('song not in the song table: no instrument group');
  // Tempo: R = bpm * 0x600 / 240 ticks per second (setTrackVoiceTickBudget).
  // updateSongTempoTickRate applies every tempo entry with tick <= the song position, so a tick-0 entry overrides the header BPM (73 songs have one).
  const segs = [{ tick: 0, bpm: song.bpm }];
  for (const x of [...song.tempo].sort((a, b) => a.tick - b.tick)) {
    if (x.tick === segs[segs.length - 1].tick) segs[segs.length - 1] = { tick: x.tick, bpm: x.bpm };
    else segs.push({ tick: x.tick, bpm: x.bpm });
  }
  let acc = 0;
  for (let i = 0; i < segs.length; i++) {
    segs[i].R = Math.floor((segs[i].bpm * 0x600) / 240);
    segs[i].sec = acc;
    if (i + 1 < segs.length) acc += (segs[i + 1].tick - segs[i].tick) / segs[i].R;
  }
  const seg = (tick) => { let i = segs.length - 1; while (i > 0 && segs[i].tick > tick) i--; return segs[i]; };
  const sec = (tick) => { const s = seg(tick); return s.sec + (tick - s.tick) / s.R; };
  // Channel events from every track, in tick order: program, controllers, pitch bend / modulation lanes.
  const chEv = Array.from({ length: 16 }, () => []), noteEv = [];
  let loopTick = Infinity, endTick = 0;
  for (const t of song.tracks) {
    for (const r of t.regions) {
      if (r.region == null) { loopTick = Math.min(loopTick, r.start); break; }
      const ev = chEv[t.channel], reg = song.regions.get(r.region);
      if (r.program !== 0xFF) ev.push({ tick: r.start, kind: 'prog', v: r.program });
      if (r.vol !== 0xFF) ev.push({ tick: r.start, kind: 'cc', n: 7, v: r.vol });
      ev.push({ tick: r.start, kind: 'bend', v: 0x2000 }, { tick: r.start, kind: 'cc', n: 1, v: 0 });
      let lt = 0, v = 0x2000;
      for (const x of reg.bend) { lt += x.dt; v += x.dv; ev.push({ tick: r.start + lt, kind: 'bend', v }); }
      lt = 0; v = 0;
      for (const x of reg.mod) { lt += x.dt; v += x.dv; ev.push({ tick: r.start + lt, kind: 'cc', n: 1, v: Math.max(0, Math.min(127, v >> 7)) }); }
      for (const e of reg.events) {
        const tick = r.start + e.tick;
        if (e.a < 0x80) {
          noteEv.push({ track: t.index, ch: t.channel, tick, end: tick + e.dur, note: Math.max(0, Math.min(127, e.a + r.transpose)), vel: Math.max(0, Math.min(127, e.b + r.velAdd)) });
          endTick = Math.max(endTick, tick + e.dur);
        } else if (e.b === 0) ev.push({ tick, kind: 'prog', v: e.a & 0x7F });
        else if (e.b >= 0x80) ev.push({ tick, kind: 'cc', n: e.b & 0x7F, v: e.a & 0x7F });
        else if (e.b === 1) ev.push({ tick, kind: 'cc', n: 0x82, v: e.a & 0x7F });
      }
    }
  }
  const lastTick = Math.min(loopTick, endTick);
  const length = Math.min(opts.maxSeconds ?? 600, sec(lastTick) + 3);
  // Channel state snapshots: setup (MIDISetup) then the sorted events; ctl(ch)(t) returns the state in force at time t.
  const snaps = chEv.map((ev, ch) => {
    const s = g?.setup[ch], cc = DEFAULT_CC.slice();
    if (s) [cc[7], cc[0xA], cc[0x5B], cc[0x5D]] = [s.vol, s.pan, s.reverb, s.chorus];
    const out = [{ t: 0, cc: cc.slice(), bend: 0x2000, program: s?.program ?? 0 }];
    for (const e of ev.sort((a, b) => a.tick - b.tick)) {
      const p = out[out.length - 1], n = { t: sec(e.tick), cc: p.cc.slice(), bend: p.bend, program: p.program };
      if (e.kind === 'prog') n.program = e.v;
      else if (e.kind === 'bend') n.bend = e.v;
      else { n.cc[e.n] = e.v; ctl.push({ channel: ch, t: n.t, cc: e.n, value: e.v }); }
      out.push(n);
    }
    return out;
  });
  const stateAt = (ch, t) => {
    const a = snaps[ch];
    let lo = 0, hi = a.length - 1;
    while (lo < hi) { const m = (lo + hi + 1) >> 1; if (a[m].t <= t) lo = m; else hi = m - 1; }
    return a[lo];
  };
  const n = Math.ceil(length * MIX_RATE), out = { L: new Float32Array(n), R: new Float32Array(n), aux: new Float32Array(n) };
  const ordered = noteEv.sort((a, b) => a.tick - b.tick);
  for (let ni = 0; ni < ordered.length; ni++) {
    const e = ordered[ni];
    if (ni % 4 === 0) yield ni / ordered.length;
    const t = sec(e.tick);
    if (t >= length) continue;
    const st = stateAt(e.ch, t), entry = g && (e.ch === 9 ? g.drums : g.progs).get(st.program);
    const voices = entry ? resolveNote(ctx.pool, entry.target, e.note, e.vel, 0x40, entry.x2) : [];
    const rec = { track: e.track, channel: e.ch, t, dur: sec(e.end) - t, note: e.note, vel: e.vel, program: st.program, target: entry?.target ?? null,
      voices: voices.map((v) => ({ macro: v.macro, note: v.note, sample: firstSample(ctx.pool, v.macro) })), silent: !voices.length };
    notes.push(rec);
    if (opts.skipTracks?.has(e.track)) continue;
    for (const [vi, v] of voices.entries()) {
      const r = renderVoice(ctx, { seed: ni * 8 + vi + 1, macro: v.macro, note: v.note, vel: v.vel, pan: v.pan, keyOff: rec.dur, tickRate: seg(e.tick).R, song: true,
        ctl: (tt) => stateAt(e.ch, tt), maxT: Math.min(length - t, rec.dur + 10) }, out, Math.floor(t * MIX_RATE));
      for (const u of r.unresolved) unresolved.add(u);
    }
  }
  const reverb = SONG_REVERB.get(songId) ?? null;
  if (reverb && !opts.dry) applyMusyxReverb(out.L, out.R, out.aux, reverb);
  return { rate: MIX_RATE, L: out.L, R: out.R, aux: opts.dry ? out.aux : null, length, loopAt: loopTick === Infinity ? null : sec(loopTick), loopFrom: sec(song.loopTick), notes, ctl, reverb,
    bpm: segs.map((s) => s.bpm), group: g?.group ?? null, unresolved };
}
function renderSong(ctx, song, songId, opts) {
  const it = renderSongSteps(ctx, song, songId, opts);
  for (;;) {
    const r = it.next();
    if (r.done) return r.value;
  }
}
// First sample a macro starts (op 0x10), for colouring timeline notes.
function firstSample(pool, macro) {
  const c = pool[0].get(macro);
  for (let o = 0; c && o + 8 <= c.length; o += 8) {
    const op = c[o + 3] & 0x7F;
    if (op === 0x10) return (c[o + 1] << 8) | c[o + 2];
    if (op === 0) break;
  }
  return null;
}

const DEFAULT_CC = (() => {
  const cc = new Uint8Array(0x86);
  cc[7] = 0x7F;
  cc[0xA] = 0x40;
  cc[0x80] = 0x40;
  cc[0x84] = 0x40;
  return cc;
})();

// Offline render of one SFX id through its SoundMacro on the shared voice engine, mono ((L + R) / 2).
// shortcut: a wait that only key-off would end (the game stops such SFX from code) is held for 2 s.
// ctx = { pool, sdir: Map(id -> sdir entry), samp, tables: parseMixTables(rom) }.
function renderSfx(ctx, sfx) {
  const len = MIX_RATE * 10, out = { L: new Float32Array(len), R: new Float32Array(len) };
  const voices = [];
  const run = (macro, note) => voices.push(renderVoice(ctx, { macro, note, vel: sfx.vel, pan: sfx.pan, keyOff: 2, tickRate: 768, maxT: 10 }, out));
  if (sfx.macro & 0x8000) for (const v of resolveNote(ctx.pool, sfx.macro, sfx.key, sfx.vel, sfx.pan, 0)) run(v.macro, v.note);
  else run(sfx.macro, sfx.key);
  const end = Math.min(len, Math.ceil(Math.max(0, ...voices.map((v) => v.end)) * MIX_RATE) + 2048);
  const pcm = new Int16Array(end);
  for (let i = 0; i < end; i++) pcm[i] = Math.max(-32768, Math.min(32767, Math.round((out.L[i] + out.R[i]) / 2)));
  return { pcm, rate: MIX_RATE, voices: voices.length, pitch: voices.map((v) => v.pitch), unresolved: new Set(voices.flatMap((v) => [...v.unresolved])) };
}

// 16-bit WAV; stereo takes interleaved L/R samples.
function wavBytes(pcm, rate, channels = 1) {
  const w = new Uint8Array(44 + pcm.length * 2), dv = new DataView(w.buffer);
  const str = (o, s) => [...s].forEach((c, i) => (w[o + i] = c.charCodeAt(0)));
  str(0, 'RIFF'); dv.setUint32(4, 36 + pcm.length * 2, true); str(8, 'WAVEfmt ');
  dv.setUint32(16, 16, true); dv.setUint16(20, 1, true); dv.setUint16(22, channels, true);
  dv.setUint32(24, rate, true); dv.setUint32(28, rate * 2 * channels, true); dv.setUint16(32, 2 * channels, true); dv.setUint16(34, 16, true);
  str(36, 'data'); dv.setUint32(40, pcm.length * 2, true);
  for (let i = 0; i < pcm.length; i++) dv.setInt16(44 + i * 2, pcm[i], true);
  return w;
}

// ---- Cutscenes: data/cuts/idN_{intro,extro,special} (VRML exports; read by the cinematic overlay) ----
// Header: +0x44 frame count, +0x4A actor count, +0x4E event count, +0x50 length (s), +0x54 frames per second.
// Events at +0x58 (0x18): type, frame, then params (type 0 = voice id). Camera: the first non-zero of 6 entries of 8 channel starts
// at +0x1318. Actors at +0x13D8 (0x4C): name[28], flags byte at +0x1F (0x80 model, 0x40 animated, low 6 bits parent actor, 0x3F root),
// 11 words at +0x20: channel starts when animated, else the static values as floats. Keys at +0x25AC: (value, slope, time 0..1).
// Camera channels: position xyz, rotation axis xyz, angle (deg), field of view (deg). Actor channels: position xyz, scale input,
// axis xyz, angle (deg), pivot xyz. Positions and pivots are 4x world units (x 0.25).
function parseCutscene(dv) {
  const u16 = (o) => dv.getUint16(o), u32 = (o) => dv.getUint32(o), f = (o) => dv.getFloat32(o);
  const nKeys = u32(0x25a8), starts = new Set();
  let camera = null;
  const cams = [];
  for (let i = 0; i < 6; i++) {
    const w = [...Array(8)].map((_, k) => u32(0x1318 + i * 0x20 + k * 4));
    if (!w.some((x) => x)) continue;
    camera ??= w;
    cams[i] = w;
    w.forEach((s) => starts.add(s));
  }
  const actors = [];
  for (let i = 0; i < u16(0x4a); i++) {
    const o = 0x13d8 + i * 0x4c, flags = dv.getUint8(o + 0x1f), animated = !!(flags & 0x40);
    const w = [...Array(11)].map((_, k) => (animated ? u32(o + 0x20 + k * 4) : f(o + 0x20 + k * 4)));
    if (animated) w.forEach((s) => starts.add(s));
    actors.push({ name: cstr(dv, o, 28), flags, model: !!(flags & 0x80), animated, parent: (flags & 0x3f) === 0x3f ? -1 : flags & 0x3f, w });
  }
  const sorted = [...starts].sort((a, b) => a - b);
  // Each channel's original start and keys, so writeCutscene keeps unedited channels (and shared runs) where they were.
  const keyStarts = new Map();
  // A channel runs to the next start, and never past its end key (time > 1, the cutscene length).
  const channel = (s) => {
    let e = Math.min(sorted.find((x) => x > s) ?? nKeys, nKeys);
    for (let i = s + 1; i < e; i++) if (f(0x25ac + i * 12 + 8) > 1) { e = i + 1; break; }
    const keys = new Float32Array(Math.max(0, e - s) * 3);
    for (let i = 0; i < keys.length; i++) keys[i] = f(0x25ac + s * 12 + i * 4);
    keyStarts.set(keys, { start: s, keys: keys.slice() });
    return keys;
  };
  const events = [];
  for (let i = 0; i < u16(0x4e); i++) {
    const o = 0x58 + i * 0x18;
    events.push({ type: u32(o), frame: u32(o + 4), p: [u32(o + 8), u32(o + 12), u32(o + 16), u32(o + 20)] });
  }
  // All six camera entries (unused ones empty); event type 4 switches between them. camera is the first used entry (the same arrays).
  const cameras = [...Array(6)].map((_, i) => cams[i] && cams[i].map(channel));
  return {
    name: cstr(dv, 0, 64), frames: u32(0x44), length: f(0x50), fps: f(0x54), events,
    camera: cameras.find(Boolean) ?? null, cameras,
    actors: actors.map((a) => ({ ...a, ch: a.animated ? a.w.map(channel) : null, value: a.animated ? null : a.w })),
    raw: new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength).slice(), keyStarts,
  };
}
// The inverse of parseCutscene, byte-exact when nothing changed. Any channel change rebuilds the key block gap-free (a channel runs to
// the next start, so a stale run would join the channel before it): unchanged channels in original order, shared runs kept, then the rest.
function writeCutscene(data) {
  const raw = data.raw, oldKeys = new DataView(raw.buffer, raw.byteOffset, raw.byteLength).getUint32(0x25a8);
  const chans = [...data.cameras.flatMap((c) => c ?? []), ...data.actors.flatMap((a) => (a.animated ? a.ch : []))];
  const kept = (keys) => {
    const was = data.keyStarts?.get(keys);
    return was && was.keys.length === keys.length && was.keys.every((v, i) => Object.is(v, keys[i])) ? was.start : null;
  };
  const same = chans.every((k) => kept(k) != null);
  const newStart = new Map(), runs = [];
  let nKeys = same ? oldKeys : 0;
  if (!same) {
    const byOld = new Map();
    for (const k of chans.filter((x) => kept(x) != null).sort((a, b) => kept(a) - kept(b))) {
      if (!byOld.has(kept(k))) {
        byOld.set(kept(k), nKeys);
        runs.push(k);
        nKeys += k.length / 3;
      }
      newStart.set(k, byOld.get(kept(k)));
    }
    for (const k of chans) if (!newStart.has(k)) {
      newStart.set(k, nKeys);
      runs.push(k);
      nKeys += k.length / 3;
    }
    // The game skips a camera entry whose first word (channel 0's start) is 0, so key 0 then holds an unused pad key.
    if (data.cameras.some((c) => c && newStart.get(c[0]) === 0)) {
      for (const [k, s] of newStart) newStart.set(k, s + 1);
      runs.unshift(new Float32Array(3));
      nKeys++;
    }
  }
  const startOf = (keys) => (same ? kept(keys) : newStart.get(keys));
  const camStarts = data.cameras.map((c) => c && c.map(startOf));
  const actorWords = data.actors.map((a) => (a.animated ? a.ch.map(startOf) : null));
  const out = new Uint8Array(0x25ac + nKeys * 12), dv = new DataView(out.buffer);
  out.set(raw.subarray(0, Math.min(raw.length, out.length)));
  const oldEvents = dv.getUint16(0x4e), oldActors = dv.getUint16(0x4a);
  dv.setUint32(0x44, data.frames);
  // The game sizes its node pools from these: cameras in use, actors, and non-model (group) actors.
  dv.setUint16(0x48, data.cameras.filter(Boolean).length);
  dv.setUint16(0x4a, data.actors.length);
  dv.setUint16(0x4c, data.actors.filter((a) => !a.model).length);
  dv.setUint16(0x4e, data.events.length);
  dv.setFloat32(0x50, data.length);
  dv.setFloat32(0x54, data.fps);
  out.fill(0, 0x58 + data.events.length * 0x18, 0x58 + Math.max(oldEvents, data.events.length) * 0x18);
  data.events.forEach((e, i) => [e.type, e.frame, ...e.p].forEach((v, k) => dv.setUint32(0x58 + i * 0x18 + k * 4, v >>> 0)));
  for (let i = 0; i < 6; i++) for (let k = 0; k < 8; k++) dv.setUint32(0x1318 + i * 0x20 + k * 4, camStarts[i]?.[k] ?? 0);
  out.fill(0, 0x13d8 + data.actors.length * 0x4c, 0x13d8 + Math.max(oldActors, data.actors.length) * 0x4c);
  data.actors.forEach((a, i) => {
    const o = 0x13d8 + i * 0x4c;
    // The name block keeps its original tail bytes unless the name changed (or the actor is new).
    if (i >= oldActors || cstr(new DataView(raw.buffer, raw.byteOffset, raw.byteLength), o, 28) !== a.name) {
      out.fill(0, o, o + 0x1f);
      for (let k = 0; k < Math.min(27, a.name.length); k++) out[o + k] = a.name.charCodeAt(k);
    }
    out[o + 0x1f] = (a.model ? 0x80 : 0) | (a.animated ? 0x40 : 0) | (a.parent < 0 ? 0x3f : a.parent);
    for (let k = 0; k < 11; k++) {
      if (a.animated) dv.setUint32(o + 0x20 + k * 4, actorWords[i][k]);
      else dv.setFloat32(o + 0x20 + k * 4, a.value[k]);
    }
  });
  dv.setUint32(0x25a8, nKeys);
  let at = 0x25ac;
  for (const keys of runs) for (const v of keys) dv.setFloat32((at += 4) - 4, v);
  return out;
}

// ---- Edit history: every asset change as before / after bytes; begin/end group a drag into one step ----
function createHistory() {
  let open = null;
  const h = {
    steps: [], at: 0,
    begin(label) {
      open ??= { label, changes: [] };
    },
    end() {
      const s = open;
      open = null;
      if (s?.changes.length) h.push(s);
    },
    commit(path, before, after, label = path) {
      if (open) open.changes.push({ path, before, after });
      else h.push({ label, changes: [{ path, before, after }] });
    },
    push(step) {
      h.steps.length = h.at;
      h.steps.push(step);
      h.at = h.steps.length;
    },
    undo() {
      if (!h.at) return [];
      return h.steps[--h.at].changes.map((c) => ({ path: c.path, bytes: c.before })).reverse();
    },
    redo() {
      if (h.at >= h.steps.length) return [];
      return h.steps[h.at++].changes.map((c) => ({ path: c.path, bytes: c.after }));
    },
  };
  return h;
}

// ---- Cutscene edits (in place on parseCutscene data; a limit throws before anything changes) ----
// The fixed layout allows 200 events (0x58..0x1318), 6 camera entries and 60 actors (0x13D8..0x25A8).
const CUT_LIMITS = { events: 200, cameras: 6, actors: 60 };
// Event types whose p0 (type 11: its low 16 bits) is an actor index.
const CUT_ACTOR_EVENTS = new Set([5, 6, 7, 8, 11, 13, 16, 18]);
const cutActorRef = (e) => (e.type === 11 ? e.p[0] & 0xffff : e.p[0]);
const cutChannel = (data, ref) => (ref.cam != null ? data.cameras[ref.cam] : data.actors[ref.actor].ch);
// Slope of key j: the line to the next key (the game's evaluation is value + slope * (t - time) from the last key); 0 on the last key.
function cutRecomputeSlopes(keys, from = 0, to = keys.length / 3 - 1) {
  const n = keys.length / 3;
  for (let j = Math.max(0, from); j <= Math.min(to, n - 1); j++) {
    // The last real key holds to the end, and the sentinel after it takes its value.
    if (j < n - 1 && keys[(j + 1) * 3 + 2] > 1 && keys[j * 3 + 2] <= 1) {
      keys[j * 3 + 1] = 0;
      keys[(j + 1) * 3] = keys[j * 3];
    } else {
      const dt = j < n - 1 ? keys[(j + 1) * 3 + 2] - keys[j * 3 + 2] : 0;
      keys[j * 3 + 1] = dt > 0 ? (keys[(j + 1) * 3] - keys[j * 3]) / dt : 0;
    }
  }
  return keys;
}
function cutAddEvent(data, ev) {
  if (data.events.length >= CUT_LIMITS.events) throw new Error(`Cutscene limit: ${CUT_LIMITS.events} events`);
  let i = data.events.length;
  while (i > 0 && data.events[i - 1].frame > ev.frame) i--;
  data.events.splice(i, 0, ev);
  return i;
}
function cutRemoveEvent(data, i) {
  data.events.splice(i, 1);
}
function cutMoveEvent(data, i, frame) {
  const [ev] = data.events.splice(i, 1);
  // An event already past the end (it fires once gameplay starts) may stay past it.
  ev.frame = Math.max(0, Math.min(ev.frame > data.frames ? Infinity : data.frames, Math.round(frame)));
  let j = data.events.length;
  while (j > 0 && data.events[j - 1].frame > ev.frame) j--;
  data.events.splice(j, 0, ev);
  return j;
}
// Sets (within half a frame) or inserts a key at normalised time t; the slopes into and out of it follow.
function cutSetKey(data, ref, t, value) {
  const chs = cutChannel(data, ref), keys = chs[ref.ch], n = keys.length / 3, half = 0.5 / data.frames;
  let j = [...Array(n).keys()].find((k) => Math.abs(keys[k * 3 + 2] - t) < half);
  let out = keys;
  if (j == null) {
    j = 0;
    while (j < n && keys[j * 3 + 2] < t) j++;
    out = new Float32Array((n + 1) * 3);
    out.set(keys.subarray(0, j * 3));
    out.set(keys.subarray(j * 3), (j + 1) * 3);
    out.set([value, 0, t], j * 3);
  } else {
    out = keys.slice();
    out[j * 3] = value;
  }
  chs[ref.ch] = cutRecomputeSlopes(out, j - 1, j);
  return j;
}
function cutRemoveKey(data, ref, j) {
  if (j === 0) throw new Error('The first key (time 0) cannot be removed');
  const chs = cutChannel(data, ref), keys = chs[ref.ch];
  if (j >= keys.length / 3 - 1) throw new Error('The last key (the channel\'s end marker) cannot be removed');
  const out = new Float32Array(keys.length - 3);
  out.set(keys.subarray(0, j * 3));
  out.set(keys.subarray((j + 1) * 3), j * 3);
  chs[ref.ch] = cutRecomputeSlopes(out, j - 1, j - 1);
}
// Every channel ends in a sentinel key past t = 1 at the cutscene length (seconds) as stock files do: the game samples forward while
// key time <= t, so without it a channel runs on into the next channel's keys.
const cutEnd = (data) => (data.length > 1 ? data.length : 2);
const cutConst = (v, end) => new Float32Array([v, 0, 0, v, 0, end]);
// A new animated actor at the origin: scale input 1 / 0.2222 (model scale 1) unless given, turned 0 deg about +Y, no pivot.
function cutAddActor(data, name, parent = -1, scale = 1 / 0.2222) {
  if (data.actors.length >= CUT_LIMITS.actors) throw new Error(`Cutscene limit: ${CUT_LIMITS.actors} actors`);
  if (name.length > 27) throw new Error('Actor names are at most 27 characters');
  const init = [0, 0, 0, scale, 0, 1, 0, 0, 0, 0, 0];
  data.actors.push({ name, flags: 0, model: true, animated: true, parent, w: null, ch: init.map((v) => cutConst(v, cutEnd(data))), value: null });
  return data.actors.length - 1;
}
// A static actor (fixed values, no keys) becomes animated with constant channels, so it can be keyed.
function cutAnimateActor(data, i) {
  const a = data.actors[i];
  if (a.animated) return;
  Object.assign(a, { animated: true, ch: a.value.map((v) => cutConst(v, cutEnd(data))), value: null });
}
function cutSetActorName(data, i, name) {
  if (name.length > 27) throw new Error('Actor names are at most 27 characters');
  data.actors[i].name = name;
}
// Removes the actor, its children (recursively) and the events that name any of them; later indices shift down.
function cutRemoveActor(data, i) {
  const gone = new Set([i]);
  for (let grew = true; grew;) {
    grew = false;
    data.actors.forEach((a, k) => {
      if (!gone.has(k) && gone.has(a.parent)) grew = gone.add(k);
    });
  }
  const map = new Map();
  data.actors.forEach((_, k) => gone.has(k) || map.set(k, map.size));
  data.events = data.events.filter((e) => !(CUT_ACTOR_EVENTS.has(e.type) && gone.has(cutActorRef(e))));
  for (const e of data.events) {
    if (!CUT_ACTOR_EVENTS.has(e.type)) continue;
    const to = map.get(cutActorRef(e));
    e.p[0] = e.type === 11 ? ((e.p[0] & ~0xffff) | to) >>> 0 : to;
  }
  data.actors = data.actors.filter((_, k) => !gone.has(k));
  for (const a of data.actors) if (a.parent >= 0) a.parent = map.get(a.parent);
}
// A new camera entry holding the first camera's opening pose (or the origin looking down +Z at 50 deg).
function cutAddCamera(data) {
  const k = data.cameras.findIndex((c) => !c);
  if (k < 0 || k >= CUT_LIMITS.cameras) throw new Error(`Cutscene limit: ${CUT_LIMITS.cameras} cameras`);
  const from = data.cameras.find(Boolean), init = from ? from.map((keys) => keys[0]) : [0, 0, 0, 0, 1, 0, 0, 50];
  data.cameras[k] = init.map((v) => cutConst(v, cutEnd(data)));
  data.camera = data.cameras.find(Boolean);
  return k;
}
function cutRemoveCamera(data, k) {
  if (data.cameras.filter(Boolean).length <= 1) throw new Error('The last camera cannot be removed');
  data.cameras[k] = undefined;
  data.events = data.events.filter((e) => !(e.type === 4 && e.p[0] === k));
  data.camera = data.cameras.find(Boolean);
}
// Keeps absolute times: keys rescale, the sentinel moves to the new length, shortening ends channels at t = 1 and drops events in the
// cut-off time; events past the old end (they fire once gameplay starts) stay past the new end.
function cutSetLength(data, seconds) {
  if (Math.abs(seconds - data.length) < 1e-6) return;
  const r = data.length / seconds, frames = Math.max(1, Math.round(seconds * data.fps)), end = seconds > 1 ? seconds : 2;
  const fit = (keys) => {
    const n = keys.length / 3, out = [];
    for (let j = 0; j < n && keys[j * 3 + 2] <= 1; j++) {
      const t = keys[j * 3 + 2] * r;
      if (t > 1 && j > 0) {
        const [v, s, t0] = out.slice(-3);
        out.push(v + s * (1 - t0), 0, 1);
        break;
      }
      out.push(keys[j * 3], keys[j * 3 + 1] / r, t);
    }
    const last = out.length - 3;
    out.push(out[last] + out[last + 1] * (1 - out[last + 2]), 0, end);
    return new Float32Array(out);
  };
  for (const c of data.cameras) if (c) c.forEach((keys, i) => (c[i] = fit(keys)));
  for (const a of data.actors) if (a.ch) a.ch.forEach((keys, i) => (a.ch[i] = fit(keys)));
  // The ending transition (10) keeps its distance from the end; other events keep their frame.
  const old = data.frames, ending = new Set(data.events.filter((e) => e.type === 10 && e.frame <= old));
  data.events = data.events.filter((e) => ending.has(e) || e.frame <= frames || e.frame > old);
  for (const e of data.events) if (ending.has(e)) e.frame = Math.max(0, e.frame + frames - old);
  else if (e.frame > old) e.frame += frames - old;
  data.events.sort((x, y) => x.frame - y.frame);
  data.frames = frames;
  data.length = seconds;
}
// A blank cutscene to build up: no actors, the first camera held at its opening pose, and only the scene events (draw range, ending
// transition, render flags) kept, scaled to the new length so the ending transition still comes last.
const CUT_SCENE_EVENTS = new Set([10, 17, 19, 21, 22]);
function cutBlank(data, seconds) {
  const k = data.cameras.findIndex(Boolean), start = data.cameras[k].map((keys) => keys[0]);
  data.actors = [];
  data.cameras = data.cameras.map((c, i) => (i === k ? c : undefined));
  const frames = Math.max(1, Math.round(seconds * data.fps)), old = data.frames;
  data.events = data.events.filter((e) => CUT_SCENE_EVENTS.has(e.type) || e.frame > old);
  // The ending transition (10) is cutSetLength's: it keeps its distance from the end.
  for (const e of data.events) if (e.frame <= old && e.type !== 10) e.frame = Math.round((e.frame * frames) / old);
  cutSetLength(data, seconds);
  data.cameras[k] = start.map((v) => cutConst(v, cutEnd(data)));
  data.camera = data.cameras[k];
}
// ---- Motion authoring: snapshots baked into dense keys (the game plays plain keys) ----
const v3 = {
  add: (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]],
  sub: (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]],
  mul: (a, s) => [a[0] * s, a[1] * s, a[2] * s],
  dot: (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2],
  cross: (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]],
  len: (a) => Math.hypot(a[0], a[1], a[2]),
  norm: (a) => { const n = Math.hypot(a[0], a[1], a[2]); return n > 1e-12 ? [a[0] / n, a[1] / n, a[2] / n] : [0, 0, 0]; },
};
// Replaces the channel's keys with t0 <= t <= t1 (a quarter frame either way: key times are float32) by samples ([t, v], times in
// t0..t1); the end sentinel stays.
function cutSetChannelSpan(data, ref, t0, t1, samples) {
  const chs = cutChannel(data, ref), keys = chs[ref.ch], out = [], eps = 0.25 / data.frames;
  for (let j = 0; j < keys.length / 3; j++) {
    const t = keys[j * 3 + 2];
    if (t > 1 || t < t0 - eps || t > t1 + eps) out.push([keys[j * 3], t]);
  }
  for (const [t, v] of samples) out.push([v, t]);
  out.sort((a, b) => a[1] - b[1]);
  const k = new Float32Array(out.length * 3);
  out.forEach(([v, t], j) => k.set([v, 0, t], j * 3));
  chs[ref.ch] = cutRecomputeSlopes(k);
}
// Centripetal Catmull-Rom through snapshots [{ t, p }] (sorted by t), exact at each snapshot and held outside them.
function cutCurve(snaps, t) {
  if (!snaps.length) return [0, 0, 0];
  if (t <= snaps[0].t) return [...snaps[0].p];
  const last = snaps[snaps.length - 1];
  if (t >= last.t) return [...last.p];
  let i = 0;
  while (i + 1 < snaps.length - 1 && snaps[i + 1].t <= t) i++;
  const a = snaps[i], b = snaps[i + 1];
  if (b.t - a.t < 1e-9) return [...b.p];
  const p0 = (snaps[i - 1] ?? a).p, p1 = a.p, p2 = b.p, p3 = (snaps[i + 2] ?? b).p;
  const knot = (x, y) => Math.max(Math.sqrt(v3.len(v3.sub(y, x))), 1e-4);
  const k0 = 0, k1 = k0 + knot(p0, p1), k2 = k1 + knot(p1, p2), k3 = k2 + knot(p2, p3), u = k1 + ((t - a.t) / (b.t - a.t)) * (k2 - k1);
  const lerp = (x, y, ka, kb) => v3.add(v3.mul(x, (kb - u) / (kb - ka)), v3.mul(y, (u - ka) / (kb - ka)));
  const a1 = lerp(p0, p1, k0, k1), a2 = lerp(p1, p2, k1, k2), a3 = lerp(p2, p3, k2, k3);
  return lerp(lerp(a1, a2, k0, k2), lerp(a2, a3, k1, k3), k1, k2);
}
function cutAxisAngleToQuat(axis, deg) {
  const n = v3.norm(axis), h = (deg * Math.PI) / 360, s = Math.sin(h);
  return v3.len(n) ? [n[0] * s, n[1] * s, n[2] * s, Math.cos(h)] : [0, 0, 0, 1];
}
function cutQuatToAxisAngle(q) {
  const s = Math.hypot(q[0], q[1], q[2]);
  if (s < 1e-9) return [0, 1, 0, 0];
  return [q[0] / s, q[1] / s, q[2] / s, (2 * Math.atan2(s, q[3]) * 180) / Math.PI];
}
// Rotation (3 columns: where model X, Y, Z go) -> quaternion [x, y, z, w].
function quatFromBasis([x, y, z]) {
  const [m00, m10, m20] = x, [m01, m11, m21] = y, [m02, m12, m22] = z, tr = m00 + m11 + m22;
  if (tr > 0) { const s = Math.sqrt(tr + 1) * 2; return [(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, s / 4]; }
  if (m00 > m11 && m00 > m22) { const s = Math.sqrt(1 + m00 - m11 - m22) * 2; return [s / 4, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s]; }
  if (m11 > m22) { const s = Math.sqrt(1 + m11 - m00 - m22) * 2; return [(m01 + m10) / s, s / 4, (m12 + m21) / s, (m02 - m20) / s]; }
  const s = Math.sqrt(1 + m22 - m00 - m11) * 2;
  return [(m02 + m20) / s, (m12 + m21) / s, s / 4, (m10 - m01) / s];
}
// Game space is Y down: up is -Y. One file unit is ~4.5 m, so gravity for the bank angle is ~2.2 units/s^2.
const CUT_UP = [0, -1, 0], CUT_BANK_G = 2.2;
// Mid-mission camera shots: a mod's camera_shots.txt, one "<level> <trigger> <camera> [personal]" (a later line wins) or
// "<level> <trigger> destroy <object>" (they add up) per line, '#' comments, as the game reads it (src/game/camera_shots.h).
// personal: in co-op only the player who set the trigger off sees it.
const shotFields = (line) => (line.trim().startsWith('#') ? [] : line.trim().split(/\s+/).filter(Boolean));
const shotIsCamera = (f, level, trigger) => f.length >= 3 && f[0] === level && f[1] === trigger && f[2] !== 'destroy';
const shotIsDestroy = (f, level, trigger) => f.length >= 4 && f[0] === level && f[1] === trigger && f[2] === 'destroy';
const shotLines = (text) => text.replace(/\r/g, '').split('\n').filter((l, i, a) => i < a.length - 1 || l);
function shotLinkGet(text, level, trigger) {
  let link = null;
  for (const line of text.split('\n')) {
    const f = shotFields(line);
    if (shotIsCamera(f, level, trigger)) link = { camera: f[2], personal: f[3] === 'personal' };
  }
  return link;
}
function shotDestroyGet(text, level, trigger) {
  const names = [];
  for (const line of text.split('\n')) {
    const f = shotFields(line);
    if (shotIsDestroy(f, level, trigger) && !names.includes(f[3])) names.push(f[3]);
  }
  return names;
}
// The text with that trigger's destroy lines replaced by one per name, appended at the end; other lines kept, LF endings.
function shotDestroySet(text, level, trigger, names) {
  const out = shotLines(text).filter((l) => !shotIsDestroy(shotFields(l), level, trigger));
  for (const n of names) out.push(`${level} ${trigger} destroy ${n}`);
  return out.length ? out.join('\n') + '\n' : '';
}
// The text with that link replaced in place, appended, or removed (camera null); other lines kept, LF endings.
function shotLinkSet(text, level, trigger, camera, personal = false) {
  const lines = shotLines(text), out = [];
  const line = `${level} ${trigger} ${camera}${personal ? ' personal' : ''}`;
  let done = !camera;
  for (const l of lines) {
    const f = shotFields(l);
    if (shotIsCamera(f, level, trigger)) {
      if (!done) out.push(line);
      done = true;
    } else out.push(l);
  }
  if (!done) out.push(line);
  return out.length ? out.join('\n') + '\n' : '';
}
// Snapshots after a length change: same seconds, so t scales by oldLen / newLen; ones past the new end are dropped.
const cutRetimeSnaps = (snaps, oldLen, newLen) => snaps.map((s) => ({ ...s, t: (s.t * oldLen) / newLen })).filter((s) => s.t <= 1 + 1e-9);
// Samples (normalised times) between the first and last snapshot at rate per second of cutscene.
const cutSampleTimes = (data, t0, t1, rate) => {
  const n = Math.max(2, Math.ceil((t1 - t0) * data.length * rate) + 1);
  return [...Array(n).keys()].map((k) => t0 + ((t1 - t0) * k) / (n - 1));
};
// Bakes a root actor's position from snapshots and its rotation so the nose (model +Z, as every stock craft) points along the path, up = -Y, rolled toward
// the turn centre by bank x the lateral acceleration (60 deg at most). Axis channels use the stock form 10 x q.xyz (smooth through 0).
function cutBakeActorPath(data, actor, snaps, { rate = 10, bank = 1 } = {}) {
  if (!snaps.length) return;
  const t0 = snaps[0].t, t1 = snaps[snaps.length - 1].t, ref = (ch) => ({ actor, ch });
  if (t1 - t0 < 1e-9) {
    [0, 1, 2].forEach((ch) => cutSetChannelSpan(data, ref(ch), 0, 1, cutHeld([[t0, snaps[0].p[ch]]])));
    return;
  }
  const ts = cutSampleTimes(data, t0, t1, rate), sec = data.length, h = 0.25 / (sec * rate);
  const at = (t) => cutCurve(snaps, Math.min(t1, Math.max(t0, t)));
  const cols = [[], [], [], [], [], [], []];
  let prevQ = null, prevUp = CUT_UP;
  for (const t of ts) {
    const p = at(t), vel = v3.mul(v3.sub(at(t + h), at(t - h)), 1 / (2 * h * sec)), acc = v3.mul(v3.add(v3.sub(at(t + h), v3.mul(p, 2)), at(t - h)), 1 / (h * h * sec * sec));
    const f = v3.len(vel) > 1e-6 ? v3.norm(vel) : (prevQ ? null : [0, 0, -1]);
    let q = prevQ;
    if (f) {
      let up = v3.norm(v3.sub(CUT_UP, v3.mul(f, v3.dot(CUT_UP, f))));
      if (!v3.len(up)) up = prevUp;
      prevUp = up;
      const side = v3.norm(v3.cross(f, up)), roll = Math.max(-60, Math.min(60, bank * (Math.atan2(v3.dot(acc, side), CUT_BANK_G) * 180) / Math.PI)) * (Math.PI / 180);
      const u = v3.add(v3.mul(up, Math.cos(roll)), v3.mul(side, Math.sin(roll)));
      const y = v3.mul(u, -1), z = f;
      q = quatFromBasis([v3.cross(y, z), y, z]);
      if (prevQ && q.reduce((s, v, i) => s + v * prevQ[i], 0) < 0) q = q.map((v) => -v);
      prevQ = q;
    }
    q ??= [0, 0, 0, 1];
    const deg = (2 * Math.acos(Math.max(-1, Math.min(1, q[3]))) * 180) / Math.PI;
    [p[0], p[1], p[2], q[0] * 10, q[1] * 10, q[2] * 10, deg].forEach((v, k) => cols[k].push([t, v]));
  }
  [0, 1, 2, 4, 5, 6, 7].forEach((ch, k) => cutSetChannelSpan(data, ref(ch), 0, 1, cutHeld(cols[k])));
}
// Samples extended to hold their first value from 0 and their last to 1 (a snapshot track owns the whole channel).
const cutHeld = (col) => [...(col[0][0] > 0 ? [[0, col[0][1]]] : []), ...col, ...(col[col.length - 1][0] < 1 ? [[1, col[col.length - 1][1]]] : [])];
const quat = {
  mul: (a, b) => [a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0], a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]],
  rot: (q, p) => { const u = [q[0], q[1], q[2]], uv = v3.cross(u, p), uuv = v3.cross(u, uv); return v3.add(p, v3.mul(v3.add(v3.mul(uv, q[3]), uuv), 2)); },
  slerp: (a, b, s) => {
    let d = a.reduce((x, v, i) => x + v * b[i], 0);
    const bb = d < 0 ? b.map((v) => -v) : b;
    d = Math.abs(d);
    if (d > 0.9995) return v3.norm4(a.map((v, i) => v + (bb[i] - v) * s));
    const th = Math.acos(d), sa = Math.sin((1 - s) * th) / Math.sin(th), sb = Math.sin(s * th) / Math.sin(th);
    return a.map((v, i) => v * sa + bb[i] * sb);
  },
};
v3.norm4 = (q) => { const n = Math.hypot(...q) || 1; return q.map((v) => v / n); };
// Camera node rotation looking from pos at target: the camera looks down its +Z, screen up is node -Y = game up (-Y).
const cutLookQuat = (pos, target) => {
  const z = v3.norm(v3.sub(target, pos));
  if (!v3.len(z)) return [0, 0, 0, 1];
  let y = v3.norm(v3.sub([0, 1, 0], v3.mul(z, z[1])));
  if (!v3.len(y)) y = [1, 0, 0];
  return quatFromBasis([v3.cross(y, z), y, z]);
};
// A camera / actor rotation as its four channel values (stock form: axis 10 x q.xyz, angle in degrees).
const cutRotValues = (q) => [q[0] * 10, q[1] * 10, q[2] * 10, (2 * Math.acos(Math.max(-1, Math.min(1, q[3]))) * 180) / Math.PI];
const CUT_ACTOR_DIVS = [6, 6, 6, 6, 3.5, 3.5, 3.5, 3.5, 3.5, 3.5, 3.5];
// A root actor's position and rotation at normalised time t, sampled like the game.
function cutActorPose(data, actor, t) {
  const a = data.actors[actor];
  if (!a || a.parent >= 0) throw new Error(`${a?.name ?? 'This actor'} rides on a parent: camera modes and shots need a root actor`);
  const v = a.animated ? a.ch.map((keys, ch) => cutsceneValue(keys, t, data.frames, CUT_ACTOR_DIVS[ch])) : a.value;
  return { p: [v[0], v[1], v[2]], q: cutAxisAngleToQuat([v[4], v[5], v[6]], v[7]) };
}
// Camera snapshots [{ t, p, q, fov }] -> camera channels: position on the curve, rotation slerped (hemisphere-continuous), FOV linear.
function cutBakeCameraPath(data, cam, snaps, { rate = 10 } = {}) {
  if (!snaps.length) return;
  const t0 = snaps[0].t, t1 = snaps[snaps.length - 1].t, ref = (ch) => ({ cam, ch }), cols = [...Array(8)].map(() => []);
  const ts = t1 - t0 < 1e-9 ? [t0] : cutSampleTimes(data, t0, t1, rate);
  let prev = null;
  for (const t of ts) {
    let i = 0;
    while (i + 1 < snaps.length - 1 && snaps[i + 1].t <= t) i++;
    const a = snaps[i], b = snaps[Math.min(i + 1, snaps.length - 1)], s = b.t - a.t > 1e-9 ? Math.min(1, Math.max(0, (t - a.t) / (b.t - a.t))) : 1;
    let q = v3.norm4(quat.slerp(a.q, b.q, s));
    if (prev && q.reduce((x, v, j) => x + v * prev[j], 0) < 0) q = q.map((v) => -v);
    prev = q;
    [...cutCurve(snaps, t), ...cutRotValues(q), a.fov + (b.fov - a.fov) * s].forEach((v, ch) => cols[ch].push([t, v]));
  }
  cols.forEach((col, ch) => cutSetChannelSpan(data, ref(ch), 0, 1, cutHeld(col)));
}
// Bakes a camera mode over t0..t1 from a root actor's pose: 'follow' (offset in the actor's frame, looking at it), 'aim' (keeps its
// position, looks at it) or 'ride' (rigidly at the offset, looking down the nose, the actor hidden for the span as id4_extro's cockpit shot).
function cutBakeCameraMode(data, cam, { mode, actor, t0, t1, offset = [0, 0, 0], rate = 10 }) {
  cutActorPose(data, actor, t0);
  const ref = (ch) => ({ cam, ch }), cols = [...Array(7)].map(() => []);
  let prev = null;
  for (const t of cutSampleTimes(data, t0, t1, rate)) {
    const pose = cutActorPose(data, actor, t);
    const pos = mode === 'aim' ? [0, 1, 2].map((ch) => cutsceneValue(data.cameras[cam][ch], t, data.frames, 1)) : v3.add(pose.p, quat.rot(pose.q, offset));
    let q = mode === 'ride' ? pose.q : cutLookQuat(pos, pose.p);
    if (prev && q.reduce((x, v, j) => x + v * prev[j], 0) < 0) q = q.map((v) => -v);
    prev = q;
    [...pos, ...cutRotValues(q)].forEach((v, ch) => cols[ch].push([t, v]));
  }
  cols.forEach((col, ch) => (mode !== 'aim' || ch >= 3) && cutSetChannelSpan(data, ref(ch), t0, t1, col));
  if (mode === 'ride') {
    cutAddEvent(data, { type: 7, frame: Math.round(t0 * data.frames), p: [actor, 0, 0, 0] });
    if (t1 < 1) cutAddEvent(data, { type: 6, frame: Math.round(t1 * data.frames), p: [actor, 0, 0, 0] });
  }
}
// How far either side of the shot time cutShootAt keys the shooter's rotation.
const cutShotSpan = (data, aimLead = 0.3) => (2 * 3.5) / data.frames + aimLead / data.length;
// One-action shot: at t the shooter's nose swings onto the target (eased in and out over aimLead seconds), fires a bolt (event 8) fast
// enough to arrive in 0.5 s, inside the 1.5 s bolt life, and the target explodes (event 5 kind 0) and disappears (event 7) on impact.
function cutShootAt(data, shooter, target, t, { category = 0, aimLead = 0.3 } = {}) {
  const fireFrame = Math.round(t * data.frames), tf = fireFrame / data.frames, impactFrame = fireFrame + Math.round(0.5 * data.fps);
  const from = cutActorPose(data, shooter, tf).p, to = cutActorPose(data, target, tf).p, dist = v3.len(v3.sub(to, from)) / 4;
  const p2 = Math.round((dist / 0.5) * 10);
  if (p2 > 0xFFFF) throw new Error(`Target out of bolt range (${dist.toFixed(0)} units; 0.5 s needs speed ${p2 / 10})`);
  // The game smooths rotation channels over +-2 samples of frames / 3.5, so the aim is held that long either side of the shot.
  const span = cutShotSpan(data, aimLead), hold = (2 * 3.5) / data.frames;
  const lead = span - hold;
  const ts = [...new Set([...cutSampleTimes(data, Math.max(0, tf - span), Math.min(1, tf + span), 30), tf])].sort((a, b) => a - b);
  const cols = [[], [], [], []];
  let prev = null;
  for (const s of ts) {
    const cur = [4, 5, 6, 7].map((ch) => cutsceneValue(data.actors[shooter].ch[ch], s, data.frames, 1));
    const pos = cutActorPose(data, shooter, s).p, f = v3.norm(v3.sub(cutActorPose(data, target, s).p, pos));
    let up = v3.norm(v3.sub(CUT_UP, v3.mul(f, v3.dot(CUT_UP, f))));
    if (!v3.len(up)) up = [0, 0, 1];
    const y = v3.mul(up, -1), z = f, aim = quatFromBasis([v3.cross(y, z), y, z]);
    const d = Math.abs(s - tf) - hold, w = d <= 0 ? 1 : d >= lead ? 0 : 0.5 * (1 + Math.cos((Math.PI * d) / lead));
    let q = v3.norm4(quat.slerp(cutAxisAngleToQuat(cur.slice(0, 3), cur[3]), aim, w));
    if (prev && q.reduce((x, v, j) => x + v * prev[j], 0) < 0) q = q.map((v) => -v);
    prev = q;
    cutRotValues(q).forEach((v, k) => cols[k].push([s, v]));
  }
  cols.forEach((col, k) => cutSetChannelSpan(data, { actor: shooter, ch: 4 + k }, ts[0], ts[ts.length - 1], col));
  cutAddEvent(data, { type: 8, frame: fireFrame, p: [shooter, category, p2, 0] });
  cutAddEvent(data, { type: 5, frame: impactFrame, p: [target, 0, 0, 0] });
  cutAddEvent(data, { type: 7, frame: impactFrame, p: [target, 0, 0, 0] });
  return { fireFrame, impactFrame };
}
// The events a bake of doc added to f (ride hide / show; shot fire, explosion, disappear), found by type, frame and actor.
function cutBakedEvents(f, doc) {
  const out = [];
  const find = (type, frame, a) => {
    const e = f.events.find((x) => !out.includes(x) && x.type === type && x.frame === frame && x.p[0] === a);
    if (e) out.push(e);
  };
  for (const [key, tr] of Object.entries(doc.tracks)) {
    const n = +key.split(':')[1], cam = key.startsWith('cam:');
    if (cam ? !f.cameras[n] : !f.actors[n]?.animated) continue;
    for (const m of cam ? tr.modes ?? [] : []) if (m.mode === 'ride') {
      find(7, Math.round(m.t0 * f.frames), m.actor);
      if (m.t1 < 1) find(6, Math.round(m.t1 * f.frames), m.actor);
    }
    for (const s of cam ? [] : tr.shots ?? []) {
      const fire = Math.round(s.t * f.frames), hit = fire + Math.round(0.5 * f.fps);
      [[8, fire, n], [5, hit, s.target], [7, hit, s.target]].forEach((d) => find(...d));
    }
  }
  return out;
}
// Undoes a bake before the next: removes the events it added and the keys inside its spans (t = 0 stays).
function cutUnbake(f, doc) {
  const baked = cutBakedEvents(f, doc);
  f.events = f.events.filter((e) => !baked.includes(e));
  // The key at 0 starts each channel and stays.
  const clear = (ref, chs, t0, t1) => chs.forEach((ch) => cutSetChannelSpan(f, { ...ref, ch }, Math.max(t0, 0.5 / f.frames), t1, []));
  for (const [key, tr] of Object.entries(doc.tracks)) {
    const n = +key.split(':')[1], cam = key.startsWith('cam:');
    if (cam ? !f.cameras[n] : !f.actors[n]?.animated) continue;
    const snaps = tr.snaps ?? [], ref = cam ? { cam: n } : { actor: n };
    if (snaps.length) clear(ref, cam ? [0, 1, 2, 3, 4, 5, 6, 7] : snaps.length > 1 ? [0, 1, 2, 4, 5, 6, 7] : [0, 1, 2], 0, 1);
    for (const m of tr.modes ?? []) clear(ref, m.mode === 'aim' ? [3, 4, 5, 6] : [0, 1, 2, 3, 4, 5, 6], m.t0, m.t1);
    for (const s of tr.shots ?? []) {
      const tf = Math.round(s.t * f.frames) / f.frames, span = cutShotSpan(f);
      clear(ref, [4, 5, 6, 7], tf - span, tf + span);
    }
  }
}
// After the cuts move, each camera mode takes the span of its camera's shot that overlaps its old span most.
function cutSyncModeSpans(doc, f) {
  const first = f.cameras.findIndex(Boolean), shots = cutClips(f, () => null).filter((c) => c.lane === 'camera');
  for (const [key, tr] of Object.entries(doc.tracks)) {
    if (!key.startsWith('cam:')) continue;
    const k = +key.split(':')[1], mine = shots.filter((c) => (c.startEvent?.p[0] ?? first) === k).map((c) => [c.start / f.frames, Math.min(1, c.end / f.frames)]);
    for (const m of tr.modes ?? []) {
      const over = (sp) => Math.min(sp[1], m.t1) - Math.max(sp[0], m.t0), best = mine.reduce((a, b) => (!a || over(b) > over(a) ? b : a), null);
      if (best) [m.t0, m.t1] = best;
    }
  }
}
// Actor paths first (camera modes and shots read them), then shots, then camera paths and modes in order.
function cutBake(f, doc) {
  const tracks = Object.entries(doc.tracks).map(([k, tr]) => [k.startsWith('cam:'), +k.split(':')[1], tr]);
  for (const [cam, n, tr] of tracks) if (!cam && f.actors[n] && tr.snaps?.length) {
    cutAnimateActor(f, n);
    cutBakeActorPath(f, n, tr.snaps);
  }
  for (const [cam, n, tr] of tracks) if (!cam && f.actors[n] && tr.shots?.length) {
    cutAnimateActor(f, n);
    for (const s of tr.shots) cutShootAt(f, n, s.target, s.t);
  }
  for (const [cam, n, tr] of tracks) if (cam && f.cameras[n]) {
    if (tr.snaps?.length) cutBakeCameraPath(f, n, tr.snaps);
    for (const m of tr.modes ?? []) cutBakeCameraMode(f, n, m);
  }
}
// Timeline clips (frames) from the events: a camera shot runs to the next cut; an actor is on screen from a show (6) to its next hide
// (7); music (1) plays to the next cue or a stop (2); S-foils (11) last p2 frames; walk rate (13) and fog (17) hold to the next of their
// kind; voice (0) and sounds (3) are as long as their audio (audioLength(e) seconds, or null) with locked ends; the rest are instants.
// startEvent / endEvent are the events a drag of that end moves (null: the clip's edge is the cutscene's, or a stop to be created).
const CUT_LANES = { 0: 'audio', 1: 'audio', 2: 'audio', 3: 'audio', 12: 'audio', 4: 'camera', 10: 'scene', 17: 'scene', 19: 'scene', 21: 'scene', 22: 'scene' };
function cutClips(data, audioLength) {
  const ev = data.events, F = data.frames, clips = [];
  const clip = (lane, kind, start, end, extra) => clips.push({ lane, kind, start, end: Math.max(start, Math.min(F, end)), startEvent: null, endEvent: null, lockStart: false, lockEnd: false, ...extra });
  const cuts = ev.filter((e) => e.type === 4);
  if (!cuts.length || cuts[0].frame > 0) clip('camera', 'shot', 0, cuts[0]?.frame ?? F, { endEvent: cuts[0] ?? null, lockStart: true, lockEnd: !cuts.length });
  cuts.forEach((e, i) => clip('camera', 'shot', e.frame, cuts[i + 1]?.frame ?? F, { startEvent: e, endEvent: cuts[i + 1] ?? null, lockEnd: !cuts[i + 1], event: e }));
  const next = (from, test) => ev.find((x) => x !== from && x.frame >= from.frame && test(x));
  for (const [i, e] of ev.entries()) {
    const lane = CUT_LANES[e.type] ?? 'action';
    if (e.type === 6) {
      const hide = next(e, (x) => x.type === 7 && x.p[0] === e.p[0]);
      clip(lane, 'actor', e.frame, hide?.frame ?? F, { startEvent: e, endEvent: hide ?? null, event: e, actor: e.p[0] });
    } else if (e.type === 7) {
      if (!ev.slice(0, i).some((x) => x.type === 6 && x.p[0] === e.p[0])) clip(lane, 'actor', 0, e.frame, { endEvent: e, lockStart: true, event: e, actor: e.p[0] });
    } else if (e.type === 1) {
      const cue = next(e, (x) => x.type === 1), stop = next(e, (x) => x.type === 2 && (!cue || x.frame <= cue.frame));
      clip(lane, 'music', e.frame, stop?.frame ?? cue?.frame ?? F, { startEvent: e, endEvent: stop ?? null, event: e });
    } else if (e.type === 11) {
      clip(lane, 'foils', e.frame, e.frame + e.p[2], { startEvent: e, event: e });
    } else if (e.type === 13 || e.type === 17) {
      const hold = next(e, (x) => x.type === e.type && (e.type === 17 || x.p[0] === e.p[0]));
      clip(lane, e.type === 13 ? 'walk' : 'fog', e.frame, hold?.frame ?? F, { startEvent: e, endEvent: hold ?? null, lockEnd: !hold, event: e });
    } else if (e.type === 0 || e.type === 3) {
      const len = audioLength(e);
      clip(lane, e.type === 0 ? 'voice' : 'sound', e.frame, e.frame + Math.max(1, Math.round((len ?? 0) * data.fps)), { startEvent: e, lockStart: true, lockEnd: true, event: e });
    } else if (e.type !== 2 && e.type !== 4) {
      clip(lane, 'instant', e.frame, e.frame, { startEvent: e, lockStart: true, lockEnd: true, event: e });
    }
  }
  return clips;
}
// Drags one end of a clip (or the whole clip with 'move') by dFrames, keeping at least 1 frame and staying inside the cutscene.
function cutDragClip(data, clip, part, dFrames) {
  const F = data.frames, at = (e, f) => (e.frame = Math.max(0, Math.min(F, Math.round(f))));
  if (part === 'move') {
    const len = clip.end - clip.start, d = Math.max(-clip.start, Math.min(F - clip.end, dFrames));
    if (clip.startEvent) at(clip.startEvent, clip.start + d);
    if (clip.endEvent && clip.kind !== 'shot') at(clip.endEvent, clip.start + d + len);
  } else if (part === 'start' && !clip.lockStart && clip.startEvent) {
    const s = Math.min(clip.end - 1, clip.start + dFrames);
    at(clip.startEvent, s);
    if (clip.kind === 'foils') clip.startEvent.p[2] = Math.max(1, clip.end - clip.startEvent.frame);
  } else if (part === 'end' && !clip.lockEnd) {
    const end = Math.max(clip.start + 1, Math.min(F, clip.end + dFrames));
    if (clip.kind === 'foils') clip.startEvent.p[2] = end - clip.start;
    else if (clip.endEvent) at(clip.endEvent, end);
    else if (clip.kind === 'music') cutAddEvent(data, { type: 2, frame: end, p: [0, 0, 0, 0] });
    else if (clip.kind === 'actor') cutAddEvent(data, { type: 7, frame: end, p: [clip.actor, 0, 0, 0] });
  }
  data.events.sort((a, b) => a.frame - b.frame);
}
// Value of one channel at normalised time t, as cinematicSlotUpdate (0x800A6904) does: each key is a line (value + slope * (t - time)) from
// the last key at or before t, blended by a uniform Catmull-Rom over 4 samples on a grid of frames / div steps (div: camera position 8,
// camera angle 2.5; actor position and scale 6, the rest 3.5); near the ends it is the plain line.
function cutsceneValue(keys, t, frames, div) {
  const n = keys.length / 3;
  if (!n) return 0;
  const lin = (tt) => {
    let k = 0;
    while (k + 1 < n && keys[(k + 1) * 3 + 2] <= tt) k++;
    return keys[k * 3] + keys[k * 3 + 1] * (tt - keys[k * 3 + 2]);
  };
  const N = frames / div, i = Math.trunc(t * N), ts = [i - 1, i, i + 1, i + 2].map((x) => x / N);
  if (ts[0] < 0 || ts[2] > 1 || ts[3] > 1) return lin(t);
  const [p0, p1, p2, p3] = ts.map(lin), u = (t - ts[1]) / Math.max(ts[2] - ts[1], 5e-5), u2 = u * u, u3 = u2 * u;
  return 0.5 * (-u3 + 2 * u2 - u) * p0 + (1.5 * u3 - 2.5 * u2 + 1) * p1 + (-1.5 * u3 + 2 * u2 + 0.5 * u) * p2 + 0.5 * (u3 - u2) * p3;
}

// ---- Mod export: texture encoding, BPS patches, stored zip ----

const CRC_TABLE = Array.from({ length: 256 }, (_, n) => {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});
function crc32(bytes) {
  let c = 0xFFFFFFFF;
  for (let i = 0; i < bytes.length; i++) c = CRC_TABLE[(c ^ bytes[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}

// Median cut over the opaque pixels of an RGBA8 image; returns up to n [r, g, b] colours.
function medianCut(rgba, n) {
  const px = [], distinct = new Map();
  for (let i = 0; i < rgba.length; i += 4) {
    if (rgba[i + 3] < 128) continue;
    px.push([rgba[i], rgba[i + 1], rgba[i + 2]]);
    distinct.set((rgba[i] << 16) | (rgba[i + 1] << 8) | rgba[i + 2], px[px.length - 1]);
  }
  if (!px.length) return [[0, 0, 0]];
  if (distinct.size <= n) return [...distinct.values()];
  const boxes = [px];
  while (boxes.length < n) {
    let best = -1, bestRange = 0, bestCh = 0;
    boxes.forEach((b, i) => {
      if (b.length < 2) return;
      for (let ch = 0; ch < 3; ch++) {
        let lo = 255, hi = 0;
        for (const p of b) { lo = Math.min(lo, p[ch]); hi = Math.max(hi, p[ch]); }
        if (hi - lo > bestRange) { best = i; bestRange = hi - lo; bestCh = ch; }
      }
    });
    if (best < 0) break;
    const b = boxes[best].sort((p, q) => p[bestCh] - q[bestCh]);
    boxes.splice(best, 1, b.slice(0, b.length >> 1), b.slice(b.length >> 1));
  }
  return boxes.map((b) => [0, 1, 2].map((ch) => Math.round(b.reduce((s, p) => s + p[ch], 0) / b.length)));
}
const nearest = (pal, r, g, b, from = 0) => {
  let best = from, bd = Infinity;
  for (let k = from; k < pal.length; k++) {
    const d = (pal[k][0] - r) ** 2 + (pal[k][1] - g) ** 2 + (pal[k][2] - b) ** 2;
    if (d < bd) { bd = d; best = k; }
  }
  return best;
};

// Writes an RGBA8 image (t.w x t.h) into texture t of an HMT file, in t's own format: the inverse of decodeTexture for HMT textures.
// A palette another texture shares is kept and the image is mapped onto it. Colour-keyed (flag 0x400) textures write transparent pixels as the key colour.
function encodeTexture(bytes, t, rgba, textures = []) {
  const { w, h, type, depth, pix, pal, flags, trans } = t;
  // The decoder rounds the height up to even; only the stored rows are written, or the last row would overwrite the next texture.
  const n = w * Math.min(h, t.rows ?? h), dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const key = [(trans >>> 24) & 255, (trans >>> 16) & 255, (trans >>> 8) & 255];
  const at = (i) => [rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], rgba[i * 4 + 3]];
  let keyed = false;
  if (flags & 0x400) for (let i = 0; i < n && !keyed; i++) keyed = rgba[i * 4 + 3] < 128;
  if (type === 0 || type === 1) {
    const four = type === 0, size = four ? 16 : 256;
    const shared = textures.some((o) => o !== t && o.pal === pal);
    let colors;
    if (shared) {
      colors = Array.from({ length: size }, (_, k) => [0, 1, 2].map((c) => dv.getUint8(pal + k * 3 + c)));
    } else {
      colors = medianCut(rgba, keyed ? size - 1 : size);
      if (keyed) colors.unshift(key);
      while (colors.length < size) colors.push([0, 0, 0]);
      colors.forEach((c, k) => c.forEach((v, ch) => dv.setUint8(pal + k * 3 + ch, v)));
    }
    const keyIndex = keyed ? colors.findIndex((c) => c.every((v, ch) => (v & 0xF8) === (key[ch] & 0xF8))) : -1;
    for (let i = 0; i < n; i++) {
      const [r, g, b, a] = at(i);
      const k = keyed && a < 128 && keyIndex >= 0 ? keyIndex : nearest(colors, r, g, b, keyed && !shared ? 1 : 0);
      if (four) {
        const o = pix + (i >> 1), v = dv.getUint8(o);
        dv.setUint8(o, i & 1 ? (v & 0xF0) | k : (v & 0x0F) | (k << 4));
      } else {
        dv.setUint8(pix + i, k);
      }
    }
  } else if (type === 2) {
    for (let i = 0; i < n; i++) {
      const [r, g, b, a] = at(i);
      dv.setUint16(pix + i * 2, (a >= 128 ? 0x8000 : 0) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
    }
  } else if (type === 3) {
    const bpp = flags & 0x800 ? 4 : 3;
    for (let i = 0; i < n; i++) for (let c = 0; c < bpp; c++) dv.setUint8(pix + i * bpp + c, at(i)[c]);
  } else if ((type === 4 || type === 5) && !t.bpp) {
    const four = type === 4 && depth === 1;
    for (let i = 0; i < n; i++) {
      const [r, g, b, a] = at(i);
      let v = Math.round(0.299 * r + 0.587 * g + 0.114 * b);
      if (keyed && a < 128) v = key[0];
      if (four) {
        const o = pix + (i >> 1), k = v >> 4, old = dv.getUint8(o);
        dv.setUint8(o, i & 1 ? (old & 0xF0) | k : (old & 0x0F) | (k << 4));
      } else {
        dv.setUint8(pix + i, v);
      }
    }
  } else {
    throw new Error(`Texture format type ${type} cannot be written yet`);
  }
}

// BPS patch turning src into dst (both Uint8Array); applied by librecomp's recomp::patcher::patch_rom. Bytes unchanged in place are
// SourceRead, bytes found elsewhere in src are SourceCopy (data that moved, e.g. DAT sections after an added item), and only what
// is really new is stored (TargetRead), so a patch made from an original asset carries none of its data.
function encodeBPS(src, dst) {
  const out = [];
  const num = (v) => {
    for (;;) {
      const x = v % 128;
      v = Math.floor(v / 128);
      if (v === 0) { out.push(0x80 | x); return; }
      out.push(x);
      v--;
    }
  };
  const same = (i) => i < src.length && src[i] === dst[i];
  const key = (a, i) => ((a[i] << 24) | (a[i + 1] << 16) | (a[i + 2] << 8) | a[i + 3]) >>> 0;
  const where = new Map();
  for (let i = 0; i + 4 <= src.length; i++) {
    const k = key(src, i);
    let l = where.get(k);
    if (!l) where.set(k, (l = []));
    if (l.length < 16) l.push(i);
  }
  out.push(0x42, 0x50, 0x53, 0x31);
  num(src.length); num(dst.length); num(0);
  let pending = -1, srcRel = 0;
  const flush = (end) => {
    if (pending < 0) return;
    num((end - pending - 1) * 4 + 1);
    for (let k = pending; k < end; k++) out.push(dst[k]);
    pending = -1;
  };
  for (let i = 0; i < dst.length;) {
    let run = 0;
    while (i + run < dst.length && same(i + run)) run++;
    // In-place runs shorter than 4 inside a changed stretch are cheaper as part of it.
    if (run >= 4 || (run && pending < 0)) {
      flush(i);
      num((run - 1) * 4);
      i += run;
      continue;
    }
    let best = 0, at = 0;
    if (i + 8 <= dst.length) {
      for (const p of where.get(key(dst, i)) || []) {
        let n = 0;
        while (p + n < src.length && i + n < dst.length && src[p + n] === dst[i + n]) n++;
        if (n > best) { best = n; at = p; }
      }
    }
    if (best >= 8) {
      flush(i);
      num((best - 1) * 4 + 2);
      const rel = at - srcRel;
      num(Math.abs(rel) * 2 + (rel < 0 ? 1 : 0));
      srcRel = at + best;
      i += best;
      continue;
    }
    if (pending < 0) pending = i;
    i++;
  }
  flush(dst.length);
  const le = (v) => out.push(v & 255, (v >>> 8) & 255, (v >>> 16) & 255, v >>> 24);
  le(crc32(src));
  le(crc32(dst));
  le(crc32(Uint8Array.from(out)));
  return Uint8Array.from(out);
}

// Uncompressed zip of [{ name, data: Uint8Array }].
function zipStore(files) {
  const enc = new TextEncoder(), parts = [], central = [];
  let off = 0;
  for (const f of files) {
    const name = enc.encode(f.name), crc = crc32(f.data), size = f.data.length;
    const head = (sig, extra) => {
      const b = new Uint8Array(extra + name.length), dv = new DataView(b.buffer);
      dv.setUint32(0, sig, true);
      return { b, dv };
    };
    const { b: lh, dv: l } = head(0x04034b50, 30);
    l.setUint16(4, 20, true); l.setUint16(12, 0x21, true); l.setUint32(14, crc, true); l.setUint32(18, size, true); l.setUint32(22, size, true); l.setUint16(26, name.length, true);
    lh.set(name, 30);
    const { b: ch, dv: c } = head(0x02014b50, 46);
    c.setUint16(4, 20, true); c.setUint16(6, 20, true); c.setUint16(14, 0x21, true); c.setUint32(16, crc, true); c.setUint32(20, size, true); c.setUint32(24, size, true);
    c.setUint16(28, name.length, true); c.setUint32(42, off, true);
    ch.set(name, 46);
    parts.push(lh, f.data);
    central.push(ch);
    off += lh.length + size;
  }
  const cdSize = central.reduce((s, b) => s + b.length, 0), end = new Uint8Array(22), e = new DataView(end.buffer);
  e.setUint32(0, 0x06054b50, true); e.setUint16(8, files.length, true); e.setUint16(10, files.length, true); e.setUint32(12, cdSize, true); e.setUint32(16, off, true);
  const all = [...parts, ...central, end], out = new Uint8Array(all.reduce((s, b) => s + b.length, 0));
  let p = 0;
  for (const b of all) { out.set(b, p); p += b.length; }
  return out;
}

// Reads a stored (uncompressed) zip such as zipStore writes, by its central directory.
function zipRead(bytes) {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength), dec = new TextDecoder(), out = new Map();
  let end = bytes.length - 22;
  while (end >= 0 && dv.getUint32(end, true) !== 0x06054b50) end--;
  if (end < 0) throw new Error('Not a zip file');
  let at = dv.getUint32(end + 16, true);
  for (let i = 0; i < dv.getUint16(end + 10, true); i++) {
    if (dv.getUint32(at, true) !== 0x02014b50) throw new Error('Bad zip directory');
    if (dv.getUint16(at + 10, true) !== 0) throw new Error('Compressed zip entries are not supported');
    const size = dv.getUint32(at + 20, true), nameLen = dv.getUint16(at + 28, true), local = dv.getUint32(at + 42, true);
    const name = dec.decode(bytes.subarray(at + 46, at + 46 + nameLen));
    const data = local + 30 + dv.getUint16(local + 26, true) + dv.getUint16(local + 28, true);
    if (data + size > bytes.length) throw new Error('Truncated zip');
    out.set(name, bytes.subarray(data, data + size));
    at += 46 + nameLen + dv.getUint16(at + 30, true) + dv.getUint16(at + 32, true);
  }
  return out;
}
// Applies a BPS patch as encodeBPS writes it (SourceRead, TargetRead, SourceCopy), checking all three CRCs.
function applyBPS(src, patch) {
  let p = 4;
  const num = () => {
    let data = 0, shift = 1;
    for (;;) {
      if (p >= patch.length - 12) throw new Error('Truncated BPS patch');
      const x = patch[p++];
      data += (x & 0x7f) * shift;
      if (x & 0x80) return data;
      shift *= 128;
      data += shift;
    }
  };
  if (String.fromCharCode(...patch.subarray(0, 4)) !== 'BPS1' || patch.length < 16) throw new Error('Not a BPS patch');
  const srcSize = num(), dst = new Uint8Array(num()), meta = num();
  p += meta;
  if (srcSize !== src.length) throw new Error('Patch is for a different file');
  let o = 0, srcOff = 0;
  while (p < patch.length - 12) {
    const a = num(), len = Math.floor(a / 4) + 1, type = a % 4;
    if (type === 0) dst.set(src.subarray(o, o + len), o);
    else if (type === 1) dst.set(patch.subarray(p, (p += len)), o);
    else if (type === 2) {
      const raw = num();
      srcOff += (raw & 1 ? -1 : 1) * Math.floor(raw / 2);
      dst.set(src.subarray(srcOff, (srcOff += len)), o);
    } else throw new Error('TargetCopy is not supported');
    o += len;
  }
  const u32 = (k) => (patch[k] | (patch[k + 1] << 8) | (patch[k + 2] << 16) | (patch[k + 3] << 24)) >>> 0;
  if (u32(patch.length - 12) !== crc32(src) || u32(patch.length - 8) !== crc32(dst)) throw new Error('Patch checksum mismatch');
  return dst;
}
// Project file: a stored zip of project.json (ROM CRC, steps), assets/<path>.bps (original -> current) and history/<n>.bps (before ->
// after; each before is the earlier afters replayed from the original). Patches only, no game data.
const PROJECT_FORMAT = 'rs64-explorer-project';
function projectWrite({ romCrc, originals, current, history }) {
  const enc = new TextEncoder(), files = [], steps = [];
  let n = 0;
  for (const s of history.steps) {
    steps.push({ label: s.label, changes: s.changes.map((c) => {
      files.push({ name: `history/${n}.bps`, data: encodeBPS(c.before, c.after) });
      return { path: c.path, patch: n++ };
    }) });
  }
  const assets = [...current.keys()].filter((p) => originals.has(p));
  for (const p of assets) files.push({ name: `assets/${p}.bps`, data: encodeBPS(originals.get(p), current.get(p)) });
  const json = { format: PROJECT_FORMAT, version: 1, romCrc: romCrc >>> 0, assets, at: history.at, steps };
  return zipStore([{ name: 'project.json', data: enc.encode(JSON.stringify(json, null, 1)) }, ...files]);
}
// readOriginal(path) -> Promise<Uint8Array> of the asset as in the ROM.
async function projectRead(bytes, romCrc, readOriginal) {
  let files, json;
  try {
    files = zipRead(bytes);
    json = JSON.parse(new TextDecoder().decode(files.get('project.json')));
  } catch {
    throw new Error('Not an RS64 Explorer project');
  }
  if (json?.format !== PROJECT_FORMAT) throw new Error('Not an RS64 Explorer project');
  if (json.romCrc !== romCrc >>> 0) throw new Error('This project was made with a different ROM');
  const patch = (name) => {
    const b = files.get(name);
    if (!b) throw new Error('Not an RS64 Explorer project');
    return b;
  };
  const state = new Map(), history = createHistory();
  for (const s of json.steps) {
    const changes = [];
    for (const c of s.changes) {
      if (!state.has(c.path)) state.set(c.path, await readOriginal(c.path));
      const before = state.get(c.path), after = applyBPS(before, patch(`history/${c.patch}.bps`));
      state.set(c.path, after);
      changes.push({ path: c.path, before, after });
    }
    history.steps.push({ label: s.label, changes });
  }
  history.at = json.at;
  const current = new Map();
  for (const p of json.assets) current.set(p, applyBPS(await readOriginal(p), patch(`assets/${p}.bps`)));
  return { current, history };
}

globalThis.RS64 = { openRom, parseShadowTextures, cutClips, cutDragClip, zipRead, applyBPS, projectWrite, projectRead, createHistory, parseCutscene, writeCutscene, CUT_LIMITS, CUT_ACTOR_EVENTS, cutRecomputeSlopes, cutAddEvent, cutRemoveEvent, cutMoveEvent, cutSetKey, cutRemoveKey, cutAddActor, cutAnimateActor, cutSetActorName, cutRemoveActor, cutAddCamera, cutRemoveCamera, cutSetLength, cutBlank, cutSetChannelSpan, cutCurve, cutQuatToAxisAngle, cutAxisAngleToQuat, cutBakeActorPath, cutActorPose, cutBakeCameraPath, cutBakeCameraMode, cutShootAt, cutRetimeSnaps, cutUnbake, cutBake, cutBakedEvents, cutSyncModeSpans, shotLinkGet, shotLinkSet, shotDestroyGet, shotDestroySet, cutsceneValue, encodeTexture, encodeBPS, zipStore, crc32, hmpGrid, writeHMP, datAddItem, datRemoveItem, datReplaceItem, datLookup, datRebuildNameMap, modLevelsParse, modLevelsWrite, modLevelNew, modLevelAliases, medalInfo, MEDALS_SIMPLE, objectivesApply, objectiveTargets, levelUsedObjectiveSlots, isUnit, datBlank, hmpFlat, recordInert, npcPathGet, npcSetPath, npcPathField, datRemoveIndexed, splinePoints, splineSetPoints, splineEvents, splineCopy, splineAttackEnd, splineSetAttackEnd, uniqueName, PATH_EVENT_KEEP, PATH_ATTACK_AT, WAVE_FIELDS, waveGet, waveSet, levelUsedFlags, waveFlagFor, triggerWave, WAVE_FOREIGN, datAddEnemy, hobSetPartGeometry, craftImportSlots, hobImportCraft, parseSpriteAnim, inflate, parseHMT, parseHOB, parseHMP, parseDAT, parseTexTable, decodeTerrainTex, parseVoiceLines, parseAnim, DAT_TYPES,
  parseSdir, decodeMusyxAdpcm, wavBytes, parseSpeech, decodeMort, encodeMort, parseVoiceTable, parseMenuVoice, parseFrontText, parseCutsceneVoice, parsePool, parseSfxTable, renderSfx, triggerVoiceIds, parseSongTable, parseSongGroups, parseSong, resolveNote, parseMixTables, renderVoice, renderSong, applyMusyxReverb };
}
rs64Lib();
