// ---- Ray-traced lights: roguesq_lights.json presets (the game's RS64 light pass; rt64_preset_light.h) on the open model or level ----
// A mesh preset whose mesh matches a part's key ("<object>#s<slot>.p<node>.<j>") lights that part in its model space, so its lights sit under
// the part's node in the model view. A level preset (no mesh) lights its level (gCurrentLevel; -2 or absent every level) in HMP map space:
// 512 per cell and the game's record height (16 x the s8 height), which the level view's world (2 per cell, height x the HMP scale) maps
// to exactly. Each light is a camera-facing bulb icon in its colour at a fixed screen size, picked and dragged with its own gizmo; the
// panel edits the picked light and its preset. Every change is one history step in mod/roguesq_lights.json, which Export mod ships.
const { lightsParse, lightsWrite, hobMeshKey, meshKeyName, lightsMeshMatches, lightNew, LIGHTS_ANY_LEVEL } = window.RS64;
const LIGHTS_FILE = 'mod/roguesq_lights.json', GAME_LIGHTS_FILE = 'roguesq_lights.json';
// Icon size as a fraction of the view height. New lights: reach and shadow offset in model units (just off the part they were placed on),
// and a level light's reach (map units, ~10 cells) placed this far above the clicked ground (world units).
// A sprite light's reach is in sprite sizes (the game's automatic sprite light uses 3).
const LIGHT_ICON_SIZE = 0.05, LIGHT_NEW_RADIUS = 4000, LIGHT_NEW_SHADOW = 200, LIGHT_LEVEL_RADIUS = 5000, LIGHT_LEVEL_LIFT = 1, LIGHT_FX_RADIUS = 3, LIGHT_HP_OFFSET = 200, LIGHT_FX_BACKDROP = 40;
const lightGizmo = new TransformControls(camera, canvas);
scene.add(lightGizmo.getHelper());
lightGizmo.addEventListener('dragging-changed', (e) => (controls.enabled = !e.value));
lightGizmo.addEventListener('mouseUp', () => lightGizmo.object && lightMoved(lightGizmo.object));
let lightsDoc = { presets: [] }, lightSel = null, lightsHere = [], lightsLevel = null, lightsFx = null, lightsHasHp = false, lightsModelGroup = null;
// The game's own light files ([{ src, text }] in the game's override order, read from the game folder) and their presets under this mod's:
// lightsBase holds those this mod does not override (a same-named preset in a later file wins, as in game), each with its source and text.
let gameLightTexts = [], lightsBase = [], lightsBaseInfo = new Map();
// The presets compiled into the game (src/video/default_lights.json), served with the site; they come before every file in the game folder.
const BUILTIN_LIGHTS = fetch('../../../src/video/default_lights.json').then((r) => (r.ok ? r.text() : '')).catch(() => '');
const lightsAll = () => [...lightsBase, ...lightsDoc.presets];
const isGameLight = (p) => lightsBaseInfo.has(p);
async function readLights() {
  lightsDoc = lightsParse(await virtualText(LIGHTS_FILE));
  const mine = new Set(lightsDoc.presets.map((p) => p.name)), byName = new Map();
  for (const { src, text } of [{ src: 'built into the game', text: await BUILTIN_LIGHTS }, ...gameLightTexts]) {
    try {
      for (const p of lightsParse(text).presets) byName.set(p.name, { p, src });
    } catch {}
  }
  lightsBase = [];
  lightsBaseInfo = new Map();
  for (const { p, src } of byName.values()) {
    if (mine.has(p.name)) continue;
    lightsBase.push(p);
    lightsBaseInfo.set(p, { src, json: JSON.stringify(p) });
  }
  return lightsDoc;
}
// Redraw after a change or a pick (markers, the picked light's reach and the panel come from showModel / showLevel / showEffect).
const lightsRefresh = () => (selectedEffect ? showEffect(selected, true) : selectedLevel ? reloadLevel(selected) : selected && showModel(selected, selectedObj, true));
async function writeLights(label) {
  // An edited game preset becomes this mod's override of it (same name), never a change to the game's file.
  for (const p of lightsBase) if (lightsBaseInfo.get(p).json !== JSON.stringify(p) && !lightsDoc.presets.some((q) => q.name === p.name)) lightsDoc.presets.push(p);
  await editText(LIGHTS_FILE, lightsWrite(lightsDoc), label);
  await lightsRefresh();
}
// Reads every roguesq_lights.json in the game folder (mods/* by folder name since the MODS list order is not in the folder, then next to the exe;
// the explorer's own preview mod is this mod, and a leftover model-lights folder is ignored as in game). ask: pick the folder if none is granted.
async function loadGameLights(ask) {
  let dir = ask ? await gameFolder().catch(() => null) : await idbKv('readonly', (s) => s.get('gameDir')).catch(() => null);
  if (!ask && dir && (await dir.queryPermission({ mode: 'readwrite' })) !== 'granted') dir = null;
  if (!dir) return false;
  const texts = [];
  const read = (h, src) => h.getFileHandle(GAME_LIGHTS_FILE).then((f) => f.getFile()).then((f) => f.text()).then((text) => texts.push({ src, text }), () => {});
  const mods = await dir.getDirectoryHandle('mods').catch(() => null), folders = [];
  if (mods) for await (const [id, h] of mods.entries()) if (h.kind === 'directory' && id !== PREVIEW_MOD && id !== 'model-lights') folders.push([id, h]);
  folders.sort(([a], [b]) => a.localeCompare(b));
  for (const [id, h] of folders) await read(h, `mods/${id}`);
  await read(dir, 'next to the exe');
  gameLightTexts = texts;
  return true;
}
if (window.showDirectoryPicker) loadGameLights(false).then((ok) => ok && selected && lightsRefresh());
const freeName = (base, taken) => {
  let name = base;
  for (let k = 2; taken.includes(name); k++) name = `${base}_${k}`;
  return name;
};
const lightPreset = (name) => lightsAll().find((p) => p.name === name);
const lightOf = (s) => lightPreset(s?.preset)?.lights.find((l) => l.name === s.light);
const isPicked = (p, l) => lightSel?.preset === p.name && lightSel.light === l.name;
// A level light's map position <-> the level world's local space (game Y-down, rotated by the world group).
const mapToWorld = (m, hs) => [m[0] / 256, (m[1] * hs) / 16, m[2] / 256];
const worldToMap = (w, hs) => [w[0] * 256, (w[1] * 16) / hs, w[2] * 256].map((v) => Math.round(v));

