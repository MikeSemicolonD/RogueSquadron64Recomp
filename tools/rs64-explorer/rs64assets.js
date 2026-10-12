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
  if (dv.getUint32(0) !== 0x80371240) throw new Error('Not a big-endian .z64 ROM (byte-swapped .v64/.n64 are not supported)');
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
  return { assets, read };
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
      w: (dv.getUint16(o + 0x28) + 1) & ~1, h: (dv.getUint16(o + 0x2A) + 1) & ~1, depth: dv.getUint8(o + 0x2D),
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
    if (dv.getUint32(o + 4) & 0x2000) p += 8;
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
  return { offset: off, verts, faces: parseFaces(dv, dv.getUint32(off + 0x20)) };
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
      const name = nameOff ? cstr(dv, it + nameOff, 48) : '';
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

// Voice ids a DAT trigger leads to: a LINE_n name is a voice id; counters and flags it sets run the handlers the level script
// registered for them. tv = one level of RS64_TRIGGER_VOICES ({ c: counter -> ids, b: flag -> ids }, from tools/rs64-explorer-data/gen_voice_cats.mjs).
function triggerVoiceIds(it, tv) {
  const ids = it.voiceId != null ? [it.voiceId] : [];
  const p = it.pathEvent, acts = [it.onEnter, it.onExit, p && { counter: p.addCounter, flag: p.setFlag, value: p.value }];
  if (tv) for (const a of acts.filter(Boolean)) ids.push(...(tv.c[a.counter] ?? []), ...(a.value ? (tv.b[a.flag] ?? []) : []));
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

globalThis.RS64 = { openRom, parseSpriteAnim, inflate, parseHMT, parseHOB, parseHMP, parseDAT, parseTexTable, decodeTerrainTex, parseVoiceLines, parseAnim, DAT_TYPES,
  parseSdir, decodeMusyxAdpcm, wavBytes, parseSpeech, decodeMort, parseVoiceTable, parseMenuVoice, parseFrontText, parseCutsceneVoice, parsePool, parseSfxTable, renderSfx, triggerVoiceIds, parseSongTable, parseSongGroups, parseSong, resolveNote, parseMixTables, renderVoice, renderSong, applyMusyxReverb };
}
rs64Lib();
