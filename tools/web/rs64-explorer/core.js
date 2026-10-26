'use strict';
// The explorer's shared core (renderer, camera, materials, markers, picking); the other area scripts load after it, in order (the loader at the end of index.html). three.js arrives through window.RS64_THREE.
const { THREE, OrbitControls, TransformControls, GLTFExporter } = window.RS64_THREE;
const { parseShadowTextures, parseMusicCues, musicZoneSet, openRom, parseHOB, parseHMT, parseHMP, parseDAT, parseTexTable, decodeTerrainTex, parseVoiceLines, parseAnim, parseSpriteAnim,
  parseSdir, decodeMusyxAdpcm, wavBytes, parseSpeech, decodeMort, encodeMort, parseVoiceTable, parseMenuVoice, parseFrontText, parsePool, parseSfxTable, renderSfx, parseCutsceneVoice, parseMixTables, parseSongTable, parseSongGroups, parseSong, encodeTexture, encodeBPS, zipStore, hmpGrid, writeHMP, datAddItem, datRemoveItem, modLevelsParse, modLevelsWrite, modLevelNew, modLevelAliases, medalInfo, datNameItem, RULE_WHENS, RULE_ACTIONS, RULES_PATH, rulesParse, rulesExtParse, rulesWrite, ruleRefs, rulesRename, rulesBroken, datBlank, hmpFlat, recordInert, npcPathGet, npcSetPath, npcPathField, datRemoveIndexed, objectivesApply, objectiveTargets, isUnit, WAVE_FIELDS, waveGet, waveSet, levelUsedFlags, waveFlagFor, triggerWave, WAVE_FOREIGN, datAddEnemy, splineSetPoints, splinePoints, splineCopy, splineAttackEnd, splineSetAttackEnd, uniqueName, shotDestroyGet, shotDestroySet, craftImportSlots, hobImportCraft, parseCutscene, cutsceneValue, createHistory, projectWrite, projectRead, crc32, writeCutscene, shotLinkGet, shotLinkSet, cutClips, cutDragClip, cutAddEvent, cutRemoveEvent, cutSetKey, cutRemoveKey, cutAddActor, cutRemoveActor, cutSetActorName, cutAddCamera, cutRemoveCamera, cutSetLength, cutBlank, cutSetChannelSpan, cutCurve, cutQuatToAxisAngle, cutAxisAngleToQuat, cutBakeActorPath, cutActorPose, cutBakeCameraPath, cutBakeCameraMode, cutShootAt, cutRetimeSnaps, cutUnbake, cutBake, cutBakedEvents, cutSyncModeSpans, CUT_LIMITS, CUT_ACTOR_EVENTS, cutMoveEvent, cutAnimateActor, CRAWL_PLANETS, CRAWL_PATH, crawlFontGaps, stockCrawls, crawlParse, crawlWrite, crawlFont, crawlLayout, encodeJpeg, CRAWL_STOCK_CAMERA, crawlCameraAt, gameFonts, glyphPackParse, glyphPackWrite, langChars, fontCovers, FONT_STRIP_HEADER, MOD_KINDS, modChangeKind, menuItemKey, menuItemKeys, menuLinks, exportPlan, menuParse, menuWrite, menuWriteSubset, menuMerge, menuLayout, menuFontGaps, NATIVE_MENUS, MENU_DEFAULT_BUTTONS, menuWithDefaults } = window.RS64;
const $ = (id) => document.getElementById(id);
function elem(tag, props, ...children) {
  const e = Object.assign(document.createElement(tag), props);
  e.append(...children);
  return e;
}
// A select of [value, label] pairs (values compared as strings), set to value when given; an entry can also be a ready element
// (an optgroup). missing: a value not in the list still shows, red, as `${value} ${missing}` (or missing(value)), so a dangling
// reference is visible instead of silently reset.
function selectOf(pairs, value, props, missing) {
  const s = elem('select', props, ...pairs.map((p) => (p instanceof Node ? p : new Option(p[1], String(p[0])))));
  if (value != null && missing && ![...s.options].some((o) => o.value === String(value))) {
    s.append(Object.assign(new Option(typeof missing === 'function' ? missing(value) : `${value} ${missing}`, String(value)), { className: 'bad' }));
  }
  if (value != null) s.value = String(value);
  return s;
}
// A DataView's bytes, without a copy.
const u8 = (dv) => new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength);
const dvOf = (bytes) => new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
// Canvas drawing cannot use the CSS tokens: white (dark theme) or black (light theme) at that opacity.
const ink = (a) => `rgba(${getComputedStyle(document.documentElement).colorScheme === 'light' ? '0,0,0' : '255,255,255'},${a})`;
// Per-viewer UI memory (panel sizes and positions): storage can be blocked or hold junk, so both fall back quietly; null removes.
function stored(key, fallback) {
  try {
    return JSON.parse(localStorage.getItem(key)) ?? fallback;
  } catch {
    return fallback;
  }
}
function store(key, value) {
  try {
    if (value == null) localStorage.removeItem(key);
    else localStorage.setItem(key, JSON.stringify(value));
  } catch {}
}

