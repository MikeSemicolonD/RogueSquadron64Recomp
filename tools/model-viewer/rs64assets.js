// Rogue Squadron 64 asset parsers (data blob, HOB, HMT, HMP, DAT). Plain script so index.html works from file://; Node can import it too.
// Formats: rogue_squadron64/docs/{data_blob,hob_files,hmt_files,hmp_files,dat_files}.

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

function parseVoiceLines(romBytes, txt) {
  const strs = [];
  const n = txt.getUint16(2), lang = txt.getUint32(4);
  for (let i = 0; i < n; i++) {
    let o = lang + txt.getUint16(lang + i * 2), prev = 0xF5, s = '';
    for (let k = 0; k < 1024 && o + k < txt.byteLength; k++) {
      const c = txt.getUint8(o + k) ^ prev;
      prev ^= c;
      if (!c) break;
      s += String.fromCharCode(c);
    }
    strs.push(s);
  }
  const map = 0x8009FFE0 - 0x80000400 + 0x1000;
  return (voiceId) => (voiceId < 0x2F4 ? strs[(romBytes[map + voiceId * 2] << 8) | romBytes[map + voiceId * 2 + 1]] : undefined);
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

globalThis.RS64 = { openRom, parseSpriteAnim, inflate, parseHMT, parseHOB, parseHMP, parseDAT, parseTexTable, decodeTerrainTex, parseVoiceLines, parseAnim, DAT_TYPES };
