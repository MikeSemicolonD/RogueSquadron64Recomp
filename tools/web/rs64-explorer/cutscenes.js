'use strict';
// ---- Cutscenes ----
// A cutscene opens over its level (cutscene N on lv_N) like the level view, with its actors and camera driven by parseCutscene /
// cutsceneValue. Actor matrix as cinematicSlotDispatcher: T(pos + pivot) S R(axis, angle) T(-pivot), positions x 0.25, children under
// their parent actor; models scale by HOB_TO_WORLD on top. The game camera looks down +Z with Y down, hence the X half-turn for three.js.
let cut = null;
// The free view's far plane while the cutscene camera imposes the game's.
let cutFreeFar = null;
// The letterbox band's share of the 4:3 screen height (170 or 224 of 240 lines; 170 of 224 for a sequence).
const cutBandK = () => 4 / 3 / cut.picAspect;
function restoreCutFar() {
  if (cutFreeFar == null) return;
  camera.far = cutFreeFar;
  cutFreeFar = null;
  camera.updateProjectionMatrix();
}
const CUT_ASSETS = 0x137580 + (0x800AF92C - 0x800A5130);
// Cutscene asset table (gCutsceneAssetPaths, 32 x 0x5C): model path at +0x00, name at +0x20, walk animation path at +0x3C, scale f32 at +0x54.
const cutStr = (o, n) => new TextDecoder().decode(romBytes.subarray(o, o + n)).replace(/\0.*$/s, '');
function cutAsset(name) {
  for (let i = 0; i < 32; i++) {
    const o = CUT_ASSETS + i * 0x5c;
    if (cutStr(o + 0x20, 16) === name) return { scale: new DataView(romBytes.buffer, o + 0x54, 4).getFloat32(0), anim: cutStr(o + 0x3c, 24), path: cutStr(o, 32), index: i, flags: romBytes[o + 0x5b] };
  }
  return { scale: 0, anim: '', path: '', index: -1, flags: 0 };
}
const cutBase = (name) => name.replace(/_\d+$/, '').replace(/^pl_crafts\//, '').replace(/^l_/, '');
// Cutscene shadows (allocateAndInitCraftNpcContext 0x8006BD18 at spawn, 0x8006BEC8 each frame): each model actor whose asset flags (+0x5B)
// have bit 2 gets a black shadow textured from data/pl_crafts/shadows_TM, per asset type from the table at 0x800B04AC (0x14 bytes:
// u16 texture at +2, half width +4, half length +8, world x / z offsets +0xC / +0x10). It is a 3x3 grid draped on the terrain (one height
// sample per vertex) around the actor's x/z, along its forward axis, and fades out by 7 units above the ground (cutscene ctx+0x14).
// shortcut: the sun-direction drift (ctx+0x10) and the camera occlusion fade are left out.
const CUT_SHADOWS = 0x137580 + (0x800B04AC - 0x800A5130);
const SHADOW_FACES = [0, 3, 4, 0, 4, 1, 1, 4, 5, 1, 5, 2, 3, 6, 7, 3, 7, 4, 4, 7, 8, 4, 8, 5];
let shadowTextures = null;
async function shadowTexture(idx) {
  shadowTextures ??= rom.assets.has('data/pl_crafts/shadows_TM') ? parseShadowTextures(await rom.read('data/pl_crafts/shadows_TM')) : [];
  const s = shadowTextures[idx];
  if (!s) return null;
  if (!s.tex) {
    const rgba = new Uint8Array(s.w * s.h * 4);
    s.alpha.forEach((a, i) => (rgba[i * 4 + 3] = a));
    s.tex = new THREE.DataTexture(rgba, s.w, s.h, THREE.RGBAFormat);
    s.tex.magFilter = s.tex.minFilter = THREE.LinearFilter;
    s.tex.needsUpdate = true;
  }
  return s.tex;
}
async function makeShadow(assetIndex) {
  const dv = new DataView(romBytes.buffer, CUT_SHADOWS + assetIndex * 0x14, 0x14), tex = dv.getUint16(2), map = await shadowTexture(tex);
  if (!map) return null;
  // s runs 0 at the outer edges to 1 on the centreline (mirrored half-shadows), except the falcon (5) and destroyer (12) textures.
  const sCol = tex === 5 || tex === 12 ? [0, 0.5, 1] : [0, 1, 0], uv = [];
  for (const t of [0, 0.5, 1]) for (const s of sCol) uv.push(s, t);
  const g = new THREE.BufferGeometry().setAttribute('position', new THREE.Float32BufferAttribute(new Float32Array(27), 3)).setAttribute('uv', new THREE.Float32BufferAttribute(uv, 2))
    .setAttribute('normal', new THREE.Float32BufferAttribute(new Float32Array(27).fill(0).map((_, k) => (k % 3 === 1 ? -1 : 0)), 3));
  g.setIndex(SHADOW_FACES);
  const mesh = new THREE.Mesh(g, new THREE.MeshBasicMaterial({ map, color: 0x000000, transparent: true, depthWrite: false, side: THREE.DoubleSide, polygonOffset: true, polygonOffsetFactor: -2 }));
  mesh.frustumCulled = false;
  return { mesh, halfW: dv.getFloat32(4), halfL: dv.getFloat32(8), dx: dv.getFloat32(12), dz: dv.getFloat32(16) };
}
const _sm = new THREE.Matrix4(), _sp = new THREE.Vector3(), _sf = new THREE.Vector3(), _su = new THREE.Vector3(), _ss = new THREE.Vector3();
function poseShadow(sh, node, world) {
  let shown = true;
  for (let o = node; o && shown; o = o.parent) shown = o.visible;
  if (!shown) return (sh.mesh.visible = false);
  _sm.copy(world.matrixWorld).invert().multiply(node.matrixWorld);
  _sp.setFromMatrixPosition(_sm);
  _sf.setFromMatrixColumn(_sm, 2).normalize();
  _su.setFromMatrixColumn(_sm, 1).normalize();
  const cx = _sp.x + sh.dx, cz = _sp.z + sh.dz, k = 1 - (levelHeight(cx, cz) - _sp.y) / 7;
  sh.mesh.visible = k > 0;
  if (!sh.mesh.visible) return;
  sh.mesh.material.opacity = Math.min(1, k);
  _ss.crossVectors(_su, _sf).multiplyScalar(_su.y < 0 ? -sh.halfW : sh.halfW);
  const p = sh.mesh.geometry.attributes.position;
  let i = 0;
  for (const r of [1, 0, -1]) for (const c of [-1, 0, 1]) {
    const x = cx + _sf.x * sh.halfL * r + _ss.x * c, z = cz + _sf.z * sh.halfL * r + _ss.z * c;
    p.setXYZ(i++, x, levelHeight(x, z) - 0.02, z);
  }
  p.needsUpdate = true;
}
const _cm = new THREE.Matrix4(), _cax = new THREE.Vector3(), CAM_FLIP = new THREE.Matrix4().makeRotationX(Math.PI);
function axisAngle(m, axis, deg) {
  _cax.set(...axis);
  if (!deg || _cax.lengthSq() < 1e-12) return m.identity();
  return m.makeRotationAxis(_cax.normalize(), (deg * Math.PI) / 180);
}
// Event type 4 (spawnCutsceneEffectsAndCues) cuts to camera entry p0 from its frame on; before any, the first entry in use is live.
const CUT_FADE = 0.8;
// weapons_HOB laser model per dispatchCueByCategory category (loadWeaponsHobObjects 0x8005E7EC; the guided / spray categories are not drawn).
const LASER_MODELS = { 0: 'red_laser', 1: 'red_laser', 2: 'green_laser', 3: 'red_laser', 4: 'green_laser', 5: 'red_laser', 6: 'red_laser', 12: 'ion_laser' };
// Laser light (byteAnimatedDebrisNpcHandler 0x8004C6C8, spawned with each bolt off Taloraan): it tints the terrain under the bolt with a
// 16-colour run of data/lights_PAL (0x20 red, 0x10 green, 0x30 ion; size 1.3). Drawn as a soft additive glow on the ground.
// shortcut: the cycle rate (30 colours/s), the radial falloff and the fade with height above the ground are by eye.
const LASER_GLOW = { red_laser: 0x20, green_laser: 0x10, ion_laser: 0x30 };
let glowTex = null;
function glowTexture() {
  if (glowTex) return glowTex;
  const c = elem('canvas', { width: 64, height: 64 }), g = c.getContext('2d'), r = g.createRadialGradient(32, 32, 0, 32, 32, 32);
  r.addColorStop(0, 'rgba(255,255,255,1)');
  r.addColorStop(1, 'rgba(255,255,255,0)');
  g.fillStyle = r;
  g.fillRect(0, 0, 64, 64);
  return (glowTex = new THREE.CanvasTexture(c));
}
const _bs = new THREE.Vector3(), _bp = new THREE.Vector3(), _bq = new THREE.Quaternion();
// The mission's moving units stay hidden during a cutscene unless "Level units" is ticked; buildings and fixed objects are left alone.
function syncCutNpcs() {
  // A sequence's moving targets are its actors, so their level copies hide.
  for (const o of root?.children ?? []) if (o.userData.npc) o.visible = (!cut || cut.seq || $('cutNpcs').checked) && !cut?.data.hidden?.includes(o.userData.placedItem?.name);
}
$('cutNpcs').onchange = syncCutNpcs;
function cutCameraAt(data, t) {
  let c = data.camera, at = -1;
  for (const e of data.events) if (e.type === 4 && e.frame <= t * data.frames && e.frame >= at && data.cameras[e.p[0]]) [c, at] = [data.cameras[e.p[0]], e.frame];
  return c;
}
// Fireworks, event type 15 (spawnCutsceneDebrisField 0x800AAC88): cases 8 and 9 add an emitter slot (insertCutsceneSlot) that, every
// 0.3 s of its life, launches an i_missile rocket from centre + ((rand & 0x7F) - 64) / 64 * extent per axis. The rocket (slot type 4,
// npcIncomingMissileUpdate 0x800ABD0C) rises 1.5 units as 1.5 sin((1 - t) pi / 2), t += 1.1 dt, then bursts (spawnNpcBurstAlongSegment
// case 6): 30 starex sparks at 80/s, up 0.25 units/s tilted +-14 deg about X and Z, gravity 0.25 down, 3 s life, size 30/256 shrinking
// 9/256 per s, coloured by one of three 32-step ramps (buildCutsceneGradientBuffers 0x800AC574: odd steps lit, even dark).
// shortcut: the game's RNG seed is not known, so the pattern differs; blending is drawn additive (the particle renderer is not traced).
const FIREWORK_EMITTERS = { 8: { c: [82.5, -1.6, 77.5], e: [1.6, 0.3, 1.6], life: 6 }, 9: { c: [144.3, -12.5, 73.0], e: [1.6, 0.8, 1.6], life: 20 } };
// emitters: [{ at: start s, c: centre, e: half-extents, life: s }] (game space, Y down).
async function buildFireworks(emitters, world, weapons, counts) {
  if (!emitters.length) return null;
  let seed = 0x12345678;
  const rand = () => (((seed = Math.imul(seed, 0xA8351D63) >>> 0) >>> 6) & 0x7FFF);
  const ramps = [0, 1, 2].map((r) => [...Array(32)].map((_, j) => {
    const v = (255 - 7 * j) / 255;
    if (!(j & 1)) return r === 2 ? [0x50 / 255, 0x50 / 255, 0xC8 / 255, 0xC8 / 255] : [0, 0, 0, 0];
    return r === 0 ? [v, v, v, v] : r === 1 ? [0, v, 0, 1] : [v, Math.floor((255 - 7 * j) / 3) / 255, 0, 1];
  }));
  const rockets = [], sparks = [];
  for (const em of emitters) {
    const t0 = em.at;
    for (let k = 1; 0.3 * k < em.life; k++) {
      const at = t0 + 0.3 * k, p = em.c.map((c, a) => c + ((rand() & 0x7F) - 64) / 64 * em.e[a]);
      // Each launch and burst plays its sound on a rand & 1 roll (play3DSoundEvent: launch 0x30 or 0x35 at 0x55, burst 0x0C at 0x78).
      rockets.push({ at, p, launchSfx: rand() & 1 ? (rand() & 1 ? 0x30 : 0x35) : 0, burstSfx: rand() & 1 ? 0x0C : 0 });
      const burst = at + 1 / 1.1, ramp = (rand() << 16 >> 16) % 3;
      for (let i = 0; i < 30; i++) {
        const q = new THREE.Quaternion().setFromAxisAngle(new THREE.Vector3(1, 0, 0), THREE.MathUtils.degToRad((rand() % 28) - 14))
          .multiply(new THREE.Quaternion().setFromAxisAngle(_zAxis, THREE.MathUtils.degToRad((rand() % 28) - 14)));
        const ring = new THREE.Vector3(1, 0, 0).applyAxisAngle(new THREE.Vector3(0, 1, 0), THREE.MathUtils.degToRad(rand() % 360)).multiplyScalar(0.02 * (rand() % 100) / 100);
        sparks.push({ at: burst + i / 80, p: new THREE.Vector3(...p).add(ring), v: new THREE.Vector3(0, -0.25, 0).applyQuaternion(q), ramp: ramps[Math.abs(ramp)] });
      }
    }
  }
  const lobj = weapons && weapons.hob.objects.find((o) => o.name === 'i_missile');
  for (const r of rockets) {
    r.mesh = lobj && buildObject(lobj, weapons, 'data/weapons', counts);
    if (!r.mesh) continue;
    r.mesh.scale.setScalar(0.1 * HOB_TO_WORLD);
    // shortcut: the game's 90 degree turn (composeRotationMatrixSlot0) is taken as nose up.
    r.mesh.rotation.x = Math.PI / 2;
    r.mesh.visible = false;
    world.add(r.mesh);
  }
  const star = parseSpriteAnim(await rom.read('data/part_spr')).find((f) => f.rgba && f.name.startsWith('starex'));
  const map = star && uprightTex(star);
  // Pool sized for the busiest moment (about 10 bursts alive at once).
  const pool = [...Array(400)].map(() => {
    const s = new THREE.Sprite(new THREE.SpriteMaterial({ map, transparent: true, depthWrite: false, fog: false, blending: THREE.AdditiveBlending }));
    s.visible = false;
    world.add(s);
    return s;
  });
  return { rockets, sparks, pool };
}
// Cutscene effects on the shared effect kit, in a Y-up group under the cutscene world (game (x, y, z) -> (x, -y, -z)), landing on the terrain.
// Type 5 (spawnNpcType0xAFromTemplate) at actor p0's position, S = p1 / 500 (1 if 0), kind p2: 0 fire explosion, 1 ion burst (A S, B 2S),
// 2 three shock_sml rings, 3 eight debris1 chunks 75-90 deg from up at 0.3-0.8 units/s. Type 16 (spawnNpcBurstAlongSegment) cases 2 / 3:
// an emitter riding actor p0, 8/s up to 100, 1 s, drag 0.8, 0.25 -> 0.33, rising at 1.75 / 1.5 (fire: 11 explo frames, part_pal ramp 1;
// smoke: 20 frames), replaced by the actor's next type 16. shortcut: kind 0's lights_PAL flash, kind 4 (water spray), the meshless
// quarter of kind 3's chunks with their fire trails, type 16's tilt with the actor, and type 15 cases 0-7 are not drawn.
// impacts: bolt hits [{ at: s, pos: game space, cat }] -> 0x80076250 by weapon type: 10/11/13/14 one fire particle at 0.6, 12 an ion particle
// (A 0.4, B 0.8), others an ember burst (0.05, 0.2) and a fire particle at 0.2 (the Effects tab's Laser / Ion / Small impact).
async function buildCutFx(data, world, nodes, impacts = []) {
  const events = data.events.filter((e) => (e.type === 5 && e.p[2] <= 4) || (e.type === 16 && [2, 3].includes(e.p[1] & 0xff)) || (e.type === 15 && e.p[0] <= 7));
  if (!events.length && !impacts.length) return null;
  const group = new THREE.Group();
  group.rotation.x = Math.PI;
  world.add(group);
  const kit = await fxKit(group, (x, z) => -levelHeight(x, -z));
  const actorAt = (i, t) => {
    poseCutscene(t / data.length);
    world.updateMatrixWorld(true);
    const p = world.worldToLocal(nodes[i].getWorldPosition(new THREE.Vector3()));
    return new THREE.Vector3(p.x, -p.y, -p.z);
  };
  const spawns = [], destroyed = [];
  // Kind 4, spawnWaterSprayDebrisAndBurst 0x80077858 (enabled in every cutscene: load_cutscene calls initWaterSprayEffectAndSfx): the wtrspray
  // splash (x/z S/10 -> S, height S sin(phase) from asin(0.1) to pi, alpha 1 - t over 1 s) with a shock_sml ring from 0.4 s, plus a ring
  // 0.03S -> 0.6S over 2.2 s and the wsm_f01 spray (0x800403EC: 60/s up to 20, up 0.4S, +-15 deg, 0.5 s, size 0.5k -> 1.5k, k = 0.75S d / 4.5,
  // part_pal ramp 2, 3 on level 4). shortcut: the rings sit at the splash point; the game drops them by D_80136FA8 + 0.05 (the start item's height).
  const sprayRamp = /lv_4$/.test(cutLevel ?? '') ? 3 : 2;
  const water = (pos, S) => {
    kit.splash(pos, { sx: S / 10, sy: S / 10, grow: 0.9 * S, rate: 1, amp: S });
    kit.shockwave(pos, [0, 0, 0.03 * S, 0.6 * S, 1, 2.2, 0xffffff, [0, 1, 0]]);
    ticker(0.4, 0.4, () => kit.shockwave(pos, [0, 0, 0.01, 0.6 * S, 1, 2.2, 0xffffff, [0, 1, 0]]));
    const k = (0.75 * S * kit.d) / 4.5;
    kit.emitter(pos, { rate: 60, max: 20, vel: 0.4 * S, spread: 30, scatter: 0.025, drag: 0.05, life: 0.5, size0: 0.5 * k, size1: 1.5 * k, frames: [14], ramp: sprayRamp });
  };
  // Type 15 case 7's spawnEffectNpcWithRampedScaleExt 0x800776EC (heavyRandomBurstNpcHandler): one starex billboard, t += 5 dt, e = 1 - cos(t pi/2),
  // width 10 + 90e by height 10 + 5e, opaque until t 0.5 then fading out. shortcut: sprite units drawn as tenths of a world unit and additive;
  // the sprite bucket's scale and blend are not traced, nor the drift toward the camera (0.45/s).
  const flare = (pos) => {
    const s = kit.sprite(kit.spr[16].map);
    s.position.copy(pos);
    let t = 0;
    kit.items.push({ obj: s, step: (dt) => {
      t += 5 * dt;
      if (t >= 1) return false;
      const e = 1 - Math.cos((t * Math.PI) / 2);
      s.scale.set((10 + 90 * e) / 10, (10 + 5 * e) / 10, 1);
      s.material.opacity = t > 0.5 ? (1 - t) / 0.5 : 1;
      return true;
    } });
  };
  // Kind 0's light (spawnByteAnimDebrisDefault 0x8005EBD8): a point light 1 unit above the blast, radius 6, stepping lights_PAL entries
  // 0-15 at 20/s and gone after the run. Drawn like the laser light, as a glow on the terrain below, fading with height above it.
  const lights = rom.assets.has('data/lights_PAL') && (await rom.read('data/lights_PAL'));
  const flash = (pos) => {
    const floor = -levelHeight(pos.x, -pos.z), above = pos.y + 1 - floor;
    if (!lights || above > 6) return;
    const mat = new THREE.MeshBasicMaterial({ map: glowTexture(), transparent: true, blending: THREE.AdditiveBlending, depthWrite: false, fog: false });
    const glow = new THREE.Mesh(new THREE.PlaneGeometry(12, 12).rotateX(-Math.PI / 2), mat);
    glow.position.set(pos.x, floor + 0.03, pos.z);
    group.add(glow);
    let t = 0;
    kit.items.push({ obj: glow, step: (dt) => {
      t += dt;
      const k = Math.floor(t * 20);
      if (k > 15) { glow.geometry.dispose(); return false; }
      mat.color.setRGB(lights.getUint8(k * 4) / 255, lights.getUint8(k * 4 + 1) / 255, lights.getUint8(k * 4 + 2) / 255, THREE.SRGBColorSpace);
      mat.opacity = 0.45 * (1 - Math.max(0, above) / 6);
      return true;
    } });
  };
  for (const e of events) {
    if (e.type === 15) continue;
    const i = e.p[0] & 0xff, at = e.frame / data.fps;
    if (!nodes[i]) continue;
    if (e.type === 5) {
      const pos = actorAt(i, at), S = e.p[1] ? e.p[1] / 500 : 1;
      spawns.push({ at, run: () => {
        if (e.p[2] === 0) {
          kit.particle({ kind: 'fire', s: S }, pos, kit.fire);
          flash(pos);
        }
        else if (e.p[2] === 1) kit.particle({ kind: 'ion1', A: S, B: 2 * S }, pos, kit.ion2);
        else if (e.p[2] === 2) for (const ring of SHOCKS['shock:cutscene'].rings(S)) kit.shockwave(pos, ring);
        else if (e.p[2] === 4) water(pos, S);
        else for (let k = 0; k < 8; k++) {
          const tilt = THREE.MathUtils.degToRad(75 + rnd() * 15), az = rnd() * Math.PI * 2, speed = 0.3 + rnd() * 0.5;
          const vel = new THREE.Vector3(Math.sin(tilt) * Math.cos(az), Math.cos(tilt), Math.sin(tilt) * Math.sin(az)).multiplyScalar(speed);
          if (k % 4) kit.chunk(pos.clone(), vel, true, 0.2 * ((Math.floor(rnd() * 128) - 64) / 256 + 0.7));
        }
      } });
    } else {
      const fire = (e.p[1] & 0xff) === 2, next = data.events.find((n) => n.type === 16 && (n.p[0] & 0xff) === i && n.frame > e.frame);
      const end = Math.min(next ? next.frame / data.fps : data.length, at + 100 / 8), path = [];
      for (let t = at; t <= end + 1; t += 0.1) path.push(actorAt(i, t));
      spawns.push({ at, run: () => kit.emitter(null, {
        at: (t) => path[Math.min(path.length - 1, Math.round(t * 10))], rate: 8, max: Math.floor((end - at) * 8), dir: [0, fire ? 1.75 : 1.5, 0],
        spread: 0, scatter: 0, drag: 0.8, life: 1, size0: 0.25, size1: 0.33, art: kit.fire, frames: [...Array(fire ? 11 : 20).keys()], ramp: fire ? 1 : null,
      }) });
    }
  }
  // Type 15 cases 0-7 (spawnCutsceneDebrisField 0x800AAC88): blasts on a level building found by mesh name, or at fixed points for the logo.
  // Slot records (insertCutsceneSlot, ticked at 30 Hz by npcIncomingMissileUpdate): type 1 = each tick with chance 1/sub, a fire explosion at
  // centre +- extent, scale base +- var; type 2 = each tick 3 tries at 50% of a debris1 chunk within 35 deg of up; type 3 = every 0.075 s a
  // shock_sph ring (0.5 -> 3, 0.35 s) and 8 fire explosions within +-1.5 x / +-2.5 z, scale 1.75-6.2. Cases 1 -> 3 -> 4 fall through, and
  // each destroys its building. shortcut: debris speeds for slot type 2 and case 3 are a viewer choice where the trace left them open, and
  // case 7's spawnEffectNpcWithRampedScaleExt is not drawn.
  const fxv = (g) => new THREE.Vector3(g[0], -g[1], -g[2]), sml = (end, Y, life, up = [0, 1, 0]) => [0, 0, 0.5, end, Y, life, 0xffffff, up], sph = (end, Y, life) => [2, 0, 0.5, end, Y, life];
  const building = (name) => {
    let found = null;
    world.traverse((o) => { if (!found && o.userData.hobObject && o.name === name) found = o; });
    if (!found) return null;
    const p = world.worldToLocal(found.getWorldPosition(new THREE.Vector3()));
    return { obj: found, pos: [p.x, p.y, p.z], r: new THREE.Box3().setFromObject(found).getSize(new THREE.Vector3()).length() / 2 };
  };
  const ticker = (life, every, tick) => {
    let t = 0, acc = 0;
    kit.items.push({ obj: new THREE.Object3D(), step: (dt) => {
      t += dt;
      for (acc += dt; acc >= every; acc -= every) tick();
      return t < life;
    } });
  };
  const fireAt = (c, ext, base, vary) => kit.particle({ kind: 'fire', s: Math.max(0.2, base + (rnd() * 2 - 1) * vary) }, c.clone().add(new THREE.Vector3(...ext.map((x) => (rnd() * 2 - 1) * x))), kit.fire);
  const slot1 = (c, sub, ext, base, vary, life) => ticker(life, 1 / 30, () => rnd() * sub < 1 && fireAt(c, ext, base, vary));
  const cone = (deg, lo, hi) => {
    const tilt = THREE.MathUtils.degToRad(rnd() * deg), az = rnd() * Math.PI * 2;
    return new THREE.Vector3(Math.sin(tilt) * Math.cos(az), Math.cos(tilt), Math.sin(tilt) * Math.sin(az)).multiplyScalar(lo + rnd() * (hi - lo));
  };
  const slot2 = (c, life) => ticker(life, 1 / 30, () => { for (let k = 0; k < 3; k++) if (rnd() < 0.5) kit.chunk(c.clone(), cone(35, 1.5, 3)); });
  const slot3 = (c, life) => ticker(life, 0.075, () => {
    const at = c.clone().add(new THREE.Vector3(0, 0, (rnd() * 2 - 1) * 15));
    kit.shockwave(at, sph(3, 1, 0.35));
    for (let k = 0; k < 8; k++) fireAt(at, [1.5, 0, 2.5], 3.975, 2.225);
  });
  for (const e of data.events.filter((ev) => ev.type === 15 && ev.p[0] <= 7)) {
    const at = e.frame / data.fps, c = e.p[0];
    if (c === 0) {
      const b = building('b_fac_hi');
      if (b) spawns.push({ at, run: () => slot1(fxv([b.pos[0], b.pos[1] - 0.5 * b.r, b.pos[2]]), 3, [4, 5, 4], b.r * 0.25, 6, 6) });
      continue;
    }
    const chain = c === 1 ? [1, 3, 4] : c === 3 ? [3, 4] : c === 4 ? [4] : [c];
    for (const k of chain) {
      if (k <= 4) {
        const b = building({ 1: 'b_fac_hi', 3: 'b_pwrtran_hi', 4: 'b_20sb_hi' }[k]);
        if (!b) continue;
        destroyed.push({ obj: b.obj, at });
        const P = (dy) => fxv([b.pos[0], b.pos[1] - dy, b.pos[2]]);
        spawns.push({ at, run: () => {
          if (k === 1) {
            [sml(15, 1.5, 0.7), sml(14, 1.5, 1.4), sml(13, 1.5, 2.1), sph(12, 0.75, 0.25)].forEach((r) => kit.shockwave(P(3), r));
          } else if (k === 3) {
            [sml(15, 1.5, 0.7), sml(14, 1.5, 1.4), sml(13, 1.5, 2.1)].forEach((r) => kit.shockwave(P(6), r));
            for (let j = 0; j < 12; j++) kit.chunk(P(6), cone(120, 2, 4), true, 0.45 + rnd() * 0.07);
          } else {
            [sml(15, 1.5, 0.7), sml(14, 1.5, 1.4), sml(13, 1.5, 2.1), sph(8, 1, 0.35)].forEach((r) => kit.shockwave(P(7), r));
            for (const o of [[-3.2, 4, -1.2], [2.5, 4, -0.8], [-1, 4, 1.2], [2.8, 4, 1.7]]) {
              const src = fxv([b.pos[0] + o[0], b.pos[1] - o[1], b.pos[2] + o[2]]);
              kit.emitter(src, { rate: 8, max: 100, dir: [0, 1.75, 0], spread: 0, scatter: 0, drag: 0.8, life: 1, size0: 2.75, size1: 11, art: kit.fire, frames: [...Array(11).keys()] });
            }
          }
        } });
      } else if (k === 5) {
        const P = fxv([47.98, 1.01, 38.24]);
        spawns.push({ at, run: () => {
          for (let j = 0; j < 12; j++) kit.chunk(P.clone(), cone(60, 1, 2));
          [sph(0.84, 1, 0.55), [2, 0, 0.5, 1.26, 1, 0.5], [2, 0, 0.3, 1.68, 1, 0.45]].forEach((r) => kit.shockwave(P, r));
          slot2(P, 3);
          slot1(P, 1, [0.4, 0.4, 0.4], 1.7, 0.8, 1.5);
        } });
      } else if (nodes[36]) {
        const a = actorAt(36, at), P = (dy) => new THREE.Vector3(a.x - 1.25, a.y + dy, a.z - 0.5);
        spawns.push({ at, run: () => {
          if (k === 6) {
            slot3(P(0), 3.5);
            slot1(P(0), 2, [9, 0.01, 4.5], 5, 2, 2.5);
            slot1(P(0), 1, [9 * 0.7077, 0.01, 4.5 * 0.7077], 5, 2, 2);
            [22, 21, 20, 19].forEach((end, j) => kit.shockwave(P(0), sml(end, 1.5, 1.2 * (j + 1), [1, -0.1, -0.1])));
          } else {
            kit.shockwave(P(-2.9), [1, 0, 0.5, 6, 6.5, 1.5, 0x8c1e1e]);
            kit.shockwave(P(-2.9), [1, 0, 0.5, 5, 5, 1, 0x8c1e1e]);
            flare(P(-2.9));
          }
        } });
      }
    }
  }
  for (const h of impacts) {
    const pos = new THREE.Vector3(h.pos.x, -h.pos.y, -h.pos.z);
    spawns.push({ at: h.at, run: () => {
      if ([10, 11, 13, 14].includes(h.cat)) kit.particle({ kind: 'fire', s: 0.6 }, pos, kit.fire);
      else if (h.cat === 12) kit.particle({ kind: 'ion1', A: 0.4, B: 0.8 }, pos, kit.ion2);
      else { kit.embers(pos, 0.05, 0.2); kit.particle({ kind: 'fire', s: 0.2 }, pos, kit.fire); }
    } });
  }
  spawns.sort((a, b) => a.at - b.at);
  return { kit, spawns, destroyed, t: Infinity, next: 0 };
}
// Effects step forward in 1/30 s ticks; scrubbing back (or a jump of over half a second) replays them from the start, as the Effects tab seeks.
function poseCutFx(fx, T) {
  for (const d of fx.destroyed) d.obj.visible = T < d.at;
  const items = fx.kit.items;
  if (T < fx.t || T - fx.t > 0.5) {
    for (const it of items) { it.obj.parent?.remove(it.obj); it.obj.material?.dispose(); }
    items.length = 0;
    seedRnd(7);
    fx.t = 0;
    fx.next = 0;
  }
  while (fx.t < T) {
    const dt = Math.min(1 / 30, T - fx.t);
    fx.t += dt;
    while (fx.next < fx.spawns.length && fx.spawns[fx.next].at <= fx.t) fx.spawns[fx.next++].run();
    for (let i = items.length - 1; i >= 0; i--) if (!items[i].step(dt)) { items[i].obj.parent?.remove(items[i].obj); items[i].obj.material?.dispose(); items.splice(i, 1); }
  }
}
// npcVortexUpdate 0x80081ABC each tick: the hardpoint's world frame on the devastator, dir spun about the down axis (-up) 2 deg a tick,
// length sy = (H - footY + 0.7) / 5 * (2.5 + 0.3 sin 3T) (H = terrain height below, T = cutscene seconds); matrix X = (up x dir) * 5,
// Y = down * sy, Z = dir * 5 on the 1/256 mesh. The crown sits at (x, H, z) scaled (4.5, 3 + (sin 2pi a + 1) / 2 * (sin 2pi b + 1), 4.5),
// a += dt, b += 1.6 dt. shortcut: ticks taken as 30/s.
const _vp = new THREE.Vector3(), _vd = new THREE.Vector3(), _vu = new THREE.Vector3(), _vx = new THREE.Vector3(), _vm = new THREE.Matrix4();
function poseVortices(T) {
  const world = root;
  world.updateMatrixWorld(true);
  const local = (node, v, out) => world.worldToLocal(node.localToWorld(out.set(v[0] * 256, v[1] * 256, v[2] * 256)));
  for (const v of cut.vortices) {
    let shown = true;
    for (let o = v.actor; o && o !== world; o = o.parent) shown &&= o.visible;
    v.vortex.visible = v.spray.visible = shown;
    if (!shown) continue;
    local(v.node, v.rec.pos, _vp);
    local(v.node, v.rec.pos.map((x, a) => x + v.rec.dir[a]), _vd).sub(_vp).normalize();
    local(v.node, v.rec.pos.map((x, a) => x + v.rec.up[a]), _vu).sub(_vp).normalize();
    const down = _vu.clone().negate();
    _vd.applyAxisAngle(down, THREE.MathUtils.degToRad((T * 60) % 360));
    const H = levelHeight(_vp.x, _vp.z), sy = Math.max(0.001, ((H - _vp.y + 0.7) / 5) * (2.5 + 0.3 * Math.sin(3 * T)));
    _vx.crossVectors(_vu, _vd).normalize();
    v.vortex.matrix.makeBasis(_vx.multiplyScalar(5), down.multiplyScalar(sy), _vd.clone().multiplyScalar(5)).multiply(_vm.makeScale(HOB_TO_WORLD, HOB_TO_WORLD, HOB_TO_WORLD)).setPosition(_vp);
    const a = (v.a + T) % 1, b = (v.b + 1.6 * T) % 1, h = 3 + ((Math.sin(2 * Math.PI * a) + 1) / 2) * (Math.sin(2 * Math.PI * b) + 1);
    v.spray.matrix.makeScale(4.5 * HOB_TO_WORLD, h * HOB_TO_WORLD, 4.5 * HOB_TO_WORLD).setPosition(_vp.x, H, _vp.z);
    v.vortex.matrixWorldNeedsUpdate = v.spray.matrixWorldNeedsUpdate = true;
  }
}
function poseFireworks(fw, T) {
  for (const r of fw.rockets) {
    if (!r.mesh) continue;
    const t = (T - r.at) * 1.1;
    r.mesh.visible = t >= 0 && t <= 1;
    if (r.mesh.visible) r.mesh.position.set(r.p[0], r.p[1] + 1.5 * Math.sin((1 - t) * Math.PI / 2), r.p[2]);
  }
  let n = 0;
  for (const s of fw.sparks) {
    const age = T - s.at;
    if (age < 0 || age >= 3 || n >= fw.pool.length) continue;
    const c = s.ramp[Math.min(31, Math.floor((age / 3) * 32))];
    if (!c[3]) continue;
    const sp = fw.pool[n++];
    sp.visible = true;
    sp.position.copy(s.p).addScaledVector(s.v, age);
    sp.position.y += 0.5 * 0.25 * age * age;
    sp.scale.setScalar(Math.max(1e-4, 30 / 256 - (9 / 256) * age));
    sp.material.color.setRGB(c[0], c[1], c[2]);
    sp.material.opacity = c[3];
  }
  for (; n < fw.pool.length; n++) fw.pool[n].visible = false;
}
// big_cor: u16 format 0x8001 (CI8, RGBA palette), u16 0, u16 w, u16 h, u32 0, 256 RGBA palette entries, then w * h indices.
function ceremonyPlanet(dv) {
  const w = dv.getUint16(4), h = dv.getUint16(6), px = 12 + 1024;
  if (dv.getUint16(0) !== 0x8001 || px + w * h > dv.byteLength) return null;
  const rgba = new Uint8Array(w * h * 4);
  for (let i = 0; i < w * h; i++) rgba.set(new Uint8Array(dv.buffer, dv.byteOffset + 12 + dv.getUint8(px + i) * 4, 4), i * 4);
  const tex = new THREE.DataTexture(rgba, w, h);
  tex.flipY = true;
  tex.colorSpace = THREE.SRGBColorSpace;
  tex.needsUpdate = true;
  // Transparent (it fades) so it draws after the opaque scene; parked near the far plane, the depth test keeps it behind every model.
  return new THREE.Mesh(new THREE.PlaneGeometry(1, 1), new THREE.MeshBasicMaterial({ map: tex, transparent: true, depthWrite: false, fog: false }));
}
// tickCutsceneCameraFromTimeline 0x800AD690: big_cor at scale 640 * 0.8 / 240 by 1 in 640 x 240 screen units, centred; shown while frame < 420
// at tier 0 or < 200 at tier >= 1, fading over 170-200. Sized against a 4:3 frame: 128/240 of the height, 1.0667 times as wide.
function poseCeremony(f) {
  if (cut.terrain) cut.terrain.visible = f >= 420;
  const p = cut.planet;
  if (!p) return;
  p.visible = cut.tier ? f < 200 : f < 420;
  p.material.opacity = cut.tier ? THREE.MathUtils.clamp((200 - f) / 30, 0, 1) : 1;
  const d = camera.far * 0.9, hgt = 2 * d * Math.tan(THREE.MathUtils.degToRad(camera.fov) / 2) * (128 / 240);
  p.position.z = -d;
  p.scale.set(hgt * 1.0667, hgt, 1);
}
function endCutscene() {
  if (!cut) return;
  if (cut.planet) camera.remove(cut.planet);
  if (cut.crawl) {
    scene.fog = null;
    if (gizmo.object === editProxy) gizmo.detach();
    editProxy.removeFromParent();
    $('cutInspector').classList.remove('crawlMode');
    $('insCut').querySelector('h4').textContent = 'Cutscene';
  }
  hoverBox.visible = selBox.visible = false;
  if (gizmo.object === editProxy && cutTrack) {
    gizmo.detach();
    editProxy.removeFromParent();
  }
  $('help').hidden = false;
  $('cutFade').hidden = $('cutBars').hidden = $('craftRow').hidden = true;
  stopCutAudio();
  // Pause suspends the shared audio context; leave it running for the other views.
  if (audioCtx?.state === 'suspended') audioCtx.resume();
  // The level view keeps its own fog toggle; the cutscene's (and its type-17 range changes) end with it.
  const fogOn = cut.levelFogOn;
  cut = null;
  restoreCutFar();
  if (fogOn != null) {
    $('optFog').checked = fogOn;
    syncFogControls();
    applyFog();
  }
  $('cutPanel').hidden = $('cutInspector').hidden = true;
  controls.enabled = true;
  camera.fov = 50;
  camera.updateProjectionMatrix();
}
// Cutscene sound effects. Event type 3: sfx = u16 table at 0x800AF6E8 [p0], volume p1 & 0xFF (0x7F if 0), pan p2 & 0xFF (0x40 if 0).
// Engine loops (emit3DSoundsForActiveNpcs 0x800A9D70): each actor whose asset flags (+0x5B) have bit 1 plays a positional sfx picked by
// asset type: [sfx, range, volume]. Walkers have none; their footsteps are type-3 events.
const CUT_SFX = 0x137580 + (0x800AF6E8 - 0x800A5130);
// shortcut: engine loops mixed at 40% under the event sounds by ear; MusyX's 3D volume and voice priorities are not traced.
const ENGINE_GAIN = 0.4;
const CUT_ENGINES = {
  awing: [0x0f, 10, 0x78], vwing: [0x12, 10, 0x78], xwing: [0x0e, 10, 0x78], ywing: [0x11, 10, 0x78], snowspeeder: [0x13, 10, 0x78],
  falcon: [0x2e, 12, 0x78], koelsch: [0x6f, 10, 0x5a], tie_fighter: [7, 21, 0x7f], tie_bomber: [7, 21, 0x7f], tie_inter: [7, 21, 0x7f],
  tie_d: [7, 21, 0x7f], wrld_dvstr: [0x38, 34, 0x7f], imp_shuttle: [0x39, 10, 0x5a], v_hls_hi: [0x39, 10, 0x5a], raptor: [0x39, 10, 0x5a],
};
function sfxBuffer(id) {
  const row = audioBank?.sfx.find((r) => r.index === id);
  if (!row) return null;
  const pcm = rowPcm(row), buf = audioCtx.createBuffer(1, Math.max(1, pcm.length), row.rate), ch = buf.getChannelData(0);
  for (let i = 0; i < pcm.length; i++) ch[i] = pcm[i] / 32768;
  return buf;
}
// Firework sounds are 3D (range 24, curve 0.2): gain = max * (1 - f), f = 0.8x + 0.2x^2, x = distance / range, silent past range.
// shortcut: the distance is taken once, from the cutscene camera at the cue's time, and the sound is not panned.
function fireworkCues(c, buf) {
  const out = [], d = c.data;
  for (const r of c.fireworks?.rockets ?? []) {
    for (const [id, at, max, dy] of [[r.launchSfx, r.at, 0x55, 1.5], [r.burstSfx, r.at + 1 / 1.1, 0x78, 0]]) {
      const b = id && buf(id), t = at / d.length;
      if (!b) continue;
      const cam = cutCameraAt(d, t), x = Math.hypot(...[0, 1, 2].map((k) => cutsceneValue(cam[k], t, d.frames, 8) * 0.25 - (r.p[k] + (k === 1 ? dy : 0)))) / 24;
      if (x <= 1) out.push({ at, buf: b, vol: (max / 127) * (1 - (0.8 * x + 0.2 * x * x)), pan: 0 });
    }
  }
  return out;
}
function loadCutSfx(c) {
  const bufs = new Map(), buf = (id) => (bufs.has(id) ? bufs.get(id) : bufs.set(id, sfxBuffer(id)).get(id));
  c.sfxCues = c.data.events.filter((e) => e.type === 3).map((e) => ({
    at: e.frame / c.data.fps, buf: buf((romBytes[CUT_SFX + e.p[0] * 2] << 8) | romBytes[CUT_SFX + e.p[0] * 2 + 1]),
    vol: ((e.p[1] & 0xff) || 0x7f) / 127, pan: (((e.p[2] & 0xff) || 0x40) - 64) / 64,
  })).concat(c.data.events.filter((e) => e.type === 8 && (e.p[3] & 0xffff)).map((e) => ({ at: e.frame / c.data.fps, buf: buf(e.p[3] & 0xffff), vol: 1, pan: 0 })))
    .filter((s) => s.buf).concat(fireworkCues(c, buf));
  c.engines = c.data.actors.flatMap((a, i) => {
    const e = a.model && CUT_ENGINES[cutBase(a.name)], b = e && buf(e[0]);
    return b ? [{ node: c.nodes[i], buf: b, range: e[1], vol: e[2] / 127 }] : [];
  });
}
// shortcut: engine falloff is an inverse curve with the game's range as its reference distance; play3DSoundEvent's own curve is not traced.
function startCutSfx(c, T, play, srcs) {
  for (const s of c.sfxCues) {
    if (s.at + s.buf.duration <= T) continue;
    const g = audioCtx.createGain(), pn = audioCtx.createStereoPanner();
    g.gain.value = s.vol;
    pn.pan.value = s.pan;
    g.connect(pn).connect(c.sfxGain);
    play(s.buf, g, s.at, 0);
  }
  for (const e of c.engines) {
    e.panner ??= Object.assign(audioCtx.createPanner(), { panningModel: 'equalpower', distanceModel: 'inverse', refDistance: e.range, rolloffFactor: 1 });
    e.gain ??= audioCtx.createGain();
    e.gain.gain.value = e.vol;
    e.gain.connect(e.panner).connect(c.sfxGain);
    const s = audioCtx.createBufferSource();
    s.buffer = e.buf;
    s.loop = true;
    s.connect(e.gain);
    s.start();
    srcs.push(s);
  }
}
const _lp = new THREE.Vector3(), _lf = new THREE.Vector3(), _lu = new THREE.Vector3();
function updateCutSfxPositions() {
  if (!cut?.engines?.length || !audioCtx) return;
  const L = audioCtx.listener;
  camera.getWorldPosition(_lp);
  camera.getWorldDirection(_lf);
  _lu.set(0, 1, 0).applyQuaternion(camera.quaternion);
  if (L.positionX) {
    [L.positionX.value, L.positionY.value, L.positionZ.value] = _lp.toArray();
    [L.forwardX.value, L.forwardY.value, L.forwardZ.value] = _lf.toArray();
    [L.upX.value, L.upY.value, L.upZ.value] = _lu.toArray();
  } else {
    L.setPosition(..._lp.toArray());
    L.setOrientation(..._lf.toArray(), ..._lu.toArray());
  }
  // One voice per engine sound, like the console's limited voices: only the nearest visible actor using it plays; copies stay silent.
  const cam = _lp.clone(), nearest = new Map();
  for (const e of cut.engines) {
    if (!e.panner) continue;
    let shown = true;
    for (let o = e.node; o && shown; o = o.parent) shown = o.visible;
    e.node.getWorldPosition(_lp);
    [e.panner.positionX.value, e.panner.positionY.value, e.panner.positionZ.value] = _lp.toArray();
    e.dist = shown ? _lp.distanceTo(cam) : Infinity;
    if (e.dist < (nearest.get(e.buf)?.dist ?? Infinity)) nearest.set(e.buf, e);
  }
  for (const e of cut.engines) if (e.gain) e.gain.gain.value = nearest.get(e.buf) === e ? e.vol * ENGINE_GAIN : 0;
}
// Cutscene audio: music cues (event type 1: song = u16 table at 0x800AF7B4 [param 0], volume = low byte of param 1, 0x7F if 0) and the
// cutscene's voice mix (the Audio tab's full-cutscene rows, at the game's speech-queue timing), scheduled on the audio clock from a
// start time. The audio clock drives the picture while it plays, and pause suspends the whole context, so voice, music and any other
// sound stop and resume together.
const CUT_SONGS = 0x137580 + (0x800AF7B4 - 0x800A5130);
// A song plays until the next cue or a type-2 event (queueAudioVoiceCmd: stop music channel p0 & 3), whichever comes first.
// A sequence's sting carries its song id; a cutscene's cue indexes the table.
const cutSong = (e) => e.song ?? ((romBytes[CUT_SONGS + e.p[0] * 4] << 8) | romBytes[CUT_SONGS + e.p[0] * 4 + 1]);
function cutMusicCues(data) {
  const stops = data.events.filter((e) => e.type === 2).map((e) => e.frame / data.fps);
  const cues = data.events.filter((e) => e.type === 1).map((e) => {
    const id = cutSong(e);
    return { at: e.frame / data.fps, vol: (e.p[1] & 0xff || 0x7f) / 127, row: audioBank?.music.find((m) => m.index === id) };
  }).filter((c) => c.row).sort((a, b) => a.at - b.at);
  cues.forEach((c, i) => (c.end = Math.min(cues[i + 1]?.at ?? Infinity, ...stops.filter((s) => s > c.at))));
  return cues;
}
function stopCutAudio() {
  for (const s of cut?.sources ?? []) {
    s.onended = null;
    try { s.stop(); } catch { /* never started */ }
  }
  if (cut) cut.sources = [];
}
// (Re)starts every cutscene sound from second T of the cutscene.
function startCutAudio(T) {
  if (!cut || !audioCtx) return;
  stopCutAudio();
  const now = audioCtx.currentTime, srcs = [], length = cut.data.length;
  const play = (buf, gain, at, offset, until) => {
    if (offset >= buf.duration || at + (buf.duration - offset) <= T) return;
    const s = audioCtx.createBufferSource();
    s.buffer = buf;
    s.connect(gain);
    s.start(now + Math.max(0, at - T), Math.max(0, offset + Math.max(0, T - at)));
    if (until != null) s.stop(now + Math.max(0, until - T));
    srcs.push(s);
  };
  if (cut.voiceBuf) play(cut.voiceBuf, cut.voiceGain, 0, 0);
  cut.cues.forEach((c, i) => {
    if (!c.row.stems) return;
    const until = Math.min(c.end, length);
    if (until <= T) return;
    if (!c.gain) {
      c.gain = audioCtx.createGain();
      c.gain.gain.value = c.vol;
      c.gain.connect(cut.musicGain);
    }
    for (const st of c.row.stems) play(st.buf, c.gain, c.at + st.start, 0, until);
  });
  startCutSfx(cut, T, play, srcs);
  cut.sources = srcs;
  cut.audioT0 = T;
  cut.audioAt = now;
}
async function loadCutAudio(path) {
  if (!audioBank) return;
  audioCtx ??= new AudioContext();
  stopAudio();
  const c = cut, name = path.split('/').pop();
  c.musicGain = audioCtx.createGain();
  c.voiceGain = audioCtx.createGain();
  c.musicGain.connect(audioOut());
  c.voiceGain.connect(audioOut());
  c.sfxGain = audioCtx.createGain();
  c.sfxGain.connect(audioOut());
  loadCutSfx(c);
  syncCutGains();
  // The voice mix is read straight from the file, so a ceremony with no medal (its voice events rewritten to hides) gets none.
  const voiceRow = c.data.voiceRow ?? (c.data.events.some((e) => e.type === 0) && audioBank.voice.find((r) => r.key === 'c' + name));
  if (voiceRow) {
    const pcm = rowPcm(voiceRow), buf = audioCtx.createBuffer(1, Math.max(1, pcm.length), voiceRow.rate), ch = buf.getChannelData(0);
    for (let i = 0; i < pcm.length; i++) ch[i] = pcm[i] / 32768;
    c.voiceBuf = buf;
  }
  c.cues = cutMusicCues(c.data);
  if (c.playing) await audioCtx.resume();
  startCutAudio(c.t * c.data.length);
  // Songs render on first use (a few seconds each); sound restarts at the current time as each one becomes ready.
  for (const cue of c.cues) {
    if (cue.row.stems) continue;
    $('cutStatus').textContent = `rendering ${cue.row.name}…`;
    await songStems(cue.row);
    if (cut !== c) return;
    if (c.playing) startCutAudio(cutNow());
  }
  $('cutStatus').textContent = '';
}
// Seconds into the cutscene: the audio clock while sound plays, else the timeline.
function cutNow() {
  if (cut.audioAt != null && audioCtx && cut.sources?.length) return cut.audioT0 + (audioCtx.currentTime - cut.audioAt);
  return cut.t * cut.data.length;
}
function syncCutGains() {
  if (!cut?.musicGain) return;
  cut.musicGain.gain.value = +$('cutMusic').checked;
  cut.voiceGain.gain.value = +$('cutVoice').checked;
  cut.sfxGain.gain.value = +$('cutSfx').checked;
}
function syncCutPlay() {
  const b = $('cutPlay'), playing = !!cut?.playing;
  b.innerHTML = playing ? ICON.pause : ICON.play;
  b.title = playing ? 'Pause' : 'Play';
  b.setAttribute('aria-label', b.title);
}
// path is a cutscene file, or seq:<level dir>:<camera item index> for an in-mission sequence (buildSequenceData).
let lastCutPath = null;
async function showCutscene(path) {
  await cutScanPathModels();
  // A different cutscene drops the selected event (rebuilds of the same one keep it).
  if (path !== lastCutPath) [cutSelEvent, cutSelKey, cutTrack, lastCutPath] = [null, null, null, path];
  cutAuth = path.startsWith('seq:') ? AUTHORING_EMPTY() : await readAuthoring(path);
  const sq = path.match(/^seq:(.+):(\d+)$/);
  const mc = modLevels.find((l) => path === `data/cuts/${l.folder}_intro` || path === `data/cuts/${l.folder}_extro`);
  const n = sq ? parseInt(sq[1].slice(-1), 36) : mc ? mc.template : +path.match(/id(\d+)_/)[1];
  const lv = sq ? sq[1] : mc ? 'data/level/' + mc.folder : n <= 15 ? 'data/level/lv_' + n.toString(36) : null;
  // The medal ceremony (cinematicLoopBody(3, 2), medal byte 0x80130B38) loads lv_3 but forces sky index 0x11 -> sky_5 (0x8005BD70).
  const ceremony = path.endsWith('id3_special');
  if (lv && rom.assets.has(lv + '/dat')) {
    await showLevel(lv, false, ceremony ? 'data/level/sky_5' : null);
    $('cutPick').value = sq ? 'seq:' + sq[2] : 'cut:' + path;
  } else {
    setContext(true);
    const world = new THREE.Group();
    world.rotation.x = Math.PI;
    setRoot(world);
    $('cutPickHead').hidden = $('cutPickRow').hidden = true;
  }
  // The orbit / fly help means little while the cutscene drives the camera.
  $('help').hidden = true;
  // Editing would change the level under a playing cutscene; the level view keeps it.
  if (editMode) setEditMode(null);
  $('editPanel').hidden = true;
  selected = 'cut:' + path;
  fillLevelButton();
  selectedLevel = true;
  renderList();
  const data = sq ? await buildSequenceData(lv, +sq[2]) : parseCutscene(await rom.read(path)), index = await buildObjectIndex(), world = root;
  // Ceremony tier (D_800B0B1E, the lowest medal over levels 0-15): with none, processCutsceneActions turns the medal's (actor 0x1B) show and
  // every voice line into hides, so no medal and no speech.
  const tier = ceremony ? MEDAL_TIERS.indexOf($('optMedal').value) : 3;
  // Event type 18 (processCutsceneActions, at load): actor p0 becomes the mission's craft (gGameSettings+1). The picker starts on the
  // level's default craft (table 0x8009EC50 + 3, as the boot shortcut uses) whenever another cutscene opens.
  const swaps = data.events.filter((e) => e.type === 18), craftSel = $('optCutCraft');
  $('craftRow').hidden = !swaps.length;
  if (swaps.length && craftSel.dataset.path !== path) {
    craftSel.dataset.path = path;
    if (n <= 15) craftSel.value = String(romBytes[0x9EC50 - 0x400 + 0x1000 + n * 4 + 3]);
  }
  for (const e of swaps) if (data.actors[e.p[0]]) data.actors[e.p[0]].name = CUT_CRAFTS[+craftSel.value] ?? data.actors[e.p[0]].name;
  if (tier === 0) for (const e of data.events) {
    if (e.type === 6 && e.p[0] === 0x1B) e.type = 7;
    else if (e.type === 0) Object.assign(e, { type: 7, p: [0x1B, ...e.p.slice(1)] });
  }
  // The level's own markers (paths, zones, triggers) would clutter the shots; the level view still has them.
  const markers = world.getObjectByName('markers');
  if (markers) markers.visible = false;
  $('legend').hidden = true;
  const counts = { tris: 0, verts: 0, parts: 0 };
  const nodes = data.actors.map((a) => Object.assign(new THREE.Group(), { name: a.name, matrixAutoUpdate: false }));
  data.actors.forEach((a, i) => (a.parent >= 0 ? nodes[a.parent] : world).add(nodes[i]));
  const scale = [], anims = [];
  for (const [i, a] of data.actors.entries()) {
    if (!a.model) continue;
    // The table's own path picks the model (atpt_imp is in both atpt_i and atpt_i_hi; the walk clip fits the _hi rig); player crafts
    // have no path there and are found by object name.
    // A sequence's level units bring the level's own model (src).
    const base = cutBase(a.name), asset = cutAsset(base), tabled = asset.path && 'data/' + asset.path;
    const id = a.src?.id ?? cutPathModelId(a.name) ?? (tabled && rom.assets.has(tabled + '_HOB') ? tabled : index.get(base));
    scale[i] = asset.scale;
    if (!id) continue;
    const m = await loadModel(id), obj = a.src ? m.hob.objects[a.src.objIndex] : m.hob.objects.find((o) => o.name === base) ?? m.hob.objects[0];
    const mesh = buildObject(obj, m, id, counts);
    mesh.scale.setScalar(HOB_TO_WORLD);
    nodes[i].add(mesh);
    nodes[i].userData.model = { mesh, obj };
    // Walkers loop their walk animation (spawnCutsceneObjectsFromList): phase = frac(t * rate) on the normalised cutscene time, rate 5
    // (load_cutscene) until an event type 13 sets actor p0's rate to p1 / 10 (spawnCutsceneEffectsAndCues).
    const animPath = a.anim ?? (asset.anim && 'data/' + asset.anim);
    const walk = animPath && rom.assets.has(animPath) && parseAnim(await rom.read(animPath)), rig = mesh.userData.rig;
    if (walk && rig && walk.tracks.length === rig.nodes.length - 1) anims.push({ i, rig, data: walk, phase0: a.phase0, rates: data.events.filter((e) => e.type === 13 && e.p[0] === i).map((e) => [e.frame / data.frames, e.p[1] / 10]) });
  }
  let camNode = null, camPath = null;
  if (data.camera) {
    // Marker: a small pyramid along the camera's +Z, plus its whole path.
    const s = 0.4, p = [0, 0, 0, -s, -s, 2 * s, s, -s, 2 * s, s, s, 2 * s, -s, s, 2 * s];
    const idx = [0, 1, 0, 2, 0, 3, 0, 4, 1, 2, 2, 3, 3, 4, 4, 1];
    const g = new THREE.BufferGeometry().setAttribute('position', new THREE.Float32BufferAttribute(idx.flatMap((k) => p.slice(k * 3, k * 3 + 3)), 3));
    camNode = new THREE.LineSegments(g, new THREE.LineBasicMaterial({ color: 0xffcc33 }));
    camNode.matrixAutoUpdate = false;
    world.add(camNode);
    // The path of whichever camera is live, broken at each cut.
    const pts = [];
    let prev = null;
    for (let k = 0; k <= 600; k++) {
      const t = k / 600, c = cutCameraAt(data, t), p = [0, 1, 2].map((i) => cutsceneValue(c[i], t, data.frames, 8) * 0.25);
      if (prev?.c === c) pts.push(...prev.p, ...p);
      prev = { c, p };
    }
    camPath = new THREE.LineSegments(new THREE.BufferGeometry().setAttribute('position', new THREE.Float32BufferAttribute(pts, 3)), new THREE.LineBasicMaterial({ color: 0xffcc33, transparent: true, opacity: 0.5 }));
    world.add(camPath);
  }
  // The render loop can pose the cutscene while the rest of its setup is still loading, so every list starts empty.
  cut = { data, nodes, scale, anims, camNode, camPath, t: 0, playing: true, fov: 50, sources: [], cues: [], shadows: [], rootOffset: [], shows: [], shots: [], foils: [],
    flipY: data.actors.map((a) => a.model && [0x1A, 0x1B].includes(cutAsset(cutBase(a.name)).index)) };
  // cinematicLoopBody 0x800A6168: below silver the ceremony stops at frame 0x222, at silver at 0x47F; gold plays it all.
  cut.endFrame = !ceremony || tier === 3 ? data.frames : tier === 2 ? 0x47F : 0x222;
  cut.end = cut.endFrame / data.frames;
  cut.picAspect = data.picAspect ?? (4 / 3) * (240 / (n === 19 ? 224 : 170));
  // The file as stored, for editing (data above may carry load-time rewrites: medal tier, craft swap). A sequence edits its level's
  // camera records; its built data stands in for the file on the timeline.
  cut.file = sq ? data : parseCutscene(await rom.read(path));
  $('cutEdit').hidden = false;
  $('cutInspector').hidden = !cutEditing;
  $('cutPanel').classList.toggle('editing', !$('cutInspector').hidden);
  applyCutPanelSize();
  $('cutEditBadge').style.visibility = $('cutLengthRow').style.visibility = $('cutInspector').hidden ? 'hidden' : 'visible';
  placeCutInspector();
  cut.seq = !!sq;
  syncSeqUi();
  if (ceremony) {
    cut.ceremony = true;
    cut.tier = tier;
    // setupCutsceneLevel level-3 case: black clear and fog (0x80136E18), the 2D backdrop big_cor (Corellia) behind the 3D, and the terrain
    // node skipped while frame < 420 (0x800B0B1D, slotEffectHandlerDispatch), so the opening is a space shot.
    if (levelFog) {
      levelFog.color.setRGB(0, 0, 0);
      applyFog();
    }
    cut.terrain = world.getObjectByName('terrain');
    cut.planet = rom.assets.has('data/backdrop/big_cor') && ceremonyPlanet(await rom.read('data/backdrop/big_cor'));
    if (cut.planet) camera.add(cut.planet);
  }
  cut.foils = data.actors.flatMap((_, i) => {
    const model = nodes[i].userData.model, rig = model?.mesh.userData.rig, at = (name) => model?.obj.nodeNames?.get(name) ?? -1;
    const a = at('wngrot_a'), b = at('wngrot_b'), events = data.events.filter((e) => e.type === 11 && (e.p[0] & 0xffff) === i && e.p[2]).sort((x, y) => x.frame - y.frame);
    return a >= 0 && b >= 0 && events.length ? [{ rig, a, b, events }] : [];
  });
  syncCutNpcs();
  buildCutTimeline(data);
  cut.shadows = [];
  for (const [i, a] of data.actors.entries()) {
    const asset = a.model && nodes[i].userData.model && cutAsset(cutBase(a.name));
    const sh = asset && asset.flags & 4 && asset.index >= 0 && (await makeShadow(asset.index));
    if (!sh) continue;
    world.add(sh.mesh);
    cut.shadows.push(Object.assign(sh, { node: nodes[i] }));
  }
  cut.rootOffset = data.actors.map((a, i) => {
    const n1 = nodes[i].userData.model?.obj.nodes[1];
    return !!(a.model && !a.animated && n1 && n1.flags & 8 && Math.hypot(...a.value.slice(0, 3)) > 0.01 && a.value.slice(0, 3).every((x, k) => Math.abs(x - n1.pos[k]) < 1e-3));
  });
  cut.shows = data.actors.map((_, i) => data.events.filter((e) => (e.type === 6 || e.type === 7) && e.p[0] === i).sort((a, b) => a.frame - b.frame));
  // Weapon fire (event type 8: actor p0, category p1, speed p2 / 10 or 2.5, sfx p3): the actor fires one bolt from every muzzle hardpoint
  // record it has among a, b, a_b, c_d (two records each; spawnCutsceneEffectsAndCues -> lookupHmtTextureByName), through
  // dispatchCueByCategory -> the laser NPC 0x8005C874: a weapons_HOB model by category (colour is baked in), the muzzle frame's axes
  // (X = up x dir, Y = up, Z = dir), scale 0.25 (doubled in the logo sequence), starting dir * sqrt(0.2293) * scale + vel / 30 ahead of
  // the muzzle and flying straight at the speed for 1.5 s. shortcut: the lights_PAL glow sprite and the hit sparks are not drawn.
  cut.shots = [];
  const weapons = rom.assets.has('data/weapons_HOB') && (await loadModel('data/weapons'));
  for (const e of weapons ? data.events : []) {
    const model = nodes[e.p[0]]?.userData.model, laser = e.type === 8 && LASER_MODELS[e.p[1]];
    const lobj = laser && weapons.hob.objects.find((o) => o.name === laser);
    if (!model || !lobj) continue;
    poseCutscene(e.frame / data.frames);
    world.updateMatrixWorld(true);
    const speed = e.p[2] ? e.p[2] / 10 : 2.5, scale = 0.25 * (n === 19 ? 2 : 1), rig = model.mesh.userData.rig;
    const local = (node, v) => world.worldToLocal(node.localToWorld(new THREE.Vector3(...v).multiplyScalar(256)));
    for (const name of ['a', 'b', 'a_b', 'c_d']) {
      const hp = model.obj.hardpoints?.find((h) => h.name === name), node = hp && rig?.nodes[hp.node];
      for (const rec of node ? hp.points.slice(0, 2) : []) {
        const pos = local(node, rec.pos);
        const dir = local(node, rec.pos.map((v, a) => v + rec.dir[a])).sub(pos).normalize();
        const up = local(node, rec.pos.map((v, a) => v + rec.up[a])).sub(pos).normalize();
        pos.addScaledVector(dir, Math.sqrt(0.2293) * scale + speed / 30);
        const mesh = buildObject(lobj, weapons, 'data/weapons', counts);
        mesh.matrixAutoUpdate = false;
        mesh.userData.shared = true;
        mesh.visible = false;
        world.add(mesh);
        const basis = new THREE.Matrix4().makeBasis(new THREE.Vector3().crossVectors(up, dir).normalize(), up, dir);
        const pal = n !== 10 && rom.assets.has('data/lights_PAL') && (await rom.read('data/lights_PAL')), run = pal && LASER_GLOW[laser];
        const colors = run != null && pal ? [...Array(16)].map((_, k) => new THREE.Color().setRGB(pal.getUint8((run + k) * 4) / 255, pal.getUint8((run + k) * 4 + 1) / 255, pal.getUint8((run + k) * 4 + 2) / 255, THREE.SRGBColorSpace)) : null;
        const glow = colors && new THREE.Mesh(new THREE.PlaneGeometry(2.6, 2.6).rotateX(Math.PI / 2), new THREE.MeshBasicMaterial({ map: glowTexture(), transparent: true, blending: THREE.AdditiveBlending, depthWrite: false, fog: false }));
        if (glow) {
          glow.visible = false;
          world.add(glow);
        }
        cut.shots.push({ at: e.frame / data.frames, pos, dir, speed, mesh, basis, size: scale * HOB_TO_WORLD, glow, colors, from: e.p[0], cat: e.p[1], life: 1.5 });
      }
    }
  }
  // A bolt ends at the first other visible model it reaches (the laser NPC's hit test), stepped at 30 Hz against the actors posed at that
  // time; the hit spawns the projectile impact (0x80076250). shortcut: a model is a sphere of 0.6 x its bounding box's half-diagonal.
  const tmp = new THREE.Vector3();
  poseCutscene(0);
  world.updateMatrixWorld(true);
  const radius = nodes.map((nd) => nd.userData.model && new THREE.Box3().setFromObject(nd.userData.model.mesh).getSize(tmp).length() * 0.3);
  const shown = (nd) => { for (let o = nd; o && o !== world; o = o.parent) if (!o.visible) return false; return true; };
  const impacts = [];
  // Missiles and torpedoes (types 10/11/13/14, guidedEffectNpcHandler 0x8005F9A4) fly straight for 0.8 s in cutscenes, then gain 0.4 units/s
  // of downward speed every 1/30 s tick until they reach the terrain and explode. shortcut: the nose turn is drawn along the velocity.
  for (const s of cut.shots) {
    if (![10, 11, 13, 14].includes(s.cat)) continue;
    const v = s.dir.clone().multiplyScalar(s.speed), p = s.pos.clone();
    s.path = [];
    for (let k = 0; k < 300; k++) {
      if (k >= 24) v.y += 0.4;
      p.addScaledVector(v, 1 / 30);
      s.path.push({ p: p.clone(), d: v.clone().normalize() });
      if (p.y >= levelHeight(p.x, p.z)) break;
    }
    s.life = s.path.length / 30;
  }
  for (const s of cut.shots) {
    let hit = false;
    for (let k = 1; k <= Math.round(s.life * 30) && !hit; k++) {
      poseCutscene(s.at + k / 30 / data.length);
      world.updateMatrixWorld(true);
      const p = s.path ? s.path[k - 1].p : s.pos.clone().addScaledVector(s.dir, (k / 30) * s.speed);
      for (const [i, nd] of nodes.entries()) {
        if (i === s.from || !radius[i] || !shown(nd) || world.worldToLocal(nd.getWorldPosition(tmp)).distanceTo(p) > radius[i]) continue;
        s.life = k / 30;
        impacts.push({ at: s.at * data.length + s.life, pos: p, cat: s.cat });
        hit = true;
        break;
      }
    }
    if (!hit && s.path) impacts.push({ at: s.at * data.length + s.life, pos: s.path[s.path.length - 1].p, cat: s.cat });
  }
  // World Devastator feet (spawnNamedTexturedEmitterNpc 0x8007D470 for asset type 0xE): at hardpoints "01"-"04" an npcVortexUpdate column
  // (wrld_dvfx "vortex", vertex-coloured, alpha fading to 0 at its end) from the foot down to the terrain, and an npcWaterSprayUpdate crown
  // (gnrc_stuff/wtrspray) on the terrain below it. shortcut: the crown's waterwhite_1 animation (wtrspray_ANM at 12 fps), its water-cell
  // visibility test and the 0x66 sound loop are left out.
  cut.vortices = [];
  const dvfx = rom.assets.has('data/imp_stuff/wrld_dvfx_HOB') && (await loadModel('data/imp_stuff/wrld_dvfx'));
  const wsp = dvfx && rom.assets.has('data/gnrc_stuff/wtrspray_HOB') && (await loadModel('data/gnrc_stuff/wtrspray'));
  for (const [i, a] of data.actors.entries()) {
    const model = nodes[i].userData.model, rig = model?.mesh.userData.rig;
    if (!wsp || !rig || cutAsset(cutBase(a.name)).index !== 0xE) continue;
    for (const [k, name] of ['01', '02', '03', '04'].entries()) {
      const hp = model.obj.hardpoints?.find((h) => h.name === name), node = hp && rig.nodes[hp.node];
      if (!node) continue;
      const vortex = buildObject(dvfx.hob.objects.find((o) => o.name === 'vortex') ?? dvfx.hob.objects[0], dvfx, 'data/imp_stuff/wrld_dvfx', counts);
      const spray = buildObject(wsp.hob.objects.find((o) => o.name === 'wtrspray') ?? wsp.hob.objects[0], wsp, 'data/gnrc_stuff/wtrspray', counts);
      for (const m of [vortex, spray]) {
        m.matrixAutoUpdate = false;
        m.userData.shared = true;
        // The game's translucent pass is a plain alpha blend (G_RM_ZB_XLU_SURF) of texture x vertex colour with the vertex alpha; the viewer's
        // material picker would make these additive glows or canopy glass.
        m.traverse((o) => {
          if (!o.isMesh) return;
          o.material = (o.userData.infos ?? [{}]).map((info) => {
            const mat = new THREE.MeshBasicNodeMaterial({ map: info.map ?? null, vertexColors: true, transparent: true, depthWrite: false, side: THREE.DoubleSide, fog: false });
            mat.opacityNode = new THREE.AttributeNode('valpha', 'float');
            return mat;
          });
          o.userData.shared = false;
        });
        world.add(m);
      }
      // The crown's two phases start at |rand & 127 - 64| / 64; fixed per foot here.
      cut.vortices.push({ actor: nodes[i], node, rec: hp.points[0], vortex, spray, a: (k * 0.37) % 1, b: (k * 0.61 + 0.2) % 1 });
    }
  }
  cut.fireworks = await buildFireworks(data.events.filter((e) => e.type === 15 && FIREWORK_EMITTERS[e.p[0]]).map((e) => ({ at: e.frame / data.fps, ...FIREWORK_EMITTERS[e.p[0]] })), world, weapons, counts);
  cut.fx = await buildCutFx(data, world, nodes, impacts);
  syncCutPlay();
  // Fog on by default for cutscenes (the level view keeps its own setting off); levels without fog hide the toggle.
  $('cutFog').closest('label').hidden = !levelFog;
  if (levelFog) {
    cut.levelFogOn = $('optFog').checked;
    $('optFog').checked = $('cutFog').checked;
    syncFogControls();
    applyFog();
  }
  loadCutAudio(path);
  stats = { cutscene: path.split('/').pop(), length: `${data.length.toFixed(1)} s`, actors: data.actors.length, models: data.actors.filter((a) => a.model).length, triangles: counts.tris };
  hud();
  $('cutPanel').hidden = false;
  poseCutscene(0);
}
function poseCutscene(t) {
  if (cut.crawl) return poseCrawl(t);
  const { data, nodes } = cut, F = data.frames;
  data.actors.forEach((a, i) => {
    const v = (c, div) => (a.animated ? cutsceneValue(a.ch[c], t, F, div) : a.value[c]);
    const pos = [0, 1, 2].map((c) => v(c, 6) * 0.25), piv = [8, 9, 10].map((c) => v(c, 3.5) * 0.25);
    // cinematicSlotDispatcher: a name starting "l_" takes the asset table scale as is; any other model scales by its channel 3 x 2/9, times the table scale when that is non-zero.
    const s = !a.model ? 1 : a.name.startsWith('l_') ? cut.scale[i] : v(3, 6) * 0.2222 * (cut.scale[i] || 1);
    // shortcut: the AT-AT actor's own position is its model's node-1 offset copied in by the exporter; applied at world scale it sinks the
    // walker 0.26 units into the snow, unlike the footage, so a position equal to the model's root offset is applied at the model's scale.
    if (cut.rootOffset[i]) for (let k = 0; k < 3; k++) pos[k] *= s;
    const m = nodes[i].matrix.makeTranslation(-piv[0], -piv[1], -piv[2]);
    // cinematicSlotDispatcher turns asset types 0x1A (cloud car) and 0x1B (end_medal) 180 deg about Y first (verified on the medal's RAM matrix).
    if (cut.flipY[i]) m.premultiply(_cm.makeRotationY(Math.PI));
    m.premultiply(axisAngle(_cm, [4, 5, 6].map((c) => v(c, 3.5)), v(7, 3.5)));
    m.premultiply(_cm.makeScale(s, s, s));
    m.premultiply(_cm.makeTranslation(pos[0] + piv[0], pos[1] + piv[1], pos[2] + piv[2]));
    nodes[i].matrixWorldNeedsUpdate = true;
  });
  // Laser bolts fly straight from their muzzle at their speed for 1.5 s (laser NPC tick: pos += vel * dt).
  for (const s of cut.shots) {
    const dt = (t - s.at) * data.length;
    s.mesh.visible = dt >= 0 && dt < s.life;
    if (s.mesh.visible && s.path) {
      const at = s.path[Math.min(s.path.length - 1, Math.floor(dt * 30))];
      s.mesh.matrix.compose(_bp.copy(at.p), _bq.setFromUnitVectors(_zAxis, at.d), _bs.setScalar(s.size));
    } else if (s.mesh.visible) s.mesh.matrix.copy(s.basis).scale(_bs.setScalar(s.size)).setPosition(_bp.copy(s.pos).addScaledVector(s.dir, dt * s.speed));
    if (!s.glow) continue;
    // Game Y is down: the bolt's height above the ground is ground y - bolt y.
    const above = s.mesh.visible ? levelHeight(_bp.x, _bp.z) - _bp.y : Infinity;
    s.glow.visible = above < 2;
    if (!s.glow.visible) continue;
    s.glow.position.set(_bp.x, levelHeight(_bp.x, _bp.z) - 0.03, _bp.z);
    s.glow.material.color.copy(s.colors[Math.floor((t - s.at) * data.length * 30) % 16]);
    s.glow.material.opacity = 0.45 * (1 - Math.max(0, above) / 2);
  }
  // Type 7 despawns actor p0 (despawnCutsceneNpcSlot; slot byte 0 = 1) and type 6 brings it back (byte 0 = 0); children go with it.
  cut.shows.forEach((list, i) => {
    let v = true;
    for (const e of list) if (e.frame <= t * F) v = e.type === 6;
    nodes[i].visible = v;
  });
  if (cut.shadows.length) {
    root.updateMatrixWorld(true);
    for (const sh of cut.shadows) poseShadow(sh, sh.node, root);
  }
  for (const w of cut.anims) {
    const rate = w.rates.reduce((r, [at, v]) => (t >= at ? v : r), 5);
    poseTracks(w.rig, w.data, (t * rate + (w.phase0 ?? 0)) % 1);
  }
  if (cut.terrain || cut.planet) poseCeremony(t * F);
  if (cut.fireworks) poseFireworks(cut.fireworks, t * data.length);
  if (cut.fx) poseCutFx(cut.fx, t * data.length);
  if (cut.vortices?.length) poseVortices(t * data.length);
  // Type 11 (scheduleCutsceneActionSlot, tickCutsceneActionSlots): the S-foils (wngrot_a +deg, wngrot_b -deg about Z, set as the node
  // rotation, composeInterpolatedNodeMatricesAlt) ease from closed (0) or the last target to p1 - 180 deg over p2 frames, quadratic in/out.
  for (const f of cut.foils) {
    let deg = 0;
    for (const e of f.events) {
      const k = Math.min(1, (t * F - e.frame) / e.p[2]);
      if (k < 0) break;
      deg += (e.p[1] - 180 - deg) * (k < 0.5 ? 2 * k * k : 1 - 2 * (1 - k) ** 2);
    }
    // Shuttle wing hinges are pivot nodes: the angle goes inside the pivot on top of their rest turn (as the model view's wings).
    for (const [i, sign] of [[f.a, 1], [f.b, -1]]) {
      if (f.rig.src[i].flags & 1) poseProc(f.rig.nodes[i], f.rig.src[i], 'z', sign * deg);
      else poseNode(f.rig.nodes[i], f.rig.src[i], new THREE.Quaternion().setFromAxisAngle(_zAxis, THREE.MathUtils.degToRad(sign * deg)));
    }
  }
  if (cut.camNode) {
    const c = cutCameraAt(data, t), cv = (k, div) => cutsceneValue(c[k], t, F, div);
    axisAngle(cut.camNode.matrix, [3, 4, 5].map((k) => cv(k, 8)), cv(6, 2.5)).setPosition(cv(0, 8) * 0.25, cv(1, 8) * 0.25, cv(2, 8) * 0.25);
    cut.camNode.matrixWorldNeedsUpdate = true;
    cut.fov = cv(7, 8);
  }
}
function applyCutscene(dt) {
  if (!cut) return;
  const { data } = cut;
  if (cut.playing) {
    cut.t = cut.sources.length && audioCtx?.state === 'running' ? cutNow() / data.length : cut.t + dt / data.length;
    if (cut.t >= cut.end && $('cutLoop').checked) {
      cut.t = 0;
      startCutAudio(0);
    } else if (cut.t >= cut.end) {
      cut.t = cut.end;
      cut.playing = false;
      // A sequence's queued speech and sting carry on into gameplay (the game keeps the speech queue running after the teardown).
      if (!cut.seq) stopCutAudio();
      syncCutPlay();
    }
    $('cutTime').value = cut.t;
  }
  poseCutscene(cut.t);
  if (cutTrack) syncTrackProxy();
  if (cutTrack && gizmo.dragging) poseFromGizmo();
  if (cutEditing) syncCutSelBox();
  $('cutTimeVal').textContent = `${(cut.t * data.length).toFixed(1)} s`;
  if (cut.tlHead) {
    cut.tlHead.style.left = `${cut.t * 100}%`;
    cut.tlHead.firstChild.textContent = `${(cut.t * data.length).toFixed(1)} s`;
    cut.tlHead.classList.toggle('atStart', cut.t < 0.03);
    cut.tlHead.classList.toggle('atEnd', cut.t > 0.97);
  }
  const own = $('cutCam').checked && cut.camNode;
  // Through the cutscene camera the game's far plane applies (the F5 clipper drops what is past it, fogged or not); free orbit keeps its own.
  if (own && levelFog) {
    cutFreeFar ??= camera.far;
    camera.far = fogFar() * +$('optFogDist').value;
  } else restoreCutFar();
  controls.enabled = !own && !gizmo.dragging;
  if (cut.camNode) cut.camNode.visible = cut.camPath.visible = !own;
  // Free orbit (Camera unticked) is the editing view, so its controls help shows; the cutscene camera needs none.
  $('help').hidden = !!own;
  // Screen transitions, through the cutscene camera only (free orbit is for editing). The start and each type-4 cut render 3 empty
  // (black) frames (cinematicLoopBody sets 0x800B09B0 = 3; slotEffectHandlerDispatch counts it down). The ending transition starts at the
  // type-10 frame or 10 frames before the end (cinematicInitializer). shortcut: the game ramps a textured overlay's alpha there
  // (initCinematicSceneAssets), drawn here as a fade to black over CUT_FADE seconds.
  const fr = cut.t * data.frames, endAt = Math.min(cut.endFrame - 10, ...data.events.filter((e) => e.type === 10).map((e) => e.frame));
  // In-mission sequences have neither: cuts are immediate and the teardown drops straight back to the gameplay view (no letterbox).
  const cutBlack = !cut.seq && (fr < 3 || data.events.some((e) => e.type === 4 && fr >= e.frame && fr < e.frame + 3));
  $('cutFade').hidden = !own || cut.seq;
  $('cutBars').hidden = !own || (cut.seq && cut.t >= cut.end);
  $('cutFade').style.opacity = cutBlack ? 1 : THREE.MathUtils.clamp((fr - endAt) / data.fps / CUT_FADE, 0, 1);
  if (own) {
    root.updateMatrixWorld();
    camera.matrixWorld.multiplyMatrices(cut.camNode.matrixWorld, CAM_FLIP).decompose(camera.position, camera.quaternion, camera.scale);
    camera.scale.set(1, 1, 1);
    // Letterbox (initSceneForMission 0x800A9ABC): the picture is 640 x 170 of 240 lines (x 2 in hi-res), 224 for id19, centred with the
    // VI leaving the rest black. The FOV is guPerspectiveF's over the whole 4:3 screen, so the band sees cutBandK of its height (the
    // canvas gets a wider FOV the bars then crop). Frame-matched against the game with capture-frames.ps1 -Cutscene.
    const r = canvas.getBoundingClientRect(), band = Math.min(r.height, r.width / cut.picAspect);
    $('cutBars').style.borderWidth = `${(r.height - band) / 2}px 0`;
    // A finished sequence is back in gameplay: full frame at the 65 deg mission FOV.
    camera.fov = cut.seq && cut.t >= cut.end ? 65 : THREE.MathUtils.radToDeg(2 * Math.atan(Math.tan(THREE.MathUtils.degToRad(cut.fov) / 2) * cutBandK() * r.height / band));
    camera.updateProjectionMatrix();
  }
}
// Rendering options rebuild the scene; a cutscene is rebuilt at the same time and play state.
async function reopenCutscene() {
  // An edit rebuilds the cutscene; the free view stays where the user left it.
  const t = cut?.t ?? 0, playing = cut?.playing ?? true, view = getView(), fov = camera.fov;
  await showCutscene(selected.slice(4));
  if (!cut) return;
  if (!$('cutCam').checked) {
    setView(view);
    camera.fov = fov;
    camera.updateProjectionMatrix();
    controls.update();
  }
  cut.t = t;
  $('cutTime').value = t;
  startCutAudio(t * cut.data.length);
  if (playing) return;
  cut.playing = false;
  syncCutPlay();
  await audioCtx?.suspend();
}
// ---- Opening crawls as cutscenes (runModelViewerLoop 0x800C9BF0), in game space (y down, the camera looks +z) under the cutscene root's
// flip. which = 'stock:<0-3>' or a mod crawl's name; the crawl window beside the view edits it.
// Every crawl runs one clock (dt-based): the logo recedes from 10.6 at 10 units/s and goes past 140, the text starts past 90 and scrolls
// its own height in 45.26 s, fog density climbs 0.5/s from 82.5 s to 2.5 (text off), a 4 s pan, a 7 s fly-by, then a 0.77 s fade.
const CRAWL_T = { text: 7.94, logoGone: 12.94, fogIn: 82.5, pan: 84.5, fly: 88.5, done: 88.5 + 0.98 * 7, end: 88.5 + 0.98 * 7 + 0.77 };
// The text plane (0x800C9FE8): I + T(0.2, C0, 4.2), then Rx(-72 deg) turning the translation too, then y - 0.12.
const CRAWL_PLANE = [new THREE.Vector3(1, 0, 0), new THREE.Vector3(0, 0.30902, -0.95106), new THREE.Vector3(0, 0.95106, 0.30902)];
// Fly-by craft per crawl (0x800CD474 counts, 0x800CD4B8 models); craft i follows route_(i % 3), offset (0.625, 1.375, 0), scale 0.0125.
const CRAWL_CRAFT = [['xwing', 3], ['xwing', 4], ['awing', 4], ['vwing', 4]], CRAWL_FLY_OFFSET = [0.625, 1.375, 0];
// Text page Y C0, logo distance C4 and fog [start, density] at second s (the camera is crawlCameraAt's).
function crawlState(s, H) {
  const T = CRAWL_T;
  const fog = s >= T.fly ? [20, 1.5] : s >= T.pan ? [15, 2.5] : s >= T.fogIn ? [15, Math.min(2.5, 1.5 + 0.5 * (s - T.fogIn))] : [15, s >= T.logoGone ? 1.5 : 1.1];
  return { C0: -5 - Math.max(0, s - T.text) * (H / 45.257), C4: 10.6 + 10 * s, fog };
}
// The page as one texture at 64 px per unit (a glyph cell is one unit), x -5..5 and y from 1.5 above the first line to 1.5 below the last.
async function crawlTexture(c) {
  const font = await crawlFontOf(), strip = (s) => [...s].filter((ch) => !crawlFontGaps(ch).length).join('');
  const text = (c.chapter ? `*}${strip(c.chapter)}\n\n` : '') + `*{${strip(c.title)}\n\n${strip(c.body)}`;
  const { glyphs, height } = crawlLayout(font, text), cv = document.createElement('canvas');
  cv.width = 640;
  cv.height = Math.round((height + 3) * 64);
  const cx = cv.getContext('2d');
  for (const g of glyphs) cx.drawImage(font.sheet, 0, g.glyph * 64, 64, 64, (g.x - g.xs / 2 + 5) * 64, (g.y - g.ys / 2 + 1.5) * 64, g.xs * 64, g.ys * 64);
  const tex = new THREE.CanvasTexture(cv);
  tex.colorSpace = THREE.SRGBColorSpace;
  return { tex, height };
}
// A quad from the given corners in its parent's space, texture top at the first two.
function crawlQuad(tl, tr, bl, br, map) {
  const g = new THREE.BufferGeometry().setAttribute('position', new THREE.Float32BufferAttribute([...tl, ...tr, ...bl, ...br], 3));
  g.setAttribute('uv', new THREE.Float32BufferAttribute([0, 1, 1, 1, 0, 0, 1, 0], 2)).setIndex([0, 2, 1, 1, 2, 3]);
  return new THREE.Mesh(g, new THREE.MeshBasicMaterial({ map, transparent: true, depthWrite: false, side: THREE.DoubleSide }));
}
// A route (DAT type 3 sub 8) as evalCubicSplineAtTime 0x80018F98 reads it: Catmull-Rom over the points, u spread by the arc fractions.
function crawlRoute(dv, it) {
  const pts = splinePoints(dv, it), o = it.offset, at = o + dv.getUint32(o + 0x20) + pts.length * 12;
  const frac = Array.from({ length: pts.length - 2 }, (_, k) => dv.getFloat32(at + k * 4));
  return (u) => {
    let j = 0;
    while (j < frac.length - 2 && u > frac[j + 1]) j++;
    const t = THREE.MathUtils.clamp((u - frac[j]) / (frac[j + 1] - frac[j] || 1), 0, 1), [p0, p1, p2, p3] = pts.slice(j, j + 4);
    return [0, 1, 2].map((a) => 0.5 * (2 * p1[a] + (p2[a] - p0[a]) * t + (2 * p0[a] - 5 * p1[a] + 4 * p2[a] - p3[a]) * t * t + (3 * p1[a] - p0[a] - 3 * p2[a] + p3[a]) * t * t * t));
  };
}
// which: the level directory whose opening crawl plays (levelCrawl).
async function showCrawl(which, keep = null) {
  if (!keep) [crawlSel, crawlKeySel] = [null, null];
  const ctx = await levelCrawl(which), c = ctx?.c;
  if (!c) return;
  setContext(true);
  const world = new THREE.Group();
  world.rotation.x = Math.PI;
  setRoot(world);
  $('help').hidden = true;
  if (editMode) setEditMode(null);
  $('editPanel').hidden = $('legend').hidden = true;
  selected = 'crawl:' + which;
  fillLevelButton();
  selectedLevel = true;
  renderList();
  const counts = { tris: 0, verts: 0, parts: 0 }, { tex, height } = await crawlTexture(c), order = (o, n) => o.traverse((m) => (m.renderOrder = n));
  const text = crawlQuad([-5, -1.5, 0], [5, -1.5, 0], [-5, height + 1.5, 0], [5, height + 1.5, 0], tex);
  text.matrixAutoUpdate = false;
  order(text, 3);
  world.add(text);
  // Stars and logo: the scene DAT's "starwars" and "starwars_logo" (rotation 180/0/180) from frontend/starwars; the logo is tinted
  // (235, 205, 105) and scaled 0.2, its distance 0.2 x C4.
  // A crawl with moved routes ships its own copy of the scene DAT (the host serves it in the stock one's place).
  const scenePath = 'data/frontend/starwars';
  const ownDat = ctx.exists && (await rom.read(crawlDat(ctx.name)).then((d) => (d.byteLength ? d : null), () => null));
  const sceneDat = ownDat || (await rom.read(scenePath + '/dat')), items = parseDAT(sceneDat).items, sm = await loadModel(scenePath + '/starwars');
  const place = (name, scale, pos) => {
    const it = items.find((x) => x.name === name), obj = sm.hob.objects.find((o) => o.name === name), mesh = it && obj && buildObject(obj, sm, scenePath + '/starwars', counts);
    if (!mesh) return null;
    const holder = new THREE.Group();
    holder.matrixAutoUpdate = false;
    holder.add(mesh);
    holder.userData.rot = placementRotation(it.rot).scale(new THREE.Vector3().setScalar(scale * HOB_TO_WORLD));
    holder.matrix.copy(holder.userData.rot).setPosition(...(pos ?? it.pos));
    world.add(holder);
    return holder;
  };
  // The stars plane is drawn first (pass 1) under everything, so it writes no depth.
  const stars = place('starwars', 1);
  if (stars) {
    order(stars, 0);
    stars.traverse((m) => m.material && [m.material].flat().forEach((mt) => (mt.depthWrite = false)));
  }
  const logo = place('starwars_logo', 0.2, [0, 0, 0]);
  if (logo) {
    order(logo, 2);
    logo.traverse((m) => m.material && [m.material].flat().forEach((mt) => mt.color?.setRGB(235 / 255, 205 / 255, 105 / 255, THREE.SRGBColorSpace)));
  }
  // The camera: the crawl's keys, or the game's own move as keys. In free orbit its marker and path show, as a cutscene camera's.
  const camKeys = c.camera?.length ? c.camera : CRAWL_STOCK_CAMERA, s4 = 0.25, mp = [0, 0, 0, -s4, -s4, 2 * s4, s4, -s4, 2 * s4, s4, s4, 2 * s4, -s4, s4, 2 * s4];
  const camNode = new THREE.LineSegments(new THREE.BufferGeometry().setAttribute('position', new THREE.Float32BufferAttribute([0, 1, 0, 2, 0, 3, 0, 4, 1, 2, 2, 3, 3, 4, 4, 1].flatMap((i) => mp.slice(i * 3, i * 3 + 3)), 3)), new THREE.LineBasicMaterial({ color: 0xffcc33, fog: false, depthTest: false }));
  camNode.matrixAutoUpdate = false;
  camNode.renderOrder = 10;
  world.add(camNode);
  const camPts = [];
  for (let i = 0; i < 400; i++) camPts.push(...crawlCameraAt(camKeys, (i / 400) * CRAWL_T.end).pos, ...crawlCameraAt(camKeys, ((i + 1) / 400) * CRAWL_T.end).pos);
  const camPath = new THREE.LineSegments(new THREE.BufferGeometry().setAttribute('position', new THREE.Float32BufferAttribute(camPts, 3)), new THREE.LineBasicMaterial({ color: 0xffcc33, transparent: true, opacity: 0.5, fog: false, depthTest: false }));
  camPath.renderOrder = 10;
  world.add(camPath);
  // applyCutscene hides the camera node (its marker) while looking through it, so what rides with the camera hangs off its own copy.
  const view = new THREE.Group();
  view.matrixAutoUpdate = false;
  world.add(view);
  // The backdrop is a 2D element over the 640 x 300 picture: 640 x 128 with its top 150 lines below the centre, rising to 22 once the
  // camera has dropped past 2.1 (128 lines over the last 2.9). Drawn behind the 3D, unfogged, here on a quad 60 units ahead of the camera.
  let backdrop = null;
  const own = ctx.exists && (await hasBytes(crawlBackdrop(ctx.name)));
  const bdPath = own ? crawlBackdrop(ctx.name) : `data/backdrop/planet_${ctx.base}`;
  const bd = await createImageBitmap(new Blob([u8(await rom.read(bdPath))], { type: 'image/jpeg' })).catch(() => null);
  if (bd) {
    const bc = document.createElement('canvas');
    bc.width = bd.width;
    bc.height = bd.height;
    bc.getContext('2d').drawImage(bd, 0, 0);
    const bt = new THREE.CanvasTexture(bc), px = (2 * 60 * Math.tan(THREE.MathUtils.degToRad(29.03 / 2))) / 300;
    bt.colorSpace = THREE.SRGBColorSpace;
    backdrop = crawlQuad([-320 * px, 0, 60], [320 * px, 0, 60], [-320 * px, 128 * px, 60], [320 * px, 128 * px, 60], bt);
    backdrop.material.fog = false;
    backdrop.material.depthTest = false;
    backdrop.material.transparent = false;
    backdrop.matrixAutoUpdate = true;
    backdrop.userData.px = px;
    backdrop.userData.crawlBackdrop = true;
    order(backdrop, 1);
    view.add(backdrop);
  }
  const crafts = [], [craftName, nCraft] = CRAWL_CRAFT[ctx.base];
  const cm = await loadModel('data/pl_crafts/' + craftName), cobj = cm.hob.objects.find((o) => o.name === craftName) ?? cm.hob.objects[0];
  for (let i = 0; i < nCraft; i++) {
    const route = items.find((x) => x.name === 'route_' + (i % 3)), holder = new THREE.Group();
    if (!route) continue;
    holder.matrixAutoUpdate = false;
    holder.add(buildObject(cobj, cm, 'data/pl_crafts/' + craftName, counts));
    order(holder, 2);
    world.add(holder);
    crafts.push({ holder, at: crawlRoute(sceneDat, route), delay: 0.05 * i });
  }
  // Editing (the cutscene panel's pencil): each route as the line its craft flies, with a handle per point.
  const routes = [];
  for (const it of cutEditing ? items.filter((x) => /^route_\d$/.test(x.name)) : []) {
    const at = crawlRoute(sceneDat, it), line = [];
    for (let i = 0; i < 64; i++) line.push(...at(i / 64), ...at((i + 1) / 64));
    const off = new THREE.Matrix4().makeTranslation(...CRAWL_FLY_OFFSET), lines = new THREE.LineSegments(new THREE.BufferGeometry().setAttribute('position', new THREE.Float32BufferAttribute(line, 3)), new THREE.LineBasicMaterial({ color: 0x66ccff, fog: false, depthTest: false }));
    lines.applyMatrix4(off);
    lines.renderOrder = 10;
    world.add(lines);
    const handles = splinePoints(sceneDat, it).map((p, k) => {
      const h = new THREE.Mesh(new THREE.SphereGeometry(0.08, 10, 8), new THREE.MeshBasicMaterial({ color: 0x66ccff, fog: false, depthTest: false }));
      h.renderOrder = 10;
      h.position.set(p[0] + CRAWL_FLY_OFFSET[0], p[1] + CRAWL_FLY_OFFSET[1], p[2] + CRAWL_FLY_OFFSET[2]);
      h.userData.routePoint = { it, k };
      world.add(h);
      return h;
    });
    routes.push({ it, handles });
  }
  const fps = 30, length = CRAWL_T.end, data = { frames: Math.round(length * fps), fps, length, actors: [], cameras: [{}],
    // Song 2 from the start (runModelViewerSoundDemo); the fade (type 10 stands in for the exit) once the first craft is 98% along.
    events: [{ type: 1, frame: 0, p: [0, 0x7f, 0, 0], song: 2 }, { type: 10, frame: Math.round(CRAWL_T.done * fps), p: [0, 0, 0, 0] }] };
  cut = { data, nodes: [], scale: [], anims: [], camNode, camPath, t: 0, playing: true, fov: 45, sources: [], cues: [], shadows: [], rootOffset: [], shows: [], shots: [], foils: [], flipY: [],
    endFrame: data.frames, end: 1, picAspect: 640 / 300, file: null, seq: false, crawl: { which, ctx, ownBackdrop: own, text, height, logo, backdrop, crafts, view, camKeys, routes, sceneBytes: u8(sceneDat).slice() } };
  $('cutEdit').hidden = false;
  $('cutEditBadge').style.visibility = $('cutLengthRow').style.visibility = 'hidden';
  $('craftRow').hidden = true;
  $('cutFog').closest('label').hidden = true;
  syncSeqUi();
  buildCutTimeline(data);
  // The camera lane shows the crawl's phases instead of one shot.
  const T = CRAWL_T, lane = $('cutTimeline').querySelector('.tlTrack'), pct = (s) => `${(s / length) * 100}%`;
  lane.replaceChildren(...[[0, T.text, 'Logo'], [T.text, T.pan, 'Text'], [T.pan, T.fly, 'Pan down'], [T.fly, T.done, 'Fly-by'], [T.done, length, 'Fade']].map(([a, b, name], i) =>
    elem('div', { className: 'tlBand', textContent: name, title: `${name}: ${a.toFixed(1)} s to ${b.toFixed(1)} s`, style: `left:${pct(a)};width:calc(${pct(b - a)} - 1px);background:${i % 2 ? '#c99a36' : '#e8b84a'}` })));
  crawlKeyRow(length);
  stats = { crawl: ctx.exists ? `${ctx.label} (${ctx.name})` : `${ctx.label} (the game's)`, length: `${length.toFixed(1)} s`, lines: Math.round(height / 1.2), triangles: counts.tris };
  hud();
  $('cutPanel').hidden = false;
  if (keep) {
    cut.t = Math.min(keep.t, 1);
    cut.playing = keep.playing;
    $('cutTime').value = cut.t;
  } else {
    controls.target.set(0, -4, -10);
    camera.position.set(14, -6, 6);
    controls.update();
  }
  syncCutPlay();
  poseCutscene(cut.t);
  loadCutAudio('data/cuts/crawl');
  // A rebuild after an edit keeps a paused crawl silent (as reopenCutscene does for a cutscene).
  if (!cut.playing) audioCtx?.suspend();
  crawlAttach();
  openCrawlPanel(ctx);
  crawlInspectorSync();
}
const _crawlM = new THREE.Matrix4();
function poseCrawl(t) {
  const k = cut.crawl, s = t * cut.data.length, T = CRAWL_T, st = crawlState(s, k.height);
  k.text.visible = s >= T.text && s < T.pan;
  k.text.matrix.makeBasis(...CRAWL_PLANE).setPosition(0.2, 0.30902 * st.C0 + 3.8745, -0.95106 * st.C0 + 1.2979);
  k.text.matrixWorldNeedsUpdate = true;
  if (k.logo) {
    k.logo.visible = s < T.logoGone;
    k.logo.matrix.copy(k.logo.userData.rot).setPosition(0, 0, st.C4 * 0.2);
    k.logo.matrixWorldNeedsUpdate = true;
  }
  // The backdrop moves only during the pan, by the camera's height then (the game's f26, which a keyed camera sets).
  if (k.backdrop) {
    const drop = s < T.pan ? 0 : crawlCameraAt(k.camKeys, Math.min(s, T.fly)).pos[1], y = drop <= 2.1 ? 150 : Math.trunc(150 - ((drop - 2.1) * 128) / 2.9);
    k.backdrop.visible = s >= T.logoGone;
    k.backdrop.position.y = y * k.backdrop.userData.px;
  }
  const cam = crawlCameraAt(k.camKeys, s), posing = gizmo.dragging && crawlSel?.camera;
  if (!posing) cut.camNode.matrix.copy(placementRotation(cam.rot)).setPosition(...cam.pos);
  k.view.matrix.copy(cut.camNode.matrix);
  k.view.matrixWorldNeedsUpdate = true;
  cut.camNode.matrixWorldNeedsUpdate = true;
  scene.fog = new THREE.Fog(0x000000, st.fog[0], st.fog[0] + 32 / st.fog[1]);
  // Craft i: t_i += dt / 7 from -0.05 i, sampled at 0.999 t + 0.001; nose along the tangent; bank = (bank + 4 x yaw turn) x 0.9375 a frame.
  // shortcut: the yaw, pitch and bank signs are read from the code but not checked against the game's picture.
  const frames = Math.max(0, Math.floor((s - T.fly) * 30));
  for (const cr of k.crafts) {
    cr.holder.visible = s >= T.fly;
    if (!cr.holder.visible) continue;
    let bank = 0, prev = null, pos = null, yaw = 0, pitch = 0;
    for (let f = 0; f <= frames; f++) {
      const u = 0.999 * THREE.MathUtils.clamp(f / 30 / 7 - cr.delay, 0, 1) + 0.001, p = cr.at(u), q = cr.at(u - 0.0009), d = [p[0] - q[0], p[1] - q[1], p[2] - q[2]];
      yaw = Math.atan2(d[0], d[2]);
      pitch = -Math.asin(d[1] / (Math.hypot(...d) || 1));
      if (prev != null) bank = (bank + THREE.MathUtils.radToDeg(Math.atan2(Math.sin(yaw - prev), Math.cos(yaw - prev))) * 4) * 0.9375;
      prev = yaw;
      pos = p;
    }
    cr.holder.matrix.makeRotationY(yaw).multiply(_crawlM.makeRotationX(pitch)).multiply(_crawlM.makeRotationZ(THREE.MathUtils.degToRad(bank)));
    cr.holder.matrix.scale(_cax.setScalar(0.0125 * HOB_TO_WORLD)).setPosition(pos[0] + CRAWL_FLY_OFFSET[0], pos[1] + CRAWL_FLY_OFFSET[1], pos[2] + CRAWL_FLY_OFFSET[2]);
    cr.holder.matrixWorldNeedsUpdate = true;
  }
}
// Editing with the pencil, as a cutscene's: the camera (picked in the crawl window) or a route point (clicked) gets the gizmo. Letting go
// keys the camera at the playhead, or writes the point into the crawl's own scene DAT (a copy of the stock one on the first move;
// splineSetPoints redoes the arc table the craft's speed comes from). Keys are { at, pos, rot } (crawlCameraAt), shown on the Keys row.
let crawlSel = null;
const crawlHandles = () => cut?.crawl?.routes.flatMap((r) => r.handles) ?? [];
const crawlKeysNow = () => cut.crawl.ctx.c.camera.length ? cut.crawl.ctx.c.camera : CRAWL_STOCK_CAMERA;
const crawlRound = (v) => Math.round(v * 1000) / 1000;
function crawlPick(e) {
  if (!cutEditing || gizmo.dragging) return false;
  const rp = pick(e, crawlHandles())?.object?.userData.routePoint;
  if (rp) crawlSel = { name: rp.it.name, k: rp.k };
  else if (!crawlSel?.camera) crawlSel = null;
  crawlAttach();
  return true;
}
function crawlAttach() {
  if (!cutEditing) {
    if (gizmo.object === editProxy) gizmo.detach();
    return;
  }
  if (crawlSel?.camera && cut?.crawl) {
    const cam = crawlCameraAt(crawlKeysNow(), cut.t * cut.data.length);
    cut.camNode.parent.add(editProxy);
    editProxy.position.set(...cam.pos);
    editProxy.quaternion.setFromRotationMatrix(placementRotation(cam.rot));
    editProxy.scale.setScalar(1);
    editProxy.updateMatrixWorld(true);
    gizmo.attach(editProxy);
    return;
  }
  const h = crawlSel && crawlHandles().find((x) => x.userData.routePoint.it.name === crawlSel.name && x.userData.routePoint.k === crawlSel.k);
  if (!h) {
    if (gizmo.object === editProxy) gizmo.detach();
    return;
  }
  h.parent.add(editProxy);
  editProxy.position.copy(h.position);
  editProxy.quaternion.identity();
  editProxy.scale.setScalar(1);
  editProxy.updateMatrixWorld(true);
  if (gizmo.mode !== 'translate') setCutTool('translate');
  gizmo.attach(editProxy);
}
const crawlSelHandle = () => crawlSel && !crawlSel.camera && cut?.crawl?.routes.find((r) => r.it.name === crawlSel.name)?.handles[crawlSel.k];
function onCrawlGizmoMove() {
  if (crawlSel?.camera && cut?.crawl) {
    cut.camNode.matrix.compose(editProxy.position, editProxy.quaternion, _cax.set(1, 1, 1));
    cut.camNode.matrixWorldNeedsUpdate = true;
    return;
  }
  crawlSelHandle()?.position.copy(editProxy.position);
}
// A camera key at second at (replacing one within 0.05 s), starting from the game's own move when the crawl has none yet.
function crawlKeyCamera(at, pos, q, label) {
  const e = new THREE.Euler().setFromQuaternion(q, 'ZYX'), key = { at: crawlRound(at), pos: pos.toArray().map(crawlRound), rot: [e.x, e.y, e.z].map((a) => crawlRound(THREE.MathUtils.radToDeg(a))) };
  const next = [...crawlKeysNow().filter((k) => Math.abs(k.at - key.at) > 0.05).map((k) => ({ ...k })), key].sort((a, b) => a.at - b.at);
  crawlKeySel = next.indexOf(key);
  return saveCrawl(cut.crawl.ctx, label, { camera: next });
}
async function onCrawlGizmoUp() {
  const k = cut?.crawl;
  if (!k || !crawlSel) return;
  if (crawlSel.camera) return crawlKeyCamera(cut.t * cut.data.length, editProxy.position, editProxy.quaternion, 'pose camera');
  const r = k.routes.find((x) => x.it.name === crawlSel.name);
  if (!r) return;
  const pts = r.handles.map((h) => h.position.toArray().map((v, a) => v - CRAWL_FLY_OFFSET[a]));
  await saveCrawl(k.ctx, `move ${r.it.name} point ${crawlSel.k + 1}`, {}, () => editAsset(crawlDat(k.ctx.name), (b) => splineSetPoints(b.length ? b : k.sceneBytes, r.it, pts), `Crawl ${k.ctx.label}: move ${r.it.name}`));
}
// With the pencil on, the cutscene inspector poses the crawl's camera (its scene has no actors or events to add): Pose… picks it, the
// tool row moves, rotates or takes the view, and the picked key's values show in the key panel as a cutscene camera key's do.
function crawlInspectorSync() {
  const ins = $('cutInspector'), on = cutEditing && !!cut?.crawl;
  ins.classList.toggle('crawlMode', on);
  $('insCut').querySelector('h4').textContent = on ? 'Opening crawl' : 'Cutscene';
  ins.hidden = !on;
  $('cutPanel').classList.toggle('editing', on);
  applyCutPanelSize();
  if (!on) return placeCrawlPanel();
  const sel = $('cutTrack');
  sel.length = 1;
  sel.append(new Option('Camera', 'crawlcam'));
  sel.value = crawlSel?.camera ? 'crawlcam' : '';
  $('insTrack').hidden = !crawlSel?.camera;
  $('insTrackName').textContent = 'Camera';
  crawlKeyPanel();
  placeCutInspector();
  placeCrawlPanel();
}
function crawlKeyPanel() {
  const p = $('cutEvPanel'), keys = cut.crawl.ctx.c.camera, k = crawlSel?.camera && crawlKeySel != null ? keys[crawlKeySel] : null;
  p.hidden = !k;
  if (!k) return;
  const names = ['x', 'y', 'z', 'rot x', 'rot y', 'rot z'], v = [...k.pos, ...k.rot];
  p.innerHTML = `<b>Key at ${k.at.toFixed(2)} s</b>` + names.map((n, c) => `<label>${n} <input type="number" step="any" data-ch="${c}" value="${+v[c].toFixed(4)}"></label>`).join('')
    + '<button id="cutKeyDel" data-icon="delete" title="Delete this key (Delete key)"></button>';
  setEditIcon($('cutKeyDel'), 'delete');
  $('cutKeyDel').onclick = crawlDeleteKey;
  for (const inp of p.querySelectorAll('[data-ch]')) inp.onchange = () => {
    const c = +inp.dataset.ch, val = +inp.value;
    if (Number.isNaN(val)) return;
    const next = keys.map((x) => ({ ...x, pos: [...x.pos], rot: [...x.rot] }));
    (c < 3 ? next[crawlKeySel].pos : next[crawlKeySel].rot)[c % 3] = val;
    saveCrawl(cut.crawl.ctx, 'key value', { camera: next });
  };
}
function crawlDeleteKey() {
  const keys = cut?.crawl?.ctx.c.camera;
  if (crawlKeySel == null || !keys?.[crawlKeySel]) return;
  const next = keys.filter((_, j) => j !== crawlKeySel);
  crawlKeySel = null;
  saveCrawl(cut.crawl.ctx, 'delete camera key', { camera: next });
}
// Snapshot from view (V): the camera takes the free view at the playhead.
function crawlSnapFromView() {
  const v = cut?.crawl && cutViewPose();
  if (v) crawlKeyCamera(cut.t * cut.data.length, v.p, v.q, 'camera from view');
}
// The Keys row under the timeline's lanes: the camera's keys (none while it is the game's own). Click one to pick it (and jump there),
// drag it to retime it, Delete removes it.
function crawlKeyRow(length) {
  const keys = cut.crawl.ctx.c.camera, tl = $('cutTimeline'), over = tl.lastElementChild, track = elem('div', { className: 'tlTrack' });
  tl.insertBefore(elem('span', { className: 'tlName', textContent: 'Keys' }), over);
  tl.insertBefore(track, over);
  keys.forEach((k, i) => {
    const d = elem('i', { className: 'tlKey' + (i === crawlKeySel ? ' sel' : ''), title: `Camera key at ${k.at.toFixed(2)} s: drag to retime, Delete removes it`, style: `left:${(k.at / length) * 100}%` });
    d.onpointerdown = (e) => {
      e.stopPropagation();
      d.setPointerCapture(e.pointerId);
      const r = track.getBoundingClientRect(), at0 = k.at;
      let at = at0;
      crawlKeySel = i;
      cut.t = at / length;
      $('cutTime').value = cut.t;
      d.onpointermove = (m) => {
        at = THREE.MathUtils.clamp(((m.clientX - r.left) / r.width) * length, 0, length);
        d.style.left = `${(at / length) * 100}%`;
      };
      d.onpointerup = () => {
        d.onpointermove = d.onpointerup = null;
        if (Math.abs(at - at0) < 0.02) {
          // A picked key picks the camera too, as a cutscene key picks its track.
          for (const m of track.children) m.classList.toggle('sel', m === d);
          crawlSel = { camera: true };
          $('cutCam').checked = false;
          crawlAttach();
          return crawlInspectorSync();
        }
        const next = keys.map((x, j) => ({ ...x, at: j === i ? crawlRound(at) : x.at })).sort((a, b) => a.at - b.at);
        crawlKeySel = next.findIndex((x) => x.at === crawlRound(at));
        saveCrawl(cut.crawl.ctx, 'retime camera key', { camera: next });
      };
    };
    track.append(d);
  });
}
addEventListener('keydown', (e) => {
  if (!cut?.crawl || !cutEditing || e.target.closest?.('input,select,textarea')) return;
  if (DELETE_KEYS.has(e.key)) crawlDeleteKey();
  else if (e.code === 'KeyV') crawlSnapFromView();
  else if (e.code === 'KeyW') setCutTool('translate');
  else if (e.code === 'KeyE' && crawlSel?.camera) setCutTool('rotate');
});
// Leaving the cutscene camera keeps its view: the free camera starts where it was (field of view included) and orbits a point ahead of it.
$('cutCam').onchange = () => {
  if (!cut || $('cutCam').checked) return;
  controls.target.copy(camera.position).add(camera.getWorldDirection(new THREE.Vector3()).multiplyScalar(5));
  controls.update();
};
// ---- Cutscene timeline: a ruler, a band per camera shot, and the events in lanes, under the playback controls ----
const TL_LANES = [['camera', 'Camera'], ['audio', 'Audio'], ['action', 'Action'], ['scene', 'Scene']];
const tlHidden = new Set();
const TL_KIND = { 0: 'Voice', 1: 'Music', 2: 'Music stop', 3: 'Sound', 5: 'Effect', 6: 'Appear', 7: 'Disappear', 8: 'Weapon fire', 10: 'End transition',
  11: 'S-foils', 12: 'Voice stop', 13: 'Walk rate', 15: 'Fireworks / debris', 16: 'NPC burst', 17: 'Fog', 19: 'Smoothing', 20: 'NPCs cleared', 21: 'Effect flag', 22: 'Render pass' };
// What each event's four parameters mean (spawnCutsceneEffectsAndCues, processCutsceneActions; see cutEventInfo): [label, tooltip].
const CUT_PARAM_NAMES = {
  0: [['Voice line', 'The voice id it plays (with its subtitle)']],
  1: [['Song', 'A cue of the cutscene song table'], ['Volume', '0-127 (0 = full)']],
  2: [['Channel', 'The music channel it stops (0-3)']],
  3: [['Sound', 'An entry of the cutscene sound table'], ['Volume', '0-127 (0 = full)'], ['Pan', '0-127, 64 centre (0 = centre)']],
  4: [['Camera', 'The camera it cuts to']],
  5: [['Actor', 'Where the effect spawns'], null, ['Effect', 'The effect id']],
  6: [['Actor', 'The actor that appears']],
  7: [['Actor', 'The actor that disappears']],
  8: [['Actor', 'The actor that fires'], ['Weapon', 'Weapon category (the bolt model and colour)'], ['Speed x10', 'Bolt speed in tenths (0 = 2.5)'], ['Sound', 'The firing sound']],
  11: [['Actor', 'The actor whose S-foils move'], ['Angle + 180', 'Target angle in degrees, plus 180'], ['Frames', 'How long the move takes']],
  13: [['Actor', 'The walker'], ['Rate x10', 'Walk animation rate in tenths']],
  15: [['Emitter', 'Which fireworks or debris case']],
  17: [['Near', 'Draw range / fog start'], ['Far', 'Draw range / fog end']],
  18: [['Actor', 'The actor that becomes the mission\'s craft']],
  19: [['Flag', 'Key smoothing on (1) or off (0)']],
  21: [['Flag', 'Effect parameter flag']],
  22: [['On', 'Extra render pass on (1) or off (0)']],
};
function cutEventInfo(e, data) {
  const actor = (i) => data.actors[i]?.name ?? 'actor ' + i, p = e.p;
  switch (e.type) {
    case 0: {
      const text = voiceLine(p[0]);
      return ['audio', '#6be0d0', `Voice line ${p[0]}${text ? `: "${text}"` : ''}`];
    }
    case 1: {
      const id = cutSong(e), row = audioBank?.music.find((m) => m.index === id);
      return ['audio', '#f0a24a', `Music: ${row?.name ?? 'song ' + id} at volume ${p[1] & 0xff || 0x7f}`];
    }
    case 2: return ['audio', '#a0703a', `Music stops (channel ${p[0] & 3})`];
    case 3: {
      const id = (romBytes[CUT_SFX + p[0] * 2] << 8) | romBytes[CUT_SFX + p[0] * 2 + 1];
      return ['audio', '#9ad0ff', `Sound: ${audioBank?.sfx.find((r) => r.index === id)?.label ?? 'sfx ' + hex(id)} (volume ${p[1] & 0xff || 0x7f}, pan ${p[2] & 0xff || 0x40})`];
    }
    case 5: return ['action', '#ffb347', `Spawn effect ${p[2]} at ${actor(p[0])}`];
    case 6: return ['action', '#e6edf7', `${actor(p[0])} appears`];
    case 7: return ['action', '#8a8f9c', `${actor(p[0])} disappears`];
    case 8: return ['action', '#ff4a3a', `${actor(p[0])} fires (weapon category ${p[1]}, speed ${p[2] ? p[2] / 10 : 2.5})`];
    case 11: return ['action', '#c792ff', `${actor(p[0] & 0xffff)} S-foils to ${p[1] - 180} deg over ${p[2]} frames`];
    case 13: return ['action', '#9be37a', `${actor(p[0])} walk rate ${p[1] / 10}`];
    case 15: return ['action', '#ffb347', FIREWORK_EMITTERS[p[0]] ? `Fireworks: a rocket every 0.3 s for ${FIREWORK_EMITTERS[p[0]].life} s` : `Debris field (case ${p[0]})`];
    case 16: return ['action', '#ffb347', 'NPC burst along a path'];
    case 20: return ['action', '#8a8f9c', 'Spawned NPCs cleared'];
    case 10: return ['scene', '#ffffff', 'Ending transition can start'];
    case 12: return ['audio', '#a0703a', 'Stop a voice'];
    case 17: return ['scene', '#9aa3b5', `Draw range / fog ${p[0]} to ${p[1]}`];
    case 19: return ['scene', '#5b6bd8', `Key smoothing flag ${p[0]}`];
    case 21: return ['scene', '#5b6bd8', `Effect parameter flag ${p[0]}`];
    case 22: return ['scene', '#5b6bd8', `Extra render pass ${p[0] ? 'on' : 'off'}`];
    default: return null;
  }
}
function buildCutTimeline(data) {
  // Events timed past the last frame sit at the end (drawn hollow: they play after the cutscene).
  const tl = $('cutTimeline'), at = (f) => `${(Math.min(f, data.frames) / data.frames) * 100}%`;
  tl.textContent = '';
  const ruler = elem('div', { className: 'tlRuler' }), step = data.length > 40 ? 10 : data.length > 15 ? 5 : 2;
  for (let s = 0; s <= data.length; s += 1) {
    const x = `${(s / data.length) * 100}%`;
    ruler.append(elem('i', { style: `left:${x}` }));
    if (s % step === 0) ruler.append(elem('span', { textContent: s + 's', style: `left:${x}` }));
  }
  tl.append(elem('span'), ruler);
  const tracks = {};
  for (const [key, label] of TL_LANES) {
    const name = elem('span', { className: 'tlName' + (tlHidden.has(key) ? ' off' : ''), textContent: label, title: `Show or hide the ${label.toLowerCase()} markers` });
    const track = elem('div', { className: 'tlTrack' + (tlHidden.has(key) ? ' off' : '') });
    name.onclick = () => {
      tlHidden.has(key) ? tlHidden.delete(key) : tlHidden.add(key);
      name.classList.toggle('off');
      track.classList.toggle('off');
    };
    tracks[key] = track;
    tl.append(name, track);
  }
  if (cutEditing && cut.file) buildClipLanes(tracks);
  else buildMarkerLanes(data, tracks, at);
  // Legend: one swatch per marker colour this cutscene uses, named by the event kinds that share it.
  const kinds = new Map([['#e8b84a', new Set(['Camera shot'])]]);
  for (const e of data.events) {
    const info = cutEventInfo(e, data);
    if (info) (kinds.get(info[1]) ?? kinds.set(info[1], new Set()).get(info[1])).add(TL_KIND[e.type]);
  }
  $('cutLegend').replaceChildren(...[...kinds].map(([color, names]) => {
    const s = elem('span');
    s.append(elem('i', { style: `background:${color}` }), [...names].join(' / '));
    return s;
  }));
  // The playhead carries its time, as the panel's readout does.
  const head = elem('div', { className: 'tlHead' }, elem('span', { className: 'tlHeadTime' }));
  const over = elem('div', { style: 'position:absolute;left:64px;right:0;top:0;bottom:0;pointer-events:none' });
  // A shorter ceremony tier never reaches the rest of the file.
  if (cut.endFrame < data.frames) over.append(elem('div', {
    textContent: `${$('optMedal').selectedOptions[0].text} ends here`,
    style: `position:absolute;left:${at(cut.endFrame)};right:0;top:0;bottom:0;background:rgba(0,0,0,.55);border-left:2px solid #fff;color:#fff;font-size:10px;padding-left:4px`,
  }));
  over.append(head);
  tl.append(over);
  cut.tlHead = head;
}
// Editing: the file's events as clips (cutClips) in their lanes, stacked into rows where they overlap. Ends that can move get a resize
// handle; locked ends (voice and sound lengths, the cutscene's own edges) have none. Clips come from cut.file, the file as stored.
let cutEditing = false;
const cutAudioLength = (e) => {
  const row = e.type === 0 && audioBank?.byIndex.get(audioBank.voiceMap[e.p[0]]);
  return row ? rowPcm(row).length / row.rate : null;
};
function buildClipLanes(tracks) {
  const data = cut.file, F = data.frames, at = (f) => `${(f / F) * 100}%`, clips = cutClips(data, cutAudioLength), rows = {};
  cut.clips = clips;
  // Events a camera mode or shot added move with it, never by hand (a hand-moved one would be added again by the next bake).
  const baked = cut.seq ? [] : cutBakedEvents(data, cutAuth);
  for (const c of clips) if ([c.event, c.startEvent, c.endEvent].some((e) => e && baked.includes(e))) {
    c.baked = true;
    c.lockStart ||= baked.includes(c.startEvent);
    c.lockEnd ||= baked.includes(c.endEvent);
  }
  // A sequence's shots trim at their end (hold time); its other clips come from scripts and paths and stay put.
  if (cut.seq) for (const c of clips) Object.assign(c, c.kind === 'shot' ? { lockStart: true, lockEnd: false } : { lockStart: true, lockEnd: true });
  // Rows that do not move as clips are dragged: one group per actor (action lane) or per kind, groups in a fixed order, and overlaps
  // within a group stacked in event order. The picked track's clips are highlighted.
  const ref = (c) => c.actor ?? (c.event && CUT_ACTOR_EVENTS.has(c.event.type) ? (c.event.type === 11 ? c.event.p[0] & 0xffff : c.event.p[0]) : null);
  const keyOf = (c) => (c.lane === 'camera' ? 'cam' : ref(c) != null ? 'a' + String(ref(c)).padStart(3, '0') : 'k' + c.kind + (c.event?.type ?? ''));
  const evIndex = (c) => (c.event ? data.events.indexOf(c.event) : c.start);
  const rowOf = new Map();
  for (const lane of new Set(clips.map((c) => c.lane))) {
    let base = 0;
    const inLane = clips.map((c, i) => i).filter((i) => clips[i].lane === lane);
    for (const key of [...new Set(inLane.map((i) => keyOf(clips[i])))].sort()) {
      const sub = [];
      for (const i of inLane.filter((j) => keyOf(clips[j]) === key).sort((a, b) => evIndex(clips[a]) - evIndex(clips[b]))) {
        const c = clips[i], stop = c.end > c.start ? c.end : c.start + F * 0.008;
        let r = sub.findIndex((sp) => sp.every(([s, e]) => e <= c.start || s >= stop));
        if (r < 0) r = sub.push([]) - 1;
        sub[r].push([c.start, stop]);
        rowOf.set(i, base + r);
      }
      base += sub.length;
    }
    rows[lane] = base;
  }
  const picked = (c) => (cutTrack?.actor != null ? ref(c) === cutTrack.actor : cutTrack?.cam != null && c.kind === 'shot' && (c.startEvent?.p[0] ?? data.cameras.findIndex(Boolean)) === cutTrack.cam);
  for (const i of clips.keys()) {
    const c = clips[i], info = c.event && cutEventInfo(c.event, data);
    const el = elem('div', { className: 'tlClip' + (c.end === c.start ? ' inst' : '') });
    el.dataset.clip = i;
    const name = c.kind === 'shot' ? `Cam ${c.startEvent?.p[0] ?? data.cameras.findIndex(Boolean)}` : c.kind === 'actor' ? data.actors[c.actor]?.name ?? '' : '';
    // Tall rows (the panel dragged taller) have room for each clip's description.
    el.textContent = c.end > c.start ? (cutRowH >= 20 && c.kind !== 'shot' && info ? info[2] : name) : '';
    el.title = c.kind === 'shot' ? `${name}: ${(c.start / data.fps).toFixed(2)} s to ${(c.end / data.fps).toFixed(2)} s` : `${(c.start / data.fps).toFixed(2)} s: ${info?.[2] ?? c.kind}`;
    el.style.background = c.kind === 'shot' ? (i % 2 ? '#c99a36' : '#e8b84a') : info?.[1] ?? '#9aa3b5';
    if (!c.lockStart && c.startEvent && c.end > c.start) el.append(elem('span', { className: 'h s' }));
    if (!c.lockEnd && c.end > c.start) el.append(elem('span', { className: 'h e' }));
    if (picked(c)) el.classList.add('hl');
    if (cut.seq) el.style.cursor = 'pointer';
    Object.assign(el.style, { left: at(c.start), top: `${1 + rowOf.get(i) * cutRowH}px`, height: `${cutRowH - 2}px`, lineHeight: `${cutRowH - 2}px`, fontSize: `${Math.min(12, 7 + cutRowH / 4)}px` });
    if (c.end > c.start) el.style.width = `calc(${at(c.end - c.start)} - 1px)`;
    tracks[c.lane].append(el);
  }
  for (const [lane, n] of Object.entries(rows)) tracks[lane].style.height = `${Math.max(cutRowH, n * cutRowH)}px`;
  // A right-click on empty space in the Audio lane adds music, a voice line or a sound there (a sequence's audio comes from its scripts).
  if (!cut.seq) tracks.audio.oncontextmenu = (e) => {
    if (e.target !== tracks.audio) return;
    e.preventDefault();
    const r = tracks.audio.getBoundingClientRect();
    cutAudioMenu(e.clientX, e.clientY, Math.round(THREE.MathUtils.clamp((e.clientX - r.left) / r.width, 0, 1) * F));
  };
  // An Audio list row dragged here (by its grip) adds its event at the drop point.
  if (!cut.seq) listDropTarget(tracks.audio, () => listDrag?.audio && cutEditing, (e) => {
    const r = tracks.audio.getBoundingClientRect(), ev = cutAudioEventFor(listDrag.audio);
    if (!ev) return void ($('cutStatus').textContent = `${listDrag.label}: cutscenes have no slot for this sound`);
    cutAddAudioEvent(ev[0], ev[1], Math.round(THREE.MathUtils.clamp((e.clientX - r.left) / r.width, 0, 1) * F));
  });
  // A clip too narrow to show a readable piece of its label shows none (the hover title still has it).
  for (const el of $('cutTimeline').querySelectorAll('.tlClip')) if (el.firstChild?.nodeType === 3 && el.offsetWidth < 40) el.firstChild.remove();
  if ($('cutAddKind').options.length < 2) for (const [t, name] of Object.entries(TL_KIND)) $('cutAddKind').append(new Option(name, t));
  buildKeyRow($('cutTimeline'));
  fillCutTrackList();
  // Re-find the selected event by what it is (an undo or an actor removal shifts indices): same type and parameters, nearest frame.
  const sig = cutSelSig, same = sig ? data.events.map((e, k) => [e, k]).filter(([e]) => e.type === sig.type && e.p.join() === sig.p) : [];
  selectCutEvent(same.length ? same.sort((a, b) => Math.abs(a[0].frame - sig.frame) - Math.abs(b[0].frame - sig.frame))[0][1] : null);
}
function buildMarkerLanes(data, tracks, at) {
  // Camera shots: from each cut (type 4) to the next; a cutscene without cuts is one shot.
  const cuts = data.events.filter((e) => e.type === 4).sort((a, b) => a.frame - b.frame);
  const shots = cuts.length ? cuts.map((e, i) => [e.frame, cuts[i + 1]?.frame ?? data.frames, e.p[0]]) : [[0, data.frames, data.cameras.findIndex(Boolean)]];
  shots.forEach(([a, b, cam], i) => tracks.camera.append(elem('div', {
    className: 'tlBand', textContent: `Cam ${cam}`, title: `Camera ${cam}: ${(a / data.fps).toFixed(1)} s to ${(b / data.fps).toFixed(1)} s`,
    style: `left:${at(a)};width:calc(${at(b - a)} - 1px);background:${i % 2 ? '#c99a36' : '#e8b84a'}`,
  })));
  // Markers closer than about 0.8% of the length stack into sub-rows of their lane, so simultaneous events stay visible.
  const rows = {}, gap = data.frames * 0.008;
  for (const e of [...data.events].sort((a, b) => a.frame - b.frame)) {
    const info = cutEventInfo(e, data);
    if (!info) continue;
    const [lane, color, label] = info, m = elem('i', { className: 'tlm', style: `left:${at(e.frame)};background:${color}` });
    m.dataset.frame = e.frame;
    const after = e.frame >= data.frames;
    // Events timed at or past the last frame fire as the mission starts (a briefing line queued into gameplay), so they sit hollow at the end.
    if (after) {
      m.classList.add('after');
      m.style.borderColor = color;
    }
    m.dataset.label = `${(e.frame / data.fps).toFixed(2)} s (frame ${e.frame}): ${label}${after ? ' (after the cutscene: plays once gameplay starts)' : ''}`;
    m.setAttribute('aria-label', m.dataset.label);
    // First row whose last marker is far enough to the left, else a new row.
    const x = Math.min(e.frame, data.frames), last = (rows[lane] ??= []);
    let r = last.findIndex((f) => x - f >= gap);
    if (r < 0) r = last.length;
    last[r] = x;
    m.dataset.row = r;
    tracks[lane].append(m);
  }
  // Lanes with stacked markers grow: 8 px per row, markers 6 px tall.
  for (const [lane, last] of Object.entries(rows)) {
    if (last.length < 2) continue;
    tracks[lane].style.height = `${last.length * 8 + 4}px`;
    for (const m of tracks[lane].querySelectorAll('.tlm')) Object.assign(m.style, { top: `${2 + m.dataset.row * 8}px`, height: '6px' });
  }
}
// Clip drags: the clip and the playhead follow the pointer; on release the file is edited (cutDragClip) as one history step and the
// cutscene is rebuilt, since shots, effects and audio are precomputed at load.
let clipDrag = null;
$('cutTimeline').addEventListener('pointerdown', (e) => {
  const el = e.target.closest?.('.tlClip');
  if (!el || !cut?.clips || e.button !== 0) return;
  e.stopPropagation();
  const clip = cut.clips[+el.dataset.clip], part = e.target.classList.contains('s') ? 'start' : e.target.classList.contains('e') ? 'end' : 'move';
  $('cutTimeline').querySelectorAll('.tlClip.sel').forEach((x) => x.classList.remove('sel'));
  el.classList.add('sel');
  selectCutEvent(clip.event ? cut.file.events.indexOf(clip.event) : null);
  // In a sequence only a shot's end drags (its hold time); everything else just selects.
  if (cut.seq && (clip.kind !== 'shot' || part !== 'end')) return;
  if (clip.baked && part === 'move') return void ($('cutStatus').textContent = 'Set by a camera mode or a shot: change it there');
  clipDrag = { el, clip, part, x0: e.clientX, d: 0, perFrame: el.parentElement.getBoundingClientRect().width / cut.file.frames };
  el.setPointerCapture(e.pointerId);
}, true);
$('cutTimeline').addEventListener('pointermove', (e) => {
  if (!clipDrag) return;
  // A rebuild mid-drag (undo, another cutscene) ends it.
  if (!cut?.file || !clipDrag.el.isConnected) return void (clipDrag = null);
  const g = clipDrag, F = cut.file.frames, c = g.clip, pct = (f) => `${(f / F) * 100}%`;
  g.d = Math.round((e.clientX - g.x0) / g.perFrame);
  let s = c.start, en = c.end;
  if (g.part === 'move') {
    const d = Math.max(-c.start, Math.min(F - c.end, g.d));
    [s, en] = [c.start + d, c.end + d];
  } else if (g.part === 'start') s = Math.max(0, Math.min(c.end - 1, c.start + g.d));
  else en = Math.max(c.start + 1, Math.min(F, c.end + g.d));
  g.el.style.left = pct(s);
  if (en > s) g.el.style.width = `calc(${pct(en - s)} - 1px)`;
  $('cutTime').value = (g.part === 'end' ? en : s) / F;
  $('cutTime').dispatchEvent(new Event('input'));
}, true);
$('cutTimeline').addEventListener('pointerup', async (e) => {
  if (!clipDrag) return;
  const g = clipDrag;
  clipDrag = null;
  e.stopPropagation();
  // A click (no drag) on an actor span or a shot also picks that actor or camera for posing.
  if (!g.d) {
    const tr = g.clip.kind === 'actor' && cut.file.actors[g.clip.actor]?.model ? { actor: g.clip.actor } : g.clip.kind === 'shot' ? { cam: g.clip.startEvent?.p[0] ?? cut.file.cameras.findIndex(Boolean) } : null;
    if (tr && JSON.stringify(tr) !== JSON.stringify(cutTrack)) setCutTrack(tr);
    return;
  }
  // A sequence shot's end sets its camera's hold time.
  if (cut.seq) {
    const k = cut.clips.filter((c) => c.kind === 'shot').indexOf(g.clip);
    return setSeqHold(k, seqShot(k).hold + g.d / cut.file.fps);
  }
  const label = 'Timeline: ' + (g.part === 'move' ? 'move' : 'trim') + ' ' + g.clip.kind, drag = (f) => cutDragClip(f, cutClips(f, cutAudioLength)[+g.el.dataset.clip], g.part, g.d);
  // Moving a cut moves the camera modes of the shots on either side, so they re-bake.
  if (g.clip.kind === 'shot' && Object.keys(cutAuth.tracks).length) await cutAuthEdit(label, (doc, f) => cutSyncModeSpans(doc, f), drag, true);
  else await editCutFile(label, drag);
}, true);
// Every cutscene edit: change a fresh parse of the stored file, write it as one history step, rebuild. A limit error shows in the bar.
// ---- In-mission sequences: edits go to the level's camera records (DAT 0/0x26): hold +0x60, target name +0x64 (item-relative, 32-bit),
// position +0x10, rotation +0x1C (deg, as the level editor writes it). Voice, music and the targets' motion come from scripts and paths.
async function editSeqDat(label, change) {
  const dir = cut.data.seq.dir;
  await editAsset(dir + '/dat', (bytes) => change(dvOf(bytes)), label);
  await reopenCutscene();
}
const seqShot = (k) => cut.data.seq.shots[k];
async function setSeqHold(k, seconds) {
  await editSeqDat(`Shot ${k + 1} hold`, (dv) => dv.setFloat32(seqShot(k).off + 0x60, Math.max(0.1, seconds)));
}
// Target: written over the old name when it fits, else pointed at the target item's own name string.
async function setSeqTarget(k, name) {
  const dv0 = await rom.read(cut.data.seq.dir + '/dat'), items = parseDAT(dv0).items, off = seqShot(k).off;
  const target = name !== '-' && items.find((i) => i.type === 0 && i.name === name);
  if (name !== '-' && !target) return;
  await editSeqDat(`Shot ${k + 1} target`, (dv) => {
    if (!target) return dv.setUint32(off + 0x64, 0);
    const rel = dv.getUint32(off + 0x64), at = rel && (off + rel) % 0x100000000, old = at ? datStr(dv, at) : null;
    if (old != null && name.length <= old.length && at >= off && at < off + dv.getUint32(off + 8)) {
      for (let c = 0; c <= old.length; c++) dv.setUint8(at + c, c < name.length ? name.charCodeAt(c) : 0);
    } else dv.setUint32(off + 0x64, (target.offset + dv.getUint32(target.offset + 0x0C) - off) >>> 0);
  });
}
async function setSeqCameraPose(k, pos, q, mode) {
  const e = new THREE.Euler().setFromQuaternion(q, 'ZYX'), rot = [e.x, e.y, e.z].map((r) => THREE.MathUtils.radToDeg(r)), off = seqShot(k).off;
  await editSeqDat(`Shot ${k + 1} camera`, (dv) => {
    if (mode !== 'rotate') pos.toArray().forEach((v, a) => dv.setFloat32(off + 0x10 + a * 4, v));
    if (mode !== 'translate') rot.forEach((v, a) => dv.setFloat32(off + 0x1C + a * 4, v));
  });
}
function fillSeqShotPanel(p, k) {
  const s = seqShot(k), names = [...new Set(cutLevelHolders().map((h) => h.userData.placedItem.name).filter(Boolean))].sort();
  p.innerHTML = `<b>Shot ${k + 1}: ${esc(s.name)}</b><label>Hold (s) <input type="number" step="0.1" min="0.1" id="seqHold" value="${+s.hold.toFixed(2)}"></label>`
    + `<label>Target <select id="seqTarget">${['-', ...new Set([s.target, ...names])].filter(Boolean).map((n) => `<option${n === s.target ? ' selected' : ''}>${esc(n)}</option>`).join('')}</select></label>`
    + `<label>Next <input disabled value="${esc(s.next)}"></label>`;
  $('seqHold').onchange = () => +$('seqHold').value > 0 && setSeqHold(k, +$('seqHold').value);
  $('seqTarget').onchange = () => setSeqTarget(k, $('seqTarget').value);
}
// A sequence has no file-level length or actor list to edit: its timing is the shots' holds.
function syncSeqUi() {
  const s = !!cut?.seq;
  $('insCut').querySelector('.insRow').hidden = s;
  $('insCut').querySelector('.insDivider').hidden = s;
  if (s) $('cutLengthRow').style.visibility = 'hidden';
}
// Authoring metadata (snapshots and camera modes per track, which the dense keys are baked from) lives in a JSON sidecar stored as the
// virtual asset explorer/<cutscene path>.json, so the edit history, undo and project files carry it; mod exports skip explorer/ paths.
const authoringPath = (cutPath) => 'explorer/' + cutPath + '.json';
const AUTHORING_EMPTY = () => ({ version: 1, tracks: {} });
const readAuthoring = (cutPath) => readJson(authoringPath(cutPath), AUTHORING_EMPTY);
// Removed actors (a Set of indices) lose their tracks and the camera modes aimed at them; later indices shift down as the file's do.
function authoringDropActors(doc, gone) {
  const to = (k) => k - [...gone].filter((g) => g < k).length, tracks = {};
  for (const [key, tr] of Object.entries(doc.tracks)) {
    const [kind, n] = key.split(':');
    if (kind === 'act' && gone.has(+n)) continue;
    tr.modes = (tr.modes ?? []).filter((m) => !gone.has(m.actor)).map((m) => ({ ...m, actor: to(m.actor) }));
    tr.shots = (tr.shots ?? []).filter((x) => !gone.has(x.target)).map((x) => ({ ...x, target: to(x.target) }));
    tracks[kind === 'act' ? `act:${to(+n)}` : key] = tr;
  }
  doc.tracks = tracks;
}
async function editAuthoring(cutPath, label, fn) {
  const doc = await readAuthoring(cutPath);
  fn(doc);
  await writeJson(authoringPath(cutPath), doc, label);
}
// Edits queue: each parses the bytes the previous one wrote, so two quick edits never overwrite each other.
let cutEditQueue = Promise.resolve();
function editCutFile(label, change, authoring = null) {
  if (cut?.seq) {
    $('cutStatus').textContent = 'In a sequence only the shots (hold, target, camera) and level objects can be edited';
    return Promise.resolve(false);
  }
  const run = async () => {
    const path = selected.slice(4), fresh = parseCutscene(await rom.read(path));
    try {
      change(fresh);
    } catch (e) {
      $('cutStatus').textContent = e.message;
      return false;
    }
    $('cutStatus').textContent = '';
    // The cutscene and its authoring sidecar (snapshots, modes) change in one undo step.
    const commit = async () => {
      await editAsset(path, () => writeCutscene(fresh), label);
      if (authoring) await editAuthoring(path, label, authoring);
    };
    await (authoring ? historyGroup(label, commit) : commit());
    await reopenCutscene();
    return true;
  };
  const p = cutEditQueue.then(run, run);
  cutEditQueue = p.catch(() => {});
  return p;
}
// The inspector sits under the timeline panel on the right, so turning editing on moves nothing; it stops above the scene panel.
// While editing, the timeline panel resizes from its bottom edge (row height) and its corner (row height and width); remembered.
let { rowH: cutRowH = 14, w: cutPanelW = null } = stored('rs64.cutSize', {});
function applyCutPanelSize() {
  const p = $('cutPanel');
  // A width saved in a wider window still stops short of the inspector's column (#cutPanel's left 190 / right 290).
  if (cutEditing && cutPanelW) Object.assign(p.style, { width: `${Math.max(500, Math.min(cutPanelW, p.parentElement.clientWidth - 190 - 290))}px`, maxWidth: 'none' });
  else Object.assign(p.style, { width: '', maxWidth: '' });
}
for (const id of ['cutResizeB', 'cutResizeC']) {
  const h = $(id);
  let drag = null, queued = false;
  h.addEventListener('pointerdown', (e) => {
    const rows = [...$('cutTimeline').querySelectorAll('.tlTrack:not(.tlKeys)')].reduce((s, t) => s + t.offsetHeight, 0) / cutRowH;
    drag = { y: e.clientY, x: e.clientX, rowH: cutRowH, rows: Math.max(1, rows), w: $('cutPanel').offsetWidth };
    h.setPointerCapture(e.pointerId);
    e.stopPropagation();
  });
  h.addEventListener('pointermove', (e) => {
    if (!drag) return;
    cutRowH = Math.round(THREE.MathUtils.clamp(drag.rowH + (e.clientY - drag.y) / drag.rows, 14, 48));
    if (id === 'cutResizeC') {
      const avail = $('cutPanel').parentElement.clientWidth - 200 - 290;
      cutPanelW = Math.round(THREE.MathUtils.clamp(drag.w + 2 * (e.clientX - drag.x), 500, Math.max(500, avail)));
      applyCutPanelSize();
    }
    if (queued) return;
    queued = true;
    requestAnimationFrame(() => {
      queued = false;
      if (cut) buildCutTimeline(cut.data);
    });
  });
  h.addEventListener('pointerup', () => {
    drag = null;
    store('rs64.cutSize', { rowH: cutRowH, w: cutPanelW });
  });
  // Double-click puts the timeline back to its normal size.
  h.addEventListener('dblclick', () => {
    [cutRowH, cutPanelW] = [14, null];
    applyCutPanelSize();
    store('rs64.cutSize', null);
    if (cut) buildCutTimeline(cut.data);
  });
}
// Default: top right beside the timeline panel when there is room, else under it. Dragged by its header to anywhere (remembered;
// double-click the header to reset), kept inside the view.
let insPos = stored('rs64.insPos', null);
function placeCutInspector() {
  const ins = $('cutInspector');
  if (ins.hidden) return;
  const main = ins.parentElement.getBoundingClientRect(), panel = $('cutPanel').getBoundingClientRect(), w = ins.offsetWidth || 270;
  let left, top;
  if (insPos) [left, top] = [insPos.x, insPos.y];
  else if (panel.right + 10 + w <= main.right - 10) [left, top] = [main.width - w - 10, 10];
  else [left, top] = [main.width - w - 10, panel.bottom - main.top + 8];
  left = THREE.MathUtils.clamp(left, 0, Math.max(0, main.width - w));
  top = THREE.MathUtils.clamp(top, 0, Math.max(0, main.height - 80));
  Object.assign(ins.style, { left: `${left}px`, top: `${top}px`, right: 'auto', maxHeight: `${Math.max(160, main.height - top - 240)}px` });
}
new ResizeObserver(placeCutInspector).observe($('cutPanel'));
addEventListener('resize', () => {
  applyCutPanelSize();
  placeCutInspector();
});
{
  const head = $('insCut').querySelector('h4');
  head.title = 'Drag to move the inspector; double-click to put it back';
  let drag = null;
  head.addEventListener('pointerdown', (e) => {
    const r = $('cutInspector').getBoundingClientRect();
    drag = { dx: e.clientX - r.left, dy: e.clientY - r.top };
    head.setPointerCapture(e.pointerId);
  });
  head.addEventListener('pointermove', (e) => {
    if (!drag) return;
    const main = $('cutInspector').parentElement.getBoundingClientRect();
    insPos = { x: e.clientX - main.left - drag.dx, y: e.clientY - main.top - drag.dy };
    placeCutInspector();
  });
  head.addEventListener('pointerup', () => {
    drag = null;
    store('rs64.insPos', insPos);
  });
  head.addEventListener('dblclick', () => {
    insPos = null;
    store('rs64.insPos', null);
    placeCutInspector();
  });
}
function syncCutEditUi() {
  $('cutEdit').setAttribute('aria-pressed', cutEditing);
  $('cutInspector').hidden = !cutEditing || !cut?.file;
  $('cutPanel').classList.toggle('editing', !$('cutInspector').hidden);
  applyCutPanelSize();
  $('cutEditBadge').style.visibility = $('cutLengthRow').style.visibility = $('cutInspector').hidden ? 'hidden' : 'visible';
  syncSeqUi();
  placeCutInspector();
  if (!cutEditing) {
    $('cutEvPanel').hidden = true;
    hoverBox.visible = selBox.visible = false;
    setCutTrack(null);
  }
}
// ---- Motion keys: a picked track (camera entry or actor) gets a gizmo proxy at its pose at the playhead; a gizmo drag keys the changed
// channels there (position x 4; rotation as axis + angle; actor scale input). Camera: 0-2 position, 3-5 axis, 6 angle, 7 FOV. Actor: 0-2
// position, 3 scale input, 4-6 axis, 7 angle, 8-10 pivot. Its keys show on the Keys row: click one to select it (and its values), drag
// to retime it, Delete removes it (never the first, at time 0).
let cutTrack = null, cutSelKey = null;
let cutLastClickTimeline = false;
// A click on the timeline outside a clip (scrubbing, or deselecting a key) disarms Delete for the picked actor or camera.
addEventListener('pointerdown', (e) => (cutLastClickTimeline = !!e.target.closest?.('#cutTimeline') && !e.target.closest('.tlClip')), true);
// Backspace too: Mac keyboards have no Delete key.
const DELETE_KEYS = new Set(['Delete', 'Backspace']);
const cutTrackChannels = (f, tr) => (tr.cam != null ? f.cameras[tr.cam] : f.actors[tr.actor].ch);
const CUT_DIVS = { cam: [8, 8, 8, 2.5, 2.5, 2.5, 2.5, 8], actor: [6, 6, 6, 6, 3.5, 3.5, 3.5, 3.5, 3.5, 3.5, 3.5] };
const CUT_CH_NAMES = { cam: ['x', 'y', 'z', 'axis x', 'axis y', 'axis z', 'angle', 'FOV'], actor: ['x', 'y', 'z', 'scale', 'axis x', 'axis y', 'axis z', 'angle', 'pivot x', 'pivot y', 'pivot z'] };
function cutTrackValues(f, tr, t) {
  if (tr.actor != null && !f.actors[tr.actor].animated) return [...f.actors[tr.actor].value];
  const divs = CUT_DIVS[tr.cam != null ? 'cam' : 'actor'];
  return cutTrackChannels(f, tr).map((keys, c) => cutsceneValue(keys, t, f.frames, divs[c]));
}
// An undo or a structure edit can remove the picked track's camera or actor.
const cutTrackValid = (f, tr) => !!f && !!tr && (tr.holder ? tr.holder.parent === root : tr.cam != null ? !!f.cameras[tr.cam] : !!f.actors[tr.actor]?.model);
function fillCutTrackList() {
  const sel = $('cutTrack'), f = cut?.file;
  sel.length = 1;
  if (!f) return;
  // A rebuilt level has new holders: keep a picked level object by its record.
  if (cutTrack?.holder && cutTrack.holder.parent !== root) {
    const off = cutTrack.holder.userData.placedItem.offset, h = cutLevelHolders().find((o) => o.userData.placedItem.offset === off);
    if (h) cutTrack.holder = h;
  }
  if (cutTrack && !cutTrackValid(f, cutTrack)) {
    [cutTrack, cutSelKey] = [null, null];
    gizmo.detach();
    editProxy.removeFromParent();
  }
  f.cameras.forEach((c, k) => c && sel.append(new Option(`Camera ${k}`, `cam:${k}`)));
  f.actors.forEach((a, i) => a.model && sel.append(new Option(`${i} ${a.name}`, `act:${i}`)));
  sel.value = cutTrack && !cutTrack.holder ? (cutTrack.cam != null ? `cam:${cutTrack.cam}` : `act:${cutTrack.actor}`) : '';
  fillCutStruct();
}
function setCutTrack(tr) {
  cutTrack = tr;
  cutSelKey = null;
  if (!tr) {
    gizmo.detach();
    editProxy.removeFromParent();
  } else {
    // Posing a camera needs the free view: its gizmo cannot be seen through the camera it moves.
    if (tr.cam != null) $('cutCam').checked = false;
    gizmo.setMode(gizmo.mode || 'translate');
    gizmo.setSpace(cutSpaceMode);
    if (tr.cam != null && gizmo.mode === 'scale') gizmo.setMode('translate');
    gizmo.showX = gizmo.showY = gizmo.showZ = true;
    syncTrackProxy();
    gizmo.attach(editProxy);
  }
  if (cut) buildCutTimeline(cut.data);
  fillCutTrackList();
}
// While editing, a click on a cutscene model (through the cutscene camera or the free view) picks its actor: its clips light up.
// Hover and selection reach the level's placed objects too: { actor: i } for a cutscene model, { holder } for a level object.
const cutLevelHolders = () => (root?.children ?? []).filter((o) => o.userData.placedItem && o.userData.source && o.visible);
function cutPickTarget(e) {
  const hit = pick(e, [...cut.nodes.filter(Boolean), ...cutLevelHolders()]);
  for (let o = hit?.object; o; o = o.parent) {
    const i = cut.nodes.indexOf(o);
    if (i >= 0 && cut.file.actors[i]?.model) return { actor: i, o };
    if (o.userData.placedItem && o.userData.source) return { holder: o, o };
  }
  return null;
}
function cutPickActor(e) {
  if (cut?.crawl) return crawlPick(e);
  if (!cutEditing || !cut?.file || gizmo.dragging) return false;
  const t = cutPickTarget(e);
  if (t?.actor != null) {
    if (cutTrack?.actor !== t.actor) setCutTrack({ actor: t.actor });
  } else if (t?.holder) setCutTrack({ holder: t.holder });
  else if (cutTrack) setCutTrack(null);
  return true;
}
canvas.addEventListener('pointermove', (e) => {
  if (!cutEditing || !cut?.file || gizmo.dragging || e.buttons) return;
  boxAround(hoverBox, cutPickTarget(e)?.o ?? null);
});
// The selection box follows the picked actor or level object each frame.
function syncCutSelBox() {
  const o = cutTrack?.actor != null ? cut.nodes[cutTrack.actor] : cutTrack?.holder ?? null;
  if (o || selBox.visible) boxAround(selBox, o);
}
// The proxy sits where the track is at the playhead, in its parent's (game) space: actors at position + pivot with their axis-angle
// rotation and scale, cameras at their position and rotation.
function syncTrackProxy() {
  if (!cutTrackValid(cut?.file, cutTrack) || gizmo.dragging) return;
  if (gizmo.object !== editProxy) gizmo.attach(editProxy);
  if (cutTrack.holder) {
    const h = cutTrack.holder;
    if (editProxy.parent !== h.parent) h.parent.add(editProxy);
    h.matrix.decompose(editProxy.position, editProxy.quaternion, editProxy.scale);
    editProxy.updateMatrixWorld(true);
    return;
  }
  const v = cutTrackValues(cut.file, cutTrack, cut.t);
  const parent = cutTrack.cam != null ? cut.camNode.parent : cut.nodes[cutTrack.actor].parent;
  if (editProxy.parent !== parent) parent.add(editProxy);
  const m = new THREE.Matrix4();
  if (cutTrack.cam != null) {
    axisAngle(m, v.slice(3, 6), v[6]);
    editProxy.position.set(v[0] * 0.25, v[1] * 0.25, v[2] * 0.25);
    editProxy.scale.setScalar(1);
  } else {
    axisAngle(m, v.slice(4, 7), v[7]);
    editProxy.position.set((v[0] + v[8]) * 0.25, (v[1] + v[9]) * 0.25, (v[2] + v[10]) * 0.25);
    editProxy.scale.setScalar(v[3]);
  }
  editProxy.quaternion.setFromRotationMatrix(m);
  editProxy.updateMatrixWorld(true);
}
// While the gizmo drags, the track follows it live (the keys are written on release): an actor as poseCutscene builds it, T(pos + pivot)
// S R T(-pivot) with the proxy's position, rotation and channel-3 scale; a camera from the proxy's position and rotation.
const _gm = new THREE.Matrix4(), _gs = new THREE.Vector3();
function poseFromGizmo() {
  if (!cutTrackValid(cut?.file, cutTrack)) return;
  if (cutTrack.holder && trackDragStart) {
    // The level object moves by the proxy's change since the drag began.
    cutTrack.holder.matrix.copy(editProxy.matrix).multiply(_gm.copy(trackDragStart.pm).invert()).multiply(trackDragStart.hm);
    cutTrack.holder.matrixWorldNeedsUpdate = true;
    return;
  }
  if (cutTrack.cam != null) {
    if (!cut.camNode) return;
    cut.camNode.matrix.compose(editProxy.position, editProxy.quaternion, _gs.set(1, 1, 1));
    cut.camNode.matrixWorldNeedsUpdate = true;
    return;
  }
  const i = cutTrack.actor, a = cut.file.actors[i], v = cutTrackValues(cut.file, cutTrack, cut.t), node = cut.nodes[i];
  const s = a.name.startsWith('l_') ? cut.scale[i] : editProxy.scale.x * 0.2222 * (cut.scale[i] || 1);
  node.matrix.compose(editProxy.position, editProxy.quaternion, _gs.set(s, s, s));
  if (cut.flipY[i]) node.matrix.multiply(_gm.makeRotationY(Math.PI));
  node.matrix.multiply(_gm.makeTranslation(-v[8] * 0.25, -v[9] * 0.25, -v[10] * 0.25));
  node.matrixWorldNeedsUpdate = true;
}
let trackDragStart = null;
// A moved level object: a model in the cutscene asset table becomes a new cutscene actor posed where it was dropped (from the playhead
// on); anything else has its level placement (DAT position, rotation, scale) rewritten. Either way the level object itself is reset.
const cutTableNameFor = (id) => [...Array(32).keys()].map((i) => CUT_ASSETS + i * 0x5c).find((o) => cutStr(o, 32) && 'data/' + cutStr(o, 32) === id);
async function dropLevelObject(holder, startMatrix) {
  const m = holder.matrix.clone(), it = holder.userData.placedItem, src = holder.userData.source;
  holder.matrix.copy(startMatrix);
  holder.matrixWorldNeedsUpdate = true;
  const pos = new THREE.Vector3(), q = new THREE.Quaternion(), sc = new THREE.Vector3();
  m.decompose(pos, q, sc);
  const tableAt = cutTableNameFor(src.id);
  if (tableAt != null && !cut.seq) {
    const name = cutStr(tableAt + 0x20, 16), tableScale = new DataView(romBytes.buffer, tableAt + 0x54, 4).getFloat32(0) || 1;
    const w = THREE.MathUtils.clamp(q.w, -1, 1), sin = Math.sqrt(1 - w * w), deg = THREE.MathUtils.radToDeg(2 * Math.acos(w));
    const axis = sin < 1e-6 ? [0, 1, 0] : [q.x / sin, q.y / sin, q.z / sin];
    const vals = [pos.x * 4, pos.y * 4, pos.z * 4, sc.x / HOB_TO_WORLD / 0.2222 / tableScale, ...axis, deg, 0, 0, 0];
    let i = -1;
    const ok = await editCutFile('Add actor ' + name + ' from the level', (f) => {
      i = cutAddActor(f, name);
      vals.forEach((v, ch) => cutSetKey(f, { actor: i, ch }, 0, v));
    });
    if (ok) setCutTrack({ actor: i });
    return;
  }
  if (holder.userData.path) {
    $('cutStatus').textContent = `${it.name || src.name} follows a path: move its spline in the level editor`;
    return;
  }
  // Placement as the level editor writes it: position +0x10, rotation (deg, ZYX) +0x1C, scale +0x28 over the model's divisor.
  const e = new THREE.Euler().setFromQuaternion(q, 'ZYX'), div = holder.userData.npc ? (await loadModel(src.id)).hob.objects[src.objIndex]?.scaleDiv || 1 : 1;
  const rot = [e.x, e.y, e.z].map((r) => THREE.MathUtils.radToDeg(r)), scale = sc.toArray().map((v) => (v / HOB_TO_WORLD) * div);
  const scaled = Math.abs(sc.x / new THREE.Vector3().setFromMatrixScale(startMatrix).x - 1) > 1e-6;
  await editAsset(cutLevel + '/dat', (bytes) => {
    const dv = dvOf(bytes), put = (off, v) => v.forEach((x, a) => dv.setFloat32(it.offset + off + a * 4, x));
    put(0x10, pos.toArray());
    if (it.rot) put(0x1C, rot);
    if (it.type === 0 && scaled) put(0x28, scale);
  }, 'Move ' + (it.name || src.name) + ' in the level');
  clearCaches();
  await reopenCutscene();
}
// Registered once the gizmo exists (hookTrackGizmo after its creation).
const onTrackGizmoDown = () => {
  if (!cutTrack) return;
  editProxy.updateMatrix();
  trackDragStart = { p: editProxy.position.clone(), q: editProxy.quaternion.clone(), s: editProxy.scale.x, pm: editProxy.matrix.clone(), hm: cutTrack.holder?.matrix.clone() };
};
async function onTrackGizmoUp() {
  if (!cutTrack || !trackDragStart) return;
  const st = trackDragStart, tr = cutTrack, t = cut.t, mode = gizmo.mode, q = editProxy.quaternion.clone(), p = editProxy.position.clone(), s = editProxy.scale.x;
  trackDragStart = null;
  // A click that moved nothing keys nothing.
  if (p.distanceToSquared(st.p) < 1e-12 && Math.abs(q.dot(st.q)) > 1 - 1e-9 && Math.abs(s - st.s) < 1e-9) return;
  if (tr.holder) return dropLevelObject(tr.holder, st.hm);
  // A sequence's shot camera is a level camera record; its actors are built from the level's paths.
  if (cut.seq) {
    if (tr.cam != null) return mode === 'scale' ? reopenCutscene() : setSeqCameraPose(tr.cam, p, q, mode);
    $('cutStatus').textContent = 'A sequence actor flies its level path: move the path in the level editor';
    return reopenCutscene();
  }
  const cam = tr.cam != null, ref = (ch) => (cam ? { cam: tr.cam, ch } : { actor: tr.actor, ch });
  await editCutFile(cam ? 'Pose camera' : 'Pose actor', (f) => {
    if (!cam) cutAnimateActor(f, tr.actor);
    const v = cutTrackValues(f, tr, t);
    if (mode === 'translate') {
      const d = p.clone().sub(st.p);
      [0, 1, 2].forEach((k) => cutSetKey(f, ref(k), t, v[k] + d.getComponent(k) * 4));
    } else if (mode === 'rotate') {
      const w = THREE.MathUtils.clamp(q.w, -1, 1), sin = Math.sqrt(1 - w * w), deg = THREE.MathUtils.radToDeg(2 * Math.acos(w));
      const axis = sin < 1e-6 ? [0, 1, 0] : [q.x / sin, q.y / sin, q.z / sin], base = cam ? 3 : 4;
      axis.forEach((a, k) => cutSetKey(f, ref(base + k), t, a));
      cutSetKey(f, ref(base + 3), t, deg);
    } else if (!cam && st.s) cutSetKey(f, ref(3), t, v[3] * (s / st.s));
  });
}
// The Keys row: one diamond per key time across the track's channels.
function cutKeyTimes(f, tr) {
  const times = [];
  for (const keys of cutTrackChannels(f, tr) ?? []) for (let j = 0; j < keys.length / 3; j++) {
    const t = keys[j * 3 + 2];
    if (t <= 1 && !times.some((x) => Math.abs(x - t) < 0.5 / f.frames)) times.push(t);
  }
  return times.sort((a, b) => a - b);
}
// Moves (to = new time) or removes (to = null) the keys at time `from` in every channel of the track; the time-0 keys stay.
function cutRetimeKeys(f, tr, from, to) {
  const chs = cutTrackChannels(f, tr), half = 0.5 / f.frames;
  chs.forEach((keys, ch) => {
    const j = [...Array(keys.length / 3).keys()].find((k) => Math.abs(keys[k * 3 + 2] - from) < half);
    if (j == null || j === 0) return;
    const value = keys[j * 3];
    cutRemoveKey(f, tr.cam != null ? { cam: tr.cam, ch } : { actor: tr.actor, ch }, j);
    if (to != null) cutSetKey(f, tr.cam != null ? { cam: tr.cam, ch } : { actor: tr.actor, ch }, Math.max(1 / f.frames, Math.min(1, to)), value);
  });
}
function buildKeyRow(tl) {
  if (!cutTrackValid(cut?.file, cutTrack) || cutTrack.holder || cut.seq) return;
  const f = cut.file, name = elem('span', { className: 'tlName', textContent: 'Keys' });
  const track = elem('div', { className: 'tlTrack tlKeys' });
  if (f.actors[cutTrack.actor]?.animated === false) track.title = 'This actor is static: posing it adds its first keys';
  // A track with snapshots shows them (the keys are baked from them).
  const snaps = cutSnaps(cutTrack), what = snaps.length ? 'Snapshot' : 'Key';
  if (snaps.length) name.textContent = 'Snapshots';
  for (const t of snaps.length ? snaps.map((x) => x.t) : cutKeyTimes(f, cutTrack)) {
    const k = elem('i', { className: 'tlKey' + (cutSelKey != null && Math.abs(cutSelKey - t) < 0.5 / f.frames ? ' sel' : ''), title: `${what} at ${(t * f.length).toFixed(2)} s` });
    k.style.left = `${t * 100}%`;
    k.dataset.t = t;
    track.append(k);
  }
  tl.append(name, track);
}
let keyDrag = null;
$('cutTimeline').addEventListener('pointerdown', (e) => {
  const k = e.target.closest?.('.tlKey');
  if (!k || e.button !== 0) return;
  e.stopPropagation();
  cutSelKey = +k.dataset.t;
  // A key and an event are never selected together, so Delete removes exactly one thing.
  cutSelEvent = null;
  $('cutTimeline').querySelectorAll('.tlClip.sel').forEach((x) => x.classList.remove('sel'));
  $('cutTimeline').querySelectorAll('.tlKey.sel').forEach((x) => x.classList.remove('sel'));
  k.classList.add('sel');
  $('cutTime').value = cutSelKey;
  $('cutTime').dispatchEvent(new Event('input'));
  fillCutKeyPanel();
  keyDrag = { el: k, x0: e.clientX, w: k.parentElement.getBoundingClientRect().width, t: cutSelKey, to: cutSelKey };
  k.setPointerCapture(e.pointerId);
}, true);
$('cutTimeline').addEventListener('pointermove', (e) => {
  if (!keyDrag) return;
  if (!keyDrag.el.isConnected) return void (keyDrag = null);
  keyDrag.to = THREE.MathUtils.clamp(keyDrag.t + (e.clientX - keyDrag.x0) / keyDrag.w, 0, 1);
  keyDrag.el.style.left = `${keyDrag.to * 100}%`;
}, true);
$('cutTimeline').addEventListener('pointerup', async (e) => {
  if (!keyDrag) return;
  const g = keyDrag;
  keyDrag = null;
  e.stopPropagation();
  const half = 0.5 / cut.file.frames, snaps = cutSnaps(cutTrack);
  if ((!g.t && !snaps.length) || Math.abs(g.to - g.t) < half) return;
  cutSelKey = g.to;
  if (snaps.length) await cutAuthEdit('Retime snapshot', (doc) => {
    const at = cutAuthTrack(doc, cutTrack), s = at.snaps.find((x) => Math.abs(x.t - g.t) < half);
    if (!s) return;
    // It stops a frame short of its neighbours rather than replacing one.
    const i = at.snaps.indexOf(s), lo = at.snaps[i - 1]?.t ?? -Infinity, hi = at.snaps[i + 1]?.t ?? Infinity;
    at.snaps = cutPutSnap(at.snaps.filter((x) => x !== s), { ...s, t: Math.min(1, Math.max(0, lo + 4 * half, Math.min(hi - 4 * half, g.to))) }, half);
  });
  else await editCutFile('Retime key', (f) => cutRetimeKeys(f, cutTrack, g.t, g.to));
}, true);
addEventListener('keydown', async (e) => {
  if (!cutEditing || !cutTrack || e.target.closest?.('input,select,textarea') || e.ctrlKey || e.metaKey || e.altKey) return;
  const mode = { KeyW: 'translate', KeyE: 'rotate', KeyR: 'scale' }[e.code];
  if (mode && (mode !== 'scale' || cutTrack.cam == null)) setCutTool(mode);
  else if (e.code === 'KeyX') setCutSpace(cutSpaceMode === 'local' ? 'world' : 'local');
  else if (e.code === 'KeyV' && !cutTrack.holder) await snapFromView();
  else if (!DELETE_KEYS.has(e.key) || cutSelEvent != null) return;
  // Delete removes the selected key or snapshot, or else the picked actor or camera (undo brings it back).
  else if (cutSelKey != null) (cutSelKey || cutSnaps(cutTrack).length) && (await deleteSelKey());
  // Not right after a scrub or deselect on the timeline, where Delete more likely meant a key.
  else if (!cutTrack.holder && !cutLastClickTimeline) $(cutTrack.cam != null ? 'cutDelCam' : 'cutDelActor').click();
});
// Ctrl+C copies the selected event, or else the picked camera's or actor's pose at the playhead; Ctrl+V pastes the event at the playhead
// (onto the picked actor when it names one) or keys the pose onto the picked track of the same kind.
let cutClipboard = null;
addEventListener('keydown', async (e) => {
  if (!cutEditing || !cut?.file || cut.seq || !(e.ctrlKey || e.metaKey) || e.shiftKey || e.altKey || e.target.closest?.('input,select,textarea')) return;
  const f = cut.file, t = cut.t, tr = cutTrackValid(f, cutTrack) && !cutTrack.holder ? cutTrack : null;
  if (e.code === 'KeyC') {
    if (cutSelEvent != null) cutClipboard = { event: { ...f.events[cutSelEvent], p: [...f.events[cutSelEvent].p] } };
    else if (tr) cutClipboard = { cam: tr.cam != null, pose: cutTrackValues(f, tr, t) };
    else return;
    e.preventDefault();
    $('cutStatus').textContent = cutClipboard.event ? `Copied: ${cutEventInfo(cutClipboard.event, f)?.[2] ?? 'event'}` : `Copied the ${cutClipboard.cam ? 'camera' : 'actor'} pose`;
  } else if (e.code === 'KeyV' && cutClipboard) {
    e.preventDefault();
    if (cutClipboard.event) {
      const ev = { ...cutClipboard.event, p: [...cutClipboard.event.p], frame: Math.round(t * f.frames) };
      if (CUT_ACTOR_EVENTS.has(ev.type) && tr?.actor != null) ev.p[0] = ev.type === 11 ? ((ev.p[0] & ~0xffff) | tr.actor) >>> 0 : tr.actor;
      if (CUT_ACTOR_EVENTS.has(ev.type) && !f.actors[ev.type === 11 ? ev.p[0] & 0xffff : ev.p[0]]) return void ($('cutStatus').textContent = 'Pick an actor to paste this onto');
      let at = -1;
      if (await editCutFile('Paste event', (g) => (at = cutAddEvent(g, ev)))) selectCutEvent(at);
    } else if (!tr || (tr.cam != null) !== cutClipboard.cam) {
      $('cutStatus').textContent = `Pick a ${cutClipboard.cam ? 'camera' : 'actor'} to paste the pose onto`;
    } else {
      const pose = cutClipboard.pose, ref = (ch) => (tr.cam != null ? { cam: tr.cam, ch } : { actor: tr.actor, ch });
      cutSelKey = t;
      await editCutFile('Paste pose', (g) => {
        if (tr.actor != null) cutAnimateActor(g, tr.actor);
        pose.forEach((v, ch) => cutSetKey(g, ref(ch), t, v));
      });
    }
  }
});
// Removes the selected key time from every channel of the track (the 0 s keys and end sentinels stay).
async function deleteSelKey() {
  const t = cutSelKey, snaps = cutSnaps(cutTrack);
  if (t == null || (!t && !snaps.length) || !cutTrack) return;
  cutSelKey = null;
  $('cutEvPanel').hidden = true;
  if (snaps.length) await cutAuthEdit('Delete snapshot', (doc) => {
    const at = cutAuthTrack(doc, cutTrack);
    at.snaps = at.snaps.filter((x) => Math.abs(x.t - t) >= 0.5 / cut.file.frames);
  });
  else await editCutFile('Delete key', (f) => cutRetimeKeys(f, cutTrack, t, null));
}
// The selected key's values, one field per channel, typed values keyed at that time.
function fillCutKeyPanel() {
  const p = $('cutEvPanel');
  if (!cutTrack || cutSelKey == null || !cut?.file) return;
  const names = CUT_CH_NAMES[cutTrack.cam != null ? 'cam' : 'actor'], v = cutTrackValues(cut.file, cutTrack, cutSelKey), snap = cutSnaps(cutTrack).length > 0;
  // A snapshot track's values come from its snapshots (move the view and press V to change one), so they show read-only.
  const ro = snap ? ' disabled title="Baked from the snapshots: fly the view and press V to change it"' : '';
  p.hidden = false;
  p.innerHTML = `<b>${snap ? 'Snapshot' : 'Key'} at ${(cutSelKey * cut.file.length).toFixed(2)} s</b>` + names.map((n, c) => `<label>${n} <input type="number" step="any" data-ch="${c}" value="${+v[c].toFixed(4)}"${ro}></label>`).join('')
    + `<button id="cutKeyDel" data-icon="delete" title="${snap ? 'Delete this snapshot (Delete key)' : cutSelKey ? 'Delete this key (Delete key)' : 'The key at 0 s starts the channel and stays'}"${cutSelKey || snap ? '' : ' disabled'}></button>`;
  setEditIcon($('cutKeyDel'), 'delete');
  $('cutKeyDel').onclick = deleteSelKey;
  for (const inp of p.querySelectorAll('[data-ch]')) inp.onchange = async () => {
    const ch = +inp.dataset.ch, val = +inp.value, t = cutSelKey, tr = cutTrack;
    if (Number.isNaN(val)) return;
    await editCutFile('Key value', (f) => {
      if (tr.actor != null) cutAnimateActor(f, tr.actor);
      cutSetKey(f, tr.cam != null ? { cam: tr.cam, ch } : { actor: tr.actor, ch }, t, val);
    });
    fillCutKeyPanel();
  };
}
// Pose tool buttons (W / E / R, cameras have no scale) and the axes toggle (X), as in the level editor.
let cutSpaceMode = 'local';
function setCutTool(mode) {
  gizmo.setMode(mode);
  syncCutTools();
}
function setCutSpace(space) {
  cutSpaceMode = space;
  gizmo.setSpace(space);
  syncCutTools();
}
function syncCutTools() {
  for (const b of $('insTools').querySelectorAll('[data-ctool]')) {
    b.setAttribute('aria-pressed', b.dataset.ctool === gizmo.mode);
    b.hidden = b.dataset.ctool === 'scale' && cutTrack?.cam != null;
  }
  $('cutCamView').hidden = !cutTrack || !!cutTrack.holder;
  const b = $('cutSpace');
  b.textContent = cutSpaceMode === 'local' ? 'Local' : 'Global';
  b.title = cutSpaceMode === 'local' ? 'Local axes: the track\'s own frame. Click or press X for global axes' : 'Global axes: X red, Y green, Z blue. Click or press X for local axes';
}
// The free view as a camera node pose: the inverse of applyCutscene's camera (node = view x CAM_FLIP, in root's space, file units x 4)
// and of its letterbox FOV, so ticking Camera afterwards shows the same picture inside the bars. Null (with a status) through the camera.
function cutViewPose() {
  if ($('cutCam').checked) {
    $('cutStatus').textContent = 'Untick Camera and fly the view to the shot first';
    return null;
  }
  root.updateMatrixWorld();
  const m = new THREE.Matrix4().copy(root.matrixWorld).invert().multiply(new THREE.Matrix4().multiplyMatrices(camera.matrixWorld, CAM_FLIP));
  const p = new THREE.Vector3(), q = new THREE.Quaternion();
  m.decompose(p, q, new THREE.Vector3());
  const r = canvas.getBoundingClientRect(), band = Math.min(r.height, r.width / cut.picAspect);
  const fov = THREE.MathUtils.radToDeg(2 * Math.atan((Math.tan(THREE.MathUtils.degToRad(camera.fov) / 2) * band) / (r.height * cutBandK())));
  return { p, q, fov };
}
// ---- Snapshots, camera modes and shots: kept in the authoring sidecar, and every change to it re-bakes all its tracks into keys ----
let cutAuth = AUTHORING_EMPTY();
const cutTrackKey = (tr) => (tr.cam != null ? `cam:${tr.cam}` : `act:${tr.actor}`);
const cutSnaps = (tr) => (tr && !tr.holder ? cutAuth.tracks[cutTrackKey(tr)]?.snaps ?? [] : []);
const cutAuthTrack = (doc, tr) => (doc.tracks[cutTrackKey(tr)] ??= {});
// One undo step: edit(doc, file) changes a copy of the sidecar (fileEdit first changes the file), then every track re-bakes. fileFirst
// runs fileEdit before the old bake is undone (an edit that finds its target by index, such as a clip drag).
function cutAuthEdit(label, edit, fileEdit = null, fileFirst = false) {
  let next = null;
  return editCutFile(label, (f) => {
    next = structuredClone(cutAuth);
    if (fileFirst) fileEdit?.(f);
    cutUnbake(f, cutAuth);
    if (!fileFirst) fileEdit?.(f);
    edit(next, f);
    cutBake(f, next);
  }, (doc) => (doc.tracks = next.tracks));
}
const cutPutSnap = (snaps, s, half) => [...snaps.filter((x) => Math.abs(x.t - s.t) >= half), s].sort((a, b) => a.t - b.t);
// Snapshot from view (V): a camera takes the free view's pose and FOV; a root actor goes 8 x its radius ahead of the view.
async function snapFromView() {
  if (cut?.crawl) return crawlSnapFromView();
  const tr = cutTrack, f = cut?.file;
  if (!tr || tr.holder || !f) return;
  const v = cutViewPose();
  if (!v) return;
  if (cut.seq) return tr.cam != null && setSeqCameraPose(tr.cam, v.p, v.q, 'pose');
  const t = cut.t, half = 0.5 / f.frames;
  let snap;
  if (tr.cam != null) snap = { t, p: v.p.clone().multiplyScalar(4).toArray(), q: v.q.toArray(), fov: v.fov };
  else {
    const a = f.actors[tr.actor];
    if (a.parent >= 0) return void ($('cutStatus').textContent = 'Snapshots move root actors: this one rides on its parent');
    const r = ((await cutModelRadius(a.name)) ?? 4) * cutTrackValues(f, tr, t)[3] * 0.2222;
    snap = { t, p: v.p.clone().add(new THREE.Vector3(0, 0, 8 * r).applyQuaternion(v.q)).multiplyScalar(4).toArray() };
  }
  cutSelKey = t;
  await cutAuthEdit('Snapshot', (doc) => {
    const at = cutAuthTrack(doc, tr);
    at.snaps = cutPutSnap(at.snaps ?? [], snap, half);
  });
}
$('cutCamView').onclick = snapFromView;
// Mode of the picked camera over the shot under the playhead: follow / aim / ride a root actor, offset = the camera's pose now.
function cutShotSpan(f, t) {
  const fr = Math.round(t * f.frames), c = cutClips(f, cutAudioLength).find((x) => x.lane === 'camera' && x.start <= fr && fr < Math.max(x.end, x.start + 1));
  return c ? [c.start / f.frames, Math.min(1, c.end / f.frames)] : [0, 1];
}
function fillCutMode() {
  const f = cut?.file, k = cutTrack?.cam, a = cutTrack?.actor;
  $('insMode').hidden = k == null;
  $('insShoot').hidden = a == null;
  if (!f) return;
  const roots = f.actors.map((x, i) => [x, i]).filter(([x]) => x.model && x.parent < 0);
  const opts = (sel, list, first) => {
    sel.length = 0;
    sel.append(new Option(first, ''));
    for (const [x, i] of list) sel.append(new Option(`${i} ${x.name}`, i));
  };
  if (k != null) {
    const m = (cutAuth.tracks[`cam:${k}`]?.modes ?? []).find((x) => x.t0 <= cut.t && cut.t <= x.t1);
    opts($('cutModeActor'), roots, 'Actor…');
    $('cutMode').value = m?.mode ?? '';
    $('cutModeActor').value = m ? m.actor : roots[0]?.[1] ?? '';
  }
  if (a != null) opts($('cutShoot'), roots.filter(([, i]) => i !== a), 'Shoot at…');
}
async function setCutMode() {
  const k = cutTrack?.cam, f = cut?.file, mode = $('cutMode').value, actor = $('cutModeActor').value;
  if (k == null || !f) return;
  if (mode && actor === '') return void ($('cutStatus').textContent = 'Pick the actor the camera works from');
  const [t0, t1] = cutShotSpan(f, cut.t);
  let offset = [0, 0, 0];
  if (mode) {
    const pose = cutActorPose(f, +actor, cut.t), cam = cutTrackValues(f, cutTrack, cut.t).slice(0, 3);
    const inv = new THREE.Quaternion(...pose.q).invert();
    offset = new THREE.Vector3(...cam.map((v, i) => v - pose.p[i])).applyQuaternion(inv).toArray();
  }
  await cutAuthEdit(mode ? `Camera ${mode}` : 'Camera mode off', (doc) => {
    const at = cutAuthTrack(doc, cutTrack);
    at.modes = (at.modes ?? []).filter((m) => m.t1 <= t0 || m.t0 >= t1);
    if (mode) at.modes.push({ mode, actor: +actor, t0, t1, offset });
  });
}
$('cutMode').onchange = setCutMode;
$('cutModeActor').onchange = () => $('cutMode').value && setCutMode();
$('cutShoot').onchange = async () => {
  const i = cutTrack?.actor, target = $('cutShoot').value, t = cut?.t;
  $('cutShoot').value = '';
  if (i == null || target === '') return;
  await cutAuthEdit('Shoot at', (doc) => {
    const at = cutAuthTrack(doc, cutTrack);
    at.shots = [...(at.shots ?? []), { target: +target, t }];
  });
};
for (const b of $('insTools').querySelectorAll('[data-ctool]')) b.onclick = () => setCutTool(b.dataset.ctool);
$('cutSpace').onclick = () => setCutSpace(cutSpaceMode === 'local' ? 'world' : 'local');
// ---- Structure: actors (models from the cutscene asset table, or player crafts), camera entries and length ----
// Only names the game can load: table entries with an asset path, and player crafts by their pl_crafts/ name. The table's own craft
// entries (awing, xwing, ...) have no path; an actor named after one crashes the game at load (tested in-game).
const cutModelNames = () => [...new Set([...[...Array(32).keys()].filter((i) => cutStr(CUT_ASSETS + i * 0x5c, 32)).map((i) => cutStr(CUT_ASSETS + i * 0x5c + 0x20, 16)).filter(Boolean), ...CUT_CRAFTS, ...(cutPathModels ?? []).map((m) => m.name)])];
// Any other shipped model by asset path: a name missing from the table makes spawnCutsceneObjectsFromList load that path and find the
// mesh by the text after the last '/'. A model whose object name differs from its file becomes 'dir/<object>', a mod-added copy (.from).
let cutPathModels = null;
async function cutScanPathModels() {
  if (cutPathModels) return;
  const table = new Set(cutModelNames()), out = [];
  for (const k of [...rom.assets.keys()].sort()) {
    const m = k.match(/^data\/(imp_stuff|reb_stuff|gnrc_stuff)\/([^/]+)_HOB$/);
    if (!m) continue;
    let obj = '';
    try {
      obj = parseHOB(await rom.read(k)).objects[0]?.name ?? '';
    } catch {
      continue;
    }
    if (!obj || obj !== obj.toLowerCase() || table.has(obj) || obj.length > 15) continue;
    if (obj === m[2]) out.push({ name: `${m[1]}/${obj}` });
    else if (!rom.assets.has(`data/${m[1]}/${obj}_HOB`)) out.push({ name: `${m[1]}/${obj}`, from: `${m[1]}/${m[2]}` });
  }
  cutPathModels = out;
}
// The model an asset-path actor shows: its own file, or the file a derived name is copied from.
function cutPathModelId(name) {
  const n = name.replace(/_\d+$/, '');
  if (!n.includes('/') || n.startsWith('pl_crafts/')) return null;
  const id = 'data/' + ((cutPathModels ?? []).find((m) => m.name === n)?.from ?? n);
  return rom.assets.has(id + '_HOB') ? id : null;
}
// The .from files a mod needs for the derived path models its cutscenes name.
async function cutDerivedModelFiles(paths = dirty) {
  const from = new Map((cutPathModels ?? []).filter((m) => m.from).map((m) => [m.name, m.from])), files = new Map();
  for (const p of paths) {
    if (!p.startsWith('data/cuts/')) continue;
    for (const a of parseCutscene(await rom.read(p)).actors) {
      const src = from.get(a.name.replace(/_\d+$/, ''));
      if (!src) continue;
      for (const ext of ['_HOB', '_HMT']) if (rom.assets.has(`data/${src}${ext}`)) files.set(`assets/${a.name.replace(/_\d+$/, '')}${ext}.from`, `${src}${ext}\n`);
    }
  }
  return [...files].map(([name, text]) => ({ name, data: new TextEncoder().encode(text) }));
}
function fillCutStruct() {
  const f = cut?.file;
  if (!f) return;
  if ($('cutAddActor').options.length < 2) for (const n of cutModelNames()) {
    $('cutAddActor').append(new Option(n, n));
    $('cutModel').append(new Option(n, n));
  }
  const a = cutTrack?.actor != null ? f.actors[cutTrack.actor] : null;
  $('insTrack').hidden = !cutTrack;
  syncCutTools();
  $('insTrackName').textContent = a ? `Actor ${cutTrack.actor}: ${a.name}` : cutTrack?.holder ? `Level object: ${cutTrack.holder.userData.placedItem.name || cutTrack.holder.userData.source.name}` : cutTrack ? `Camera ${cutTrack.cam}` : '';
  $('insModel').hidden = $('cutDelActor').hidden = !a;
  $('cutDelCam').hidden = cutTrack?.cam == null;
  fillCutMode();
  $('cutModel').value = a ? (cutModelNames().find((n) => cutBase(n) === cutBase(a.name)) ?? '') : '';
  $('cutDelCam').disabled = f.cameras.filter(Boolean).length < 2;
  $('cutLength').value = +f.length.toFixed(2);
}
// Bounding radius (root units) of an actor model at its table scale, found the way showCutscene loads it; null when it has no model.
async function cutModelRadius(name) {
  const base = cutBase(name), asset = cutAsset(base), tabled = asset.path && 'data/' + asset.path;
  const id = cutPathModelId(name) ?? (tabled && rom.assets.has(tabled + '_HOB') ? tabled : (await buildObjectIndex()).get(base));
  if (!id) return null;
  const m = await loadModel(id), mesh = buildObject(m.hob.objects.find((o) => o.name === base) ?? m.hob.objects[0], m, id, { tris: 0, verts: 0, parts: 0 });
  mesh.scale.setScalar(HOB_TO_WORLD * (asset.scale || 1));
  // Only what is drawn: the group also holds hidden LODs and parts.
  const box = new THREE.Box3();
  mesh.updateMatrixWorld(true);
  mesh.traverseVisible((o) => o.isMesh && box.expandByObject(o));
  return box.isEmpty() ? null : box.getBoundingSphere(new THREE.Sphere()).radius;
}
// The scale channel the stock cutscenes give a model most often (player crafts 0.056, atpt_imp 0.24, ...); the default 1 / 0.2222
// (model scale 1) is ~70x a stock X-wing. Models no stock cutscene uses get the craft or level-object value.
let cutStockScales = null;
async function cutStockScale(name) {
  if (!cutStockScales) {
    const counts = new Map();
    for (const p of [...rom.assets.keys()].filter((k) => k.startsWith('data/cuts/id'))) {
      for (const a of parseCutscene(await rom.read(p)).actors) {
        if (!a.model) continue;
        const v = +(a.animated ? a.ch[3][0] : a.value[3]).toFixed(4), m = counts.get(cutBase(a.name)) ?? new Map();
        counts.set(cutBase(a.name), m.set(v, (m.get(v) ?? 0) + 1));
      }
    }
    cutStockScales = new Map([...counts].map(([k, m]) => [k, [...m].sort((x, y) => y[1] - x[1])[0][0]]));
  }
  return cutStockScales.get(cutBase(name)) ?? (name.startsWith('pl_crafts/') ? 0.0562 : 0.24);
}
$('cutAddActor').onchange = () => {
  const name = $('cutAddActor').value;
  $('cutAddActor').value = '';
  cutAddActorNamed(name);
};
// The actor name a model list row (asset id without _HOB, and object index for a package) adds as: the asset table's or a stock craft's
// name, else the asset path; null when cutscenes can't load it.
async function cutActorNameFor(id) {
  await cutScanPathModels();
  const names = cutModelNames(), rel = id.replace(/^data\//, '');
  return names.find((n) => n === rel) ?? names.find((n) => n === rel.split('/').pop()) ?? null;
}
async function cutAddActorNamed(name) {
  if (!name || !cut?.file) return;
  const scale = await cutStockScale(name);
  // Starts where the first actor is at the playhead (its keys are in its parent's space; a root actor's are the scene's).
  const t = cut.t, root = cut.file.actors.findIndex((x) => x.parent < 0 && x.model);
  // With no actor to start from, it goes in front of the camera (which shares the root actors' parent and looks down its +Z, CAM_FLIP),
  // far enough that its bounding sphere fills about a third of the picture height.
  const inFront = async () => {
    const p = new THREE.Vector3(), q = new THREE.Quaternion(), r = ((await cutModelRadius(name)) ?? 4) * scale * 0.2222;
    cut.camNode.matrix.decompose(p, q, new THREE.Vector3());
    const d = Math.max(1, (r * 3) / (Math.tan(THREE.MathUtils.degToRad(cut.fov || 50) / 2) * cutBandK()));
    return p.add(new THREE.Vector3(0, 0, d).applyQuaternion(q)).multiplyScalar(4).toArray();
  };
  const at = root >= 0 ? cutTrackValues(cut.file, { actor: root }, t).slice(0, 3) : cut.camNode ? await inFront() : [0, 0, 0];
  let i = -1;
  const ok = await editCutFile('Add actor ' + name, (f) => {
    i = cutAddActor(f, name, -1, scale);
    at.forEach((v, k) => [0, 1].forEach((tt) => cutSetKey(f, { actor: i, ch: k }, tt, v)));
  });
  if (ok) setCutTrack({ actor: i });
}
$('cutModel').onchange = async () => {
  const i = cutTrack?.actor, name = $('cutModel').value;
  if (i == null || !name) return;
  const suffix = cut.file.actors[i].name.match(/_\d+$/)?.[0] ?? '';
  await editCutFile('Change model', (f) => cutSetActorName(f, i, name + suffix));
  fillCutStruct();
};
$('cutDelActor').onclick = async () => {
  const i = cutTrack?.actor, f = cut?.file;
  if (i == null || !f) return;
  // The ending ceremony's code names the medal by its index (0x1B, processCutsceneActions), so nothing before it may shift.
  if (lastCutPath?.endsWith('id3_special') && i <= 0x1b) {
    $('cutStatus').textContent = 'The medal ceremony finds the medal by its actor number (27): actors up to it cannot be removed';
    return;
  }
  // The actor, its children and the events naming any of them all go.
  const gone = new Set([i]);
  for (let grew = true; grew;) {
    grew = false;
    f.actors.forEach((a, k) => !gone.has(k) && gone.has(a.parent) && (grew = gone.add(k)));
  }
  const uses = f.events.filter((e) => CUT_ACTOR_EVENTS.has(e.type) && gone.has(e.type === 11 ? e.p[0] & 0xffff : e.p[0])).length;
  const kids = gone.size - 1, also = [kids && `${kids} child actor${kids > 1 ? 's' : ''}`, uses && `${uses} event${uses > 1 ? 's' : ''}`].filter(Boolean).join(' and ');
  if (also && !confirm(`Removing ${f.actors[i].name} also removes ${also}.`)) return;
  setCutTrack(null);
  await cutAuthEdit('Remove actor', (doc) => authoringDropActors(doc, gone), (f) => cutRemoveActor(f, i));
};
$('cutBlank').onclick = async () => {
  if (!cut?.file) return;
  // The ending ceremony's code finds the medal by its actor number, so it cannot lose its actors.
  if (lastCutPath?.endsWith('id3_special')) {
    $('cutStatus').textContent = 'The medal ceremony needs its medal actor (27): it cannot start from blank';
    return;
  }
  if (!confirm('Start this cutscene from blank? Every actor, camera move, voice, sound and music event goes (undo brings them back).')) return;
  setCutTrack(null);
  await editCutFile('Start from blank', (f) => cutBlank(f, f.length), (doc) => (doc.tracks = {}));
};
$('cutAddCam').onclick = async () => {
  let k = -1;
  if (await editCutFile('Add camera', (f) => (k = cutAddCamera(f)))) setCutTrack({ cam: k });
};
$('cutDelCam').onclick = async () => {
  const k = cutTrack?.cam;
  if (k == null) return;
  setCutTrack(null);
  await cutAuthEdit('Remove camera', (doc) => delete doc.tracks['cam:' + k], (f) => cutRemoveCamera(f, k));
};
$('cutLength').onchange = async () => {
  const s = +$('cutLength').value;
  if (!(s > 0) || !cut?.file || Math.abs(s - cut.file.length) < 1e-3) return;
  // Snapshots, modes and shots keep their seconds; ones past the new end go.
  const old = cut.file.length;
  await cutAuthEdit('Length', (doc, f) => {
    for (const tr of Object.values(doc.tracks)) {
      if (tr.snaps) tr.snaps = cutRetimeSnaps(tr.snaps, old, s);
      if (tr.modes) tr.modes = tr.modes.map((m) => ({ ...m, t0: (m.t0 * old) / s, t1: Math.min(1, (m.t1 * old) / s) })).filter((m) => m.t0 < 1);
      if (tr.shots) tr.shots = cutRetimeSnaps(tr.shots, old, s);
    }
    // Rounded to frames, a mode span can miss its cut by one; snap each to its shot.
    cutSyncModeSpans(doc, f);
  }, (f) => cutSetLength(f, s));
};
$('cutTrack').onchange = () => {
  const v = $('cutTrack').value;
  if (cut?.crawl) {
    crawlSel = v ? { camera: true } : null;
    // As a cutscene camera: its gizmo cannot be seen through the camera it moves.
    if (v) $('cutCam').checked = false;
    crawlAttach();
    return crawlInspectorSync();
  }
  setCutTrack(!v ? null : v.startsWith('cam:') ? { cam: +v.slice(4) } : { actor: +v.slice(4) });
};
$('cutEdit').onclick = () => {
  cutEditing = !cutEditing;
  syncCutEditUi();
  if (cut?.crawl) showCrawl(cut.crawl.which, { t: cut.t, playing: cut.playing });
  else if (cut) buildCutTimeline(cut.data);
};
// Add: a new event of the picked kind at the playhead, its parameters copied from the nearest one of that kind in the file (or zeros).
$('cutAddKind').onchange = async () => {
  const type = +$('cutAddKind').value;
  $('cutAddKind').value = '';
  if (!cut?.file || Number.isNaN(type)) return;
  const frame = Math.round(cut.t * cut.file.frames);
  const like = cut.file.events.filter((e) => e.type === type).sort((a, b) => Math.abs(a.frame - frame) - Math.abs(b.frame - frame))[0];
  let at = -1;
  if (await editCutFile('Add ' + (TL_KIND[type] ?? 'event'), (f) => (at = cutAddEvent(f, { type, frame, p: like ? [...like.p] : [0, 0, 0, 0] })))) selectCutEvent(at);
};
addEventListener('keydown', async (e) => {
  if (!DELETE_KEYS.has(e.key) || !cutEditing || cutSelEvent == null || !cut?.file || e.target.closest?.('input,select,textarea')) return;
  const i = cutSelEvent;
  if (cutBakedEvents(cut.file, cutAuth).includes(cut.file.events[i])) return void ($('cutStatus').textContent = 'Set by a camera mode or a shot: change it there');
  selectCutEvent(null);
  await editCutFile('Delete ' + (TL_KIND[cut.file.events[i]?.type] ?? 'event'), (f) => cutRemoveEvent(f, i));
});
let cutSelSig = null;
// The selected event's panel: its time and parameters, with pickers where a parameter names something (voice line, song, actor, camera).
let cutSelEvent = null;
function selectCutEvent(i) {
  cutSelEvent = i;
  const ev = i != null && cut?.file?.events[i];
  cutSelSig = ev ? { type: ev.type, p: ev.p.join(), frame: ev.frame } : null;
  if (i != null) cutSelKey = null;
  const tl = $('cutTimeline');
  tl.querySelectorAll('.tlClip.sel').forEach((x) => x.classList.remove('sel'));
  const k = i == null || !cut?.clips ? -1 : cut.clips.findIndex((c) => c.event && cut.file.events.indexOf(c.event) === i);
  if (k >= 0) tl.querySelector(`.tlClip[data-clip="${k}"]`)?.classList.add('sel');
  fillCutEvPanel();
}
function cutParamPicker(e, k) {
  const f = cut.file, opts = (list) => list.map(([v, l]) => `<option value="${v}"${v === e.p[k] ? ' selected' : ''}>${esc(l)}</option>`).join('');
  if (k === 0 && e.type === 0) return opts([...new Set([e.p[0], ...(audioBank?.voiceMap ?? []).map((_, v) => v)])].sort((a, b) => a - b).map((v) => [v, `${v} ${voiceLine(v) ?? ''}`.slice(0, 70)]));
  if (k === 0 && e.type === 1) return opts([...Array(64).keys()].map((n) => [n, audioBank?.music.find((m) => m.index === cutSong({ p: [n] }))?.name ?? `cue ${n}`]));
  if (k === 0 && e.type === 3) return opts([...Array(CUT_SFX_COUNT).keys()].map((n) => [n, cutSfxLabel(n)]));
  if (k === 0 && CUT_ACTOR_EVENTS.has(e.type) && e.type !== 11) return opts(f.actors.map((a, n) => [n, `${n} ${a.name}`]));
  if (k === 0 && e.type === 4) return opts(f.cameras.map((c, n) => c && [n, `Camera ${n}`]).filter(Boolean));
  return null;
}
function fillCutEvPanel() {
  const p = $('cutEvPanel'), e = cutSelEvent != null && cut?.file?.events[cutSelEvent];
  p.hidden = !cutEditing || !e;
  if (p.hidden) return void (cutEditing && cutSelKey != null && fillCutKeyPanel());
  const info = cutEventInfo(e, cut.file);
  if (!cut.seq && cutBakedEvents(cut.file, cutAuth).includes(e)) {
    p.innerHTML = `<b>${esc(info?.[2] ?? 'Event type ' + e.type)}</b><label>From <input disabled value="a camera mode or a shot"></label>`;
    return;
  }
  if (cut.seq) {
    if (e.type === 4) return fillSeqShotPanel(p, e.p[0]);
    p.innerHTML = `<b>${esc(info?.[2] ?? 'Event type ' + e.type)}</b><label>From <input disabled value="the level script / paths"></label>`;
    return;
  }
  p.innerHTML = `<b>${esc(info?.[2] ?? 'Event type ' + e.type)}</b> <label>Time (s) <input type="number" step="0.05" data-k="t" value="${(e.frame / cut.file.fps).toFixed(2)}"></label>`
    + e.p.map((v, k) => {
      // A parameter the event does not read is left out unless it holds something (then it shows by its slot number).
      const name = CUT_PARAM_NAMES[e.type]?.[k];
      if (!name && !v) return '';
      const [label, tip] = name ?? [`p${k}`, 'Not read by this event (as far as is traced)'];
      const pick = cutParamPicker(e, k);
      return `<label title="${esc(tip)}">${esc(label)} ${pick ? `<select data-k="${k}">${pick}</select>` : `<input type="number" data-k="${k}" value="${v}">`}</label>`;
    }).join('') + `<button data-icon="delete" title="Delete this event (Delete key)" id="cutEvDel"></button>`;
  setEditIcon($('cutEvDel'), 'delete');
  $('cutEvDel').onclick = () => document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Delete', bubbles: true }));
  for (const inp of p.querySelectorAll('[data-k]')) inp.onchange = async () => {
    const i = cutSelEvent, k = inp.dataset.k, v = +inp.value;
    if (Number.isNaN(v)) return;
    await editCutFile('Edit event', (f) => {
      if (k === 't') cutSelEvent = cutMoveEvent(f, i, v * f.fps);
      else f.events[i].p[+k] = v >>> 0;
    });
    selectCutEvent(cutSelEvent);
  };
}
// The Audio lane's add menu: every song cue (type 1, the cue table at CUT_SONGS), voice line (type 0) and cutscene sound (type 3, the
// 102-entry table at CUT_SFX), searchable; a pick adds it at the clicked frame with the parameters of the nearest event of its kind.
const CUT_SFX_COUNT = 102;
// Cutscene sound table entry n: its sound's name.
function cutSfxLabel(n) {
  const id = (romBytes[CUT_SFX + n * 2] << 8) | romBytes[CUT_SFX + n * 2 + 1];
  return `${n} ${audioBank?.sfx.find((r) => r.index === id)?.label ?? 'sfx ' + hex(id)}`;
}
// One audio event at a frame, with the other parameters of the nearest event of its kind.
async function cutAddAudioEvent(type, p0, frame) {
  const like = cut.file.events.filter((e) => e.type === type).sort((a, b) => Math.abs(a.frame - frame) - Math.abs(b.frame - frame))[0];
  let at = -1;
  if (await editCutFile('Add ' + TL_KIND[type], (f) => (at = cutAddEvent(f, { type, frame, p: [p0, ...(like ? like.p.slice(1) : [0, 0, 0])] })))) selectCutEvent(at);
}
// The event an Audio list row adds as, [type, p0]: a song cue, a voice line or a cutscene sound; null when cutscenes have no slot for it
// (a sound outside the 102-entry table, a sample).
function cutAudioEventFor(r) {
  const find = (n, ok) => [...Array(n).keys()].find(ok);
  const k = r.kind === 'music' ? find(64, (n) => cutSong({ p: [n] }) === r.index)
    : r.kind === 'sfx' ? find(CUT_SFX_COUNT, (n) => ((romBytes[CUT_SFX + n * 2] << 8) | romBytes[CUT_SFX + n * 2 + 1]) === r.index)
    : r.kind === 'voice' || r.kind === 'group' ? r.voiceIds?.[0] : undefined;
  return k == null ? null : [{ music: 1, sfx: 3, voice: 0, group: 0 }[r.kind], k];
}
let cutAudioTab = 'music';
function cutAudioMenu(x, y, frame) {
  document.getElementById('cutAudioMenu')?.remove();
  const m = elem('div', { id: 'cutAudioMenu', className: 'panel' }), list = elem('div', { className: 'cutAudioList' });
  const search = elem('input', { type: 'search', placeholder: 'Search' });
  const choices = {
    music: [...Array(64).keys()].map((n) => [1, n, audioBank?.music.find((r) => r.index === cutSong({ p: [n] }))?.name ?? `cue ${n}`]),
    voice: Array.from(audioBank?.voiceMap ?? [], (_, v) => [0, v, `${v} ${voiceLine(v) ?? ''}`]).filter(([, , l]) => l.trim().includes(' ')),
    sound: [...Array(CUT_SFX_COUNT).keys()].map((n) => [3, n, cutSfxLabel(n)]),
  };
  const tabs = elem('div', { className: 'cutAudioTabs' }), close = () => m.remove();
  const fill = () => {
    const q = search.value.trim().toLowerCase();
    for (const b of tabs.children) b.classList.toggle('on', b.dataset.tab === cutAudioTab);
    list.replaceChildren(...choices[cutAudioTab].filter(([, , l]) => !q || l.toLowerCase().includes(q)).map(([type, p0, label]) => {
      const b = elem('button', { textContent: label, title: `Add at ${(frame / cut.file.fps).toFixed(2)} s` });
      b.onclick = () => {
        close();
        cutAddAudioEvent(type, p0, frame);
      };
      return b;
    }));
  };
  for (const [tab, label] of [['music', 'Music'], ['voice', 'Voice lines'], ['sound', 'Sound effects']]) {
    const b = elem('button', { textContent: label });
    b.dataset.tab = tab;
    b.onclick = () => {
      cutAudioTab = tab;
      fill();
      search.focus();
    };
    tabs.append(b);
  }
  search.oninput = fill;
  m.append(elem('b', { textContent: `Add at ${(frame / cut.file.fps).toFixed(2)} s` }), tabs, search, list);
  document.body.append(m);
  fill();
  Object.assign(m.style, { left: `${Math.min(x, innerWidth - m.offsetWidth - 8)}px`, top: `${Math.min(y, innerHeight - m.offsetHeight - 8)}px` });
  search.focus();
  // Closes on Escape or a press anywhere outside it.
  const away = (e) => {
    if (e.type === 'keydown' ? e.key === 'Escape' : !m.contains(e.target)) {
      close();
      removeEventListener('pointerdown', away, true);
      removeEventListener('keydown', away, true);
    }
  };
  addEventListener('pointerdown', away, true);
  addEventListener('keydown', away, true);
}
// Click or drag anywhere on the ruler or lanes to scrub; a marker click lands on its frame. It drives the hidden range input the rest uses.
function tlSeek(e) {
  const r = $('cutTimeline').getBoundingClientRect(), x0 = r.left + 64, w = r.width - 64;
  const f = e.target.dataset?.frame;
  const t = f != null && cut ? +f / cut.data.frames : THREE.MathUtils.clamp((e.clientX - x0) / w, 0, 1);
  $('cutTime').value = t;
  $('cutTime').dispatchEvent(new Event('input'));
}
$('cutTimeline').addEventListener('pointerdown', (e) => {
  if (!cut || e.target.classList.contains('tlName')) return;
  $('cutTimeline').setPointerCapture(e.pointerId);
  tlSeek(e);
});
$('cutTimeline').addEventListener('pointermove', (e) => {
  if (cut && e.buttons & 1 && $('cutTimeline').hasPointerCapture(e.pointerId)) tlSeek({ clientX: e.clientX, target: {} });
});
// Hovering a lane lists every event marker within a few pixels of the cursor (they often bunch up) in the tip panel, at once.
$('cutTimeline').addEventListener('pointermove', (e) => {
  const tip = $('tip'), track = e.target.closest?.('.tlTrack');
  const near = track && !track.classList.contains('off') ? [...track.querySelectorAll('.tlm')].filter((m) => {
    const r = m.getBoundingClientRect();
    return Math.abs(r.left + r.width / 2 - e.clientX) <= 4;
  }) : [];
  if (!near.length || e.buttons) {
    tip.hidden = true;
    return;
  }
  tip.innerHTML = near.slice(0, 12).map((m) => `<div><span class="swatch" style="display:inline-block;background:${m.style.background}"></span> ${esc(m.dataset.label)}</div>`).join('') + (near.length > 12 ? `<div>… ${near.length - 12} more</div>` : '');
  const r = canvas.getBoundingClientRect();
  tip.style.left = Math.min(e.clientX - r.left + 14, r.width - 420) + 'px';
  tip.style.top = e.clientY - r.top + 16 + 'px';
  tip.hidden = false;
});
$('cutTimeline').addEventListener('pointerleave', () => ($('tip').hidden = true));
// Sound only ever runs while playing: Play starts it from the current time (from the top at the end), pause suspends it, and scrubbing
// while paused just stops it, so nothing is heard while the viewer shows paused.
$('cutPlay').onclick = async () => {
  if (!cut) return;
  cut.playing = !cut.playing;
  syncCutPlay();
  if (cut.playing && cut.t >= cut.end) cut.t = 0;
  if (cut.playing) startCutAudio(cut.t * cut.data.length);
  if (!audioCtx) return;
  if (cut.playing) await audioCtx.resume();
  else await audioCtx.suspend();
};
$('cutTime').oninput = () => {
  if (!cut) return;
  cut.t = +$('cutTime').value;
  if (cut.playing) startCutAudio(cut.t * cut.data.length);
  else stopCutAudio();
};
$('cutVoice').onchange = $('cutMusic').onchange = $('cutSfx').onchange = syncCutGains;
$('cutFog').onchange = () => {
  $('optFog').checked = $('cutFog').checked;
  syncFogControls();
  applyFog();
};

// Level item placement: every DAT item has its position at +0x10 (game space, Y down); placed objects (type 0) and zones (type 6)
// add an Euler rotation in degrees at +0x1C, then scale (type 0) or box size (type 6) at +0x28. Values are rewritten in place.
const PLACE_FIELDS = [['Position', 0x10, 'pos'], ['Rotation°', 0x1C, 'rot'], ['Scale', 0x28, 'scale'], ['Size', 0x28, 'size']];
// Placed objects (type 0) only: mission scripts find objects by name (getLevelDatItemByName), so adding one or dropping one from the
// list is safe unless something names it. Zones and triggers refer to each other by index, so they are not added or removed here.
const BLOB_START = 0x144340;
function nameUses(bytes, name, skipFrom, skipTo) {
  const pat = new TextEncoder().encode(name);
  let n = 0;
  for (let i = bytes.indexOf(pat[0]); i >= 0 && i + pat.length < bytes.length; i = bytes.indexOf(pat[0], i + 1)) {
    if (i >= skipFrom && i < skipTo) continue;
    let k = 0;
    while (k < pat.length && bytes[i + k] === pat[k]) k++;
    // A whole word: the name is followed by a terminator or list separator (activator lists are "a|b|c").
    if (k === pat.length && (bytes[i + k] === 0 || bytes[i + k] === 0x7C)) n++;
  }
  return n;
}
async function reloadLevel(dir) {
  $('info').hidden = true;
  clearCaches();
  await showLevel(dir, true);
  if (editMode === 'events') fillAddKinds();
  if (editMode === 'objects') fillAddEnemies();
}
async function deleteItem(it) {
  const dir = selected;
  if (it.name && nameUses(romBytes.subarray(0, BLOB_START), it.name, 0, 0)) {
    alert(`"${it.name}" is named by the mission's script, so deleting it could break the mission. Move it away instead.`);
    return;
  }
  const dv = await rom.read(dir + '/dat'), bytes = u8(dv);
  const refs = it.name ? nameUses(bytes, it.name, it.offset, it.offset + dv.getUint32(it.offset + 8)) : 0;
  if (refs && !confirm(`Other items in this level mention "${it.name}" (${refs}x: triggers, paths or areas). Delete it anyway?`)) return;
  try {
    // Zones and triggers are referred to by index: the rest are renumbered.
    await editAsset(dir + '/dat', (b) => (it.type === 0 ? datRemoveItem(b, 0, it.index) : datRemoveIndexed(b, it.type, it.index)));
  } catch (e) {
    alert(`${e.message}, so it cannot be deleted. Move it away instead.`);
    return;
  }
  await reloadLevel(dir);
}
const canDelete = (it) => !it.pathEvent && [0, 6, 7].includes(it.type);