// HOB vertices are s16 model units; levels (DAT positions, terrain) use world units. Item scale applies on top.
const HOB_TO_WORLD = 1 / 256;
const MODEL_VIEW_SCALE = 1 / 1000;
// NPC subtypes that stand still (turrets, radar dish, shield and power generators, power-ups): part of the scenery in cutscenes too.
const STATIC_NPCS = new Set([0x0D, 0x1E, 0x28, 0x2A, 0x32, 0x41, 0x42, 0x4A, 0x4C]);
// Subtypes whose handler (setupNpcUpdateFunctions table) sets path-actor flag 4, terrain height: AT-AT, AT-ST, tank droid, ground
// transports, World Devastator, troops, 0x46 (tickCraftWithPhasedDestruction) and AT-PT; the ground transports also sink.
// shortcut: water craft (0x3E, 0x40) set flag 4 too but keep their placed height: the game's height grid there is presumably the water
// surface, while the level mesh's is the bed (lv_f skimmers sit at 7, the mesh at 8.2).
const GROUND_NPCS = new Set([0x0F, 0x1F, 0x25, 0x2B, 0x2D, 0x2E, 0x37, 0x3B, 0x3C, 0x3D, 0x46, 0x47, 0x50]), GROUND_SINK = new Set([0x2B, 0x2D, 0x2E]);
const SUBTYPE_MODEL = {
  0x0C: 'imp_stuff/tiefighter', 0x0D: 'imp_stuff/gun_turret', 0x0E: 'imp_stuff/prbdroid', 0x0F: 'imp_stuff/atat',
  0x12: 'imp_stuff/tieinter', 0x13: 'imp_stuff/tiebomber', 0x1E: 'imp_stuff/mis_turret', 0x1F: 'imp_stuff/atst',
  0x24: 'imp_stuff/shuttle', 0x25: 'imp_stuff/tankdroid', 0x28: 'imp_stuff/megaturret', 0x29: 'imp_stuff/tie_d',
  0x2A: 'gnrc_stuff/radardish', 0x2B: 'reb_stuff/r_transp', 0x2C: 'imp_stuff/hvyshut', 0x2D: 'imp_stuff/cargo01',
  0x2E: 'reb_stuff/r_combat', 0x32: 'gnrc_stuff/shield', 0x37: 'imp_stuff/wrld_dvstr', 0x3B: 'imp_stuff/trooper',
  0x3D: 'gnrc_stuff/civilian', 0x3E: 'imp_stuff/assboat', 0x40: 'imp_stuff/wskimmer', 0x41: 'reb_stuff/r_turret',
  0x42: 'imp_stuff/tal_turret', 0x43: 'gnrc_stuff/cloudcar', 0x47: 'reb_stuff/atpt_i', 0x48: 'imp_stuff/bosshut',
  0x4A: 'imp_stuff/pwrgen', 0x4C: 'gnrc_stuff/powerup', 0x4D: 'imp_stuff/raptor', 0x3C: 'reb_stuff/r_pilot',
  // Trains follow splines like NPCs; '#name' picks the object inside a multi-object HOB.
  0x2F: 'imp_stuff/train#tr_engine', 0x30: 'imp_stuff/train#tr_defense', 0x34: 'reb_stuff/r_train#r_tr_engine', 0x35: 'reb_stuff/r_train#r_tr_cargo',
};
// DAT item types; subtypes from rogue_squadron64/docs/dat_files/dat_file.md (several are educated guesses there).
const TYPES = {
  player: { color: 0xe8b84a, label: 'Player start / restarts', icon: 'tri', tip: 'Where the player begins (0x01) and respawns after dying (0x02); the cone shows the facing' },
  wing: { color: 0xe6edf7, label: 'Wingmen', icon: 'wing', tip: 'Wingman start positions (0x27), e.g. WEDGE, ROGUE_10' },
  0: { color: 0xc9b38a, label: 'Other units / objects', icon: 'tri', tip: 'Placed units or objects the viewer has no model for (trains, pilots, landing pads, unnamed items); the cone shows the facing' },
  cam: { color: 0xff79c6, label: 'Camera positions', icon: 'cam', tip: 'Cutscene and intro camera placements (subtype 0x26); the pyramid opens toward where it looks' },
  // DAT type 2 is a generic point list; the subtype says what each point is.
  2: { color: 0xff6a3d, label: 'Lava vents', icon: 'circle', tip: 'Lava vent (point subtype 0x51, LavaFart* in Raid on Sullust): erupts lava' },
  wdgun: { color: 0xb0b8c8, label: 'Devastator gun settings', icon: 'square', tip: 'Subtype 0x49 (lv_f): settings records for the World Devastator gun turrets, all at one dummy point; the guns themselves are parts of the wrld_dvstr model' },
  point: { color: 0x9ad0ff, label: 'Menu points', icon: 'circle', tip: 'Frontend point (subtype 0x09): hangar camera starts (camstart_*) and bridge scene points (steps*)' },
  3: { color: 0x4ac0e8, label: 'Unassigned paths', icon: 'cross', tip: 'Splines no NPC names (a level script may switch an NPC onto them); the cross marks the start' },
  6: { color: 0x9aa3b5, label: 'Other zones', icon: 'zother', tip: 'Zone of an unidentified kind (subtype 0x39)' },
  zhi: { color: 0x8a7cff, label: 'High-detail regions', icon: 'zhi', tip: 'LOD zone (0x04): objects use their high-detail models while the player is over this area' },
  zlo: { color: 0x5b6bd8, label: 'Low-detail regions', icon: 'zlo', tip: 'LOD zone (0x05): objects use their low-detail models while the player is over this area' },
  zmus: { color: 0xf0a24a, label: 'Music regions', icon: 'zmus', tip: 'Music zone (0x10): the music changes while the player is over this area' },
  zmtrig: { color: 0xffd36b, label: 'Music trigger areas', icon: 'zmtrig', tip: 'Music trigger area (0x39): the linked music zone switches to its track while the player is inside; it can be gated on mission objectives' },
  zunit: { color: 0xff5f8f, label: 'Ground-unit areas', icon: 'zunit', tip: 'Ground-unit area (0x16): troopers and civilians spawn and roam inside this area' },
  tvoice: { color: 0x6be0d0, label: 'Voice-line triggers', icon: 'tvoice', tip: 'Event trigger (0x18) named for a voice line (LINE_n / say): entering the area sets objective flags that make the level script play the line' },
  tevent: { color: 0x5fd17a, label: 'Objective triggers', icon: 'tevent', tip: 'Event trigger (0x18): entering or leaving the area adjusts objective flags and counters that the level script reacts to' },
  tspawn: { color: 0xc3e65a, label: 'Spawn triggers', icon: 'tspawn', tip: 'Trigger (0x19, usually Gen_*): like an event trigger, and also flags the unit it names so it spawns' },
  troam: { color: 0xd17aff, label: 'NPC roam areas', icon: 'troam', tip: 'Roam area (0x11 For_*, 0x17 *_SPHERE): an NPC names it as its box; it wanders inside the radius and turns back toward the centre when it strays out' },
  // Path events (fired when an NPC on a spline passes the node; func_800F3CB0), one kind per subtype.
  pspeed: { color: 0x7ee0ff, label: 'Speed changes', icon: 'bead', tip: 'Path event 0x1B: eases the NPC to a new speed along its spline' },
  phold: { color: 0xffb347, label: 'Hold orders', icon: 'bead', tip: 'Path event 0x22: the NPC stops at this node until a timer runs out and an objective flag/counter condition is met (func_800F9AB8)' },
  pattack: { color: 0xff3b30, label: 'Attack orders', icon: 'bead', tip: 'Path event 0x20: sets the NPC attack target (red dashed arrow)' },
  paction: { color: 0xff9ad5, label: 'Craft actions', icon: 'bead', tip: 'Path event 0x1A: a per-craft action; TIE bombers start a bombing run, shuttles play their wing/landing animation, wingmen trigger an effect' },
  pfly: { color: 0xc792ff, label: 'Fly-to orders', icon: 'bead', tip: 'Path event 0x4E: sends the NPC off its spline toward a named item (or the player); TIE fighters and the boss shuttle only' },
  pweave: { color: 0x9be37a, label: 'Weave changes', icon: 'bead', tip: 'Path event 0x1C: changes how much the craft sways side to side along its spline' },
  ppitch: { color: 0xf5e663, label: 'Landing pitch', icon: 'bead', tip: 'Path event 0x23: tilts the craft up to 90 degrees over a few seconds (landing) or back level (take-off)' },
  twaypoint: { color: 0xffe14a, label: 'Objective arrow targets', icon: 'twaypoint', tip: 'HUD arrow target (0x3A): the objective arrow points at the nearest one whose objective flag/counter conditions hold' },
  7: { color: 0x3f9a58, label: 'Other triggers', icon: 'diamond', tip: 'Trigger records no game code was found to read (none left among the subtypes in the levels). Lines point at what they name' },
};
const SUBTYPES = {
  0x01: 'Player start', 0x02: 'Restart point', 0x03: 'Spline', 0x04: 'LOD region (high detail)', 0x05: 'LOD region (low detail)',
  0x06: 'Hangar placement', 0x07: 'Scene placement', 0x08: 'Scene spline', 0x09: 'Menu point', 0x0C: 'TIE fighter',
  0x0D: 'Gun turret', 0x0E: 'Probe droid', 0x0F: 'AT-AT', 0x10: 'Music region', 0x11: 'Proximity trigger', 0x12: 'TIE interceptor',
  0x13: 'TIE bomber', 0x14: 'Building / prop', 0x16: 'Zone', 0x17: 'Sphere trigger', 0x18: 'Event trigger', 0x19: 'Trigger',
  0x1A: 'Trigger', 0x1B: 'Trigger', 0x1C: 'Trigger', 0x1E: 'Missile turret', 0x1F: 'AT-ST', 0x20: 'Spawn trigger',
  0x21: 'Stationary NPC spawn', 0x22: 'Trigger', 0x23: 'Trigger', 0x24: 'Shuttle', 0x25: 'Tank droid', 0x26: 'Camera position',
  0x27: 'Wingman position', 0x28: 'Mega turret', 0x29: 'TIE (D)', 0x2A: 'Radar dish', 0x2B: 'Rebel transport',
  0x2C: 'Heavy shuttle', 0x2D: 'Cargo', 0x2E: 'Rebel combat', 0x2F: 'Imperial train', 0x30: 'Imperial train',
  0x31: 'Imperial train', 0x32: 'Shield', 0x33: 'Train pillar', 0x34: 'Rebel train', 0x35: 'Rebel train',
  0x36: 'Rebel train pillar', 0x37: 'World Devastator', 0x39: 'Music trigger area', 0x3A: 'Trigger', 0x3B: 'Stormtrooper',
  0x3C: 'Rebel pilot', 0x3D: 'Civilian', 0x3E: 'Assault boat', 0x40: 'Wave skimmer', 0x41: 'Rebel turret',
  0x42: 'Taloraan turret', 0x43: 'Cloud car', 0x44: 'Imperial platform', 0x46: 'Moving NPC spawn', 0x47: 'AT-PT',
  0x48: 'Boss hut', 0x49: 'World Devastator gun', 0x4A: 'Power generator', 0x4B: 'Large building', 0x4C: 'Power-up',
  0x4D: 'Heavy raptor shuttle', 0x4E: 'Trigger', 0x4F: 'Exhaust port', 0x50: 'B soldier', 0x51: 'Lava vent', 0x52: 'Unnamed item',
};
const ICONS = {
  cam: '<path d="M2 4 H9 V10 H2 Z M9 7 L13 4 V10 Z"/>', tri: '<path d="M7 1 L13 12 L1 12 Z"/>', circle: '<circle cx="7" cy="7" r="5"/>', cross: '<path d="M7 1 V13 M1 7 H13 M2.5 2.5 L11.5 11.5"/>',
  square: '<rect x="2" y="2" width="10" height="10"/>', diamond: '<path d="M7 1 L13 7 L7 13 L1 7 Z"/>',
  // Trigger kinds: speech bubble = voice line, flag = objective, circled plus = spawn, down arrow = HUD arrow target, dashed ring = roam area.
  tvoice: '<path d="M1.5 2 H12.5 V9 H6 L3 12 V9 H1.5 Z"/>', tevent: '<path d="M3 13 V1.5 M3 2 H11.5 L9.5 4.75 L11.5 7.5 H3"/>',
  tspawn: '<circle cx="7" cy="7" r="5.5"/><path d="M7 4 V10 M4 7 H10"/>', twaypoint: '<path d="M7 1.5 V12 M3 8 L7 12 L11 8"/>',
  troam: '<circle cx="7" cy="7" r="5.5" stroke-dasharray="2.6 1.8"/><circle cx="7" cy="7" r="1.4" fill="currentColor" stroke="none"/>',
  // Path event: a bead threaded on its spline.
  bead: '<path d="M1 7 H13" stroke-width="1.4"/><circle cx="7" cy="7" r="3.4"/>',
  // Zone kinds: dense grid = high detail, coarse grid = low detail, note = music, figure = ground units, dashed = unknown.
  zhi: '<rect x="1.5" y="1.5" width="11" height="11"/><path d="M5.2 1.5 V12.5 M8.8 1.5 V12.5 M1.5 5.2 H12.5 M1.5 8.8 H12.5" stroke-width="1"/>',
  zlo: '<rect x="1.5" y="1.5" width="11" height="11"/><path d="M7 1.5 V12.5 M1.5 7 H12.5" stroke-width="1"/>',
  zmus: '<path d="M9.5 2 V10.5 M9.5 2 L13 3.5"/><circle cx="6.5" cy="10.5" r="2.5" fill="currentColor"/>',
  zunit: '<circle cx="7" cy="3.8" r="2.2"/><path d="M2.5 13 C2.5 8.2 11.5 8.2 11.5 13"/>',
  wing: '<path d="M7 2 L9 7 L13 9 L9 9 L7 13 L5 9 L1 9 L5 7 Z"/>',
  zmtrig: '<rect x="1.5" y="1.5" width="11" height="11" stroke-width="1.2"/><path d="M8.5 3.5 V9 M8.5 3.5 L11 4.5" stroke-width="1.4"/><circle cx="6.5" cy="9" r="1.8" fill="currentColor" stroke="none"/>',
  zother: '<rect x="2" y="2" width="10" height="10" stroke-dasharray="2.5 2"/>',
};
const icon = (t) => `<svg width="14" height="14" viewBox="0 0 14 14" fill="none" stroke="#${t.color.toString(16).padStart(6, '0')}" color="#${t.color.toString(16).padStart(6, '0')}" stroke-width="2">${ICONS[t.icon]}</svg>`;

// ---------- renderer ----------
const canvas = $('view');
const renderer = new THREE.WebGPURenderer({ canvas, antialias: true });
// Not awaited at top level, so the ROM picker is live immediately; loading waits on it instead.
const ready = renderer.init();
// The N64 lights and blends straight in display space; skipping sRGB conversion keeps translucent glass/light shafts as faint as in-game.
THREE.ColorManagement.enabled = false;
renderer.outputColorSpace = THREE.LinearSRGBColorSpace;

const scene = new THREE.Scene();
const bgColor = new THREE.Color(0x0f1115);
// The view's own background (no ROM yet, models, levels without a sky) is the theme's page colour.
const syncThemeBg = () => bgColor.set(getComputedStyle(document.documentElement).getPropertyValue('--bg').trim() || '#0f1115');
syncThemeBg();
matchMedia('(prefers-color-scheme: light)').addEventListener('change', syncThemeBg);
// Effects get their own background (the Background picker), so dark debris can be seen against a light one.
const fxBg = new THREE.Color(0x0f1115);
scene.background = bgColor;
const hemi = new THREE.HemisphereLight(0xffffff, 0x404040, 2.2);
scene.add(hemi);
const sun = new THREE.DirectionalLight(0xffffff, 1.4);
sun.position.set(1, 2, 1.5);
scene.add(sun, sun.target);
// Left on permanently: toggling castShadow at runtime crashes three r170's shadow node, so shadows are switched per mesh.
sun.castShadow = true;
sun.shadow.intensity = 0;
renderer.shadowMap.enabled = true;
// Unfiltered depth test: hard shadow edges (the default PCF filter blurs them).
renderer.shadowMap.type = THREE.BasicShadowMap;
sun.shadow.mapSize.set(4096, 4096);
sun.shadow.bias = -0.0005;

// Sun from the level's HMP: colour 0-255, direction in game (Y-down) space. A few levels (lv_6) store it pointing up; the game's terrain is lit from above, so force that.
function applySun(light, w, h) {
  const c = (v) => new THREE.Color().setRGB(v[0] / 255, v[1] / 255, v[2] / 255, THREE.SRGBColorSpace);
  hemi.color.copy(c(light.shadow));
  hemi.groundColor.copy(c(light.shadow));
  hemi.intensity = 1.6;
  sun.color.copy(c(light.sunColor));
  sun.intensity = 2.4;
  const centre = new THREE.Vector3(w / 2, 0, -h / 2);
  const travel = new THREE.Vector3(light.sunDir[0], -Math.abs(light.sunDir[1]), -light.sunDir[2]).normalize();
  const r = Math.max(w, h) * 0.75;
  sun.target.position.copy(centre);
  sun.position.copy(centre).addScaledVector(travel, -r * 2);
  Object.assign(sun.shadow.camera, { left: -r, right: r, top: r, bottom: -r, near: 1, far: r * 4 });
  sun.shadow.camera.updateProjectionMatrix();
}

const camera = new THREE.PerspectiveCamera(50, 1, 0.01, 5000);
scene.add(camera);
camera.position.set(4, 3, 6);
const controls = new OrbitControls(camera, canvas);
controls.enableDamping = true;
controls.dampingFactor = 0.06;
controls.rotateSpeed = 0.7;
controls.mouseButtons = { LEFT: THREE.MOUSE.ROTATE, MIDDLE: THREE.MOUSE.PAN, RIGHT: THREE.MOUSE.ROTATE };
// The inspect card belongs to the spot it was opened on; any orbit, pan, zoom or fly closes it.
controls.addEventListener('start', () => ($('info').hidden = true));