// The icon: a bulb with rays in the light's colour over a dark outline, one texture per colour.
const lightIcons = new Map();
const lightHex = (rgb) => {
  const peak = Math.max(...rgb, 1e-6);
  return '#' + rgb.map((c) => Math.round(Math.min(c / peak, 1) * 255).toString(16).padStart(2, '0')).join('');
};
function lightIcon(rgb) {
  const hex = lightHex(rgb);
  if (lightIcons.has(hex)) return lightIcons.get(hex);
  const c = document.createElement('canvas');
  c.width = c.height = 64;
  const g = c.getContext('2d');
  const disc = (r, x, y, fill) => {
    g.fillStyle = fill;
    g.beginPath();
    g.arc(x, y, r, 0, Math.PI * 2);
    g.fill();
  };
  g.lineCap = 'round';
  for (const [w, col] of [[7, 'rgba(0,0,0,0.75)'], [3.5, hex]]) {
    g.strokeStyle = col;
    g.lineWidth = w;
    for (let k = 0; k < 8; k++) {
      const a = (k * Math.PI) / 4, r0 = 19, r1 = k % 2 ? 26 : 29;
      g.beginPath();
      g.moveTo(32 + Math.cos(a) * r0, 32 + Math.sin(a) * r0);
      g.lineTo(32 + Math.cos(a) * r1, 32 + Math.sin(a) * r1);
      g.stroke();
    }
  }
  disc(15, 32, 32, 'rgba(0,0,0,0.75)');
  disc(12, 32, 32, hex);
  disc(4.5, 28, 28, 'rgba(255,255,255,0.9)');
  const tex = new THREE.CanvasTexture(c);
  tex.colorSpace = THREE.SRGBColorSpace;
  lightIcons.set(hex, tex);
  return tex;
}
// ---- Shaded preview: the game's light pass (RS64LightsPS.hlsl, gain 1, wrap 0.3) added on top of the explorer's surfaces ----
// Each light adds colour x (1 - distance / radius)^falloff x wrapped N.L x its spot cone to the prelit image, as in game; no shadows.
const LIGHT_MAX = 32, LIGHT_WRAP = 0.3;
const lightUniforms = () => THREE.uniformArray(Array.from({ length: LIGHT_MAX }, () => new THREE.Vector4()));
// The count is a float uniform turned into the loop bound in the shader, capped at LIGHT_MAX: three r170 uploads an 'int' uniform's value
// as float bits, so 1 read back as ~10^9 loop passes and hung the GPU.
const lpPos = lightUniforms(), lpCol = lightUniforms(), lpPar = lightUniforms(), lpDir = lightUniforms(), lpCount = THREE.uniform(0);
const rtLightSum = THREE.Fn(() => {
  const sum = THREE.vec3(0).toVar();
  THREE.Loop({ start: THREE.int(0), end: THREE.int(lpCount.min(LIGHT_MAX)), type: 'int', condition: '<' }, ({ i }) => {
    const pr = lpPos.element(i), par = lpPar.element(i);
    const toLight = pr.xyz.sub(THREE.positionWorld), dist = toLight.length(), l = toLight.div(dist.max(1e-4));
    const att = THREE.saturate(dist.div(pr.w).oneMinus()).pow(par.x);
    const ndl = THREE.saturate(THREE.normalWorld.dot(l).add(LIGHT_WRAP).div(1 + LIGHT_WRAP));
    // RT64's cone: lit where dot(surface -> light, beam) <= the max cosine (par.y), fading in from the falloff cosine (par.z); 1 = no cone.
    const d = l.dot(lpDir.element(i).xyz), span = par.y.sub(par.z);
    const inCone = THREE.select(span.greaterThan(1e-5), THREE.saturate(d.sub(par.z).div(span)).oneMinus(), THREE.float(1));
    const spot = THREE.select(par.y.lessThan(1), THREE.select(d.greaterThan(par.y), THREE.float(0), inCone), THREE.float(1));
    sum.addAssign(lpCol.element(i).xyz.mul(att).mul(ndl).mul(spot));
  });
  return sum;
});
// Opaque materials get the preview term (the game lights its G-buffer surfaces, not glows and other blended draws).
function rtLit(mat) {
  mat.outputNode = THREE.vec4(THREE.output.rgb.add(rtLightSum()), THREE.output.a);
  return mat;
}
const _lpPos = new THREE.Vector3(), _lpScale = new THREE.Vector3(), _lpDir = new THREE.Vector3();
// Light icons placed for the open view (emptied when a view shows its lights), so the per-frame pass need not walk a whole level.
const lightPreviewIcons = [];
// Each frame: every shown light icon with a preview (userData.preview: its light, reach in its parent's units, beam in its parent's
// space) becomes one of the shader's lights, in world space; past LIGHT_MAX the nearest to the camera, as the game's pass keeps.
function lightsPreviewTick() {
  let n = 0;
  const all = root && mode !== 'audio' && mode !== 'fonts' && mode !== 'menus' ? [...lightPreviewIcons, ...(lightsFx?.icons?.children ?? [])] : [];
  let icons = all.filter((o) => o.userData.preview && o.visible && o.parent);
  if (icons.length > LIGHT_MAX) {
    const dist = new Map(icons.map((o) => [o, o.getWorldPosition(_lpPos).distanceToSquared(camera.position)]));
    icons = icons.sort((a, b) => dist.get(a) - dist.get(b)).slice(0, LIGHT_MAX);
  }
  for (const o of icons) {
    const pv = o.userData.preview;
    const d = pv.light.description ?? {}, rgb = d.diffuseColor ?? [1, 1, 1], k = pv.gain ?? 1;
    o.getWorldPosition(_lpPos);
    o.parent.getWorldScale(_lpScale);
    _lpDir.fromArray(pv.dir ?? d.direction ?? [0, -1, 0]).transformDirection(o.parent.matrixWorld);
    lpPos.array[n].set(_lpPos.x, _lpPos.y, _lpPos.z, Math.max(pv.reach * _lpScale.x, 1e-6));
    lpCol.array[n].set(rgb[0] * k, rgb[1] * k, rgb[2] * k, 0);
    lpPar.array[n].set(d.attenuationExponent ?? 1, d.spotMaxCosine ?? 1, d.spotFalloffCosine ?? 1, 0);
    lpDir.array[n].set(_lpDir.x, _lpDir.y, _lpDir.z, 0);
    n++;
  }
  lpCount.value = n;
}
const lightReachGeo = new THREE.SphereGeometry(1, 24, 12);
// One light under parent at local position pos; reach (parent-local units, null = none) shows while it is picked. Switched-off lights fade.
// fromLocal turns a dragged local position back into the file's; dir is the beam in parent-local space (default: the file's).
function lightPlace(parent, p, l, pos, reach, fromLocal, dir) {
  const on = l.enabled !== false && p.enabled !== false;
  const icon = new THREE.Sprite(new THREE.SpriteMaterial({ map: lightIcon(l.description?.diffuseColor ?? [1, 1, 1]), sizeAttenuation: false, depthTest: false, transparent: true }));
  icon.material.opacity = on ? 1 : 0.35;
  icon.renderOrder = 10;
  icon.position.fromArray(pos);
  icon.userData.light = { preset: p.name, light: l.name, fromLocal };
  icon.userData.preview = on && reach != null ? { light: l, reach, dir } : null;
  if (icon.userData.preview) lightPreviewIcons.push(icon);
  // A screen-sized sprite still takes its parents' world scale (models draw at MODEL_VIEW_SCALE): divide it back out.
  parent.updateWorldMatrix(true, false);
  const s = new THREE.Vector3().setFromMatrixScale(parent.matrixWorld);
  icon.scale.set(LIGHT_ICON_SIZE / s.x, LIGHT_ICON_SIZE / s.y, 1);
  parent.add(icon);
  if (!isPicked(p, l)) return;
  lightGizmo.attach(icon);
  if (reach == null) return;
  const sphere = new THREE.Mesh(lightReachGeo, new THREE.MeshBasicMaterial({ color: 0xffd060, wireframe: true, transparent: true, opacity: 0.25 }));
  sphere.userData.shared = true;
  sphere.position.fromArray(pos);
  sphere.scale.setScalar(reach);
  sphere.raycast = () => {};
  parent.add(sphere);
}

