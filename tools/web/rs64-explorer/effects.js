'use strict';
// ---------- effects ----------
// Destruction bursts from the spawnDebris1Npc callers: n is the count for the tip, count() rolls it; R = (rand & 0x7F) - 64 per use.
// shortcut: radii read from object fields (obj+0x50 * 0.5 etc.) are 0 here, so those chunks start at the centre; only fixed radii are kept.
const R = () => Math.floor(rnd() * 128) - 64, stdScale = () => Math.abs(R() / 320) + 0.2, smallScale = () => Math.abs(R() / 213.3) + 0.3;
const roll = (base, n) => base + Math.floor(rnd() * n);
const BURSTS = {
  'sim:destroy': { label: 'Object destroyed', src: 'updateDestructionDebrisNpc 0x8006D54C at size 1', n: '4 + rand % 3 (times the object size, up to 6x)', count: () => roll(4, 3), cone: 60, speed: [2, 3.5], scale: stdScale },
  'sim:atst': { shock: 'shock:atst', label: 'AT-ST destroyed', src: 'npcAtStHandleDestruction 0x800CFC18', n: '5 + rand % 3', count: () => roll(5, 3), cone: 60, speed: [3, 4], scale: stdScale },
  'sim:atat': { shock: 'shock:atat', label: 'AT-AT destroyed', src: 'npcAtAtHandleDestruction 0x800CD020', n: '40 (15 on levels 3/6/9/13)', count: () => 40, cone: 160, speed: [1.5, 3], scale: smallScale, note: 'A second stage (0x800CD300) throws 12 more; its timing is not traced, so it is left out.' },
  'sim:atpt': { shock: 'shock:atpt', label: 'AT-PT destroyed', src: 'npcAtPtUpdate 0x800EB220', n: '4 + rand % 3', count: () => roll(4, 3), cone: 60, speed: [3, 4], scale: stdScale },
  'sim:shuttle': { label: 'Shuttle / train car destroyed', src: 'npcShuttleHandleDestruction 0x800D367C (npcTrainCarUpdate 0x800E3688 and npcDebrisFieldUpdate 0x800D5858 use 90 degrees)', n: '5 + rand % 5', count: () => roll(5, 5), cone: 135, speed: [3, 4], scale: smallScale },
  'sim:tie': { shock: 'shock:tie', label: 'TIE destroyed', src: 'npcTieVariantHandleDestruction 0x800DAE8C', n: 'a 50% roll, then 3 + rand % 2', count: () => (chance(50) ? roll(3, 2) : 0), cone: 135, speed: [3, 4], scale: smallScale },
  'sim:turret': { label: 'Turret destroyed', src: 'npcTurretUpdate 0x800C99A8 (type != 2)', n: '3 + rand % 4', count: () => roll(3, 4), cone: 60, speed: [3, 4], scale: smallScale },
  'sim:tank': { label: 'Tank droid / ground transport destroyed', src: 'npcTankDroidUpdate 0x800D711C (npcGroundTransportUpdate 0x800DF474 throws 3 + rand % 3)', n: '3 + rand % 2', count: () => roll(3, 2), cone: 90, speed: [3, 4], scale: stdScale },
  'sim:probe': { shock: 'shock:probe', label: 'Probe droid destroyed', src: 'npcProbeDriod 0x800CB254', n: '2 + rand % 3', count: () => roll(2, 3), cone: 90, speed: [3, 4], scale: () => 0.3 },
  'sim:shieldgen': { shock: 'shock:shieldgen', label: 'Shield generator / radar dish destroyed', src: 'npcShieldGeneratorUpdate 0x800817D4, npcRadarDishUpdate 0x800DCCFC', n: '7 + rand % 3', count: () => roll(7, 3), cone: 60, speed: [1.5, 3], scale: stdScale },
  'sim:powergen': { shock: 'shock:powergen', label: 'Power generator destroyed', src: 'spawnDebrisBurstFromTemplate 0x80079000 (npcPowerGeneratorUpdate)', n: '12', count: () => 12, cone: 120, radius: 1.5, speed: [2, 4], scale: () => (Math.abs(R() / 320) + 0.6) * 1.5, note: 'The game fires it twice, at two template points.' },
  'sim:wd': { shock: 'shock:wd', label: 'World Devastator destroyed', src: 'npcWordDevastatorUpdate 0x8007FEC8', n: '40', count: () => 40, cone: 60, radius: 2, speed: [1.5, 2.8], scale: stdScale },
  'sim:watercraft': { shock: 'shock:watercraft', label: 'Water craft destroyed', src: 'npcWaterCraftHandleDestruction 0x800E5DB0 + 0x800E60B8', n: '4 + rand % 3 then 10 + rand % 5', count: () => roll(4, 3) + roll(10, 5), cone: 60, speed: [2, 3], scale: stdScale, note: 'The two calls play at once here.' },
  'sim:crash': { shock: 'shock:crash', label: 'Player craft ground crash', src: 'onPlayerCraftGroundCrash 0x800B0814', n: '10 + rand % 5', count: () => roll(10, 5), cone: 60, speed: [3, 4], scale: stdScale },
};
// Shockwave groups from the func_80072684 callers, all spawned on the same frame; ring = [mesh, pulse, f1, f2, f3, life, rgb, up].
// S is the owning object's size field, which the viewer cannot read: the Size slider sets it (sized entries only).
const SH_BROWN = 0x9c5210;
const SHOCKS = {
  'shock:ground': { label: 'Explosion shock sphere', src: 'triggerDistanceScaledExplosion 0x80074600', rings: () => [[2, 1, 0.05, 0.18, 0.4, 0.5, 0x803030]] },
  'shock:spray': { label: 'Spray emitter rings', src: 'func_8007559C (sprayEffectEmitterNpcHandler, S = 1)', sized: true, rings: (S) => [[3, 0, 0.1 * S, 0.7 * S, 0.7 * S, 0.37 * S, 0xe7faff], [1, 0, 0.15 * S, 1.5 * S, 0.5 * S, 0.7 * S, 0xe7faff]] },
  'shock:blast': { label: 'Large blast rings', src: 'func_80077AF0 (R = obj+0x12C)', sized: true, rings: (R) => [[2, 0, 1.3 * R, 1.9 * R, 0.8, 0.6], [2, 0, 1.1 * R, 2 * R, 0.8, 0.5], [3, 0, 0.9 * R, 2.2 * R, 0.8, 0.45]] },
  'shock:powergen': { label: 'Power generator rings', src: 'spawnDebrisBurstFromTemplate 0x80078E3C', rings: () => [[0, 0, 0.5, 15, 1.5, 0.5, 0xffffff, [0, 1, 0]], [0, 0, 0.5, 14, 1.5, 1], [0, 0, 0.5, 13, 1.5, 1.5], [2, 0, 0.5, 12, 0.75, 0.25]] },
  'shock:wd': { label: 'World Devastator part', src: 'npcWordDevastatorUpdate (each destroyed part)', rings: () => [[2, 0, 2.25, 3.6, 1, 0.6], [1, 0, 2.25, 5.7, 2, 1]] },
  // R = 1 / shldgen mesh size (+0x34) x item size; the 9259-unit shldgen mesh puts it near 0.055, so the spheres are 1.2x / 1.6x the generator.
  'shock:shieldgen': { label: 'Shield generator', src: 'npcShieldGeneratorUpdate (R = obj+0x3C)', sized: true, defSize: 0.055, rings: (R) => [[2, 0, 25 * R, 30 * R, 1, 1], [3, 0, 22 * R, 34 * R, 1, 0.8]] },
  'shock:crash': { label: 'Player craft ground crash', src: 'onPlayerCraftGroundCrash', rings: () => [[2, 1, 0.1875, 1.875, 1, 0.6, SH_BROWN], [1, 1, 0.1875, 1.875, 1, 0.4, SH_BROWN]] },
  'shock:probe': { label: 'Probe droid', src: 'npcProbeDriod', rings: () => [[1, 0, 0.1, 0.5, 0.1, 1]] },
  'shock:atat': { label: 'AT-AT destroyed', src: 'npcAtAtHandleDestruction stage 0 (S = obj+0x54 * 23.71)', sized: true, rings: (S) => [[2, 1, 2.2 * S, 3.5 * S, 1, 1, SH_BROWN], [3, 1, 2 * S, 3.3 * S, 1, 0.9, SH_BROWN], [1, 0, S, 4.9 * S, 1.5, 2, SH_BROWN]] },
  'shock:atst': { label: 'AT-ST destroyed', src: 'npcAtStHandleDestruction (S = 2 * obj+0x54)', sized: true, rings: (S) => [[2, 1, 0.75 * S, 1.2 * S, 1, 0.6, SH_BROWN], [1, 1, 0.75 * S, 0.9 * S, 1, 0.4, SH_BROWN]] },
  'shock:atpt': { label: 'AT-PT destroyed', src: 'npcAtPtUpdate (S = obj+0x54 * 13.333)', sized: true, rings: (S) => [[2, 1, 0.75 * S, 1.2 * S, 1, 0.6, SH_BROWN], [3, 1, 0.6 * S, S, 1, 0.6, SH_BROWN]] },
  // shortcut: the game aims this ring's axis at the player; here it lies flat (axis up).
  'shock:tie': { label: 'TIE destroyed (near the player)', src: 'npcTieVariantHandleDestruction', rings: () => [[1, 0, 0.1, 0.7, 0.15, 1]] },
  'shock:radar': { label: 'Radar dish destroyed', src: 'npcRadarDishUpdate (S = obj+0x50 * 20)', sized: true, rings: (S) => [[2, 1, 0.75 * S, 1.2 * S, 1, 0.4]] },
  'shock:watercraft': { label: 'Water craft destroyed', src: 'npcWaterCraftHandleDestruction (S = obj+0x70 * 40)', sized: true, rings: (S) => [[2, 1, 0.75 * S, 1.2 * S, 1, 0.6, 0xcefefb], [1, 1, 0.5 * S, 0.7 * S, 1, 0.4, 0xcefefb]] },
  'shock:missile': { label: 'Incoming missile (cutscene)', src: 'npcIncomingMissileUpdate', rings: () => [[2, 0, 0.5, 3, 1, 0.35]] },
  'shock:cutscene': { label: 'Cutscene blast rings', src: 'spawnCutsceneEffectsAndCues type 5/2 (S = cmd[0xC] / 500)', sized: true, rings: (S) => [[0, 0, 0.5, S, 1.5, 0.7, 0xffffff, [0, 1, 0]], [0, 0, 0.5, 0.9 * S, 1.5, 1.4], [0, 0, 0.5, 0.81 * S, 1.5, 2.1]] },
};
// Effect flipbooks share the sprite-anim frame format (parseSpriteAnim). Explosion sheets are the ones loadExplosionTextures
// (0x8006108C) loads; the rest are particle, environment and craft effect textures.
const EFFECTS = {
  simulations: ['sim:ground', ...Object.keys(BURSTS), 'sim:land', 'sim:shotdown', 'sim:water', 'sim:water_player', 'sim:impact_fire', 'sim:impact_ion', 'sim:impact_small', 'sim:fireworks'],
  shockwaves: Object.keys(SHOCKS),
  explosions: ['data/explo2_IMGS', 'data/explo2hi_IMGS', 'data/ionexp_IMGS', 'data/ionex2_IMGS', 'data/smkexp_IMGS'],
  // part_spr packs several effects; one entry per frame-name group (srp_s_01 -> srp_s), in the game's D_80130C08 order.
  particles: ['data/part_spr#srp_s', 'data/part_spr#srp_f', 'data/part_spr#sm_f', 'data/part_spr#wsm_f', 'data/part_spr#spark', 'data/part_spr#starex', 'data/part_spr#trail', 'data/part_spr#halo'],
  environment: ['data/gnrc_stuff/wtrspray_ANM'],
  craft: ['data/pl_crafts/engine_TM', 'data/pl_crafts/engine2_TM', 'data/pl_crafts/xwng_glw', 'data/weapon_spr/tie_bomb', 'data/imp_stuff/wrld_vsanim'],
  // Texture sets, not animations: shown laid out, with no playback.
  textures: ['data/gnrc_stuff/tl_clouds', 'data/pl_crafts/shadows_TM'],
};
let selectedEffect = false, effect = null, effectClock = 0;
const TEX_NAMES = ['CI4', 'CI8', 'RGBA16', 'RGB24/RGBA32', 'Gray', 'Gray8'];
// Sprites sample v = 0 at the bottom, while decoded rows run top-down, so their textures get the rows reversed.
function modelTex(f) {
  const t = new THREE.DataTexture(f.rgba, f.w, f.h, THREE.RGBAFormat);
  setFilter(t);
  t.needsUpdate = true;
  return t;
}
function uprightTex(f) {
  const row = f.w * 4, data = new Uint8Array(f.rgba.length);
  for (let y = 0; y < f.h; y++) data.set(f.rgba.subarray(y * row, (y + 1) * row), (f.h - 1 - y) * row);
  const t = new THREE.DataTexture(data, f.w, f.h, THREE.RGBAFormat);
  t.wrapS = t.wrapT = THREE.ClampToEdgeWrapping;
  setFilter(t);
  t.needsUpdate = true;
  return t;
}
async function showEffect(id, keepView = false) {
  setContext(false, true);
  selected = id;
  selectedLevel = false;
  selectedEffect = true;
  renderList();
  $('objRow').hidden = $('lodRow').hidden = true;
  $('fxChanceRow').hidden = $('fxChanceRow2').hidden = !(BURSTS[id] || id === 'sim:land');
  // Simulations run on the game's own timing, so the flipbook frame rate and blend toggle do not apply to them.
  $('fxFpsRow').hidden = $('fxAddRow').hidden = !!SIMS[id];
  const sizedShock = SHOCKS[id]?.sized ? SHOCKS[id] : SHOCKS[BURSTS[id]?.shock]?.sized ? SHOCKS[BURSTS[id].shock] : null;
  $('fxSizeRow').hidden = !sizedShock;
  $('fxCraftRow').hidden = id !== 'sim:shotdown';
  if (id === 'sim:shotdown' && !$('optFxCraft').options.length) {
    // Wingman models first (the craft npcWingmanUpdate flies), then the other player and Imperial craft; c_* models are cockpits.
    const crafts = [...rom.assets.values()].filter((a) => a.name.endsWith('_HOB') && a.size > 8 && /^data\/(pl_crafts|imp_stuff\/tie)/.test(a.path) && !a.name.startsWith('c_')).map((a) => a.path.replace(/^data\/|_HOB$/g, ''));
    const wm = crafts.filter((c) => /\/wm/.test(c)), other = crafts.filter((c) => !/\/wm/.test(c));
    $('optFxCraft').innerHTML = [['Wingmen', wm], ['Other craft', other]].map(([g, list]) => `<optgroup label="${g}">${list.map((c) => `<option value="${esc(c)}">${esc(c.split('/').pop())}</option>`).join('')}</optgroup>`).join('');
    $('optFxCraft').value = crafts.includes('pl_crafts/wmxwng') ? 'pl_crafts/wmxwng' : crafts[0];
  }
  // A newly picked effect starts at its own size estimate; a refresh keeps the slider where the user put it.
  if (sizedShock && !keepView) {
    $('optFxSize').value = sizedShock.defSize ?? 1;
    $('fxSizeVal').textContent = $('optFxSize').value;
  }
  if (SIMS[id]) {
    $('fxModeRow').hidden = $('fxBurstRow').hidden = true;
    return showSim(id, keepView);
  }
  // Smoke is the only explosion sheet that defaults to particle playback; the rest play as flipbooks until the user switches.
  if (!keepView) $('optFxMode').value = EFFECTS.explosions.includes(id) && id !== 'data/smkexp_IMGS' ? 'flipbook' : 'particle';
  const [path, part] = id.split('#');
  const frames = parseSpriteAnim(await rom.read(path)).filter((f) => f.rgba && (!part || f.name.replace(/[_\d]+$/, '') === part));
  // Craft shadows narrower than tall are left halves cut at the right edge (the square Falcon/destr2 are whole), so mirror them out.
  if (path.endsWith('shadows_TM')) frames.forEach((f, i) => {
    if (f.w >= f.h) return;
    const w = f.w * 2, rgba = new Uint8Array(w * f.h * 4);
    for (let y = 0; y < f.h; y++) for (let x = 0; x < w; x++) {
      const s = (y * f.w + (x < f.w ? x : w - 1 - x)) * 4;
      rgba.set(f.rgba.subarray(s, s + 4), (y * w + x) * 4);
    }
    frames[i] = { ...f, w, rgba };
  });
  const add = $('optFxAdd').checked;
  const texOf = (f) => {
    const t = new THREE.DataTexture(f.rgba, f.w, f.h, THREE.RGBAFormat);
    t.wrapS = t.wrapT = THREE.ClampToEdgeWrapping;
    setFilter(t);
    t.needsUpdate = true;
    return t;
  };
  const matOf = (map) => new THREE.MeshBasicMaterial({ map, transparent: true, depthWrite: false, side: THREE.DoubleSide, fog: false, blending: add ? THREE.AdditiveBlending : THREE.NormalBlending });
  // Texture rows run top-down (v = 0 at the top, as on the models), so the plane's v is flipped.
  const plane = (w, h) => {
    const g = new THREE.PlaneGeometry(w, h);
    const uv = g.attributes.uv;
    for (let i = 0; i < uv.count; i++) uv.setY(i, 1 - uv.getY(i));
    return g;
  };
  const group = new THREE.Group();
  const maps = frames.map(texOf);
  // The playing frame, 1 unit per texel, with every frame laid out in a strip below it.
  const maxW = Math.max(...frames.map((f) => f.w)), maxH = Math.max(...frames.map((f) => f.h));
  const main = new THREE.Mesh(plane(frames[0].w, frames[0].h), matOf(maps[0]));
  group.add(main);
  // Texture sets and single frames have nothing to play: shown as stills with no playback controls.
  const still = EFFECTS.textures.includes(id) || frames.length === 1;
  // Removed rather than hidden: framing measures invisible meshes too, which pushed the view up.
  if (still) group.remove(main);
  const cell = Math.max(maxW, maxH) * (still ? 1 : 0.5), perRow = Math.max(1, Math.min(frames.length, still ? 5 : 10));
  frames.forEach((f, i) => {
    const s = cell / Math.max(f.w, f.h);
    const m = new THREE.Mesh(plane(f.w * s, f.h * s), matOf(maps[i]));
    m.position.set(((i % perRow) - (perRow - 1) / 2) * cell * 1.15, -maxH / 2 - cell * (0.9 + Math.floor(i / perRow) * 1.15), 0);
    group.add(m);
  });
  setRoot(group);
  scene.background = fxBg;
  const n = frames.length;
  // Flipbook: one pass through the frames at the chosen frame rate; the main quad resizes when frame sizes differ (part_spr).
  const showFrame = (i) => {
    const f = frames[i];
    main.material.map = maps[i];
    main.material.needsUpdate = true;
    if (main.geometry.parameters.width !== f.w || main.geometry.parameters.height !== f.h) {
      main.geometry.dispose();
      main.geometry = plane(f.w, f.h);
    }
  };
  const fx = {
    reset() { effectClock = 0; showFrame(0); },
    step(dt) { effectClock += dt * +$('optFxFps').value; showFrame(Math.floor(effectClock) % n); },
    done: () => effectClock >= n,
  };
  frames.forEach((f, i) => (f.map = maps[i]));
  const def = !still && PARTICLES[id], particle = def && $('optFxMode').value === 'particle';
  $('fxModeRow').hidden = !def;
  $('fxBurstRow').hidden = !particle;
  if (particle) {
    // The hi-detail fire plays the 20 lo frames with hi frames swapped in for the second half of each step, so it needs both sets.
    const lo = def.hi ? parseSpriteAnim(await rom.read('data/explo2_IMGS')).filter((f) => f.rgba) : frames;
    // Sprite copies with upright textures (the strip planes keep theirs).
    const up = (list) => list.map((f) => ({ ...f, map: uprightTex(f) }));
    const P = { def, frames: up(lo), hiFrames: def.hi ? up(frames) : null, group: new THREE.Group(), live: [] };
    // Smoke takes its RGB from smkexp_PAL by level (the game indexes it with gCurrentLevel); level 0's entry here.
    if (def.kind === 'smoke' && rom.assets.has('data/smkexp_PAL')) {
      const pal = await rom.read('data/smkexp_PAL');
      P.tint = new THREE.Color(pal.getUint8(0) / 255, pal.getUint8(1) / 255, pal.getUint8(2) / 255);
    }
    main.visible = false;
    group.add(P.group);
    fx.particles = P;
    fx.reset = () => {
      for (const p of P.live) { P.group.remove(p.sprite); p.sprite.material.dispose(); }
      P.live = [];
      seedRnd(fxSeed);
      spawnParticles();
    };
    fx.step = tickParticles;
    fx.done = () => !P.live.length;
  }
  stats = { effect: id, frames: frames.length, size: [...new Set(frames.map((f) => f.w + 'x' + f.h))].join(', '), format: [...new Set(frames.map((f) => TEX_NAMES[f.type] ?? 'type ' + f.type))].join(', ') };
  hud();
  if (!keepView) frame(group, new THREE.Vector3(0, 0, 1));
  $('fxPanel').hidden = still;
  if (!still) timelineStart(fx);
}
// Particle handlers from the game code (asm/main/5FA70.s, constants 3BF00.rodata.s); r = (rand & 0xFF) / 255, dt in seconds:
// fire 0x80060690: scale (r + 2.5) * s * 0.25, timer += dt * 20, frame = trunc(timer), gone at 20 (hi set: hi frame when frac > 0.5).
// ion 0x80060900 kind 0: scale0 (|rand7 - 64| / 256 + 0.625) * A, vel B * 0.875 - scale0 per second, frame timer * 20 % 6,
//   full alpha for 0.5 s then -512/s. kind 1: frame (age * 16) % 8, alpha 255 - 510 * age, scale += vel * dt * 2, gone at 0.5 s.
// smoke 0x80060D8C: scale (r + 2.5) * s * 0.25, * 1.1 per 30 Hz tick, alpha -512/s, rises 1 unit/s, RGB from smkexp_PAL[level].
const PARTICLES = {
  'data/explo2_IMGS': { kind: 'fire', s: 1 }, 'data/explo2hi_IMGS': { kind: 'fire', s: 1, hi: true },
  'data/ionexp_IMGS': { kind: 'ion0', A: 0.4, B: 0.85 }, 'data/ionex2_IMGS': { kind: 'ion1', A: 0.4, B: 0.8 },
  'data/smkexp_IMGS': { kind: 'smoke', s: 0.5 },
};
function spawnParticles() {
  const P = effect.particles, frames = P.frames, spread = Math.max(frames[0].w, frames[0].h) * 0.6;
  for (let k = 0; k < +$('optFxBurst').value; k++) {
    const r = rnd(), def = P.def;
    const p = { age: 0, timer: 0, alpha: 255, tick: 0 };
    if (def.kind === 'fire' || def.kind === 'smoke') p.scale = (r + 2.5) * def.s * 0.25;
    else {
      p.scale = (Math.abs(Math.floor(rnd() * 128) - 64) / 256 + 0.625) * def.A;
      p.vel = def.B * 0.875 - p.scale;
    }
    const mat = new THREE.SpriteMaterial({ map: frames[0].map, transparent: true, depthWrite: false, fog: false,
      blending: $('optFxAdd').checked ? THREE.AdditiveBlending : THREE.NormalBlending, color: P.tint ?? 0xffffff });
    p.sprite = new THREE.Sprite(mat);
    p.sprite.scale.setScalar(0);
    // Spread is a viewer choice (one particle at the centre, the rest around it).
    if (k) p.sprite.position.set((rnd() - 0.5) * spread, (rnd() - 0.5) * spread, (rnd() - 0.5) * spread);
    P.group.add(p.sprite);
    P.live.push(p);
  }
}
// One particle step per its game handler; returns the frame texture to show, or null once the particle is gone.
function stepParticle(p, def, dt, frames, hiFrames) {
  p.age += dt;
  if (def.kind === 'fire') {
    p.timer += dt * 20;
    const idx = Math.floor(p.timer);
    if (idx >= 20) return null;
    return def.hi && hiFrames && idx < 19 && p.timer - idx > 0.5 ? hiFrames[idx].map : frames[Math.min(idx, frames.length - 1)].map;
  }
  if (def.kind === 'ion0') {
    p.timer += dt * 20;
    p.scale += p.vel * dt;
    if (p.age >= 0.5) p.alpha -= dt * 512;
    return p.alpha > 0 ? frames[Math.floor(p.timer) % frames.length].map : null;
  }
  if (def.kind === 'ion1') {
    if (p.age > 0.5) return null;
    p.alpha = 255 - 510 * p.age;
    p.scale += p.vel * dt * 2;
    return frames[Math.floor(p.age * 16) % frames.length].map;
  }
  // Smoke grows per 30 Hz game tick, not per second, and rises 1 unit/s.
  for (p.tick += dt; p.tick >= 1 / 30; p.tick -= 1 / 30) p.scale *= 1.1;
  p.alpha -= dt * 512;
  p.sprite.position.y += dt;
  return p.alpha > 0 ? frames[0].map : null;
}
function tickParticles(dt) {
  const P = effect.particles;
  for (const p of P.live) {
    const map = stepParticle(p, P.def, dt, P.frames, P.hiFrames);
    if (!map) { p.dead = true; continue; }
    p.sprite.material.map = map;
    p.sprite.material.opacity = p.alpha / 255;
    // shortcut: quad size = scale x frame width in texels; how the game maps scale to world size is not traced.
    p.sprite.scale.setScalar(p.scale * P.frames[0].w);
  }
  for (const p of P.live) if (p.dead) { P.group.remove(p.sprite); p.sprite.material.dispose(); }
  P.live = P.live.filter((p) => !p.dead);
}
// ---------- effect simulations ----------
// Whole effects rebuilt from the game's spawners (asm/main/738F0.s), in world units on a ground plane (three.js Y up; the game's
// "up" is -Y). Effect textures come from part_spr in the order of the game's table D_80130C08: 6..11 = srp_f comet fireballs.
const PART_LABELS = { 'data/part_spr#srp_s': 'Smoke trail puffs', 'data/part_spr#srp_f': 'Comet fireballs (debris)', 'data/part_spr#sm_f': 'Smoke (plume)', 'data/part_spr#wsm_f': 'Water mist (splash)', 'data/part_spr#spark': 'Spark (embers)', 'data/part_spr#starex': 'Star flare', 'data/part_spr#trail': 'Trail streak', 'data/part_spr#halo': 'Halo glow' };
const SIMS = {
  ...Object.fromEntries(Object.entries(SHOCKS).map(([k, s]) => [k, { label: s.label, tip: `${s.src}: ${s.rings(1).length} shockwave(s) via func_80072684, all spawned together; the ring meshes grow, spin 640 deg/s and fade over their own lifetimes.${s.sized ? ' Sizes scale with the object size field (Size slider).' : ''}` }])),
  'sim:ground': { label: 'Ground explosion', tip: 'triggerDistanceScaledExplosion 0x80074600 at 2.5 units from the camera: 7 debris fireballs (50 degree cone around up, 0.8..3.5 units/s, gravity 5) and a shock sphere (0.05 -> 0.18 over 0.5 s) and an ember burst (0x80073E4C: 20 sparks in all directions at 0.4 units/s, shrinking to nothing over 1 s). Each fireball that lands kicks up a brown dust splash (the wtrspray mesh, func_80072EC4 type 3, tinted 0x7C2206: 0.2 rising to 0.4 tall and back over 0.5 s, widening 0.05 -> 0.2).' },
  ...Object.fromEntries(Object.entries(BURSTS).map(([k, b]) => [k, { label: b.label, tip: `${b.src}: ${b.n} debris1 chunks (spawnDebris1Npc 0x80072384) thrown within ${b.cone} degrees of up at ${b.speed[0]}..${b.speed[1]} units/s${b.radius ? `, starting ${b.radius} units out along their direction` : ''}. Each chunk runs the debris landing effect when it hits the ground.${b.shock ? ` It also spawns its shockwaves (${SHOCKS[b.shock].label} in Shockwaves).` : ''}${b.note ? ' ' + b.note : ''}` }])),
  'sim:shotdown': { label: 'Craft shot down', tip: 'npcWingmanUpdate shot-down state (+0x5C = 2) -> func_800D8988: three emitters ride the craft (smoke sm_f, tiny srp_f fireballs, srp_s smoke-trail puffs) for 4 s. The crash (a ~60 degree dive, then a short slide to a stop) is a viewer choice; pick the craft above the background colour.' },
  'sim:land': { label: 'Debris landing', tip: 'triggerObjectDestructionEffects 0x8007483C, called by a landing debris1 chunk (npcDebris1Update): a 0.5 smoke puff, then rand % 100 rolls another chunk (under 2) or a 2-puff smoke plume (0x80073750, 2..9). The Chunk / Plume chances set those odds (game: 2% and 8%, only when the player made the kill).' },
  'sim:impact_fire': { label: 'Laser impact', tip: 'Projectile impact 0x80076250, weapon types 10/11/13/14: one fire particle at scale 0.6.' },
  'sim:impact_ion': { label: 'Ion impact', tip: 'Projectile impact 0x80076250, weapon type 12: one ion particle (ionex2, A 0.4, B 0.8).' },
  'sim:impact_small': { label: 'Small impact', tip: 'Projectile impact 0x80076250, other weapon types: an ember burst (0x80073E4C, 0.05, 0.2) and one fire particle at 0.2.' },
  'sim:water': { label: 'Water hit', tip: '0x8007483C over water: a 20-puff spray (0x800739A4: +-15 degrees, 0.38 units/s, 0.5 s) water-tinted smoke and the wtrspray splash model (func_80072EC4 type 1: 0.3 tall, widening 0.05 -> 0.4 and fading over 0.3 s, its texture cycling wtrspray_ANM at 12 fps). This is a hit by anyone other than the player.' },
  'sim:fireworks': { label: 'Fireworks', tip: 'spawnCutsceneDebrisField 0x800AAC88 (medal ceremony event type 15, case 8): an i_missile rocket every 0.3 s for 6 s from a 3.2 x 0.6 x 3.2 box, each rising 1.5 units in 0.9 s and bursting into 30 starex sparks (spawnNpcBurstAlongSegment case 6) coloured by one of three ramps.' },
  'sim:water_player': { label: 'Water hit (player)', tip: 'The water hit when the player made it: 0x8007483C then also calls func_80074600(pos, 0.1, 0.4, 1.0), the full ground explosion (fireballs that splash where they land, the shock sphere, embers).' },
};
// Effect kit: the particle, shockwave, debris and emitter helpers, spawning into `group` (Y up) with items stepped by the caller.
// ground(x, z) is the floor height debris lands on (the Effects tab's grid is 0; cutscenes pass the terrain).
async function fxKit(group, ground = () => 0) {
  // Parsed once per sheet (cleared with the other caches): re-decoding every sheet made each sim slow to open.
  // Effect sprites are always bilinear, as the RDP draws them: these sheets are small (fire 40x42) and drawn large, so the Filtering option's
  // Nearest default would turn them into blocks.
  const smooth = (t) => Object.assign(t, { magFilter: THREE.LinearFilter, minFilter: THREE.LinearFilter, generateMipmaps: false, needsUpdate: true });
  const art = (path) => {
    if (!artCache.has(path)) artCache.set(path, rom.read(path).then((dv) => parseSpriteAnim(dv).filter((f) => f.rgba).map((f) => ({ ...f, map: smooth(uprightTex(f)) }))));
    return artCache.get(path);
  };
  // fireHi: the Expansion Pak's in-between fire frames (explo2hi, explo004, explo012, ...), shown for the second half of each lo frame
  // (0x80130B63 set); the recomp runs with the pak.
  const [spr, fire, ion2, smoke, sfx, wtr, wtrAnm, ionx, fireHi] = await Promise.all([art('data/part_spr'), art('data/explo2_IMGS'), art('data/ionex2_IMGS'), art('data/smkexp_IMGS'), loadModel('data/gnrc_stuff/sfx'),
    loadModel('data/gnrc_stuff/wtrspray'), art('data/gnrc_stuff/wtrspray_ANM'), art('data/ionexp_IMGS'), art('data/explo2hi_IMGS')]);
  const pal = await rom.read('data/smkexp_PAL');
  const smokeTint = new THREE.Color(pal.getUint8(0) / 255, pal.getUint8(1) / 255, pal.getUint8(2) / 255);
  // Water smoke (D_801398C0) takes its RGB from the palette's second plane at +0x80.
  const waterTint = new THREE.Color(pal.getUint8(0x80) / 255, pal.getUint8(0x81) / 255, pal.getUint8(0x82) / 255);
  const items = [];
  const blend = () => ($('optFxAdd').checked ? THREE.AdditiveBlending : THREE.NormalBlending);
  // A sprite is drawn at one depth (its centre; the F5 0xBD sprite's primDepth), so one whose centre is under the terrain is hidden whole;
  // a camera-facing quad would otherwise poke its upper half out of the ground.
  const buried = (o) => o.position.y < ground(o.position.x, o.position.z);
  const sprite = (map, tint) => {
    const s = new THREE.Sprite(new THREE.SpriteMaterial({ map, transparent: true, depthWrite: false, fog: false, blending: blend(), color: tint ?? 0xffffff }));
    // Zero until its first step sizes it; three's default 1x1 unit quad would flash for a frame when spawned mid-tick.
    s.scale.setScalar(0);
    group.add(s);
    return s;
  };
  // Billboard particles reuse the handlers; shortcut: a particle's scale is drawn as its quad size in world units (not traced).
  const particle = (def, pos, frames, tint) => {
    const p = { age: 0, timer: 0, alpha: 255, tick: 0 }, hi = frames === fire && fireHi.length ? fireHi : null, sdef = hi ? { ...def, hi: true } : def;
    if (def.kind === 'ion1') {
      p.scale = (Math.abs(Math.floor(rnd() * 128) - 64) / 256 + 0.625) * def.A;
      p.vel = def.B * 0.875 - p.scale;
    } else p.scale = (rnd() + 2.5) * def.s * 0.25;
    p.sprite = sprite(frames[0].map, tint);
    p.sprite.position.copy(pos);
    items.push({ obj: p.sprite, step: (dt) => {
      const map = stepParticle(p, sdef, dt, frames, hi);
      if (!map) return false;
      p.sprite.material.map = map;
      p.sprite.material.opacity = p.alpha / 255;
      p.sprite.scale.setScalar(p.scale);
      p.sprite.visible = !buried(p.sprite);
      return true;
    } });
  };
  // Debris fireball 0x8007413C: launched within 50 degrees of up at 0.8..3.5 units/s, gravity 5, frame += 10/s while falling
  // (max 5), rolled to its travel direction + 45 degrees, removed on hitting the ground.
  const debris = (pos, size) => {
    const tilt = THREE.MathUtils.degToRad(rnd() * 50), az = rnd() * Math.PI * 2, speed = 0.8 + rnd() * 2.7;
    const vel = new THREE.Vector3(Math.sin(tilt) * Math.cos(az), Math.cos(tilt), Math.sin(tilt) * Math.sin(az)).multiplyScalar(speed);
    const s = sprite(spr[6].map);
    s.position.copy(pos);
    s.scale.setScalar(size);
    let frame = 0;
    items.push({ obj: s, step: (dt) => {
      s.position.addScaledVector(vel, dt);
      vel.y -= 5 * dt;
      if (vel.y < 0) frame = Math.min(5, frame + 10 * dt);
      s.material.map = spr[6 + Math.floor(frame)].map;
      s.material.rotation = Math.atan2(-vel.x, vel.y) + Math.PI / 4;
      const floor = ground(s.position.x, s.position.z);
      if (s.position.y > floor) return true;
      // Landing: a type-3 splash 0.2 above the ground, tinted by the fireball's +0x60 colour: D_8003D0CC 0x7C2206FF, a brown dust kick-up.
      splash(new THREE.Vector3(s.position.x, floor + 0.2, s.position.z), { sx: 0.05, sy: 0.2, grow: 0.15, rate: 2, amp: 0.4, color: 0x7c2206 });
      return false;
    } });
  };
  // Shock sphere 0x8006FC90 (sfx shock_sph): 0.5 s, scale 0.05 -> 0.18, alpha 1 -> 0.
  // Splash func_80072EC4: the wtrspray mesh scaled (sx, sy, sx) with no yaw, primitive colour (white unless given) with alpha 1 - age, age += rate/s
  // (gone at 1); x/z grow by grow * rate per second. With amp, y follows amp * sin(phase), phase sweeping asin(sy / amp) -> pi over
  // the life (min 0.01). Its waterwhite_1 texture cycles the wtrspray_ANM frames at 12 fps (func_80072CF0).
  const splash = (pos, { sx, sy, grow, rate, amp, color = 0xffffff }) => {
    const obj = wtr.hob.objects.find((o) => o.name === 'wtrspray') ?? wtr.hob.objects[0];
    const g = buildObject(obj, wtr, 'data/gnrc_stuff/wtrspray', { tris: 0, verts: 0, parts: 0 });
    g.rotation.x = Math.PI;
    const tex = new THREE.DataTexture(wtrAnm[0].rgba, wtrAnm[0].w, wtrAnm[0].h, THREE.RGBAFormat);
    tex.wrapS = tex.wrapT = THREE.RepeatWrapping;
    setFilter(tex);
    tex.needsUpdate = true;
    // shortcut: plain alpha blend; the handler sets only the primitive colour, and its render mode (func_8005955C 0x09010000) is not decoded.
    const mat = new THREE.MeshBasicMaterial({ map: tex, color, transparent: true, depthWrite: false, side: THREE.DoubleSide, fog: false });
    g.traverse((o) => o.isMesh && (o.material = mat));
    const holder = new THREE.Group();
    holder.add(g);
    holder.position.copy(pos);
    holder.scale.set(sx, sy, sx).multiplyScalar(HOB_TO_WORLD);
    group.add(holder);
    const phase0 = amp ? Math.asin(Math.min(1, sy / amp)) : 0;
    let age = 0, t = 0, frame = 0;
    items.push({ obj: holder, step: (dt) => {
      age += rate * dt;
      t += dt;
      if (age >= 1) { tex.dispose(); return false; }
      sx += grow * rate * dt;
      if (amp) sy = Math.max(0.01, amp * Math.sin(phase0 + (Math.PI - phase0) * age));
      holder.scale.set(sx, sy, sx).multiplyScalar(HOB_TO_WORLD);
      mat.opacity = 1 - age;
      const f = Math.floor(t * 12) % wtrAnm.length;
      if (f !== frame) { frame = f; tex.image.data = wtrAnm[f].rgba; tex.needsUpdate = true; }
      return true;
    } });
  };
  // Shockwave func_80072684 -> handler func_8006FC90; ring = [mesh, pulse, f1, f2, f3, life, rgb, up] (see SHOCKS). Mesh 0 shock_sml,
  // 1 shock_big, 2/3 shock_sph. t runs 1 -> 0 over life; p = 1 - t. Scale (s, f3 * s, s): plain s = f1 + (f2 - f1) sin(p pi/2); pulse
  // (flag 0x8000) s = f1 + (f2 - f1) sin(p pi) to p = 0.5, then f2 sin(p pi). Alpha t, spin 640 deg/s about the up axis. shock_sph's
  // explo_tex material steps explo2 frames trunc(p * 20) (mesh 2) or ionexp frames trunc(p * 6) (mesh 3); sml/big keep their textures.
  const SHOCK_MESH = ['shock_sml', 'shock_big', 'shock_sph', 'shock_sph'];
  const shockwave = (pos, [mi, pulse, f1, f2, f3, life, rgb = 0xffffff, up]) => {
    const obj = sfx.hob.objects.find((o) => o.name === SHOCK_MESH[mi]);
    const g = buildObject(obj, sfx, 'data/gnrc_stuff/sfx', { tris: 0, verts: 0, parts: 0 });
    g.rotation.x = Math.PI;
    // Model UVs read rows top-down, so the sphere gets unflipped copies of the frames (the sprite maps are flipped for billboards).
    const anim = (mi === 2 ? fire : mi === 3 ? ionx : null)?.map((f) => (f.modelMap ??= smooth(modelTex(f))));
    const mats = [];
    g.traverse((o) => {
      if (!o.isMesh) return;
      const own = (Array.isArray(o.material) ? o.material : [o.material]).map((m) => m.map ?? null);
      // The game's translucent pass (render mode G_RM_ZB_XLU_SURF, combine TEXEL0 * SHADE, alpha TEXEL0_A * PRIM_A): a normal alpha blend with no
      // depth write, the white shckwv texture times the face colour, and the face alpha (shock_sml 0x80) times life (applyRgbaTintToMeshVertices).
      const faceA = o.geometry.attributes.color?.itemSize === 4 ? o.geometry.attributes.color.getW(0) : 1;
      o.material = own.map((map) => {
        // shortcut: the shock_sph fireball (explosion frames swapped in) stays additive; drawn with that blend its frames' dark fringe turns it into a
        // dark dome. The frames' alpha path through the same combine is not traced.
        const m = anim
          ? new THREE.MeshBasicMaterial({ map: anim[0], color: rgb, transparent: true, depthWrite: false, side: THREE.DoubleSide, blending: THREE.AdditiveBlending, fog: false })
          : new THREE.MeshBasicMaterial({ map, color: rgb, vertexColors: true, transparent: true, depthWrite: false, side: THREE.DoubleSide, fog: false });
        m.userData.faceA = anim ? 1 : faceA;
        mats.push(m);
        return m;
      });
    });
    const spin = new THREE.Group(), holder = new THREE.Group();
    spin.add(g);
    holder.add(spin);
    holder.position.copy(pos);
    if (up) holder.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0), new THREE.Vector3(...up).normalize());
    holder.scale.set(f1, f3 * f1, f1).multiplyScalar(HOB_TO_WORLD);
    group.add(holder);
    let t = 1;
    items.push({ obj: holder, step: (dt) => {
      t -= dt / life;
      if (t <= 0) { mats.forEach((m) => m.dispose()); return false; }
      const p = 1 - t;
      const s = !pulse ? f1 + (f2 - f1) * Math.sin(p * Math.PI / 2) : p < 0.5 ? f1 + (f2 - f1) * Math.sin(p * Math.PI) : f2 * Math.sin(p * Math.PI);
      holder.scale.set(s, f3 * s, s).multiplyScalar(HOB_TO_WORLD);
      spin.rotation.y += THREE.MathUtils.degToRad(640) * dt;
      const map = anim && anim[Math.min(anim.length - 1, Math.trunc(p * (mi === 2 ? 20 : 6)))];
      for (const m of mats) {
        m.opacity = t * m.userData.faceA;
        if (map && m.map !== map) { m.map = map; m.needsUpdate = true; }
      }
      return true;
    } });
  };
  // debris1 model chunk 0x8006F6A0: velocity {vec.x, up 1, vec.z}, spin |v| * 360 deg/s, gravity 4, bounces, 2 s life.
  const chunk = (pos, vel = new THREE.Vector3((rnd() - 0.5) * 0.6, 1, (rnd() - 0.5) * 0.6), lands = true, scale = 0.1) => {
    const obj = sfx.hob.objects.find((o) => o.name === 'debris1');
    const g = buildObject(obj, sfx, 'data/gnrc_stuff/sfx', { tris: 0, verts: 0, parts: 0 });
    const holder = new THREE.Group();
    holder.add(g);
    // shortcut: the record's scale is drawn relative to the landing chunk's 0.1 at a viewer-chosen 0.025; the real mesh scale is not traced.
    holder.scale.setScalar(0.25 * scale * HOB_TO_WORLD);
    holder.position.copy(pos);
    group.add(holder);
    // The default velocity is the landing chunk's {vec.x, 1, vec.z} with a small random drift standing in for the object's vec.
    const spin = vel.length() * 2 * Math.PI;
    let t = 0, landed = !lands;
    items.push({ obj: holder, step: (dt) => {
      t += dt;
      holder.position.addScaledVector(vel, dt);
      vel.y -= 4 * dt;
      const floor = ground(holder.position.x, holder.position.z);
      if (holder.position.y < floor) {
        holder.position.y = floor;
        vel.y = -vel.y * 0.5;
        // shortcut: the landing effect fires on the first ground contact; npcDebris1Update's exact trigger condition is not traced.
        if (!landed) { landed = true; landing(holder.position.clone().setY(floor + 0.3)); }
      }
      holder.rotation.x += spin * dt;
      holder.rotation.z += spin * 0.7 * dt;
      return t < 2;
    } });
  };
  // Emitter 0x800403EC (spawn func_800409A4, tick func_8004118C in asm/main/40F10.s): `rate` particles/s up to `max`, each launched
  // at `vel` up, turned by +-spread/2 degrees about Z and X, scattered by +-scatter, moving at v * (1 - drag) with no gravity, size
  // start -> end over `life`, frames stepped evenly over life, colour/alpha from a 16-step part_pal ramp (alpha in the 0x200 plane).
  const pp = await rom.read('data/part_pal');
  const rampColor = (ramp, step) => {
    const e = ramp * 16 + step;
    return [pp.getUint8(e * 4) / 255, pp.getUint8(e * 4 + 1) / 255, pp.getUint8(e * 4 + 2) / 255, pp.getUint8(0x200 + e * 4) / 255];
  };
  const emitter = (pos, e) => {
    // e.at(t) moves the source over the emitter's time (attached emitters); e.dir is a launch vector instead of e.vel straight up;
    // e.art is the frame sheet (part_spr by default); without e.ramp the sprites stay untinted.
    let acc = 0, left = e.max, time = 0;
    const spawnOne = () => {
      const rot = new THREE.Euler(THREE.MathUtils.degToRad(Math.floor(rnd() * e.spread) - e.spread / 2), 0, THREE.MathUtils.degToRad(Math.floor(rnd() * e.spread) - e.spread / 2));
      const vel = (e.dir ? new THREE.Vector3(...e.dir) : new THREE.Vector3(0, e.vel, 0)).applyEuler(rot).multiplyScalar(1 - e.drag);
      const s = sprite((e.art ?? spr)[e.frames[0]].map);
      s.position.copy(e.at ? e.at(time) : pos).add(new THREE.Vector3((rnd() * 2 - 1) * e.scatter, 0, (rnd() * 2 - 1) * e.scatter));
      let age = 0;
      items.push({ obj: s, step: (dt) => {
        age += dt;
        if (age >= e.life) return false;
        const f = age / e.life;
        s.position.addScaledVector(vel, dt);
        s.material.map = (e.art ?? spr)[e.frames[Math.min(e.frames.length - 1, Math.floor(f * e.frames.length))]].map;
        const [r, g, b, a] = e.ramp == null ? [1, 1, 1, 1] : rampColor(e.ramp, Math.floor(f * 16));
        s.material.color.setRGB(r, g, b);
        s.material.opacity = a;
        // shortcut: size is drawn as the quad's full width in world units; half-extent vs width is not traced.
        s.scale.setScalar(Math.max(1e-4, e.size0 + (e.size1 - e.size0) * f));
        s.visible = !buried(s);
        return true;
      } });
    };
    // The emitter itself is an item until its count is spent, spawning floor(accumulated rate) particles per tick.
    items.push({ obj: new THREE.Object3D(), step: (dt) => {
      time += dt;
      for (acc += dt * e.rate; acc >= 1 && left > 0; acc--, left--) spawnOne();
      return left > 0;
    } });
  };
  // 0x8007483C on land: smoke, then r = rand % 100 picks another chunk (r < 2) or the plume (r < 10). Rolled separately here so each
  // slider works at any value; the extra chunk does not land-trigger again, or 100% would chain forever.
  const landing = (pos) => {
    particle({ kind: 'smoke', s: 0.5 }, pos, smoke, smokeTint);
    if (chance(+$('optChunkPct').value)) chunk(pos, undefined, false);
    if (chance(+$('optPlumePct').value)) plume(pos);
  };
  // 0x80073750 (pos, id, 1.2, 0.25, [0.8, 0.5]): 2 smoke puffs at 4.8/s rising 0.8 * 0.2 units/s, 1.5 s, sm_f01 -> sm_f02, ramp 0.
  const plume = (pos) => emitter(pos, { rate: 4.8, max: 2, vel: 0.8, spread: 0, scatter: 0.03, drag: 0.8, life: 1.5, size0: 33 / 256, size1: 97 / 256, frames: [12, 13], ramp: 0 });
  // Camera distance the size/count formulas use (dist 2.5 = the ground explosion's full 7-chunk range).
  const d = 2.5;
  // 0x80073E4C ember burst (pos, s, vy): n = 20 within 6 units, all directions at vy, 1 s, size 1.5*sz -> ~0, spark, ramp 4.
  const embers = (pos, s, vy) => {
    const sz = (s * d) / 4.5;
    emitter(pos, { rate: 120, max: d <= 6 ? 20 : Math.max(1, Math.trunc(120 / d)), vel: vy, spread: 360, scatter: 0.025, drag: 0, life: 1, size0: 1.5 * sz + 1 / 256, size1: 0.0005 * sz, frames: [15], ramp: 4 });
  };
  // triggerDistanceScaledExplosion 0x80074600(pos, 0.1, 0.4, 1.0): dist 2.5 -> ratio 1 -> 7 chunks of size 0.1 +- 0.05, shock sphere, embers.
  const explosion = (pos) => {
    for (let i = 0; i < 7; i++) debris(pos, Math.max(0.001, 0.1 + (Math.floor(rnd() * 128) - 64) / 1280));
    shockwave(pos, SHOCKS['shock:ground'].rings(1)[0]);
    embers(pos, 0.1, 0.4);
  };
  return { items, blend, sprite, particle, debris, splash, SHOCK_MESH, shockwave, chunk, pp, rampColor, emitter, landing, plume, d, embers, explosion, fireHi,
    spr, fire, ion2, smoke, sfx, wtr, wtrAnm, ionx, smokeTint, waterTint };
}
async function showSim(id, keepView) {
  const craftId = id === 'sim:shotdown' && $('optFxCraft').value, craft = craftId && (await loadModel('data/' + craftId));
  const group = new THREE.Group();
  group.add(new THREE.GridHelper(10, 20, 0x444a55, 0x2a2f3a));
  // Depth-only ground just under the grid: hides what sinks below the surface (the lower half of shock spheres), as terrain and water do in game.
  const ground = new THREE.Mesh(new THREE.PlaneGeometry(200, 200).rotateX(-Math.PI / 2), new THREE.MeshBasicMaterial({ colorWrite: false }));
  ground.position.y = -0.002;
  ground.renderOrder = -1;
  group.add(ground);
  setRoot(group);
  scene.background = fxBg;
  if (id === 'sim:fireworks') {
    // The cutscene launcher, centred so the rockets start on the grid (game space is Y down, so the bursts sit 1.5 above it).
    const space = new THREE.Group(), em = FIREWORK_EMITTERS[8];
    space.rotation.x = Math.PI;
    group.add(space);
    const fw = await buildFireworks([{ ...em, at: 0, c: [0, -1.5, 0] }], space, await loadModel('data/weapons'), { tris: 0, verts: 0, parts: 0 });
    let t = 0;
    stats = { simulation: SIMS[id].label, source: SIMS[id].tip.split(':')[0] };
    hud();
    if (!keepView) frameBox(new THREE.Box3(new THREE.Vector3(-2, 0, -2), new THREE.Vector3(2, 2.5, 2)), new THREE.Vector3(0.6, 0.3, 0.8));
    timelineStart({ sim: true, reset() { t = 0; poseFireworks(fw, 0); }, step(dt) { t += dt; poseFireworks(fw, t); }, done: () => t > em.life + 4.4 });
    return;
  }
  const kit = await fxKit(group);
  const { items, sprite, particle, debris, splash, SHOCK_MESH, shockwave, chunk, emitter, landing, plume, d, embers, explosion, spr, fire, ion2, smoke, sfx, wtr, wtrAnm, ionx, smokeTint, waterTint } = kit;
  const at = new THREE.Vector3(0, 0, 0), above = new THREE.Vector3(0, 0.3, 0);
  // npcWingmanUpdate's shot-down state calls func_800D8988: three emitters on the craft node at (0, 0.05, -0.2) (game space), all 20/s,
  // +-2.5 deg, scatter 0.1, part_pal ramp 0: sm_f smoke (80, 1.2 s, 0.33 -> 1, vel (-1, -0.4, -1), drag 0.8), srp_f fireballs (same, size
  // 4/256) and srp_s puffs (20, 0.5 s, 8/256 -> 21/256, vel (0, 0.5, 0), no drag). Game vectors map to the viewer as (x, -y, -z).
  // shortcut: the fall path is a viewer choice (see shotDown). the state's real flight is not traced.
  const shotDown = () => {
    const obj = craft.hob.objects[0];
    const g = buildObject(obj, craft, 'data/' + craftId, { tris: 0, verts: 0, parts: 0 });
    g.rotation.x = Math.PI;
    const holder = new THREE.Group();
    holder.add(g);
    // Placed like a level NPC: flying craft records (TIE patrols, bombers, attack splines) all use item scale 0.014, over the model's +0x34 divisor.
    holder.scale.setScalar((0.014 / (obj.scaleDiv || 1)) * HOB_TO_WORLD);
    // Dive, hit the ground at tHit, then slide forward decelerating (s0 at decel a) with the nose easing level over 0.2 s, and rest.
    // shortcut: the whole crash path is a viewer choice; posed from t alone so seeking and the attached emitters stay in step.
    const v = new THREE.Vector3(0, -1.6, -0.9), start = new THREE.Vector3(0, 4.2, 2.5), s0 = 1.6, a = 1.4;
    const dive = Math.atan2(v.y, -v.z), slideDir = new THREE.Vector3(0, 0, -1);
    // Ground contact from the craft's own vertices: clearance(pitch) = how far its lowest point hangs below the origin at that pitch,
    // sampled from the dive angle to level, so the nose (or a wingtip) touches down and the wreck rests on its belly.
    const clear = [];
    for (let k = 0; k <= 16; k++) {
      holder.rotation.x = dive * (1 - k / 16);
      holder.updateMatrixWorld(true);
      clear.push(-new THREE.Box3().setFromObject(holder, true).min.y);
    }
    const clearance = (pitch) => {
      const f = Math.min(16, Math.max(0, (1 - pitch / dive) * 16)), k = Math.min(15, Math.floor(f));
      return clear[k] + (clear[k + 1] - clear[k]) * (f - k);
    };
    const tHit = (start.y - clear[0]) / -v.y;
    const pitchAt = (t) => (t <= tHit ? dive : dive * Math.max(0, 1 - (t - tHit) / 0.2));
    const posAt = (t) => {
      if (t <= tHit) return start.clone().addScaledVector(v, t);
      const u = Math.min(t - tHit, s0 / a);
      return start.clone().addScaledVector(v, tHit).setY(clearance(pitchAt(t))).addScaledVector(slideDir, s0 * u - (a * u * u) / 2);
    };
    const at = (t) => posAt(t).add(new THREE.Vector3(0, -0.05, 0.2).applyEuler(new THREE.Euler(pitchAt(t), 0, 0)));
    holder.position.copy(start);
    holder.rotation.x = dive;
    group.add(holder);
    let t = 0;
    items.push({ obj: holder, step: (dt) => {
      t += dt;
      holder.position.copy(posAt(t));
      holder.rotation.x = pitchAt(t);
      // Lingers while the last smoke fades.
      return t < 5.3;
    } });
    const base = { at, spread: 5, scatter: 0.1, rate: 20, ramp: 0 };
    emitter(null, { ...base, dir: [-1, 0.4, 1], drag: 0.8, max: 80, life: 1.2, size0: 0x55 / 256, size1: 1, frames: [12, 13] });
    emitter(null, { ...base, dir: [-1, 0.4, 1], drag: 0.8, max: 80, life: 1.2, size0: 4 / 256, size1: 4 / 256, frames: [6, 7, 8, 9, 10, 11] });
    emitter(null, { ...base, dir: [0, -0.5, 0], drag: 0, max: 20, life: 0.5, size0: 8 / 256, size1: 0x15 / 256, frames: [0, 1, 2, 3, 4, 5] });
  };
  const waterHit = (pos) => {
    const S = (0.1 * d) / 4.5;
    emitter(pos, { rate: 60, max: 20, vel: 0.4, spread: 30, scatter: 0.025, drag: 0.05, life: 0.5, size0: 0.5 * S + 1 / 256, size1: 1.5 * S + 1 / 256, frames: [14], ramp: 2 });
    particle({ kind: 'smoke', s: 0.5 }, pos.clone().add(above), smoke, waterTint);
    // Type-1 splash at the water surface: 0.3 tall, widening 0.05 -> 0.4 over 0.3 s.
    splash(pos, { sx: 0.05, sy: 0.3, grow: 0.35, rate: 10 / 3 });
  };
  const spawn = {
    'sim:ground': () => explosion(at),
    // Water hit: 0x800739A4 spray (20 at 60/s, +-15 deg, 0.4 up, drag 0.05, 0.5 s, wsm_f01, ramp 2) plus water-tinted smoke
    // (smkexp_PAL second plane). The splash model is func_80072EC4.
    'sim:water': () => waterHit(at),
    // A player's hit also gets the full explosion, func_80074600(pos, 0.1, 0.4, 1.0).
    'sim:water_player': () => { waterHit(at); explosion(at); },
    'sim:land': () => landing(above),
    'sim:shotdown': () => shotDown(),
    'sim:impact_fire': () => particle({ kind: 'fire', s: 0.6 }, above, fire),
    'sim:impact_ion': () => particle({ kind: 'ion1', A: 0.4, B: 0.8 }, above, ion2),
    'sim:impact_small': () => { embers(above, 0.05, 0.2); particle({ kind: 'fire', s: 0.2 }, above, fire); },
  }[id] ?? (SHOCKS[id] && (() => {
    for (const ring of SHOCKS[id].rings(+$('optFxSize').value)) shockwave(at, ring);
  })) ?? (() => {
    // composeRandomRotationMatrix: a direction within `cone` degrees of up, the chunk placed `radius` along it, speed uniform in range.
    const b = BURSTS[id];
    // The same destruction function also spawns its shockwave group on that frame.
    if (b.shock) for (const ring of SHOCKS[b.shock].rings(+$('optFxSize').value)) shockwave(at, ring);
    for (let i = b.count(); i > 0; i--) {
      const tilt = THREE.MathUtils.degToRad(rnd() * b.cone), az = rnd() * Math.PI * 2;
      const dir = new THREE.Vector3(Math.sin(tilt) * Math.cos(az), Math.cos(tilt), Math.sin(tilt) * Math.sin(az));
      const pos = above.clone().addScaledVector(dir, b.radius ?? 0);
      chunk(pos, dir.multiplyScalar(b.speed[0] + rnd() * (b.speed[1] - b.speed[0])), true, b.scale());
    }
  });
  // Items are removed in place: the spawners hold this same array.
  const fx = {
    sim: true,
    reset() {
      for (const it of items) { group.remove(it.obj); it.obj.material?.dispose(); }
      items.length = 0;
      seedRnd(fxSeed);
      spawn();
    },
    step(dt) {
      for (let i = items.length - 1; i >= 0; i--) if (!items[i].step(dt)) { group.remove(items[i].obj); items[i].obj.material?.dispose(); items.splice(i, 1); }
    },
    done: () => !items.length,
  };
  stats = { simulation: SIMS[id].label, source: SIMS[id].tip.split(':')[0] };
  hud();
  // Debris bursts fly ~3 units out; shockwaves frame their widest ring (mesh radius x peak scale); the rest stay within half a unit.
  const meshR = (mi) => new THREE.Box3().setFromObject(buildObject(sfx.hob.objects.find((o) => o.name === SHOCK_MESH[mi]), sfx, 'data/gnrc_stuff/sfx', { tris: 0, verts: 0, parts: 0 })).getSize(new THREE.Vector3()).x / 2;
  const h = BURSTS[id] ? 2 : SHOCKS[id] ? Math.max(0.5, ...SHOCKS[id].rings(+$('optFxSize').value).map((r) => meshR(r[0]) * r[3] * HOB_TO_WORLD)) : 0.5;
  // Frame the whole dive and slide from the side.
  if (!keepView) frameBox(id === 'sim:shotdown' ? new THREE.Box3(new THREE.Vector3(-1.5, 0, -1.5), new THREE.Vector3(1.5, 4.5, 3)) : new THREE.Box3(new THREE.Vector3(-h, 0, -h), new THREE.Vector3(h, h, h)), new THREE.Vector3(id === 'sim:shotdown' ? 1 : 0.6, 0.45, 0.8));
  timelineStart(fx);
}
// ---------- effect timeline ----------
// Every playback kind exposes reset() / step(dt) / done(); the timeline plays, loops, stops and seeks through them. Effect randomness
// comes from a seeded rnd(), so seeking replays the same take (reset + fixed 1/60 s steps up to the target time).
let rnd = Math.random, fxSeed = 1, fxDry = false;
const chance = (pct) => pct > 0 && (fxDry || rnd() * 100 < pct);
function seedRnd(seed) {
  let a = seed >>> 0;
  rnd = () => {
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
const TL = { time: 0, dur: 0, playing: true };
function timelineStart(fx) {
  effect = fx;
  // The timeline spans the longest the effect can run: dry runs over several seeds with every non-zero chance roll passing, capped at 10 s.
  let dur = 1 / 60;
  fxDry = true;
  for (let i = 0; i < 8; i++) {
    fxSeed = (Math.random() * 2 ** 31) | 0;
    fx.reset();
    let t = 0;
    for (; !fx.done() && t < 10; t += 1 / 60) fx.step(1 / 60);
    dur = Math.max(dur, t);
  }
  fxDry = false;
  Object.assign(TL, { time: 0, dur, playing: true });
  fx.reset();
  syncTimeline();
}
function seekTimeline(t) {
  effect.reset();
  for (let s = 0; s < t; s += 1 / 60) effect.step(Math.min(1 / 60, t - s));
  TL.time = t;
  syncTimeline();
}
function syncTimeline() {
  $('fxTime').max = TL.dur;
  $('fxTime').value = TL.time;
  $('fxTimeVal').textContent = `${TL.time.toFixed(2)} / ${TL.dur.toFixed(2)} s`;
  $('fxPlay').textContent = TL.playing ? 'Pause' : 'Play';
}
function applyEffect(dt) {
  dt *= +$('optFxSpeed').value;
  if (!effect?.step || !TL.playing) return;
  effect.step(dt);
  TL.time += dt;
  // A short take still plays out the full timeline, so every loop has the same length.
  if (TL.time >= TL.dur) {
    if ($('optFxLoop').checked) {
      // Each loop is a fresh take.
      fxSeed = (Math.random() * 2 ** 31) | 0;
      effect.reset();
      TL.time = 0;
    } else {
      TL.playing = false;
      TL.time = Math.min(TL.time, TL.dur);
    }
  }
  syncTimeline();
}

// Animations sit beside their model and share its name prefix (atat -> atat_walk/start/stop; atst_hi -> atst_*).
async function setupAnimations(id, objGroup) {
  const sel = $('optAnim');
  if (sel.dataset.id !== id) {
    sel.dataset.id = id;
    const dir = id.slice(0, id.lastIndexOf('/') + 1), prefix = id.split('/').pop().split('_')[0] + '_';
    const found = [];
    for (const a of rom.assets.values()) {
      if (!a.path.startsWith(dir) || a.dir + '/' !== dir || !a.name.startsWith(prefix) || /_(HOB|HMT)$/.test(a.name)) continue;
      const data = parseAnim(await rom.read(a.path));
      if (data && data.tracks.length) found.push({ name: a.name, data });
    }
    for (const name of SPRITE_ANIMS[id.replace(/^data\//, '')] ?? []) {
      const a = rom.assets.get(dir + name);
      if (a) found.push({ name, spriteData: await rom.read(a.path) });
    }
    // Hinged wings (the S-foils / shuttle wings slider) as animations too: a 1 s sweep each way (the game's sweep time is not traced).
    const kind = foilKind(id), names = objGroup && modelCache.get(id)?.hob.objects.find((o) => o.name === objGroup.name)?.nodeNames;
    if (kind && names?.has('wngrot_a') && names.has('wngrot_b')) {
      const [close, open] = kind === 'shuttle' ? ['wings_lower', 'wings_raise'] : ['sfoils_close', 'sfoils_open'];
      found.push({ name: close, foil: [0, 1] }, { name: open, foil: [1, 0] });
    }
    // Only clips whose nodes this model actually has.
    for (const [re, clips] of PROC_ANIMS) if (re.test(id)) for (const clip of clips) if (clip[3].every(([node]) => names?.has(node))) found.push({ name: clip[0], proc: clip, nodes: clip[3].map(([node, ...rest]) => [names.get(node), ...rest]) });
    sel.innerHTML = '<option value="">None (rest pose)</option>' + found.map((f, i) => `<option value="${i}">${esc(f.name)}</option>`).join('');
    sel.found = found;
    sel.value = '';
  }
  $('animRow').hidden = $('animSpeedRow').hidden = !sel.found.length;
  const pick = sel.found[sel.value];
  const rig = objGroup?.userData.rig;
  if (spriteRestore) {
    spriteRestore.map.image.data = spriteRestore.data;
    spriteRestore.map.needsUpdate = true;
    spriteRestore = null;
  }
  if (pick?.spriteData) {
    // The frames are written over the shown object's textured slot (civ_run, st_run/st_sht) and keyed with its colour key.
    let info = null;
    objGroup?.traverse((o) => { info ??= o.userData.infos?.find((i) => i.map && i.tex); });
    const sprite = info && parseSpriteAnim(pick.spriteData, info.tex.trans);
    anim = sprite && sprite[0].w === info.tex.w && sprite[0].h === info.tex.h ? { sprite, map: info.map } : null;
    if (anim) spriteRestore = { map: info.map, data: info.map.image.data };
    return;
  }
  if (pick?.foil) {
    anim = { foil: pick.foil, seconds: 1 };
    return;
  }
  if (pick?.proc && rig) {
    anim = { proc: pick.proc, rig, nodes: pick.nodes };
    return;
  }
  // Only play when the track count fits this model's nodes (one per node after the root).
  anim = pick && rig && pick.data.tracks.length === rig.nodes.length - 1
    ? { data: pick.data, rig, seconds: (pick.data.lengthMs || 2000) / 1000 }
    : null;
}

async function showModel(id, objIndex = -1, keepView = false) {
  setContext(false);
  selected = id;
  selectedObj = objIndex;
  selectedLevel = false;
  selectedEffect = false;
  renderList();
  const model = await loadModel(id);
  const { hob, hmt } = model;
  const sel = $('objSel');
  $('objRow').hidden = hob.objects.length < 2;
  if (sel.dataset.id !== id) {
    sel.dataset.id = id;
    sel.innerHTML = '<option value="-1">All objects</option>' + hob.objects.map((o, i) => `<option value="${i}">${esc(o.name)}</option>`).join('');
    sel.onchange = () => showModel(id, +sel.value);
    if (objIndex < 0 && hob.objects.length > 1) objIndex = 0;
  }
  sel.value = String(objIndex);
  const counts = { tris: 0, verts: 0, parts: 0 };
  const group = new THREE.Group();
  const objs = objIndex < 0 ? hob.objects : [hob.objects[objIndex]];
  objs.forEach((o, i) => {
    const g = buildObject(o, model, id, counts);
    if (objIndex < 0 && objs.length > 1) g.position.x = i * 12000;
    group.add(g);
  });
  // The game is Y-down; rotating about X keeps handedness, unlike a Y mirror.
  group.rotation.x = Math.PI;
  group.scale.setScalar(MODEL_VIEW_SCALE);
  // Auto-rotate turns the model about its own centre (not the camera), so panning doesn't throw it off.
  const pivot = new THREE.Group();
  pivot.name = 'spin';
  group.position.sub(new THREE.Box3().setFromObject(group).getCenter(new THREE.Vector3()));
  pivot.add(group);
  setRoot(pivot);
  await setupAnimations(id, group.children[0]);
  setupFoils(id, group.children[0]);
  setupExhaust(group.children[0]);
  // The slider only appears when this model has more than one LOD, and stops at its last one.
  $('lodRow').hidden = counts.lods < 2;
  $('optLod').max = Math.max(0, counts.lods - 1);
  $('lodVal').textContent = lodLevel();
  stats = { model: id, objects: hob.objects.length, 'LOD levels': counts.lods, parts: counts.parts, triangles: counts.tris, vertices: counts.verts, textures: hmt.textures.length };
  hud();
  // Models face +Z in game space; after the Y-down flip their nose points at -Z, so frame them from that side.
  if (!keepView) frame(root, new THREE.Vector3(-0.6, 0.5, -0.8));
}

// One 64x64 texture per tile, packed into an atlas; tiles get their own vertices so UVs don't share across tile edges.
function buildTerrain(hmp, texIds, bank) {
  const W = hmp.w * 4 + 1, H = hmp.h * 4 + 1;
  const ht = new Float32Array(W * H);
  const tileAt = (tx, ty) => hmp.tiles[hmp.map[ty * hmp.w + tx]];
  for (let ty = 0; ty < hmp.h; ty++) for (let tx = 0; tx < hmp.w; tx++) {
    const tile = tileAt(tx, ty);
    for (let py = 0; py < 5; py++) for (let px = 0; px < 5; px++) ht[(ty * 4 + py) * W + tx * 4 + px] = tile ? tile.hts[py * 5 + px] * hmp.heightScale : 0;
  }
  const hAt = (x, y) => ht[Math.min(H - 1, Math.max(0, y)) * W + Math.min(W - 1, Math.max(0, x))];

  const slots = new Map();
  for (const t of hmp.tiles) if (texIds[t.tex] !== undefined && !slots.has(texIds[t.tex])) slots.set(texIds[t.tex], slots.size);
  const cols = 16, AW = cols * 64, AH = Math.max(1, Math.ceil(slots.size / cols)) * 64;
  const atlas = new Uint8Array(AW * AH * 4).fill(255);
  for (const [bankId, slot] of slots) {
    const px = decodeTerrainTex(bank, bankId), ox = (slot % cols) * 64, oy = ((slot / cols) | 0) * 64;
    for (let y = 0; y < 64; y++) atlas.set(px.subarray(y * 256, y * 256 + 256), ((oy + y) * AW + ox) * 4);
  }
  for (let i = 3; i < atlas.length; i += 4) atlas[i] = 255;
  const map = new THREE.DataTexture(atlas, AW, AH, THREE.RGBAFormat);
  setFilter(map);

  // LOD drops the 4x4 quads per tile to 2x2 then 1x1, like the game's distant tiles.
  const st = 1 << +$('optTerrainLod').value, n = 4 / st + 1;
  const pos = [], nrm = [], uv = [], idx = [], col = [];
  // Baked like the game: shadow colour as ambient plus world light scaled by N.L, clamped (matches the warm, full-bright Tatooine sand in the PJ64 reference; a plain lerp greyed it out).
  // The stored horizontal direction points away from the sun used for the baked shading (verified on Tatooine and level 6 in the recomp).
  const { shadow, world, sunDir } = hmp.light;

  const lit = world.map((w) => w / 255);
  const dark = shadow.map((s) => s / 255);
  const L = [-sunDir[0], -Math.abs(sunDir[1]), -sunDir[2]], Ll = Math.hypot(...L) || 1;
  for (let ty = 0; ty < hmp.h; ty++) for (let tx = 0; tx < hmp.w; tx++) {
    const slot = slots.get(texIds[tileAt(tx, ty)?.tex]) ?? 0;
    const u0 = (slot % cols) * 64 + 0.5, v0 = ((slot / cols) | 0) * 64 + 0.5;
    const base = pos.length / 3;
    for (let j = 0; j < n; j++) for (let i = 0; i < n; i++) {
      const x = tx * 4 + i * st, y = ty * 4 + j * st;
      pos.push(x * 0.5, hAt(x, y), y * 0.5);
      // Height slope per world unit (grid step 0.5); sign matches the triangle winding below.
      const gx = hAt(x - 1, y) - hAt(x + 1, y), gz = hAt(x, y - 1) - hAt(x, y + 1), l = Math.hypot(gx, 1, gz);
      nrm.push(gx / l, 1 / l, gz / l);
      // Up normal in game (Y-down) space is (-gx, -1, -gz)/l.
      const ndl = Math.max(0, (gx * L[0] + L[1] + gz * L[2]) / (l * Ll) * -1);
      col.push(...dark.map((d, k) => Math.min(1, d + lit[k] * ndl)));
      // Texture rows run opposite to tile z: edge-seam scoring across levels picks this flip over the other 7 orientations.
      uv.push((u0 + (i * st) / 4 * 63) / AW, (v0 + (1 - (j * st) / 4) * 63) / AH);
    }
    for (let j = 0; j < n - 1; j++) for (let i = 0; i < n - 1; i++) {
      const a = base + j * n + i;
      idx.push(a, a + n, a + 1, a + 1, a + n, a + n + 1);
    }
  }
  const g = new THREE.BufferGeometry();
  g.setAttribute('position', new THREE.Float32BufferAttribute(pos, 3));
  g.setAttribute('normal', new THREE.Float32BufferAttribute(nrm, 3));
  g.setAttribute('uv', new THREE.Float32BufferAttribute(uv, 2));
  g.setAttribute('color', new THREE.Float32BufferAttribute(col, 3));
  g.setIndex(idx);
  return { g, map, tris: idx.length / 3 };
}
// Terrain geometry and atlas are cached per level and terrain LOD (cleared with the other caches); the mesh and material are rebuilt.
const terrainCache = new Map();
function terrainMesh(hmp, texIds, bank, key) {
  if (!terrainCache.has(key)) terrainCache.set(key, buildTerrain(hmp, texIds, bank));
  const { g, map, tris } = terrainCache.get(key);
  texCache.set('terrain', { map });
  const params = { map: $('optTex').checked ? map : null, side: THREE.DoubleSide, wireframe: $('optWire').checked };
  const m = new THREE.Mesh(g, gameLighting() ? new THREE.MeshBasicMaterial({ ...params, vertexColors: true }) : new THREE.MeshLambertMaterial(params));
  m.name = 'terrain';
  m.userData.shared = true;
  m.userData.terrain = map;
  m.userData.tiles = { hmp, texIds };
  return { mesh: m, tris };
}


// Fog colour is byte RGB at +0x28 of the level config table (0x800A05D0, 0x30 per level); the game fogs from 0.9 (level 2: 0.3) of the camera far plane to the far plane, ~32 world units.
let levelFog = null;
// Fog controls only show on levels that have a fog setting (frontend scenes have none).
function syncFogControls() {
  $('optFog').closest('label').hidden = $('optFogDist').closest('label').hidden = !levelFog;
  $('optFogDist').disabled = !$('optFog').checked;
}
// Fog follows the draw distance (computeGridLayerScrollOffset 0x800455C8): the game blends a range D from low to high over camera altitude
// (game y 2 .. -8, level 12: -2 .. -20; Taloraan pins 22), draws to 2 * (0.75 * D + 1) world units and fogs from levelFog.frac of that to
// the end. Levels use 16 / 24; in a cutscene, event type 17 (setGridLayerScrollBounds) sets low / high from its frame on.
const _cfp = new THREE.Vector3();
function fogFar() {
  let lo = 16, hi = 24, at = -1;
  if (cut) for (const e of cut.data.events) if (e.type === 17 && e.frame <= cut.t * cut.data.frames && e.frame >= at) [lo, hi, at] = [e.p[0], e.p[1], e.frame];
  const lv = parseInt(stockDir(cut ? cutLevel : selected)?.split('/lv_')[1] ?? '', 36), [a0, a1] = lv === 12 ? [-2, -20] : [2, -8];
  const gy = -camera.getWorldPosition(_cfp).y;
  const D = lv === 10 ? 22 : lo + (hi - lo) * THREE.MathUtils.clamp((a0 - gy) / (a0 - a1), 0, 1);
  return 2 * (0.75 * D + 1);
}
// Every frame, since the range moves with the camera.
function updateFog() {
  if (!scene.fog || !levelFog) return;
  const far = fogFar() * +$('optFogDist').value;
  scene.fog.near = far * levelFog.frac;
  scene.fog.far = far;
}
function applyFog() {
  if (!levelFog || !$('optFog').checked) { scene.fog = null; return; }
  const far = fogFar() * +$('optFogDist').value;
  scene.fog = new THREE.Fog(levelFog.color, far * levelFog.frac, far);
}

async function showLevel(dir, keepView = false, skyOverride = null) {
  setContext(true);
  $('lodRow').hidden = false;
  $('optLod').max = 3;
  $('btnBack').hidden = true;
  selected = dir;
  selectedLevel = true;
  selectedEffect = false;
  renderList();
  fillLevelButton();
  $('objRow').hidden = true;
  const counts = { tris: 0, verts: 0, parts: 0 };
  const world = new THREE.Group();
  let hmp = null, terrain = null;
  levelHeight = () => 0;
  if (rom.assets.has(dir + '/hmp')) {
    hmp = parseHMP(await rom.read(dir + '/hmp'));
    const [texIds, bank] = await Promise.all([rom.read(stockDir(dir) + '_TEX').then(parseTexTable), rom.read('data/level/tex')]);
    const t = terrainMesh(hmp, texIds, bank, dir + '|' + $('optTerrainLod').value);
    // Bilinear ground height on the 0.5-unit vertex grid (4x4 quads per 2-unit tile).
    const gw = hmp.w * 4, gh = hmp.h * 4;
    const vert = (gx, gz) => {
      gx = Math.min(gw, Math.max(0, gx));
      gz = Math.min(gh, Math.max(0, gz));
      const tile = hmp.tiles[hmp.map[Math.min(hmp.h - 1, gz >> 2) * hmp.w + Math.min(hmp.w - 1, gx >> 2)]];
      const px = gx === gw ? 4 : gx & 3, pz = gz === gh ? 4 : gz & 3;
      return tile ? tile.hts[pz * 5 + px] * hmp.heightScale : 0;
    };
    levelHeight = (wx, wz) => {
      const fx = wx * 2, fz = wz * 2, x0 = Math.floor(fx), z0 = Math.floor(fz), tx = fx - x0, tz = fz - z0;
      return (vert(x0, z0) * (1 - tx) + vert(x0 + 1, z0) * tx) * (1 - tz) + (vert(x0, z0 + 1) * (1 - tx) + vert(x0 + 1, z0 + 1) * tx) * tz;
    };
    terrain = t.mesh;
    world.add(terrain);
    counts.tris += t.tris;
  }
  // Level-local models (opkg) by object name, then shared models by subtype.
  const local = new Map();
  for (const a of rom.assets.values()) {
    if (a.dir !== dir || !a.name.endsWith('_HOB')) continue;
    const id = a.path.replace(/_HOB$/, '');
    if (!rom.assets.has(id + '_HMT')) continue;
    const m = await loadModel(id);
    m.hob.objects.forEach((o) => local.set(o.name, { obj: o, model: m, key: id }));
  }
  const datDv = await rom.read(dir + '/dat'), dat = parseDAT(datDv);
  const homes = await buildObjectIndex();
  const byName = new Map(dat.items.filter((i) => i.name).map((i) => [i.name, i]));
  // Resolve zone links (indices into the zone list) so trigger areas know their music zone and vice versa.
  const zoneList = dat.items.filter((i) => i.type === 6);
  const links = zoneList.flatMap((z) => (z.zoneLinks || []).map((k) => [z, zoneList[k]])).filter(([z, t]) => t && t !== z);
  for (const [z, t] of links) (t.linkedFrom ??= []).push(z.name || 'zone #' + z.index);
  // Music trigger areas (0x39) are unnamed in the DAT; label them by the music zone that owns them.
  for (const t of zoneList) if (!t.name && t.sub === 0x39 && t.linkedFrom) t.label = 'Music trigger for ' + t.linkedFrom.join(', ');
  for (const [z, t] of links) (z.linkedTo ??= []).push(t.name || t.label || (TYPES[markerKind(t)]?.label ?? 'zone') + ' #' + t.index);
  // Spline event nodes name the type-7 item they fire; record the spline (and how far along) on that item.
  // The event fires at the node, so its marker goes there (the record's own position is not used by the path code).
  const triggers = dat.items.filter((i) => i.type === 7);
  for (const s of dat.items) {
    if (!s.events?.length || s.points?.length < 2) continue;
    const curve = new THREE.CatmullRomCurve3(s.points.map((p) => new THREE.Vector3(...p)), false, 'centripetal');
    for (const [k, e] of s.events.entries()) {
      const t = triggers[e.trigger];
      if (!t) continue;
      (t.paths ??= []).push(`${s.name} at ${Math.round(e.at * 100)}%`);
      (t.nodePoints ??= []).push(curve.getPointAt(THREE.MathUtils.clamp(e.at, 0, 1)).toArray());
      (t.nodeTangents ??= []).push(curve.getTangentAt(THREE.MathUtils.clamp(e.at, 0, 1)));
      (t.pathNodes ??= []).push({ s, k, curve });
    }
  }
  for (const t of triggers) if (t.pathEvent && t.nodePoints) [t.recordPos, t.pos] = [t.pos, t.nodePoints[0]];
  // Path events are unnamed in the DAT; label them by what they do (0x1A by its craft mode word).
  for (const t of triggers) {
    const e = t.pathEvent;
    if (!e || t.name) continue;
    t.label = {
      0x1A: ['Bombing run', 'Shuttle landing animation', 'Wingman effect'][e.w[0]] ?? 'Craft action',
      0x1B: 'Speed change', 0x1C: 'Weave change', 0x22: 'Hold order',
      0x20: e.target && e.target[0] !== '-' ? 'Attack ' + e.target : 'Clear attack target',
      0x23: e.ia ? 'Landing pitch' : 'Take-off level-out',
      0x4E: 'Fly to ' + (e.target || 'player'),
    }[t.sub];
  }
  for (const it of dat.items) {
    if (it.type !== 7) continue;
    const named = it.strings.filter((s) => s !== it.name && byName.has(s));
    if (it.pathEvent?.target && byName.has(it.pathEvent.target)) named.push(it.pathEvent.target);
    if (it.name.startsWith('For_') && byName.has(it.name.slice(4))) named.push(it.name.slice(4));
    if (it.waypoint?.follow && byName.has(it.waypoint.follow)) named.push(it.waypoint.follow);
    // A roam area's users are the NPCs whose box name (the string after their own name) is this item.
    if (it.sub === 0x11 || it.sub === 0x17) {
      for (const n of dat.items) if (n.type === 0 && n.model === it.name) named.push(n.name);
    }
    it.targets = [...new Set(named)];
    // Not "Spline_01": LINE_ must not follow a letter.
    const line = /(?<![a-z])LINE_(\d+)/i.exec(it.name);
    if (line) {
      it.voiceId = +line[1];
      it.voice = voiceLine(it.voiceId);
    }
  }
  // Group splines by the NPCs that follow them: an NPC names its spline right after its own name (see npcAtAtUpdate).
  // A spline nobody names joins the group of a sibling with the same name stem (WEDGE_Spline_02 -> WEDGE_Spline_01's).
  const splines = dat.items.filter((s) => s.type === 3);
  const npcLabel = (n) => (n.sub === 0x27 ? 'Wingmen' : SUBTYPES[n.sub] ?? 'NPC');
  for (const s of splines) {
    const users = dat.items.filter((n) => n.type === 0 && n.model === s.name);
    if (!users.length) continue;
    s.users = users.map((n) => n.name);
    s.pathKind = 'path:' + [...new Set(users.map(npcLabel))].sort().join(' / ');
  }
  const stem = (n) => n.replace(/[_\d.]+$/, '');
  for (const s of splines) s.pathKind ??= splines.find((o) => o.users && stem(o.name) === stem(s.name))?.pathKind;
  // Hues spread evenly over this level's path kinds so neighbouring groups stay distinguishable.
  const kinds = [...new Set(splines.map((s) => s.pathKind).filter(Boolean))].sort();
  kinds.forEach((k, i) => {
    const label = k.slice(5);
    TYPES[k] = { color: new THREE.Color().setHSL((0.08 + i / kinds.length) % 1, 0.8, 0.62).getHex(), label, icon: 'cross', tip: `Splines followed by ${label}; the cross marks the start` };
  });
  const markers = new THREE.Group();
  markers.name = 'markers';
  let placed = 0;
  for (const it of dat.items) {
    let src = it.model && local.get(it.model), npc = false;
    if (!src && it.model && homes.has(it.model)) {
      const id = homes.get(it.model), m = await loadModel(id), obj = m.hob.objects.find((o) => o.name === it.model);
      src = { obj, model: m, key: id };
    }
    if (!src && it.type === 0 && SUBTYPE_MODEL[it.sub]) {
      // Prefer the Expansion Pak "_hi" variant when the ROM has one (atst_hi, atpt_i_hi).
      const [path, objName] = SUBTYPE_MODEL[it.sub].split('#'), base = 'data/' + path;
      const id = rom.assets.has(base + '_hi_HOB') ? base + '_hi' : base;
      if (rom.assets.has(id + '_HOB')) {
        const m = await loadModel(id);
        src = { obj: (objName && m.hob.objects.find((o) => o.name === objName)) || m.hob.objects[0], model: m, key: id };
        npc = true;
      }
    }
    // Hangar slots (0x06) are named after what stands there: a craft asset (xwing, ywing) or an object name with a "*N" copy suffix
    // (gt_rprun*3 = a running pilot).
    const slot = it.type === 0 && it.sub === 0x06 && it.name.replace(/[*#]\d+$/, '');
    if (!src && slot && rom.assets.has('data/pl_crafts/' + slot + '_HOB')) {
      const id = 'data/pl_crafts/' + slot, m = await loadModel(id);
      src = { obj: m.hob.objects[0], model: m, key: id };
    } else if (!src && slot && homes.has(slot)) {
      const id = homes.get(slot), m = await loadModel(id);
      src = { obj: m.hob.objects.find((o) => o.name === slot), model: m, key: id };
    }
    // The hangar's "planet" slot is a flat backdrop image (JPEG in data/backdrop/planet_N, chosen by the menu code), not a model.
    if (!src && it.type === 0 && it.sub === 0x06 && it.name === 'planet' && rom.assets.has('data/backdrop/planet_0')) {
      const dv = await rom.read('data/backdrop/planet_0');
      const bitmap = await createImageBitmap(new Blob([u8(dv)], { type: 'image/jpeg' }));
      // The JPEG has no alpha: derive it from brightness so the black above the planet's limb shows the stars, while the planet stays opaque.
      const cv = new OffscreenCanvas(bitmap.width, bitmap.height), cx = cv.getContext('2d');
      // Rows flipped while drawing: data textures put row 0 at v = 0 (the bottom), and flipY is not applied to them under WebGPU.
      cx.scale(1, -1);
      cx.drawImage(bitmap, 0, -bitmap.height);
      const img = cx.getImageData(0, 0, bitmap.width, bitmap.height), px = img.data;
      for (let i = 0; i < px.length; i += 4) px[i + 3] = Math.min(255, Math.max(px[i], px[i + 1], px[i + 2]) * 4);
      const tex = new THREE.DataTexture(px, bitmap.width, bitmap.height, THREE.RGBAFormat);
      tex.needsUpdate = true;
      // A fixed panel facing back into the hangar (the bay looks out along +Z), not a camera-facing sprite.
      // shortcut: 48 world units of width per unit of item scale is tuned by eye (wide enough to span the bay opening), not traced from
      // the game's sizing code; the image's 640x128 aspect is kept.
      const pw = it.scale[0] * 48, ph = pw * bitmap.height / bitmap.width;
      // Top edge on the item point (the horizon), planet hanging below behind the floor; rotated about X so the Y-down world shows it
      // upright and unmirrored from inside.
      const panel = new THREE.Mesh(new THREE.PlaneGeometry(pw, ph).translate(0, -ph / 2, 0).rotateX(Math.PI),
        new THREE.MeshBasicMaterial({ map: tex, fog: false, transparent: true, depthWrite: false, side: THREE.DoubleSide }));
      panel.position.set(...it.pos);
      panel.userData.item = it;
      world.add(panel);
      placed++;
      continue;
    }
    if (src) {
      const holder = new THREE.Group();
      holder.userData.source = { id: src.key, name: src.obj.name, objIndex: src.model.hob.objects.indexOf(src.obj) };
      holder.userData.placedItem = it;
      // Moving NPCs (fighters, walkers, transports, troops); cutscenes bring their own actors, so they can hide them. Fixed ones stay.
      holder.userData.npc = npc && !STATIC_NPCS.has(it.sub);
      // NPC spawns divide the item scale by the model's +0x34 value (npcAtStUpdate); buildings use the item scale as is.
      const div = npc ? src.obj.scaleDiv : 1;
      // Spline-following NPCs spawn at their path's start (npcAtAtUpdate/npcAtStUpdate record type 3), not the shared item
      // position. The reference is the spline's full name.
      const path = npc && it.model && dat.items.find((s) => s.type === 3 && s.points?.length && s.name === it.model);
      if (path) holder.userData.path = path;
      // Rebuilt from the item whenever the editor changes it (or moves its spline).
      holder.userData.compose = () => {
        let pos = path ? path.points[0] : it.pos;
        // Ground units stand on the terrain whatever their placed height (actor flag 4: Y = terrain - 0.25 * obj+0x40, which only the
        // ground transports set, from record +0xA0).
        if (npc && GROUND_NPCS.has(it.sub)) pos = [pos[0], levelHeight(pos[0], pos[2]) - (GROUND_SINK.has(it.sub) ? 0.25 * datDv.getFloat32(it.offset + 0xA0) : 0), pos[2]];
        // Face along the path's first segment: models look down +Z, so yaw = atan2(dx, dz) in (Y-down) game space.
        const next = path ? path.points.find((p) => Math.hypot(p[0] - pos[0], p[2] - pos[2]) > 1e-3) : null;
        const rotation = next ? new THREE.Matrix4().makeRotationY(Math.atan2(next[0] - pos[0], next[2] - pos[2])) : placementRotation(it.rot);
        const sc = (it.scale[0] ? it.scale : [0.25, 0.25, 0.25]).map((s) => (s / div) * HOB_TO_WORLD);
        holder.matrix.compose(new THREE.Vector3(...pos), new THREE.Quaternion(), new THREE.Vector3(1, 1, 1))
          .multiply(new THREE.Matrix4().makeScale(...sc)).multiply(rotation);
        holder.matrixWorldNeedsUpdate = true;
      };
      holder.matrixAutoUpdate = false;
      holder.userData.compose();
      holder.add(buildObject(src.obj, src.model, src.key, counts));
      // The hangar's star wall is a backdrop: drawn first without depth (like the skybox) so the planet sprite in front isn't cut by it.
      if (src.obj.name === 'stars') holder.traverse((o) => {
        if (!o.isMesh || !o.userData.infos) return;
        o.material = o.userData.infos.map((i) => new THREE.MeshBasicNodeMaterial({ map: i.map, vertexColors: true, side: THREE.DoubleSide, depthWrite: false, fog: false }));
        delete o.userData.infos;
        o.renderOrder = -1;
      });
      world.add(holder);
      placed++;
    } else {
      const mk = makeMarker(it);
      // A path event fired from several nodes gets a bead at each of the others.
      if (it.pathEvent) (it.nodePoints || []).slice(1).forEach((p, k) => {
        const d = new THREE.Mesh(beadGeometry(it.nodeTangents[k + 1]), meshMat(markerKind(it)));
        d.position.set(p[0] - it.pos[0], p[1] - it.pos[1], p[2] - it.pos[2]);
        d.userData.item = it;
        mk.add(d);
      });
      // Trigger -> target links, drawn under the trigger so they hide with it.
      for (const t of it.targets || []) {
        const p = byName.get(t).pos, end = new THREE.Vector3(p[0] - it.pos[0], p[1] - it.pos[1], p[2] - it.pos[2]);
        // Attack orders (0x20) point at their victim: red, dashed, with an arrowhead at the target.
        const attack = it.sub === 0x20 && t === it.pathEvent?.target;
        const link = new THREE.Line(new THREE.BufferGeometry().setFromPoints([new THREE.Vector3(), end]), attack ? attackLineMat : lineMat(markerKind(it), 0.45));
        if (attack) {
          link.computeLineDistances();
          const head = new THREE.Mesh(GEO.arrow, attackHeadMat);
          head.position.copy(end).addScaledVector(end.clone().normalize(), -0.3);
          head.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0), end.clone().normalize());
          head.userData.item = it;
          link.add(head);
        }
        link.userData.item = it;
        mk.add(link);
      }
      markers.add(mk);
    }
  }
  // Stacked splines: two splines overlap when 15% of either one's samples lie within 1.5 units of the other. Greedy colouring gives
  // overlapping splines different layers (fewest layers first); "Spread paths" lifts each spline by its layer.
  const pathMarkers = markers.children.filter((m) => m.userData.item.type === 3 && m.userData.item.points?.length > 1);
  const samples = pathMarkers.map((m) => new THREE.CatmullRomCurve3(m.userData.item.points.map((p) => new THREE.Vector3(...p)), false, 'centripetal').getPoints(m.userData.item.points.length * 4));
  const nearShare = (a, b) => a.filter((p) => b.some((q) => p.distanceToSquared(q) < 2.25)).length / a.length;
  pathMarkers.forEach((m, j) => {
    const taken = new Set();
    for (let i = 0; i < j; i++) if (Math.max(nearShare(samples[j], samples[i]), nearShare(samples[i], samples[j])) >= 0.15) taken.add(pathMarkers[i].userData.spreadRank);
    let layer = 0;
    while (taken.has(layer)) layer++;
    m.userData.spreadRank = layer;
  });
  applySpread(markers);

  world.add(markers);
  world.rotation.x = Math.PI;
  setRoot(world);
  if (hmp) applySun(hmp.light, hmp.w * 2, hmp.h * 2);

  world.traverse((o) => { if (o.isMesh && o.parent !== markers) o.castShadow = o.receiveShadow = true; });
  const lvIndex = parseInt(stockDir(dir).split('_').pop(), 36);
  if (stockDir(dir).includes('/level/lv_') && lvIndex < 21) {
    const o = 0x800A05D0 - 0x80000400 + 0x1000 + lvIndex * 0x30 + 0x28;
    levelFog = { color: new THREE.Color().setRGB(romBytes[o] / 255, romBytes[o + 1] / 255, romBytes[o + 2] / 255, THREE.SRGBColorSpace), frac: lvIndex === 2 ? 0.3 : 0.9 };
    applyFog();
  }
  syncFogControls();
  stats = { level: dir.split('/').pop(), items: dat.items.length, placed, markers: markers.children.length, triangles: counts.tris, vertices: counts.verts };
  hud();
  renderLegend(markers);
  if (!keepView) frameLevel(world, markers);
  // Added after framing so the dome doesn't inflate the bounds; the render loop keeps it centred on the camera.
  const skyId = skyOverride ?? stockDir(dir).replace(/.*\/lv_/, 'data/level/sky_');
  if (rom.assets.has(skyId + '_HOB')) {
    const m = await loadModel(skyId);
    sky = buildObject(m.hob.objects[0], m, skyId, { tris: 0, verts: 0, parts: 0 });
    sky.name = 'sky';
    sky.scale.setScalar(2);
    sky.visible = $('optSky').checked;
    // Below the horizon is filled with the sky's bottom edge: the texel (times vertex colour) at each of its lowest vertices, averaged.
    // Game space is Y-down, so the lowest vertices have the largest y (the horizon ring's bottom row, v = 1, on most skies).
    const edge = [];
    for (const parts of m.hob.objects[0].lods[0]) for (const p of parts) for (const f of p.faces) {
      const mat = m.hmt.materials[f.mat], t = mat && mat.type & 1 ? m.hmt.textures[mat.tex] : null;
      f.idx.forEach((vi, k) => edge.push({ y: p.verts[vi * 3 + 1], t, uv: f.uvs?.[k], c: f.colors ? (f.colors[k] ?? f.colors[0]) : 0xFFFFFFFF }));
    }
    const low = Math.max(...edge.map((q) => q.y));
    let r = 0, g = 0, b = 0, n = 0;
    for (const q of edge) {
      if (q.y < low - 1) continue;
      let tr = 255, tg = 255, tb = 255;
      if (q.t?.rgba && q.uv) {
        const x = THREE.MathUtils.euclideanModulo(Math.floor(q.uv[0] * q.t.w), q.t.w), y = THREE.MathUtils.clamp(Math.floor(q.uv[1] * q.t.h), 0, q.t.h - 1);
        const i = (y * q.t.w + x) * 4;
        [tr, tg, tb] = [q.t.rgba[i], q.t.rgba[i + 1], q.t.rgba[i + 2]];
      }
      r += tr * (q.c >>> 24) / 255; g += tg * ((q.c >>> 16) & 255) / 255; b += tb * ((q.c >>> 8) & 255) / 255; n++;
    }
    if (n) {
      skyColor = new THREE.Color().setRGB(r / n / 255, g / n / 255, b / n / 255, THREE.SRGBColorSpace);
      if (sky.visible) scene.background = skyColor;
    }
    // The game never fogs the sky.
    sky.traverse((o) => {
      if (!o.isMesh) return;
      o.material = o.userData.infos.map((i) => new THREE.MeshBasicMaterial({ map: i.map, vertexColors: true, side: THREE.DoubleSide, depthTest: false, depthWrite: false, fog: false }));
      delete o.userData.infos;
      o.userData.shared = true;
      o.renderOrder = -1;
    });
    world.add(sky);
  }
  fillCutPick(dir);
}

const busy = (text) => {
  $('busy').hidden = !text;
  $('busyText').textContent = text || '';
};
async function openFile(file) {
  busy('Loading ROM...');
  $('drop').classList.add('loading');
  // Let the spinner paint before the ROM is parsed on the main thread.
  await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
  await ready;
  $('dropErr').textContent = '';
  try {
    const buf = await file.arrayBuffer();
    rom = openRom(buf);
    romBytes = new Uint8Array(buf);
    romCrc = crc32(romBytes);
    projectHandle = null;
    clearCaches();
    modelCache.clear();
    cutStockScales = null;
    cutPathModels = null;
    edits.clear();
    dirty.clear();
    history = createHistory();
    savedStep = null;
    updateModButton();
    applyModLevelsText('');
    objectHome = null;
    if (rom.assets.has('data/voice_TXT')) voiceLine = parseVoiceLines(romBytes, await rom.read('data/voice_TXT'));
    frontText = rom.assets.has('data/front_TXT') ? parseFrontText(await rom.read('data/front_TXT')) : [];
    missionNames = frontText.slice(0, 19).map(titleCase);
    stopAudio();
    audioSel = null;
    musicSel = null;
    $('musicPanel').hidden = true;
    $('audioPanel').hidden = true;
    pcmCache.clear();
    audioBank = await loadAudioBank().catch((e) => (console.warn('audio', e), null));
    $('drop').hidden = true;
    document.querySelector('header input').hidden = true;
    document.body.classList.remove('no-rom');
    renderList();
    const first = 'data/pl_crafts/xwing';
    if (rom.assets.has(first + '_HOB')) showModel(first);
    busy('Indexing object packages...');
    const pkgs = [...rom.assets.values()].filter((a) => /\/opkg\d*_HOB$/.test(a.path) && a.size > 8);
    await Promise.all(pkgs.map((a) => loadModel(a.path.replace(/_HOB$/, ''))));
    buildObjectIndex();
    renderList();
  } catch (e) {
    $('dropErr').textContent = e.message;
  } finally {
    busy(null);
    $('drop').classList.remove('loading');
  }
}

// Rebuild the current view in place (same camera).
const refresh = () => selected && (selected.startsWith('cut:') ? reopenCutscene() : selectedEffect ? showEffect(selected, true) : selectedLevel ? showLevel(selected, true) : showModel(selected, +$('objSel').value, true));