let root = null;
let stats = {};
// Pixel ratio is re-read every time: fullscreen or a move to another monitor can change it, leaving a stale buffer size.
const resize = () => {
  const r = canvas.parentElement.getBoundingClientRect();
  renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
  renderer.setSize(r.width, r.height, false);
  camera.aspect = r.width / Math.max(1, r.height);
  camera.updateProjectionMatrix();
};
new ResizeObserver(resize).observe(canvas.parentElement);
addEventListener('resize', resize);
resize();

// ---------- camera: fly keys, framing, reset ----------
const keys = new Set();
addEventListener('keydown', (e) => {
  if (e.target.tagName === 'INPUT' || e.target.tagName === 'SELECT') return;
  if (editKey(e)) return;
  // The pose tool keys belong to the gizmo while a cutscene track is picked.
  if (cutEditing && cutTrack && ['KeyW', 'KeyE', 'KeyR', 'KeyX'].includes(e.code)) return;
  keys.add(e.code);
  if (e.code === 'KeyR' && homeView) setView(homeView);
});
addEventListener('keyup', (e) => keys.delete(e.code));
addEventListener('blur', () => keys.clear());
const fwd = new THREE.Vector3(), right = new THREE.Vector3(), move = new THREE.Vector3();
// Unity-style boost: Shift starts at 3x and keeps accelerating while you keep moving (up to 40x); releasing Shift or stopping resets it.
let boost = 1;
function fly(dt) {
  const shift = keys.has('ShiftLeft') || keys.has('ShiftRight');
  if (!keys.size) { boost = 1; return; }
  camera.getWorldDirection(fwd);
  right.crossVectors(fwd, camera.up).normalize();
  move.set(0, 0, 0);
  if (keys.has('KeyW')) move.add(fwd);
  if (keys.has('KeyS')) move.sub(fwd);
  if (keys.has('KeyD')) move.add(right);
  if (keys.has('KeyA')) move.sub(right);
  if (keys.has('KeyE')) move.y += 1;
  if (keys.has('KeyQ')) move.y -= 1;
  if (!move.lengthSq()) { boost = 1; return; }
  boost = shift ? Math.min(40, Math.max(3, boost) * (1 + 1.5 * dt)) : 1;
  const speed = camera.position.distanceTo(controls.target) * boost;
  move.normalize().multiplyScalar(Math.max(speed, 0.5) * dt);
  $('info').hidden = true;
  camera.position.add(move);
  controls.target.add(move);
}

let homeView = null;
const getView = () => ({ pos: camera.position.clone(), target: controls.target.clone(), near: camera.near, far: camera.far });
function setView(v) {
  camera.position.copy(v.pos);
  controls.target.copy(v.target);
  camera.near = v.near;
  camera.far = v.far;
  camera.updateProjectionMatrix();
}
const frame = (obj, dir) => frameBox(new THREE.Box3().setFromObject(obj), dir);
function frameBox(box, dir = new THREE.Vector3(0.6, 0.5, 0.8)) {
  if (box.isEmpty()) return;
  const c = box.getCenter(new THREE.Vector3());
  dir = dir.clone().normalize();
  const r = Math.max(box.getSize(new THREE.Vector3()).length() * 0.5, 1e-3);
  controls.target.copy(c);
  camera.position.copy(c).addScaledVector(dir, r / Math.sin(THREE.MathUtils.degToRad(camera.fov / 2)) * 0.9);
  camera.near = r / 500;
  camera.far = r * 50;
  camera.updateProjectionMatrix();
  homeView = getView();
}

// Levels frame on what is rendered (terrain + placed models, not markers), trimming the 2% outliers on each axis:
// a few items sit far off the map and would otherwise shrink the level to a speck (lv_3).
function frameLevel(world, markers) {
  world.updateMatrixWorld(true);
  const pts = [];
  const v = new THREE.Vector3();
  for (const o of world.children) {
    if (o === markers || o === sky) continue;
    if (o.name === 'terrain') {
      const b = new THREE.Box3().setFromObject(o);
      pts.push(b.min.clone(), b.max.clone(), b.getCenter(new THREE.Vector3()));
    } else {
      pts.push(o.getWorldPosition(v).clone());
    }
  }
  if (!pts.length) return frame(world);
  const q = (axis, f) => pts.map((p) => p[axis]).sort((a, b) => a - b)[Math.min(pts.length - 1, Math.floor(f * (pts.length - 1)))];
  const box = new THREE.Box3(new THREE.Vector3(q('x', 0.02), q('y', 0.02), q('z', 0.02)), new THREE.Vector3(q('x', 0.98), q('y', 0.98), q('z', 0.98)));
  // Small scenes (frontend bridge/starwars) place every model at one point, which gives an empty box; frame their geometry instead.
  if (pts.length < 20 || box.getSize(v).length() < 1) {
    box.makeEmpty();
    for (const o of world.children) if (o !== markers && o !== sky) box.expandByObject(o);
  }
  frameBox(box);
}

const clock = new THREE.Clock();
let spin = 0;
let sky = null, skyColor = null;
const skyPos = new THREE.Vector3();
ready.then(() => renderer.setAnimationLoop(() => {
  const dt = Math.min(clock.getDelta(), 0.1);
  fly(dt);
  applyAnim(dt);
  applyExhaust(dt);
  applyEffect(dt);
  // About 0.1 rad/s per slider step, roughly OrbitControls' old autoRotate pace.
  if (spin && !selectedLevel && root?.name === 'spin') root.rotation.y += spin * 0.1 * dt;
  controls.update();
  applyCutscene(dt);
  applyMedalEnv(cut ? cut.t * cut.data.length : clock.elapsedTime);
  sizeCorners();
  updateCutSfxPositions();
  updateFog();
  // The dome rim sits 38 units above its origin (x2 scale); drop it so the rim meets the horizon.
  if (sky && sky.parent) sky.position.copy(sky.parent.worldToLocal(skyPos.copy(camera.position))).y += 76;
  renderer.render(scene, camera);
}));

// In Objects / Events mode the box describes the hovered item, else the selection; otherwise the view's stats.
function hud() {
  const lines = [], o = (editMode === 'objects' || editMode === 'events') && (editHovered ?? sel?.o);
  for (const [k, v] of o ? editHudRows(o) : Object.entries(stats)) lines.push(`${k.padEnd(10)} ${typeof v === 'number' ? v.toLocaleString() : v}`);
  $('hud').textContent = lines.join('\n');
}

// ---------- materials ----------
const lodLevel = () => +$('optLod').value;
function setFilter(map) {
  const mode = $('optFilter').value;
  const mips = mode === 'trilinear' || mode === 'aniso';
  map.magFilter = mode === 'nearest' ? THREE.NearestFilter : THREE.LinearFilter;
  map.minFilter = mips ? THREE.LinearMipmapLinearFilter : map.magFilter;
  map.generateMipmaps = mips;
  map.anisotropy = mode === 'aniso' ? 16 : 1;
  map.needsUpdate = true;
}
// Textures, materials and part geometry are cached; a filter change clears them all and rebuilds the view.
const texCache = new Map(), matPool = new Map(), partCache = new Map(), artCache = new Map();
function clearCaches() {
  for (const t of texCache.values()) t.map?.dispose();
  for (const m of matPool.values()) m.dispose();
  for (const p of partCache.values()) p.geometry.dispose();
  for (const t of terrainCache.values()) { t.g.dispose(); t.map.dispose(); }
  for (const a of artCache.values()) a.then((frames) => frames.forEach((f) => { f.map?.dispose(); f.modelMap?.dispose(); }));
  terrainCache.clear();
  artCache.clear();
  texCache.clear();
  matPool.clear();
  partCache.clear();
}

function materialFor(model, matIndex, key) {
  const { hmt, clamp } = model;
  const m = hmt.materials[matIndex];
  const t = m && (m.type & 1) ? hmt.textures[m.tex] : null;
  const ck = key + ':' + matIndex;
  if (!texCache.has(ck)) {
    let map = null, cutout = false, blend = false;
    if (t && t.rgba) {
      map = new THREE.DataTexture(t.rgba, t.w, t.h, THREE.RGBAFormat);
      // Textures whose UVs never leave 0..1 are clamped, so tiled skies and panels don't bleed the opposite edge in at seams.
      map.wrapS = map.wrapT = clamp.has(matIndex) ? THREE.ClampToEdgeWrapping : THREE.RepeatWrapping;
      setFilter(map);
      // Soft alpha (glows, shockwaves) blends; on/off alpha (colour-keyed rails, walkways) is a cutout.
      let soft = 0;
      for (let i = 3; i < t.rgba.length; i += 4) {
        if (t.rgba[i] < 128) cutout = true;
        if (t.rgba[i] > 0 && t.rgba[i] < 255) soft++;
      }
      // Emissive alpha-format textures always blend, or opaque tiles of a set (centre of 1ade_moon) draw differently from their neighbours.
      blend = soft > t.rgba.length / 160 || (!!m.emissive && !!(t.flags & 0x800));
    }
    texCache.set(ck, { map, cutout, blend, emissive: !!m?.emissive, index: matIndex, matName: m?.name ?? '', tex: t && { name: t.name, w: t.w, h: t.h, type: t.type, flags: t.flags, trans: t.trans } });
  }
  return texCache.get(ck);
}

// "game": models are prelit in the ROM and the game never lights them, so they are drawn unlit. "dynamic": lit by the level sun with shadows.
const gameLighting = () => $('optLight').value === 'game';
function threeMaterial(info, textured, wire) {
  const game = gameLighting();
  const k = `${info.map ? info.map.id : 0}|${textured}|${wire}|${!!info.valpha}|${game}`;
  if (!matPool.has(k)) {
    const blend = (textured && info.blend) || !!info.valpha;
    // Glass (emissive, opaque texture made translucent by vertex alpha, e.g. sky_lv_01 on canopies) is a sphere-mapped reflection:
    // the models carry per-face normals for it, and the texture slides across the glass as the view turns.
    const env = textured && info.emissive && info.valpha && !info.blend;
    const Mat = env ? THREE.MeshMatcapNodeMaterial : game ? THREE.MeshBasicNodeMaterial : THREE.MeshLambertNodeMaterial;
    const mat = new Mat({
      ...(env ? { matcap: info.map } : { map: textured ? info.map : null }), vertexColors: true, side: THREE.DoubleSide,
      alphaTest: textured && info.cutout && !blend ? 0.5 : 0, transparent: blend, depthWrite: !blend, ...(env ? {} : { wireframe: wire }),
      // Glow effects add light: emissive soft-alpha textures (engine glows) and untextured translucent strips (exhaust, light shafts). Glass is a plain alpha blend.
      blending: blend && ((info.emissive && info.blend) || !info.map) ? THREE.AdditiveBlending : THREE.NormalBlending,
    });
    // three r170's node materials read vertex colour as RGB only, and the colour attribute's w reads back as 1, so the alpha has its own attribute.
    // Glass stores ~60% alpha but the game draws it nearly see-through; 25% is tuned by eye, not taken from the game's blend setup.
    if (env) mat.opacity = 0.25;
    else if (info.valpha) mat.opacityNode = new THREE.AttributeNode('valpha', 'float');
    matPool.set(k, mat);
  }
  return matPool.get(k);
}