// Called by showModel with the model group (one child per HOB object, each with its rig and LOD slot).
async function lightsShow(group) {
  lightGizmo.detach();
  lightPreviewIcons.length = 0;
  await readLights();
  lightsHere = [];
  lightsLevel = lightsFx = null;
  lightsModelGroup = group;
  lightsHasHp = (await lightHardpoints(group)).length > 0;
  const model = await loadModel(selected);
  for (const objGroup of group.children) {
    // A camera-units reach is in game world units, whatever the model's draw scale. shortcut: that scale is estimated as a flying NPC's
    // (item scale 0.014 over the HOB's +0x34 divisor, ~0.0126 for crafts as measured in game); models drawn at other scales (lasers,
    // buildings) show a reach off by their own scale. A level shows placed models at their real scale (lightsShowLevel).
    const scaleDiv = model.hob.objects.find((o) => o.name === objGroup.name)?.scaleDiv ?? 1;
    meshLightsOn(objGroup, scaleDiv / 0.014);
  }
  lightsPanel();
}
// The mesh presets on one HOB object group (buildObject: its rig and LOD slot), as icons under the matching parts' nodes; perCameraUnit
// is model units per game world unit, for camera-units presets.
function meshLightsOn(objGroup, perCameraUnit) {
  const rig = objGroup.userData.rig, slot = objGroup.userData.lodSlot;
  if (!rig) return;
  rig.nodes.forEach((node, i) => {
    const keys = node.children.filter((m) => m.userData.hob).map((m) => hobMeshKey(meshKeyName(objGroup.name, rig.src.length), slot, i, m.userData.hob.j));
    if (!keys.length) return;
    for (const p of lightsAll()) {
      if (!p.mesh || !keys.some((k) => lightsMeshMatches(p.mesh, k))) continue;
      if (!lightsHere.includes(p)) lightsHere.push(p);
      const reach = (l) => (l.description?.attenuationRadius ?? 500) * (p.cameraUnits ? perCameraUnit : 1);
      for (const l of p.lights) lightPlace(node, p, l, l.description?.position ?? [0, 0, 0], reach(l), (v) => v.map((x) => Math.round(x)));
    }
  });
}
// Called by showLevel with its world group (game space), the HMP (null: no terrain) and the level index (-1: not a mission level).
async function lightsShowLevel(world, hmp, level) {
  lightGizmo.detach();
  lightPreviewIcons.length = 0;
  await readLights();
  lightsFx = null;
  lightsLevel = hmp && level >= 0 ? { level, hs: hmp.heightScale } : null;
  lightsHere = lightsLevel ? lightsAll().filter((p) => !p.mesh && !p.sprite && ((p.level ?? LIGHTS_ANY_LEVEL) === LIGHTS_ANY_LEVEL || p.level === level)) : [];
  const hs = lightsLevel?.hs;
  for (const p of lightsHere) {
    for (const l of p.lights) {
      // Map space scales height apart from x / z, so the beam is mapped through the same conversion as the position.
      const m = l.description?.position ?? [0, 0, 0], beam = l.description?.direction ?? [0, -1, 0];
      const a = mapToWorld(m, hs), b = mapToWorld(m.map((v, i) => v + beam[i]), hs);
      lightPlace(world, p, l, a, (l.description?.attenuationRadius ?? 500) / 256, (v) => worldToMap(v, hs), b.map((v, i) => v - a[i]));
    }
  }
  // Placed models carry their mesh presets too, at their real draw scale: a game world unit is 1/256 of the level's world units.
  world.updateMatrixWorld(true);
  const worldScale = world.getWorldScale(new THREE.Vector3()).x, s = new THREE.Vector3();
  const placed = [];
  world.traverse((o) => o.userData.hobObject && placed.push(o));
  for (const objGroup of placed) meshLightsOn(objGroup, 1 / 256 / (objGroup.getWorldScale(s).x / worldScale));
  lightsPanel();
}
// Called by showEffect / showSim with the frame keys their sprites can show (spriteKeys). sheet: the effect's own file when it is one
// sheet ("Light this effect" lights all its frames), null for simulations.
async function lightsShowFx(keys, sheet) {
  lightGizmo.detach();
  lightPreviewIcons.length = 0;
  await readLights();
  lightsLevel = null;
  lightsFx = { keys, sheet, icons: null, lit: new Map() };
  lightsHere = lightsAll().filter((p) => p.sprite && keys.some((k) => lightsMeshMatches(p.sprite, k)));
  if (lightsHere.length) fxLitSurface();
  lightsPanel();
}
// Called by a cutscene's effect kit with its group: its sprites carry lights (bulbs and shading on the level) during playback; the panel
// stays the level's, and only that group is searched for sprites each frame.
async function lightsCutFx(keys, group) {
  await readLights();
  lightsFx = { keys, sheet: null, icons: null, lit: new Map(), cut: true, scope: group };
}
// Something for sprite lights to fall on, in the background colour so only their light shows: a simulation's ground (otherwise depth-only),
// else a backdrop behind a sheet's sprites.
function fxLitSurface() {
  const bg = scene.background?.isColor ? scene.background : new THREE.Color(0);
  let surface = root.getObjectByName('fxGround');
  if (!surface) {
    surface = new THREE.Mesh(new THREE.PlaneGeometry(4000, 4000));
    surface.position.z = -LIGHT_FX_BACKDROP;
    surface.renderOrder = -1;
    surface.raycast = () => {};
    root.add(surface);
  }
  surface.material = rtLit(new THREE.MeshBasicNodeMaterial({ color: bg, depthWrite: surface.name === 'fxGround' }));
}
// The light a frame key is lit with (the first light of the first preset on that key), or null; cached per showing.
function fxLight(key) {
  if (!lightsFx.lit.has(key)) {
    const p = [...lightsAll()].reverse().find((q) => q.enabled !== false && q.sprite && lightsMeshMatches(q.sprite, key) && q.lights.some((l) => l.enabled !== false));
    lightsFx.lit.set(key, p ? p.lights.find((l) => l.enabled !== false) : null);
  }
  return lightsFx.lit.get(key);
}
// Each frame: a bulb on every live sprite showing a lit frame, faded with it (the game scales a sprite light by its alpha). Particles
// spawn, cycle frames and die at random, so a light exists exactly while its sprite does, as in game.
function lightsFxTick() {
  if (!lightsFx || !root || (mode !== 'effects' && !lightsFx.cut)) return;
  if (lightsFx.icons?.parent !== root) {
    lightsFx.icons = new THREE.Group();
    root.add(lightsFx.icons);
  }
  const icons = lightsFx.icons, at = new THREE.Vector3(), size = new THREE.Vector3(), up = new THREE.Vector3();
  let n = 0;
  (lightsFx.scope ?? root).traverseVisible((o) => {
    const key = (o.isSprite || o.userData.fxMain) && o.material.map?.userData.sprKey, light = key && o.material.opacity > 0 && fxLight(key);
    if (!light) return;
    const rgb = light.description?.diffuseColor ?? [1, 1, 1];
    let icon = icons.children[n++];
    if (!icon) {
      icon = new THREE.Sprite(new THREE.SpriteMaterial({ sizeAttenuation: false, depthTest: false, transparent: true }));
      icon.renderOrder = 10;
      icon.scale.setScalar(LIGHT_ICON_SIZE * 0.6);
      icons.add(icon);
    }
    icon.material.map = lightIcon(rgb);
    icon.material.opacity = o.material.opacity;
    icon.position.copy(root.worldToLocal(o.getWorldPosition(at)));
    // The game's sprite light: reach = radius x the sprite's size, brightness x its alpha. The reach is in the icon group's units.
    const spriteSize = o.isSprite ? o.getWorldScale(size).x : new THREE.Box3().setFromObject(o).getSize(size).x;
    icon.userData.preview = { light, reach: ((light.description?.attenuationRadius ?? LIGHT_FX_RADIUS) * spriteSize) / icons.getWorldScale(up).x, gain: o.material.opacity };
  });
  icons.children.forEach((c, i) => (c.visible = i < n));
}
async function lightsAddFx() {
  const { sheet, keys } = lightsFx;
  await lightAddTo((q) => q.sprite === keys.join('|'), () => ({ name: freeName('fx_' + sheet.split(/[/#]/).pop(), lightsAll().map((q) => q.name)), enabled: true, sprite: keys.join('|'), lights: [] }),
    lightNew([0, 0, 0], LIGHT_FX_RADIUS));
}
// A panel button drawn as an icon, its label as the tooltip and accessible name.
function iconButton(icon, title, onclick) {
  const b = elem('button', { title, onclick });
  setEditIcon(b, icon);
  return b;
}
const lightMarkers = () => {
  const out = [];
  root?.traverse((o) => o.userData.light && out.push(o));
  return out;
};
// Left click: a light icon picks it (and attaches the gizmo); a click elsewhere drops the pick.
function lightsClick(e) {
  if (lightGizmo.axis) return true;
  if ((mode !== 'models' && mode !== 'levels') || !root) return false;
  const hit = pick(e, lightMarkers());
  if (!hit?.object.userData.light) {
    if (lightSel) lightSelect(null);
    return false;
  }
  lightSelect(hit.object.userData.light);
  return true;
}
function lightSelect(s) {
  lightSel = s && { preset: s.preset, light: s.light };
  lightGizmo.detach();
  lightsRefresh();
}
async function lightMoved(icon) {
  const l = lightOf(icon.userData.light);
  if (!l) return;
  l.description ??= {};
  l.description.position = icon.userData.light.fromLocal(icon.position.toArray());
  await writeLights('Move light');
}
async function lightAddTo(match, make, description) {
  await readLights();
  let p = lightsAll().find(match);
  if (!p) {
    p = make();
    lightsDoc.presets.push(p);
  }
  const name = freeName('light', p.lights.map((l) => l.name));
  p.lights.push({ name, enabled: true, description });
  lightSel = { preset: p.name, light: name };
  await writeLights('Add light');
}
// Player crafts and their wingman models (rs64lights::movingCraft: "wm" + craft + "wng"): one model space, and the same named joints
// (wngrot_a, flap_r), which the wingman models split into more nodes. A light on one is the same light on the other.
const CRAFT_VARIANTS = [['pl_crafts/xwing', 'pl_crafts/wmxwng'], ['pl_crafts/ywing', 'pl_crafts/wmywng'], ['pl_crafts/awing', 'pl_crafts/wmawng'],
  ['pl_crafts/vwing', 'pl_crafts/wmvwng'], ['pl_crafts/snowspeeder', 'pl_crafts/wmspdr'], ['pl_crafts/falcon', 'pl_crafts/wmfalc']];
// A node's joint: itself or its nearest ancestor named in names (the model root when none is), as that name ('' = root).
function nodeJoint(obj, i, names) {
  const nameOf = new Map([...obj.nodeNames].map(([n, k]) => [k, n]));
  for (let k = i; k >= 0; k = obj.nodes[k].parent) if (names.has(nameOf.get(k))) return nameOf.get(k);
  return '';
}
// The keys of the same part in the craft's variant models (none for other models): node i of object obj in model id, slot = its LOD slot.
async function variantKeys(id, obj, i) {
  const pair = CRAFT_VARIANTS.find((p) => p.some((v) => id === 'data/' + v));
  if (!pair) return [];
  const keys = [];
  for (const v of pair.filter((p) => id !== 'data/' + p)) {
    const other = (await loadModel('data/' + v)).hob.objects[0];
    const shared = new Set([...obj.nodeNames.keys()].filter((n) => other.nodeNames.has(n)));
    const joint = nodeJoint(obj, i, shared), slot = other.lodSlots[0] ?? 0;
    other.nodes.forEach((_, k) => (other.lods[0]?.[k]?.length && nodeJoint(other, k, shared) === joint && keys.push(`${meshKeyName(other.name, other.nodes.length)}#s${slot}.p${k}`)));
  }
  return keys;
}
// From a model part's right-click card: a light at the clicked point, in a preset for that part (made when the part has none; a craft's
// preset also names the same part of its wingman / player model, so it lights both).
async function lightsAdd(hit) {
  const mesh = hit.object, node = mesh.parent;
  let objGroup = node;
  while (objGroup && !objGroup.userData.hobObject) objGroup = objGroup.parent;
  if (!objGroup) return;
  const i = mesh.userData.hob.node, keyName = meshKeyName(objGroup.name, objGroup.userData.rig.src.length), key = `${keyName}#s${objGroup.userData.lodSlot}.p${i}`;
  const obj = (await loadModel(selected)).hob.objects.find((o) => o.name === objGroup.name);
  const meshKeys = [key, ...(obj ? await variantKeys(selected, obj, i) : [])].join('|');
  const local = node.worldToLocal(hit.point.clone()).toArray().map((v) => Math.round(v));
  return lightAddTo((q) => q.mesh?.split('|').includes(key), () => ({ name: freeName(`${keyName}_p${i}`, lightsAll().map((q) => q.name)), enabled: true, mesh: meshKeys, lights: [] }),
    { ...lightNew(local, LIGHT_NEW_RADIUS), shadowOffset: LIGHT_NEW_SHADOW });
}
// The open model's light hardpoints: glw* (engine nozzle / lamp glows, where the game draws its glow sprites). trl* are contrail points
// (wingtips), not light sources. [{ objGroup, obj, hp }] for each object.
async function lightHardpoints(group = lightsModelGroup) {
  const model = await loadModel(selected), out = [];
  group?.updateMatrixWorld(true);
  for (const objGroup of group?.children ?? []) {
    const obj = model.hob.objects.find((o) => o.name === objGroup.name);
    const hp = (obj?.hardpoints ?? []).filter((h) => h.name.startsWith('glw'));
    if (objGroup.userData.rig && hp.length) out.push({ objGroup, obj, hp });
  }
  return out;
}
// A light just behind each hardpoint (LIGHT_HP_OFFSET along its direction, as the hand-placed exhausts sit behind their nozzles), one preset per
// drawn node: a hardpoint on a node with no geometry moves to its nearest ancestor that has some (a mesh preset lights only drawn parts).
async function lightsFromHardpoints() {
  await readLights();
  const taken = lightsAll().map((q) => q.name);
  for (const { objGroup, obj, hp } of await lightHardpoints()) {
    const rig = objGroup.userData.rig, slot = objGroup.userData.lodSlot, keyName = meshKeyName(objGroup.name, rig.src.length);
    const drawn = (i) => rig.nodes[i].children.some((m) => m.userData.hob);
    const byNode = new Map();
    for (const h of hp) {
      let t = h.node;
      while (t > 0 && !drawn(t)) t = obj.nodes[t].parent;
      h.points.forEach((p, k) => {
        const at = new THREE.Vector3(...p.pos).multiplyScalar(256).addScaledVector(new THREE.Vector3(...p.dir), LIGHT_HP_OFFSET);
        const local = rig.nodes[t].worldToLocal(rig.nodes[h.node].localToWorld(at)).toArray().map((v) => Math.round(v));
        if (!byNode.has(t)) byNode.set(t, []);
        byNode.get(t).push({ name: `${h.name}_${k}`, enabled: true, description: { ...lightNew(local, LIGHT_NEW_RADIUS), shadowOffset: LIGHT_NEW_RADIUS } });
      });
    }
    for (const [t, lights] of byNode) {
      const mesh = [`${keyName}#s${slot}.p${t}`, ...(await variantKeys(selected, obj, t))].join('|'), name = freeName(`hp_${keyName}_p${t}`, taken);
      taken.push(name);
      lightsDoc.presets.push({ name, enabled: true, mesh, lights });
    }
  }
  await writeLights('Lights from hardpoints');
}
// From a terrain tile's right-click card: a light just above the clicked ground, in this level's preset (made when it has none).
function lightsAddLevel(hit) {
  if (!lightsLevel) return;
  const { level, hs } = lightsLevel, w = root.worldToLocal(hit.point.clone()).toArray();
  w[1] -= LIGHT_LEVEL_LIFT;
  return lightAddTo((q) => !q.mesh && q.level === level, () => ({ name: freeName(`level_${level}`, lightsAll().map((q) => q.name)), enabled: true, level, lights: [] }),
    { ...lightNew(worldToMap(w, hs), LIGHT_LEVEL_RADIUS), shadowOffset: 5 });
}
// Merges another roguesq_lights.json (the game's editing copy next to the exe, or another mod's) into the mod's: a same-named preset is replaced.
function lightsImport() {
  const input = elem('input', { type: 'file', accept: '.json,application/json' });
  input.onchange = async () => {
    const file = input.files[0];
    if (!file) return;
    let other;
    try {
      other = lightsParse(await file.text());
    } catch (err) {
      alert(`${file.name} is not a light preset file: ${err.message}`);
      return;
    }
    await readLights();
    for (const p of other.presets) {
      const i = lightsDoc.presets.findIndex((q) => q.name === p.name);
      if (i >= 0) lightsDoc.presets[i] = p;
      else lightsDoc.presets.push(p);
    }
    await writeLights(`Open lights ${file.name}`);
  };
  input.click();
}

// What a preset lights, in words: "On xwing part 1 · wmxwng parts 1, 2, 3", "On every red_laser", "Level 0"; the raw keys are the tooltip.
function lightScope(p) {
  if (p.sprite) return `On ${p.sprite.split('|').filter(Boolean).length} effect frame(s)`;
  const scenes = { [LIGHTS_ANY_LEVEL]: 'Every level', [-1]: 'Outside missions', [-3]: 'Briefing room', [-4]: 'Hangar' };
  if (!p.mesh) return scenes[p.level ?? LIGHTS_ANY_LEVEL] ?? `Level ${p.level}`;
  const models = new Map();
  for (const key of p.mesh.split('|').filter(Boolean)) {
    const [model, rest = ''] = key.split('#');
    const part = /\.p(\d+)/.exec(rest)?.[1];
    if (!models.has(model)) models.set(model, []);
    if (part != null) models.get(model).push(part);
  }
  return 'On ' + [...models].map(([m, parts]) => (parts.length ? `${m} part${parts.length > 1 ? 's' : ''} ${parts.join(', ')}` : `every ${m}`)).join(' · ');
}
function lightsPanel() {
  const panel = $('lightPanel');
  const shown = !!root && ((mode === 'models' && !selectedLevel && !selectedEffect) || (mode === 'levels' && selectedLevel && !!lightsLevel) || (mode === 'effects' && selectedEffect && !!lightsFx));
  panel.hidden = !shown;
  panel.classList.toggle('inLevel', mode !== 'models');
  if (!shown) {
    lightGizmo.detach();
    return;
  }
  const count = lightsHere.reduce((n, p) => n + p.lights.length, 0);
  const tools = [iconButton('open', 'Open lights file: merges a roguesq_lights.json (the game\'s copy next to the exe, or another mod\'s) into this mod; a preset with the same name is replaced', lightsImport)];
  if (mode === 'models' && lightsHasHp) tools.unshift(iconButton('hardpoints', 'Lights from hardpoints: a light behind each of the model\'s glow hardpoints (engine nozzles, lamps)', lightsFromHardpoints));
  if (window.showDirectoryPicker) tools.push(iconButton('gameLights', `Game's lights: reads every roguesq_lights.json in the game folder (mods and next to the exe) and shows them under this mod's; editing one overrides it in this mod. Asks for the folder the first time. ${gameLightTexts.length} file(s) loaded`, async () => (await loadGameLights(true)) && lightsRefresh()));
  if (lightsFx?.sheet) tools.unshift(iconButton('lightAdd', 'Light this effect: a light on every sprite of this effect while it shows one of its frames, fading with the sprite', lightsAddFx));
  const kids = [elem('div', { className: 'editHead' }, elem('b', { textContent: `Ray-traced lights (${count})` }), elem('span', { className: 'lightTools' }, ...tools))];
  const none = lightsFx ? (lightsFx.sheet ? 'No light preset on this effect\'s sprites.' : 'No light preset on these sprites. Light an effect sheet (Explosions, Particles) and it lights here too.')
    : lightsLevel ? 'No light preset for this level. Right-click the ground and pick "Add light here".' : 'No light preset matches this model. Right-click a part and pick "Add light here".';
  if (!lightsHere.length) kids.push(elem('small', { className: 'shotNote', textContent: none }));
  for (const p of lightsHere) {
    const from = isGameLight(p) ? ` · from the game's ${lightsBaseInfo.get(p).src}` : '';
    kids.push(elem('div', { className: 'lightRow' }, elem('span', { textContent: p.name }), elem('small', { textContent: lightScope(p) + from, title: p.mesh || p.sprite ? `${p.mesh ? 'Mesh' : 'Sprite'} keys: ${(p.mesh || p.sprite).split('|').join(', ')}` : '' })));
    for (const l of p.lights) kids.push(elem('button', { textContent: l.name, className: isPicked(p, l) ? 'on' : '', onclick: () => lightSelect({ preset: p.name, light: l.name }) }));
  }
  const p = lightPreset(lightSel?.preset), l = lightOf(lightSel);
  if (p && l && lightsHere.includes(p)) kids.push(...lightFields(p, l));
  panel.replaceChildren(...kids);
}
const coneDeg = (cos) => (cos == null || cos >= 1 ? 0 : Math.round(THREE.MathUtils.radToDeg(Math.acos(Math.min(Math.max(-cos, -1), 1)))));
const coneCos = (deg) => (deg > 0 ? +(-Math.cos(THREE.MathUtils.degToRad(Math.min(deg, 180)))).toFixed(4) : 1);
function lightFields(p, l) {
  const d = (l.description ??= {});
  const rgb = d.diffuseColor ?? [1, 1, 1], peak = Math.max(...rgb, 1e-6);
  const commit = (label) => writeLights(label);
  const num = (label, value, step, set, tip) => elem('label', { title: tip ?? '' }, label + ' ', elem('input', { type: 'number', value, step, onchange: (e) => { set(+e.target.value); commit(`Light ${label.toLowerCase()}`); } }));
  const check = (label, value, set, tip) => elem('label', { title: tip ?? '' }, elem('input', { type: 'checkbox', checked: value, onchange: (e) => { set(e.target.checked); commit(label); } }), ' ' + label);
  // A section heading under a divider: its on/off box before the text, its delete icon alone at the far right so the two are not misclicked
  // for each other (their labels are tooltips).
  const headCheck = (text, label, value, set, delLabel, del) => elem('div', { className: 'lightHead' },
    elem('label', { title: label }, elem('input', { type: 'checkbox', checked: value, ariaLabel: label, onchange: (e) => { set(e.target.checked); commit(label); } }), elem('b', { textContent: text })),
    iconButton('delete', delLabel, () => { del(); lightSel = null; commit(delLabel); }));
  const lightHead = headCheck(`${p.name} / ${l.name}`, 'Light on', l.enabled !== false, (v) => (l.enabled = v), 'Delete light', () => (p.lights = p.lights.filter((q) => q !== l)));
  // A mod cannot remove another file's preset, only override it: deleting a game preset switches it off in this mod.
  const presetHead = isGameLight(p)
    ? headCheck('Preset', 'Preset on', p.enabled !== false, (v) => (p.enabled = v), 'Switch off this game preset in this mod (a mod can override it, not remove it)', () => (p.enabled = false))
    : headCheck('Preset', 'Preset on', p.enabled !== false, (v) => (p.enabled = v), 'Delete light preset', () => (lightsDoc.presets = lightsDoc.presets.filter((q) => q !== p)));
  const colour = elem('input', { type: 'color', value: lightHex(rgb), onchange: (e) => {
    const v = e.target.value;
    d.diffuseColor = [1, 3, 5].map((i) => +((parseInt(v.slice(i, i + 2), 16) / 255) * peak).toFixed(3));
    commit('Light colour');
  } });
  if (p.sprite) {
    // A sprite light sits on the sprite, reaches a multiple of its size and fades with it: no position, cone or shadow offset.
    return [
      lightHead,
      elem('label', {}, 'Colour ', colour),
      num('Strength', +peak.toFixed(3), 0.1, (v) => (d.diffuseColor = rgb.map((c) => +((c / peak) * v).toFixed(3))), 'Colour multiplier: above 1 is brighter than white'),
      num('Radius', d.attenuationRadius ?? LIGHT_FX_RADIUS, 0.5, (v) => (d.attenuationRadius = v), 'Reach, in sprite sizes: the light grows and shrinks with the sprite'),
      num('Falloff', d.attenuationExponent ?? 1, 0.1, (v) => (d.attenuationExponent = v), 'Exponent of (1 - distance / radius); 2 is the game\'s usual'),
      num('Flicker', d.flickerIntensity ?? 0, 0.05, (v) => (d.flickerIntensity = Math.min(Math.max(v, 0), 1)), 'How much it dims at random, 0 (steady) to 1 (flickers out)'),
      presetHead,
      elem('label', { title: 'Frame names (spr:<texture name>), "|"-separated; the game\'s F1 Lights tab lists the ones drawn' }, 'Sprites ', elem('input', { value: p.sprite, onchange: (e) => { p.sprite = e.target.value.trim(); commit('Light preset sprites'); } })),
    ];
  }
  const unit = p.mesh ? (p.cameraUnits ? 'camera units' : 'model units') : 'map units (512 per terrain cell)';
  const where = p.mesh ? 'Position in the part\'s model space' : 'Position in the level\'s map space (512 per terrain cell, Y down)';
  const scope = p.mesh
    ? [elem('label', { title: 'Mesh key or prefix: "red_laser" lights every red bolt, "xwing#s0.p1" one part' }, 'Mesh ', elem('input', { value: p.mesh, onchange: (e) => { p.mesh = e.target.value.trim(); commit('Light preset mesh'); } })),
      check('Radius in camera units', !!p.cameraUnits, (v) => (p.cameraUnits = v), 'For models drawn at varying scales (laser bolts): the reach stays the same on screen'),
      check('Follows the game\'s blinking lamp', !!p.lamp, (v) => (p.lamp = v), 'For lights on a model\'s glow hardpoints: in game each light shines only while the lamp drawn there is lit, in its colour and fade')]
    : [num('Level', p.level ?? LIGHTS_ANY_LEVEL, 1, (v) => (p.level = v), 'gCurrentLevel the preset lights; -1 outside missions, -2 every level, -3 briefing room, -4 hangar')];
  return [
    lightHead,
    elem('label', {}, 'Colour ', colour),
    num('Strength', +peak.toFixed(3), 0.1, (v) => (d.diffuseColor = rgb.map((c) => +((c / peak) * v).toFixed(3))), 'Colour multiplier: above 1 is brighter than white'),
    num('Radius', d.attenuationRadius ?? 500, 50, (v) => (d.attenuationRadius = v), `Reach, in ${unit}`),
    num('Falloff', d.attenuationExponent ?? 1, 0.1, (v) => (d.attenuationExponent = v), 'Exponent of (1 - distance / radius); 2 is the game\'s usual'),
    num('Shadow offset', d.shadowOffset ?? 5, 10, (v) => (d.shadowOffset = v), `Occluders closer than this to the light are ignored (${unit}); the radius makes it unshadowed`),
    ...[0, 1, 2].map((i) => num('XYZ'[i], (d.position ?? [0, 0, 0])[i], 10, (v) => ((d.position ??= [0, 0, 0])[i] = v), where)),
    num('Flicker', d.flickerIntensity ?? 0, 0.05, (v) => (d.flickerIntensity = Math.min(Math.max(v, 0), 1)), 'How much it dims at random, 0 (steady) to 1 (flickers out)'),
    // RT64's cone cosines are negative: lit where the angle from the beam is under acos(-spotMaxCosine); 1 = no cone.
    num('Cone (deg)', coneDeg(d.spotMaxCosine), 1, (v) => (d.spotMaxCosine = coneCos(v)), 'Spotlight: the beam\'s outer half-angle, 0 = a plain point light. It shines along Beam X / Y / Z'),
    num('Cone inner (deg)', coneDeg(d.spotFalloffCosine), 1, (v) => (d.spotFalloffCosine = Math.min(coneCos(v), d.spotMaxCosine ?? 1)), 'Full brightness inside this half-angle, fading to the outer one'),
    ...[0, 1, 2].map((i) => num('Beam ' + 'XYZ'[i], (d.direction ?? [0, -1, 0])[i], 0.1, (v) => ((d.direction ??= [0, -1, 0])[i] = v), 'Spotlight direction, in the same space as the position')),
    presetHead,
    ...scope,
  ];
}