// One geometry per part (shared by every placement of it) with a group per material.
function partGeometry(part, model, key) {
  const ck = key + ':' + part.offset;
  if (partCache.has(ck)) return partCache.get(ck);
  const byMat = new Map();
  for (const f of part.faces) {
    if (!byMat.has(f.mat)) byMat.set(f.mat, []);
    byMat.get(f.mat).push(f);
  }
  // vidx: the part vertex behind each buffer position, so effects that move part vertices (engine flicker) can rewrite them.
  const pos = [], uv = [], col = [], groups = [], vidx = [], nidx = [];
  let tris = 0;
  for (const [mat, faces] of byMat) {
    const start = pos.length / 3;
    let valpha = false;
    // Faces without vertex colours are not prelit; the game lights them from the material, whose +4 value is the grey level (tow cable 0.2).
    const d = model.hmt.materials[mat]?.diffuse, g = d > 0 ? Math.round(Math.min(d, 1) * 255) : 255;
    const unlit = ((g << 24) | (g << 16) | (g << 8) | 255) >>> 0;
    for (const f of faces) {
      const order = f.idx.length === 4 ? [0, 1, 2, 0, 2, 3] : [0, 1, 2];
      // Vertex alpha is opacity (canopy glass, exhaust, light shafts). On ordinary materials all-zero alpha means "unset" (~3.5k faces);
      // on emissive ones (exhaust cones) zero really is see-through.
      // The medals store alpha 0xE0 but draw opaque (render mode 0x00552230; translucency needs mesh flags 0x20000|0x40000).
      const alphaSet = f.colors && !MEDAL_ENV[key] && (f.colors.some((c) => c & 255) || !!model.hmt.materials[mat]?.emissive);
      for (const k of order) {
        const vi = f.idx[k];
        pos.push(part.verts[vi * 3], part.verts[vi * 3 + 1], part.verts[vi * 3 + 2]);
        vidx.push(vi);
        nidx.push(f.flags & 4 && f.nidx ? f.nidx[k] : -1);
        uv.push(f.uvs ? f.uvs[k][0] : 0, f.uvs ? f.uvs[k][1] : 0);
        const c = f.colors ? (f.colors[k] ?? f.colors[0]) : unlit;
        const a = alphaSet ? (c & 255) / 255 : 1;
        if (a < 1) valpha = true;
        col.push((c >>> 24) / 255, ((c >>> 16) & 255) / 255, ((c >>> 8) & 255) / 255, a);
      }
      tris += order.length / 3;
    }
    groups.push({ start, count: pos.length / 3 - start, mat, valpha });
  }
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute('position', new THREE.Float32BufferAttribute(pos, 3));
  geometry.setAttribute('uv', new THREE.Float32BufferAttribute(uv, 2));
  geometry.setAttribute('color', new THREE.Float32BufferAttribute(col, 4));
  geometry.setAttribute('valpha', new THREE.Float32BufferAttribute(col.filter((_, i) => i % 4 === 3), 1));
  geometry.computeVertexNormals();
  const infos = groups.map((gr, i) => {
    geometry.addGroup(gr.start, gr.count, i);
    return { ...materialFor(model, gr.mat, key), valpha: gr.valpha };
  });
  const entry = { geometry, infos, tris, verts: part.verts.length / 3, vidx, nidx };
  partCache.set(ck, entry);
  return entry;
}

// HOB node matrices are 4x3 row-vector transforms (v' = v * M), relative to the parent node.
// Level item rotation, as the game builds it (func_8001CB9C/CCCC/CDFC + func_800191C4): rotX, then rotY, then rotZ (degrees), then scale.
function placementRotation(rot) {
  // rotX, rotY, rotZ applied in that order with the game's signs: the hangar ship slots (stored as 180/a/180 or 0/a/0) then all face the
  // hangar centre; the old negated angles turned each one by 2a.
  return new THREE.Matrix4().makeRotationFromEuler(new THREE.Euler(...rot.map((d) => THREE.MathUtils.degToRad(d)), 'ZYX'));
}

// Node pose from the HOB rest values, or from an animation's rotation/translation when given (translation in vertex units).
const _one = new THREE.Vector3(1, 1, 1), _piv = new THREE.Matrix4();
const _zAxis = new THREE.Vector3(0, 0, 1);
function poseNode(g, n, q = null, t = null) {
  const m = n.matrix;
  g.matrix.set(m[0], m[3], m[6], m[9], m[1], m[4], m[7], m[10], m[2], m[5], m[8], m[11], 0, 0, 0, 1);
  if (!q) {
    const [qw, qx, qy, qz] = n.quat;
    q = n.flags & 4 ? new THREE.Quaternion(qx, qy, qz, qw).normalize() : new THREE.Quaternion();
  }
  if (!t) t = n.flags & 8 ? new THREE.Vector3(...n.pos).multiplyScalar(256) : new THREE.Vector3();
  // Flag 2 = node scale at +0x5C (only the AT-ST root uses it: 0.0296).
  g.matrix.premultiply(new THREE.Matrix4().compose(t, q, n.flags & 2 ? new THREE.Vector3(...n.scale) : _one));
  // Flag 1 joints (AT-AT/AT-PT legs) keep vertices in whole-model space and bend about a pivot: T(pivot) R T(-pivot).
  if (n.flags & 1) {
    const p = new THREE.Vector3(...n.pivot).multiplyScalar(256);
    g.matrix.multiply(_piv.makeTranslation(-p.x, -p.y, -p.z)).premultiply(_piv.makeTranslation(p.x, p.y, p.z));
  }
}

function buildObject(obj, model, key, counts) {
  const group = new THREE.Group();
  group.name = obj.name;
  group.userData.hobObject = true;
  const lod = obj.lods[Math.min(lodLevel(), obj.lods.length - 1)] || [];
  counts.lods = Math.max(counts.lods || 0, obj.lods.length);
  const nodes = obj.nodes.map((n, i) => {
    const g = new THREE.Group();
    g.matrixAutoUpdate = false;
    poseNode(g, n);
    for (const p of lod[i] || []) {
      const part = partGeometry(p, model, key);
      const mesh = new THREE.Mesh(part.geometry, part.infos.map((info) => threeMaterial(info, $('optTex').checked, $('optWire').checked)));
      mesh.userData.infos = part.infos;
      mesh.userData.shared = true;
      mesh.userData.hob = { part: p, vidx: part.vidx };
      mesh.name = p.name;
      const envK = MEDAL_ENV[key]?.[i];
      if (envK && p.normals) {
        mesh.userData.env = { k: envK, ending: key.endsWith('end_medal'), nidx: part.nidx };
        envMeshes.add(mesh);
        // The file UVs stay in 0..1 (so the map was set to clamp) but the env map's reach k * 2.
        for (const info of part.infos) if (info.map && info.map.wrapS !== THREE.RepeatWrapping) {
          info.map.wrapS = info.map.wrapT = THREE.RepeatWrapping;
          info.map.needsUpdate = true;
        }
      }
      counts.tris += part.tris;
      counts.verts += part.verts;
      counts.parts++;
      g.add(mesh);
    }
    return g;
  });
  obj.nodes.forEach((n, i) => (n.parent >= 0 ? nodes[n.parent] : group).add(nodes[i]));
  group.userData.rig = { nodes, src: obj.nodes };
  return group;
}

// Medals are env-mapped on the CPU (end_medal 0x800ADBF8, menu medal 0x800C2574): each frame every textured corner gets
// R = reflect(normalize(P - E), N) in object space and u = k(1 - R.x), v = k(1 - R.z). k per node (menu medal layer_1/2/0 = nodes 1/2/3).
const MEDAL_ENV = { 'data/frontend/end_medal/end_medal': [0.9], 'data/frontend/medal/medal': [0, 2.5, 1, 0.5] };
// Tier colours: the ceremony's vertex tint table 0x800AF6C0 (the menu medal's directional light presets / 1.7 give the same hues).
const MEDAL_TINT = { bronze: [175, 105, 73], silver: [210, 210, 210], gold: [255, 178, 0] };
const MEDAL_TIERS = ['none', 'bronze', 'silver', 'gold'];
// processCutsceneActions' craft names (0x800A517C on), by gGameSettings+1.
const CUT_CRAFTS = ['pl_crafts/xwing', 'pl_crafts/ywing', 'pl_crafts/awing', 'pl_crafts/vwing', 'pl_crafts/snowspeeder', 'pl_crafts/falcon', 'tie_inter', 'pl_crafts/t16', 'pl_crafts/koelsch'];
const envMeshes = new Set();
const _em = new THREE.Matrix4(), _enm4 = new THREE.Matrix4(), _enm = new THREE.Matrix3(), _eP = new THREE.Vector3(), _eN = new THREE.Vector3(), _eE = new THREE.Vector3();
function applyMedalEnv(time) {
  for (const mesh of envMeshes) {
    let root = mesh;
    while (root.parent && !root.userData.hobObject) root = root.parent;
    let top = root;
    while (top.parent) top = top.parent;
    if (top !== scene) {
      if (!mesh.parent) envMeshes.delete(mesh);
      continue;
    }
    const { k, ending, nidx } = mesh.userData.env, part = mesh.userData.hob.part, vidx = mesh.userData.hob.vidx;
    // shortcut: the game's angle counters are taken as 30 per second (end_medal multiplies 0x800B0B2C by 30); the menu medal's E is in its model units.
    const d = THREE.MathUtils.degToRad(time * 30);
    if (ending) _eE.set(50 * Math.cos(1.1 * d), 50 * Math.sin(1.2 * d), 10 * Math.sin(1.3 * d));
    else _eE.set(0.9 * Math.cos(1.1 * d), 0.8 * Math.sin(1.3 * d), 6 + 0.5 * Math.sin(d));
    // In the ceremony M is the medal's cutscene world matrix (*(0x800B1900) + 0x21DC), in world units (the mesh's 1/256 included);
    // elsewhere object space, the node chain below the HOB object (vertex units, so the game's /256 applies).
    let space = root;
    if (ending && cut) while (space.parent && space.parent !== scene) space = space.parent;
    _em.copy(space.matrixWorld).invert().multiply(mesh.matrixWorld);
    if (space !== root) _em.premultiply(_enm4.makeScale(256, 256, 256));
    _enm.getNormalMatrix(_em);
    const uv = mesh.geometry.attributes.uv, n = part.normals, v = part.verts;
    for (let j = 0; j < nidx.length; j++) {
      const ni = nidx[j];
      if (ni < 0) continue;
      _eN.set(n[ni * 3], n[ni * 3 + 1], n[ni * 3 + 2]).applyMatrix3(_enm).normalize();
      const vi = vidx[j];
      _eP.set(v[vi * 3], v[vi * 3 + 1], v[vi * 3 + 2]).applyMatrix4(_em).multiplyScalar(1 / 256).sub(_eE).normalize();
      _eP.addScaledVector(_eN, -2 * _eN.dot(_eP));
      uv.setXY(j, k * (1 - _eP.x), k * (1 - _eP.z));
    }
    uv.needsUpdate = true;
    const tint = MEDAL_TINT[$('optMedal').value] ?? [255, 255, 255];
    // In the ceremony the tint's alpha (tickCutsceneCameraFromTimeline) fades the medal in over frames 170-200 and out over 390-420.
    const f = cut ? cut.t * cut.data.frames : 300, alpha = ending ? THREE.MathUtils.clamp(Math.min((f - 170) / 30, (420 - f) / 30), 0, 1) : 1;
    for (const m of [mesh.material].flat()) {
      if (m.map || m.matcap) m.color.setRGB(tint[0] / 255, tint[1] / 255, tint[2] / 255);
      if (m.transparent !== alpha < 1) {
        m.transparent = alpha < 1;
        m.needsUpdate = true;
      }
      m.opacity = alpha;
    }
  }
  $('medalRow').hidden = !cut?.ceremony;
}

// ---------- skeletal animation ----------
let anim = null, animClock = 0, spriteRestore = null;
// Sprite-animated NPCs swap their texture per frame. 12 fps is the rate the spawn code (0x80065260) passes for the 10-frame sets.
const SPRITE_FPS = 12;
const SPRITE_ANIMS = { 'gnrc_stuff/civilian': ['civ_anm_run'], 'imp_stuff/trooper': ['trp_anm_run', 'trp_anm_sht', 'blk_anm_run'], 'reb_stuff/r_pilot': ['r_pilot_anm'] };
const _qa = new THREE.Quaternion(), _qb = new THREE.Quaternion();
function sampleTrack(times, phase) {
  let k = 0;
  while (k < times.length - 1 && times[k + 1] <= phase) k++;
  const k2 = Math.min(k + 1, times.length - 1);
  const span = times[k2] - times[k];
  return [k, k2, span > 0 ? (phase - times[k]) / span : 0];
}
// X-wing S-foils (playerXwingInit/playerXwingUpdate): boost progress t in 0..1 sets the hinge roll about the long axis,
// wngrot_a = -(1 - t) * 10 deg, wngrot_b = +(1 - t) * 10 deg, written into the node's base matrix (func_800AA424, degrees).
// Imperial shuttle (npcShuttleUpdate): progress p in 0..1 eases the wings from folded (rest pose) to 110 * (1 - cos(p * pi)) / 2 deg
// about each hinge pivot, wngrot_a +deg and wngrot_b -deg (state +0x60, rotZ via func_8001CDFC).
let foils = null;
// Code-driven node animations from the player-craft and NPC updates, played as clips: [name, seconds, drive(phase) -> driver values,
// [[node, local axis, game angle (deg) from the driver, optional game-space y offset]]]. Stick-driven poses sweep stick X -1..1 (sin).
// A-wing func_800B5B4C, V-wing func_800B8C30 / Naboo func_800BDC9C, snowspeeder func_800AADD4, npcPowerUpUpdate, npcRadarDishUpdate.
const stickSweep = (p) => ({ x: Math.sin(2 * Math.PI * p) });
const PROC_ANIMS = [
  [/pl_crafts\/awing$/, [['bank_sweep', 2, stickSweep, [['f_trns_l', 'y', (d) => d.x * -10.125], ['f_trns_r', 'y', (d) => d.x * -10.125], ['rot_a', 'y', (d) => d.x * -15.75], ['rot_b', 'y', (d) => d.x * -15.75]]]]],
  [/pl_crafts\/(vwing|koelsch|rmobil)$/, [['bank_sweep', 2, stickSweep, [['f_trns_l', 'x', (d) => 45 * d.x], ['f_trns_r', 'x', (d) => -45 * d.x]]]]],
  // botrns: a hold timer 0..0.5 s -> -180 * t deg; uptrns: the turned-toward side eases up to 45 deg.
  [/pl_crafts\/snowspeeder$/, [
    ['brake_left', 1, (p) => ({ t: 0.5 * (1 - Math.abs(2 * p - 1)) }), [['botrns_l', 'x', (d) => -180 * d.t]]],
    ['brake_right', 1, (p) => ({ t: 0.5 * (1 - Math.abs(2 * p - 1)) }), [['botrns_r', 'x', (d) => -180 * d.t]]],
    // uptrns are plain nodes with a rest orientation (flags 12): the generic sign flip turned them the wrong way, so their angle is pre-negated.
    ['bank_sweep', 2, stickSweep, [['uptrns_l', 'x', (d) => (d.x < 0 ? -45 * (1 - Math.cos((-d.x * Math.PI) / 2)) : 0)], ['uptrns_r', 'x', (d) => (d.x > 0 ? -45 * (1 - Math.cos((d.x * Math.PI) / 2)) : 0)]]],
  ]],
  [/gnrc_stuff\/powerup$/, [['spin', 4, (p) => ({ a: 360 * p, b: Math.sin(8 * Math.PI * p) }), [['rot_a', 'y', (d) => d.a, (d) => 0.5 * d.b]]]]],
  // Radar dish at speed scalar 1 (30 deg/s); its target aiming is not a clip.
  [/gnrc_stuff\/radardish$/, [['spin', 12, (p) => ({ a: 360 * p }), [['dish', 'y', (d) => d.a]]]]],
];
const _axes = { x: new THREE.Vector3(1, 0, 0), y: new THREE.Vector3(0, 1, 0), z: _zAxis };
// The game rebuilds the node matrix with the angle added in node space. Like the S-foils, a plain node takes it on its matrix with the
// sign flipped (row- vs column-vector rotation); a pivot (flag 1) node takes it inside the pivot with the game's sign (as the shuttle).
function poseProc(g, n, axis, deg, dy = 0) {
  const r = new THREE.Quaternion().setFromAxisAngle(_axes[axis], THREE.MathUtils.degToRad(n.flags & 1 ? deg : -deg));
  if (n.flags & 1) {
    const [qw, qx, qy, qz] = n.quat;
    poseNode(g, n, (n.flags & 4 ? new THREE.Quaternion(qx, qy, qz, qw).normalize() : new THREE.Quaternion()).multiply(r));
  } else {
    poseNode(g, n);
    g.matrix.multiply(new THREE.Matrix4().makeRotationFromQuaternion(r));
  }
  if (dy) g.matrix.premultiply(new THREE.Matrix4().makeTranslation(0, dy * 256, 0));
}
// The other shuttle models carry the same wngrot hinge nodes, so they take the traced shuttle rule.
const foilKind = (id) => (/xwing|xwng/.test(id) ? 'xwing' : /imp_stuff\/(shuttle|hvyshut|bosshut)/.test(id) ? 'shuttle' : null);
function setupFoils(id, objGroup) {
  const rig = objGroup?.userData.rig, names = objGroup && modelCache.get(id)?.hob.objects.find((o) => o.name === objGroup.name)?.nodeNames;
  const a = names?.get('wngrot_a'), b = names?.get('wngrot_b');
  const kind = foilKind(id);
  foils = kind && rig && a != null && b != null ? { rig, a, b, kind } : null;
  $('foilsRow').hidden = !foils;
  if (foils) $('foilsLabel').textContent = kind === 'shuttle' ? 'Wings lowered' : 'S-foils closed';
  applyFoils();
}
// Engine flicker (func_80053C04 setup, func_80053F7C per frame): each frame one of eng_a/eng_b (alternating) gets its leading vertices'
// z set to rest z - rand() % N, walking from vertex 0 while z stays within 16 of vertex 0's; N = t * 1024 + 1 with
// t = max(0, (speed + 0.5 - lo) / (hi - lo)). Per-craft (lo, hi) from the game's table.
const EXHAUST_RANGE = { awing: [3, 5.625], vwing: [3, 4.5], xwing: [1.875, 3.75], ywing: [2.25, 3.375], snowspeeder: [2.625, 3.5625],
  falcon: [2.25, 5.625], tie_inter: [2.625, 4.5], koelsch: [2.25, 6], t16: [2.25, 6] };
let exhaust = null, exhaustClock = 0, exhaustParity = 0;
function setupExhaust(objGroup) {
  if (exhaust) for (const e of exhaust.engines) writeEngineZ(e, (i) => e.rest[i]);
  const range = objGroup && EXHAUST_RANGE[objGroup.name];
  const engines = [];
  objGroup?.traverse((o) => {
    const h = o.userData.hob;
    if (!h || !/^eng_[ab]$/.test(o.name)) return;
    // Rest z of the leading run of vertices the game rewrites.
    const v = h.part.verts, rest = [v[2]];
    for (let i = 1; i < 16 && i * 3 + 2 < v.length && v[i * 3 + 2] - v[2] < 16; i++) rest.push(v[i * 3 + 2]);
    engines.push({ mesh: o, vidx: h.vidx, rest, name: o.name });
  });
  engines.sort((a, b) => a.name.localeCompare(b.name));
  exhaust = range && engines.length ? { engines, range } : null;
  $('speedRow').hidden = !exhaust;
}
function writeEngineZ(e, zOf) {
  const pos = e.mesh.geometry.attributes.position;
  for (let j = 0; j < e.vidx.length; j++) if (e.vidx[j] < e.rest.length) pos.setZ(j, zOf(e.vidx[j]));
  pos.needsUpdate = true;
}
function applyExhaust(dt) {
  if (!exhaust) return;
  exhaustClock += dt;
  if (exhaustClock < 1 / 30) return;
  exhaustClock = 0;
  const [lo, hi] = exhaust.range, speed = +$('optSpeed').value;
  const n = Math.floor(Math.max(0, (speed + 0.5 - lo) / (hi - lo)) * 1024 + 1);
  const e = exhaust.engines[exhaustParity++ & 1] ?? exhaust.engines[0];
  writeEngineZ(e, (i) => e.rest[i] - Math.floor(Math.random() * n));
}
function applyFoils(v = +$('optFoils').value) {
  if (!foils) return;
  // X-wing: the game writes the angle into the row-vector node matrix and three's column-vector rotation runs the other way.
  // Shuttle: the angle goes inside the pivot, where the game's sign carries over unflipped (wings fold down outward).
  const deg = foils.kind === 'shuttle' ? 55 * (1 - Math.cos(v * Math.PI)) : (1 - v) * 10;
  for (const [i, sign] of [[foils.a, 1], [foils.b, -1]]) {
    const g = foils.rig.nodes[i], q = new THREE.Quaternion().setFromAxisAngle(_zAxis, THREE.MathUtils.degToRad(sign * deg));
    // The shuttle hinges are flag-1 pivot nodes, so the rotation goes inside the pivot; the X-wing's go on the node matrix.
    if (foils.kind === 'shuttle') poseNode(g, foils.rig.src[i], q);
    else {
      poseNode(g, foils.rig.src[i]);
      g.matrix.multiply(new THREE.Matrix4().makeRotationFromQuaternion(q));
    }
  }
}

function poseProcAt(a, phase) {
  const d = a.proc[2](phase);
  for (const [i, axis, angle, dy] of a.nodes) poseProc(a.rig.nodes[i], a.rig.src[i], axis, angle(d), dy?.(d));
}

function applyAnim(dt) {
  if (anim?.sprite) {
    animClock += dt * +$('optAnimSpeed').value;
    const f = anim.sprite[Math.floor(animClock * SPRITE_FPS) % anim.sprite.length];
    if (anim.map.image.data !== f.rgba) {
      anim.map.image.data = f.rgba;
      anim.map.needsUpdate = true;
    }
    return;
  }
  if (anim?.foil) {
    // Drives the wings slider, so the slider shows where the sweep is.
    animClock += dt * +$('optAnimSpeed').value;
    const [from, to] = anim.foil, v = from + (to - from) * ((animClock / anim.seconds) % 1);
    $('optFoils').value = v;
    $('foilsVal').textContent = Math.round(v * 100) + '%';
    applyFoils(v);
    return;
  }
  if (anim?.proc) {
    animClock += dt * +$('optAnimSpeed').value;
    poseProcAt(anim, (animClock / anim.proc[1]) % 1);
    return;
  }
  if (!anim || !anim.rig) return;
  animClock += dt * +$('optAnimSpeed').value;
  poseTracks(anim.rig, anim.data, (animClock / anim.seconds) % 1);
}
// Poses a skeletal animation at a phase (0..1 over the clip): one track per rig node after the root.
function poseTracks(rig, data, phase) {
  data.tracks.forEach((tr, i) => {
    const g = rig.nodes[i + 1], n = rig.src[i + 1];
    if (!tr || !g) return;
    const [a, b, f] = sampleTrack(tr.times, phase);
    let q = null, t = null;
    if (tr.rot) q = new THREE.Quaternion().slerpQuaternions(_qa.set(...tr.rot[a]), _qb.set(...tr.rot[b]), f).normalize();
    if (tr.pos) t = new THREE.Vector3(...tr.pos[a]).lerp(new THREE.Vector3(...tr.pos[b]), f);
    poseNode(g, n, q, t);
  });
}

function applyMaterialOptions() {
  if (!root) return;
  root.traverse((o) => {
    if (o.isMesh && o.userData.infos) o.material = o.userData.infos.map((i) => threeMaterial(i, $('optTex').checked, $('optWire').checked));
    if (o.isMesh && o.userData.terrain) {
      o.material.wireframe = $('optWire').checked;
      o.material.map = $('optTex').checked ? o.userData.terrain : null;
      o.material.needsUpdate = true;
    }
  });
}

// ---------- level item descriptions ----------
let voiceLine = () => undefined;
const fmt = (v) => (Array.isArray(v) ? v.map((n) => n.toFixed(2)).join(', ') : v);
const hex = (v) => '0x' + v.toString(16).toUpperCase();
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
const TEX_TYPES = ['CI4', 'CI8', 'RGB16', 'RGB(A)32', 'Gray 4/8', 'Gray 8'];

// What an item does, in words; trigger targets and voice lines are filled in by showLevel.
function describe(it) {
  if (it.type === 6) {
    let s = (TYPES[markerKind(it)]?.tip ?? 'Zone').replaceAll('; ', ';\n');
    if (/LevelSett|LevelDefa/.test(it.name)) s += ". Sits off the map; likely the level's default music settings";
    if (it.linkedFrom && !it.label) s += '. Used by ' + it.linkedFrom.join(', ');
    if (it.sub === 0x10 && it.linkedTo) s += '. Plays while the player is inside its linked trigger area';
    return s;
  }
  if (it.type === 3) return it.users?.length ? 'Path followed by ' + it.users.join(', ') : it.pathKind ? `Path with no NPC naming it; grouped with ${TYPES[it.pathKind].label.toLowerCase()} by its name` : 'Path that no NPC names (a level script may switch an NPC onto it)';
  if (it.type === 7) {
    const lines = [];
    if (it.radius > 0 && (it.sub === 0x11 || it.sub === 0x17)) {
      // npcProbeDriod / func_800F4E74: the NPC wanders inside the radius (probe droids subtract their own size) and heads back to the centre when outside.
      lines.push(`Roam area${it.targets?.length ? ' for ' + it.targets.join(', ') : ''}: wanders within ${it.radius.toFixed(1)} units of the centre and turns back when it strays out`);
      return lines.join('\n');
    }
    if (it.waypoint) {
      const w = it.waypoint;
      lines.push(`Objective arrow target: the HUD arrow points at the nearest active one${w.follow ? `, following ${w.follow}` : ''}`);
      const on = [w.flag && `flag #${w.flag} is set`, w.counter && `counter #${w.counter} ≥ ${w.atLeast}`].filter(Boolean);
      const off = [w.hideFlag && `flag #${w.hideFlag} is set`, w.hideCounter && `counter #${w.hideCounter} reaches ${w.below}`].filter(Boolean);
      if (on.length) lines.push('Shown once ' + on.join(' and '));
      if (off.length) lines.push('Hidden once ' + off.join(' or '));
      return lines.join('\n');
    }
    if (it.pathEvent) {
      // func_800F3CB0 writes 0x1B/0x1C/0x23 into the path follower; 0x1A/0x22/0x4E/0x20 are channel-9 orders to the NPC's own handler.
      const e = it.pathEvent, sc = (x) => +x.toFixed(2), w = e.w, fl = e.fl;
      lines.push(it.paths?.length ? `Fires when an NPC on ${it.paths.join(', ')} passes it` : 'Fires when an NPC passes this spline node (no spline references it)');
      // 0x22 hold (func_800F9AB8 + func_800F410C): stop while the timer runs, flag w[1] != w[2], or counter w[3] < w[4].
      const hold = () => {
        const until = [w[1] && `flag #${w[1]} = ${w[2]}`, w[3] && `counter #${w[3]} ≥ ${w[4]}`].filter(Boolean);
        const time = fl[0] >= 9000 ? 'Stops here for good' : fl[0] > 0 ? `Stops here for ${sc(fl[0])} s` : 'Stops here';
        return until.length ? `${time}${fl[0] > 0 && fl[0] < 9000 ? ' and' : ''} until ${until.join(' and ')}` : time;
      };
      // 0x1A word selects the craft: 0 = TIE bomber bombing run (count fl[1], interval fl[2]; npcTieVariantUpdate),
      // 1 = shuttle wing/landing animation (direction fl[1], duration fl[2]; npcShuttleUpdate), 2 = wingman effect (npcWingmanUpdate).
      const action = () => ({
        0: `Bombing run: drops ${sc(fl[1])} bomb${fl[1] === 1 ? '' : 's'}${fl[2] > 0 ? ` every ${sc(fl[2])} s` : ''}`,
        1: `Shuttle wing/landing animation ${fl[1] ? 'forward' : 'backward'} over ${sc(fl[2])} s`,
        2: `Wingman effect ${fl[1] ? '0x1F' : '0x20'} (probably engines)`,
      })[w[0]] ?? `Craft action mode ${w[0]}`;
      const what = {
        0x1A: action(),
        0x1B: `Speed ${e.ib ? '×' : '='} ${sc(e.a)}${e.c > 0 ? `, eased over ${sc(e.c)} s` : ''}`,
        0x1C: `Side-to-side weave ${e.ib ? '×' : '='} ${sc(e.a * 0.25)}, eased over ${sc(e.c)} s`,
        0x20: e.target && e.target[0] !== '-' ? `Attacks ${e.target} (shot multiplier ${sc(fl[4])})` : 'Clears its attack target',
        0x22: hold(),
        0x23: e.ia ? `Pitches up to 90° over ${sc(e.b)} s (landing)` : `Levels out over ${sc(e.b)} s (take-off)`,
        0x4E: `Leaves its spline and flies to ${e.target || 'the player'}`,
      }[it.sub];
      if (what) lines.push(what);
      const need = [e.flag && `flag #${e.flag} is set`, e.counter && `counter #${e.counter} ≥ ${e.atLeast}`].filter(Boolean);
      if (need.length) lines.push('Only if ' + need.join(' and '));
      const act = [e.addCounter && `counter #${e.addCounter} ${e.amount >= 0 ? '+' : '−'}= ${Math.abs(e.amount)}`, e.setFlag && `flag #${e.setFlag} = ${e.value}`].filter(Boolean);
      if (act.length) lines.push('Also ' + act.join(', '));
      return lines.join('\n');
    }
    if (it.activators) {
      // func_800AA6D0 / func_80065980: an activator entering/leaving the area adjusts mission flags and counters,
      // which the level's own script (lvN_checkComplexObjectives) reacts to.
      const who = it.activators.map((a) => (a === 'ply' ? 'the player' : a)).join(', ');
      lines.push(`Fires when ${who} ${it.cylinder ? 'flies over' : 'comes within'} ${it.radius.toFixed(1)} units`);
      const r = it.requires;
      const need = r && [r.flag && `flag #${r.flag} is set`, r.counter && `counter #${r.counter} ≥ ${r.atLeast}`].filter(Boolean);
      if (need?.length) lines.push('Only if ' + need.join(' and '));
      // Enter/exit flags are just set / cleared, so only counter changes are worth listing.
      const act = (b) => (b.counter ? `counter #${b.counter} ${b.add >= 0 ? '+' : '−'}= ${Math.abs(b.add)}` : '');
      if (act(it.onEnter)) lines.push('On enter: ' + act(it.onEnter));
      if (act(it.onExit)) lines.push('On exit: ' + act(it.onExit));
    } else if (it.targets?.length) {
      lines.push('Linked to ' + it.targets.join(', '));
    }
    if (it.voice) lines.push(`Name points to voice line ${it.voiceId}: "${it.voice}"`);
    return lines.join('\n') || (SUBTYPES[it.sub] ?? 'Trigger') + ' (no activators or actions decoded for this subtype yet)';
  }
  return SUBTYPES[it.sub] ?? 'subtype ' + hex(it.sub);
}
// The item's category and subtype names; one when the category is just the subtype's plural (Music trigger areas / Music trigger area).
function kindParts(it) {
  const cat = TYPES[markerKind(it)]?.label ?? 'Type ' + it.type, sub = SUBTYPES[it.sub] ?? hex(it.sub);
  return cat.toLowerCase().replace(/s$/, '') === sub.toLowerCase() ? [sub] : [cat, sub];
}
function itemRows(it) {
  return [['What it is', esc(describe(it)).replaceAll('\n', '<br>')], ['Kind', kindParts(it).join(' · ')],
    ['DAT type / subtype', `${it.type} / ${hex(it.sub)}`], ['Index / offset', `${it.index} / ${hex(it.offset)}`],
    it.model && ['Reference', esc(it.model)], [it.recordPos ? 'Position (node)' : 'Position', fmt(it.pos)], it.recordPos && ['Record position', fmt(it.recordPos)], it.rot && ['Rotation°', fmt(it.rot)],
    it.scale && ['Scale', fmt(it.scale)], it.size && ['Footprint', it.size[0].toFixed(1) + ' × ' + it.size[2].toFixed(1)], it.points && ['Spline points', it.points.length]];
}

// ---------- level markers ----------
const markerMat = new Map();
const lineMat = (type, opacity = 1) => {
  const k = type + '|' + opacity + '|' + TYPES[type]?.color;
  if (!markerMat.has(k)) markerMat.set(k, new THREE.LineBasicMaterial({ color: TYPES[type]?.color ?? 0xffffff, transparent: opacity < 1, opacity, fog: false }));
  return markerMat.get(k);
};
const roamMat = new THREE.LineDashedMaterial({ color: 0xd17aff, dashSize: 0.9, gapSize: 0.6, fog: false });
const attackLineMat = new THREE.LineDashedMaterial({ color: 0xff3b30, dashSize: 0.5, gapSize: 0.3, fog: false });
const attackHeadMat = new THREE.MeshBasicMaterial({ color: 0xff3b30, fog: false });
const meshMat = (type) => {
  const k = 'm' + type + '|' + TYPES[type]?.color;
  if (!markerMat.has(k)) markerMat.set(k, new THREE.MeshBasicMaterial({ color: TYPES[type]?.color ?? 0xffffff, wireframe: true, fog: false }));
  return markerMat.get(k);
};
const GEO = {
  cone: new THREE.ConeGeometry(0.3, 0.9, 8).rotateX(Math.PI / 2),
  camera: new THREE.ConeGeometry(0.45, 0.8, 4).rotateY(Math.PI / 4).rotateX(-Math.PI / 2),
  diamond: new THREE.OctahedronGeometry(0.4),
  arrow: new THREE.ConeGeometry(0.2, 0.6, 8),
  bead: new THREE.TorusGeometry(0.35, 0.09, 6, 18),
  cube: new THREE.BoxGeometry(0.55, 0.55, 0.55),
  // Tip at game +y, which is down on screen (game space is Y-down).
  downCone: new THREE.ConeGeometry(0.3, 0.7, 8),
  ball: new THREE.IcosahedronGeometry(0.35, 0),
  cross: new THREE.BufferGeometry().setFromPoints([[-0.6, 0, 0], [0.6, 0, 0], [0, -0.6, 0], [0, 0.6, 0], [0, 0, -0.6], [0, 0, 0.6]].map((p) => new THREE.Vector3(...p))),
};
// 3D marker per trigger kind, matching the legend: ball = voice line, cube = spawn, down cone = HUD arrow target, small ball = roam area.
const TRIGGER_GEO = { tvoice: GEO.ball, tspawn: GEO.cube, twaypoint: GEO.downCone, troam: new THREE.IcosahedronGeometry(0.2, 0) };
// The ring's plane faces along the path at the node, so it reads as a bead threaded on the spline. Rotated per marker in the geometry
// (not the mesh) so the marker's link lines keep their positions.
const beadGeometry = (tangent) => (tangent ? GEO.bead.clone().applyQuaternion(new THREE.Quaternion().setFromUnitVectors(new THREE.Vector3(0, 0, 1), tangent)) : GEO.bead);
function makeMarker(it) {
  const [x, y, z] = it.pos;
  let mk;
  if (it.type === 6) {
    // Zones are 2D footprints from pos to pos + size (X/Z); height is ignored (npcGroundTroopUpdate scatters units over
    // [pos, pos+size] and takes Y from the terrain). Drawn as an outline + see-through fill draped on the ground.
    const kind = markerKind(it), rot = placementRotation(it.rot);
    const sx = it.size[0], sz = it.size[2], n = Math.max(2, Math.ceil(Math.max(Math.abs(sx), Math.abs(sz)) / 0.5));
    const at = (u, v) => {
      const p = new THREE.Vector3(u * sx, 0, v * sz).applyMatrix4(rot);
      return new THREE.Vector3(p.x, levelHeight(x + p.x, z + p.z) - y - 0.15, p.z);
    };
    const ring = [];
    for (const [u0, v0, u1, v1] of [[0, 0, 1, 0], [1, 0, 1, 1], [1, 1, 0, 1], [0, 1, 0, 0]]) {
      for (let i = 0; i < n; i++) ring.push(at(u0 + (u1 - u0) * i / n, v0 + (v1 - v0) * i / n));
    }
    ring.push(ring[0].clone());
    mk = new THREE.Line(new THREE.BufferGeometry().setFromPoints(ring), lineMat(kind));
    const fill = new THREE.PlaneGeometry(1, 1, n, n);
    const fp = fill.attributes.position;
    for (let i = 0; i < fp.count; i++) {
      const q = at(fp.getX(i) + 0.5, fp.getY(i) + 0.5);
      fp.setXYZ(i, q.x, q.y, q.z);
    }
    const area = new THREE.Mesh(fill, zoneFillMat(kind));
    area.userData.item = it;
    area.userData.zone = mk.userData.zone = true;
    mk.add(area);
  } else if (it.type === 3) {
    mk = new THREE.LineSegments(GEO.cross, lineMat(markerKind(it)));
    if (it.points?.length > 1) {
      const pts = it.points.map((p) => new THREE.Vector3(p[0] - x, p[1] - y, p[2] - z));
      const curve = new THREE.CatmullRomCurve3(pts, false, 'centripetal');
      const line = new THREE.Line(new THREE.BufferGeometry().setFromPoints(curve.getPoints(pts.length * 12)), lineMat(markerKind(it)));
      line.userData.item = it;
      mk.add(line);
    }
  } else if (it.type === 0) {
    const cam = markerKind(it) === 'cam';
    mk = new THREE.Mesh(cam ? GEO.camera : GEO.cone, meshMat(markerKind(it)));
    if (it.rot) mk.quaternion.setFromRotationMatrix(placementRotation(it.rot));
  } else {
    mk = new THREE.Mesh(it.pathEvent ? beadGeometry(it.nodeTangents?.[0]) : TRIGGER_GEO[markerKind(it)] ?? (it.type === 7 ? GEO.diamond : GEO.ball), meshMat(markerKind(it)));
    // The real trigger area: a wire sphere, or for cylinder triggers a ring draped on the ground.
    if (it.radius > 0) {
      let area;
      if (it.cylinder) {
        const pts = [];
        for (let k = 0; k <= 64; k++) {
          const a = (k / 64) * Math.PI * 2, px = Math.cos(a) * it.radius, pz = Math.sin(a) * it.radius;
          pts.push(new THREE.Vector3(px, levelHeight(x + px, z + pz) - y - 0.15, pz));
        }
        area = new THREE.Line(new THREE.BufferGeometry().setFromPoints(pts), lineMat(markerKind(it), 0.7));
      } else if (markerKind(it) === 'troam') {
        // Roam areas: a flat dashed ring at the area's height (as in the legend), so they read apart from the trigger spheres.
        const pts = [];
        for (let k = 0; k <= 96; k++) pts.push(new THREE.Vector3(Math.cos((k / 96) * Math.PI * 2) * it.radius, 0, Math.sin((k / 96) * Math.PI * 2) * it.radius));
        area = new THREE.Line(new THREE.BufferGeometry().setFromPoints(pts), roamMat);
        area.computeLineDistances();
      } else {
        area = new THREE.LineSegments(new THREE.WireframeGeometry(new THREE.IcosahedronGeometry(it.radius, 1)), lineMat(markerKind(it), 0.25));
      }
      area.userData.item = it;
      mk.add(area);
    }
  }
  mk.position.set(x, y, z);
  mk.name = it.name;
  mk.userData.item = it;
  return mk;
}

// Legend rows double as per-type visibility toggles.
// Camera placements are type-0 items like spawns, but listed separately.
const ZONE_KINDS = { 0x04: 'zhi', 0x05: 'zlo', 0x10: 'zmus', 0x16: 'zunit', 0x39: 'zmtrig' };
const SPAWN_KINDS = { 0x01: 'player', 0x02: 'player', 0x26: 'cam', 0x27: 'wing', 0x49: 'wdgun' };
const PATH_KINDS = { 0x1B: 'pspeed', 0x22: 'phold', 0x20: 'pattack', 0x1A: 'paction', 0x4E: 'pfly', 0x1C: 'pweave', 0x23: 'ppitch' };
const triggerKind = (it) => (it.sub === 0x11 || it.sub === 0x17 ? 'troam' : it.sub === 0x19 ? 'tspawn' : it.sub === 0x3A ? 'twaypoint' : it.pathEvent ? PATH_KINDS[it.sub] : it.sub !== 0x18 ? 7 : /(?<![a-z])LINE_\d|say/i.test(it.name) ? 'tvoice' : 'tevent');
const markerKind = (it) => (it.type === 2 && it.sub !== 0x51 ? 'point' : it.type === 3 && it.pathKind ? it.pathKind : it.type === 0 ? SPAWN_KINDS[it.sub] ?? 0 : it.type === 6 ? ZONE_KINDS[it.sub] ?? 6 : it.type === 7 ? triggerKind(it) : it.type);
// Game space is Y-down, so lifting a spline means subtracting from its y.
function applySpread(markers) {
  const step = +$('optSpread').value;
  for (const m of markers.children) if (m.userData.spreadRank != null) m.position.y = m.userData.item.pos[1] - m.userData.spreadRank * step;
}
// Terrain height (game Y-down) at world x/z, set per level; 0 when the level has no terrain.
let levelHeight = () => 0;
const zoneFill = new Map();
function zoneFillMat(kind) {
  if (!zoneFill.has(kind)) zoneFill.set(kind, new THREE.MeshBasicMaterial({ color: TYPES[kind]?.color ?? 0xffffff, transparent: true, opacity: 0.18, depthWrite: false, side: THREE.DoubleSide, fog: false }));
  return zoneFill.get(kind);
}
const hiddenTypes = new Set();
function applyMarkerVisibility(markers) {
  const kinds = new Set(markers.children.map((m) => String(markerKind(m.userData.item))));
  for (const m of markers.children) m.visible = !hiddenTypes.has(String(markerKind(m.userData.item)));
  // The master checkbox mirrors the per-kind toggles: off when all are hidden, indeterminate when some are.
  const hidden = [...kinds].filter((k) => hiddenTypes.has(k)).length;
  $('optMarkers').checked = hidden < kinds.size;
  $('optMarkers').indeterminate = hidden > 0 && hidden < kinds.size;
}
const LEGEND_GROUPS = [
  ['Spawns & cameras', ['player', 'wing', '0', 'cam', 'point', 'wdgun']],
  // Paths get one row per NPC kind found on the level (path:*), then unassigned splines.
  ['Paths', null],
  ['Triggers', ['tvoice', 'tevent', 'tspawn', 'twaypoint', '7', '2']],
  ['Path events', ['pspeed', 'phold', 'pattack', 'paction', 'pfly', 'pweave', 'ppitch']],
  ['Zones', ['zhi', 'zlo', 'zmus', 'zmtrig', 'zunit', 'troam', '6']],
];
function renderLegend(markers) {
  const counts = {};
  for (const m of markers.children) { const k = markerKind(m.userData.item); counts[k] = (counts[k] || 0) + 1; }
  const el = $('legendRows');
  el.innerHTML = '';
  // Each group header toggles all of its rows; it shows as indeterminate when only some are on.
  for (const [group, types] of LEGEND_GROUPS) {
    const present = (types ?? [...Object.keys(counts).filter((k) => k.startsWith('path:')).sort(), '3']).filter((t) => counts[t]);
    if (!present.length) continue;
    const head = elem('label');
    head.className = 'group';
    head.innerHTML = `<input type="checkbox"><span>${group}</span><em>${present.reduce((s, t) => s + counts[t], 0)}</em>`;
    const headBox = head.querySelector('input');
    const rows = [];
    const sync = () => {
      const on = present.filter((t) => !hiddenTypes.has(t)).length;
      headBox.checked = on === present.length;
      headBox.indeterminate = on > 0 && on < present.length;
    };
    headBox.onchange = () => {
      for (const t of present) {
        if (headBox.checked) hiddenTypes.delete(t);
        else hiddenTypes.add(t);
      }
      rows.forEach((r) => (r.checked = headBox.checked));
      applyMarkerVisibility(markers);
    };
    el.append(head);
    for (const type of present) {
      const info = TYPES[type];
      const row = elem('label');
      row.className = 'sub';
      row.title = info.tip;
      row.innerHTML = `<input type="checkbox" ${hiddenTypes.has(type) ? '' : 'checked'}>${icon(info)}<span>${info.label}</span><em>${counts[type]}</em>`;
      const box = row.querySelector('input');
      rows.push(box);
      box.onchange = () => {
        if (box.checked) hiddenTypes.delete(type);
        else hiddenTypes.add(type);
        sync();
        applyMarkerVisibility(markers);
      };
      el.append(row);
    }
    sync();
  }
  applyMarkerVisibility(markers);
  $('legend').hidden = false;
}

// ---------- picking: hover tips, click to pin, right-click to inspect ----------
const raycaster = new THREE.Raycaster();
raycaster.params.Line.threshold = 0.3;
const pointer = new THREE.Vector2();
const isUnderSky = (o) => { for (; o; o = o.parent) if (o === sky) return true; return false; };
function pick(e, targets) {
  const r = canvas.getBoundingClientRect();
  pointer.set(((e.clientX - r.left) / r.width) * 2 - 1, -((e.clientY - r.top) / r.height) * 2 + 1);
  raycaster.setFromCamera(pointer, camera);
  const hits = raycaster.intersectObjects(targets, true).filter((h) => {
    for (let o = h.object; o; o = o.parent) if (!o.visible) return false;
    return !isUnderSky(h.object);
  });
  // Zones (outline and see-through fill) are the last pick: anything else under the cursor wins.
  return hits.find((h) => !h.object.userData.zone) ?? hits[0];
}
const visibleMarkers = () => {
  const m = root && root.getObjectByName('markers');
  return m && m.visible ? m.children.filter((c) => c.visible) : [];
};
canvas.addEventListener('pointermove', (e) => {
  const tip = $('tip');
  // The right-click card takes over while it is open.
  if (editHover(e)) return;
  const hit = !e.buttons && $('info').hidden && pick(e, editMode === 'events' ? editTargets() : visibleMarkers());
  if (!hit) { tip.hidden = true; return; }
  const it = hit.object.userData.item;
  let what = describe(it);
  // Splines often run on top of each other; name every other one under the cursor too.
  if (it.type === 3) {
    const others = new Set(raycaster.intersectObjects(visibleMarkers(), true).map((h) => h.object.userData.item).filter((o) => o && o.type === 3 && o !== it).map((o) => o.name));
    if (others.size) what += '\nAlso here: ' + [...others].join(', ');
  }
  // The kind line keeps only what the description does not already say; an area's description opens with its kind ("Music zone (0x10): ...").
  const kind = it.type === 6 ? '' : kindParts(it).filter((p) => !what.toLowerCase().includes(p.toLowerCase())).join(' · ');
  tip.innerHTML = `<b>${esc(it.name || it.label || '(unnamed)')}</b><br>${esc(what).replaceAll('\n', '<br>')}${it.type === 6 ? `<br>Footprint ${it.size[0].toFixed(1)} × ${it.size[2].toFixed(1)}` : ''}${kind ? `<br><span>${esc(kind)}</span>` : ''}`;
  const r = canvas.getBoundingClientRect();
  tip.style.left = e.clientX - r.left + 14 + 'px';
  tip.style.top = e.clientY - r.top + 14 + 'px';
  tip.hidden = false;
});
// Leaving the view for the cutscene timeline keeps the tip: the timeline shows its own there.
canvas.addEventListener('pointerleave', (e) => !e.relatedTarget?.closest?.('#cutTimeline') && ($('tip').hidden = true));

function inspect(hit) {
  const o = hit.object;
  if (o.userData.crawlBackdrop) return crawlBackdropCard();
  if (o.userData.item) return { title: o.userData.item.name || o.userData.item.label || '(unnamed)', rows: itemRows(o.userData.item), place: o.userData.item, voiceIds: triggerVoiceIds(o.userData.item, selected?.split('/').pop()) };
  if (o.userData.tiles) {
    const { hmp, texIds } = o.userData.tiles;
    const p = o.worldToLocal(hit.point.clone());
    const tx = Math.floor(p.x / 2), ty = Math.floor(p.z / 2);
    const ti = hmp.map[ty * hmp.w + tx], tile = hmp.tiles[ti];
    if (!tile) return null;
    return { title: `Terrain tile ${tx}, ${ty}`, rows: [['Tile record', ti], ['Texmap id', tile.tex], ['Bank texture', texIds[tile.tex] ?? '-'],
      ['Flags', hex(tile.flags)], ['Height range', `${Math.min(...tile.hts)} .. ${Math.max(...tile.hts)} (x${+hmp.heightScale.toFixed(3)})`]] };
  }
  const info = o.userData.infos?.[hit.face?.materialIndex ?? 0];
  if (!info) return null;
  let obj = o;
  while (obj.parent && obj.parent !== root && !obj.userData.hobObject) obj = obj.parent;
  let placed = o;
  while (placed && !placed.userData.source) placed = placed.parent;
  const t = info.tex;
  const editId = placed?.userData.source?.id ?? (selectedLevel ? null : selected);
  return { title: `${obj.name || 'model'} / ${o.name || 'part'}`, open: placed?.userData.source, edit: t && editId && { id: editId, tex: t.name }, rows: [['Material', `#${info.index} ${esc(info.matName)}`],
    ['Texture', t ? `${esc(t.name)} ${t.w}x${t.h}` : 'none (vertex colour only)'], t && ['Format', `${TEX_TYPES[t.type] ?? 'type ' + t.type}, flags ${hex(t.flags)}`],
    t && ['Wrap', info.map?.wrapS === THREE.ClampToEdgeWrapping ? 'clamp (UVs stay in 0..1)' : 'repeat'],
    info.emissive && info.valpha && !info.blend && t && ['Shading', 'sphere-mapped reflection (glass)'],
    ['Alpha', info.valpha ? 'blended (vertex alpha)' : info.blend ? 'blended (texture alpha)' : info.cutout ? 'cutout (alpha test 0.5)' : 'opaque'],
    ['Part triangles', o.geometry.attributes.position.count / 3]] };
}
let backTo = null;
function showInfo(card, x, y) {
  const el = $('info');
  if (!card) { el.hidden = true; return; }
  el.innerHTML = `<b>${esc(card.title)}</b>` + card.rows.filter(Boolean).map(([k, v]) => `<span>${k}</span><span>${v}</span>`).join('');
  if (card.open) {
    const btn = elem('button');
    btn.textContent = `Open ${card.open.name} in model view`;
    btn.onclick = () => {
      el.hidden = true;
      backTo = { level: selected, view: getView() };
      $('tabModels').click();
      showModel(card.open.id, card.open.objIndex);
      $('btnBack').textContent = '← Back to ' + backTo.level.split('/').pop();
      $('btnBack').hidden = false;
    };
    el.append(btn);
  }
  if (card.edit) {
    const dl = elem('button');
    dl.textContent = 'Download texture PNG';
    dl.onclick = () => downloadTexture(card.edit.id, card.edit.tex);
    const rep = elem('button');
    rep.textContent = 'Replace texture from image…';
    rep.title = 'Scaled to the texture size and converted to its format (palette textures are re-quantised); Export mod saves the change';
    rep.onclick = () => { el.hidden = true; replaceTexture(card.edit.id, card.edit.tex); };
    el.append(dl, rep);
  }
  for (const [label, tip, fn] of card.actions ?? []) {
    const b = elem('button', { textContent: label, title: tip });
    b.onclick = () => {
      el.hidden = true;
      fn();
    };
    el.append(b);
  }
  if (card.voiceIds?.length && audioBank) {
    const btn = elem('button');
    btn.textContent = `Play voice line${card.voiceIds.length > 1 ? 's' : ''} ${card.voiceIds.join(', ')}`;
    btn.onclick = () => playVoiceIds(card.voiceIds);
    el.append(btn);
  } else if (card.place?.type === 7 && (card.place.onEnter || card.place.pathEvent)) {
    const note = elem('small', { className: 'shotNote' });
    el.append(note);
    triggerEffect(card.place, selected).then((t) => (note.textContent = t));
  }
  const r = canvas.getBoundingClientRect();
  el.style.left = Math.min(x, r.width - 390) + 'px';
  el.style.top = Math.min(y, r.height - 220) + 'px';
  el.hidden = false;
}
// What a trigger that plays no line does: who reads the flags and counters it writes (the level script, or DAT items gated on them:
// units and restart points by flag +0x60 / counter +0x64 (isDatItemConditionMet), other triggers, objective arrows).
async function triggerEffect(it, dir) {
  const p = it.pathEvent, acts = [it.onEnter, it.onExit, p && { counter: p.addCounter, flag: p.setFlag }].filter(Boolean);
  const flags = [...new Set(acts.map((a) => a.flag).filter(Boolean))], counters = [...new Set(acts.map((a) => a.counter).filter(Boolean))];
  if (!flags.length && !counters.length) return 'It sets no flag or counter, so it does nothing.';
  const reads = new Set(globalThis.RS64_TRIGGER_VOICES?.[(modLevelOf(dir) ? 'lv_' + modLevelOf(dir).template.toString(36) : dir.split('/').pop())]?.reads ?? []);
  const dv = await rom.read(dir + '/dat'), users = [];
  for (const x of parseDAT(dv).items) {
    if (x === it || x.offset === it.offset) continue;
    const size = dv.getUint32(x.offset + 8);
    const gate = x.type === 0 && size > 0x6C ? { flag: dv.getUint32(x.offset + 0x60), counter: dv.getUint32(x.offset + 0x64) } : null;
    const uses = [gate, x.requires, x.waypoint && { flag: x.waypoint.flag }, x.pathEvent && { flag: x.pathEvent.flag, counter: x.pathEvent.counter }].filter(Boolean);
    if (uses.some((u) => flags.includes(u.flag) || counters.includes(u.counter))) users.push(x.name || TYPES[markerKind(x)]?.label || 'item');
  }
  const what = [...flags.map((f) => `flag ${f}`), ...counters.map((c) => `counter ${c}`)].join(' and ');
  const script = [...flags.map((f) => 'b' + f), ...counters.map((c) => 'c' + c)].some((k) => reads.has(k));
  if (users.length) return `It plays no voice line; ${what} enables ${users.slice(0, 4).join(', ')}${users.length > 4 ? ` and ${users.length - 4} more` : ''}${script ? ', and the level script reads it' : ''}.`;
  if (script) return `It plays no voice line; the level script reads ${what} (objectives, camera cuts or spawns).`;
  return `It has no effect: nothing in the level or its script reads ${what}.`;
}
// A press that barely moves is a click; anything longer is an orbit drag.
const downAt = {};
canvas.addEventListener('pointerdown', (e) => (downAt[e.button] = [e.clientX, e.clientY]));
const wasClick = (e, button) => downAt[button] && Math.hypot(e.clientX - downAt[button][0], e.clientY - downAt[button][1]) <= 4;
canvas.addEventListener('pointerup', (e) => {
  if (e.button !== 0 || !wasClick(e, 0) || sculptActive() || editClick(e) || cutPickActor(e)) return;
  const hit = pick(e, visibleMarkers());
  const r = canvas.getBoundingClientRect();
  showInfo(hit && inspect(hit), e.clientX - r.left + 12, e.clientY - r.top + 12);
});
canvas.addEventListener('contextmenu', (e) => {
  e.preventDefault();
  if (!root || !wasClick(e, 2) || editPickList(e)) return;
  const r = canvas.getBoundingClientRect();
  // In a crawl the backdrop is picked first: it is drawn behind everything but sits past the stars plane.
  const hit = (cut?.crawl?.backdrop?.visible && pick(e, [cut.crawl.backdrop])) || pick(e, [root]);
  showInfo(hit && inspect(hit), e.clientX - r.left + 12, e.clientY - r.top + 12);
});

function setRoot(obj) {
  endCutscene();
  select(null);
  placing = null;
  $('legend').hidden = true;
  $('tip').hidden = true;
  $('info').hidden = true;
  if (root) {
    scene.remove(root);
    root.traverse((o) => {
      if ((o.isMesh || o.isLine) && !o.userData.shared && !Object.values(GEO).includes(o.geometry)) o.geometry.dispose();
    });
  }
  root = obj;
  anim = null;
  setupExhaust(null);
  effect = null;
  sky = null;
  skyColor = null;
  scene.background = bgColor;
  scene.fog = null;
  levelFog = null;
  hemi.color.set(0xffffff);
  hemi.groundColor.set(0x404040);
  hemi.intensity = 2.2;
  sun.color.set(0xffffff);
  sun.intensity = 1.4;
  sun.position.set(1, 2, 1.5);
  sun.target.position.set(0, 0, 0);
  scene.add(root);
  loadSculpt();
}

// ---------- ROM + lists ----------
let rom = null, romBytes = null, mode = 'models', selected = null, selectedLevel = false, selectedObj = -1;
const modelCache = new Map();

