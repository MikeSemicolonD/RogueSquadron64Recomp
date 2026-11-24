'use strict';

// The explorer's shared core (renderer, camera, materials, markers, picking); the other area scripts load after it, in order (the loader at the end of index.html). three.js arrives through window.RS64_THREE.
const { THREE, OrbitControls, TransformControls, GLTFExporter } = window.RS64_THREE;
const { parseShadowTextures, parseMusicCues, musicZoneSet, musicZoneEdit, musicConditionSources, musicGaps, musicRegionBox, musicMargin, musicAddTrigger, openRom, parseHOB, parseHMT, parseHMP, parseDAT, parseTexTable, decodeTerrainTex, encodeTerrainTex, terrainTexRGBA, paintApply, paintPath, paintBudget, PAINT_MARK, modAssetFile, parseVoiceLines, parseAnim, parseSpriteAnim, spriteKeys,
  orderSetTarget, recordSetName, shotDropCameras, parseSdir, decodeMusyxAdpcm, wavBytes, parseSpeech, decodeMort, encodeMort, parseVoiceTable, parseMenuVoice, parseFrontText, parsePool, parseSfxTable, renderSfx, parseCutsceneVoice, parseMixTables, parseSongTable, parseSongGroups, parseSong, encodeTexture, encodeBPS, zipStore, hmpGrid, writeHMP, hmpResize, hmpShiftLights, datShift, cutsceneShift, hmpSetSun, datAddItem, datRemoveItem, modLevelsParse, modLevelsWrite, modLevelNew, modLevelAliases, SHIP_TEMPLATES, shipsParse, shipsWrite, shipBehaviours, SHIP_MAX, shipFreeSpot, shipNew, shipAliases, hobSetObjectName, hmtOwnTextures, hudIconSize, hudIconWrite, blueprintWrite, texFileRename, shipStandAt, shipSpotAt, shipHangarRemove, shipHangarAdd, medalInfo, datNameItem, RULE_WHENS, RULE_ACTIONS, RULES_PATH, RULES_JSON_PATH, rulesParse, rulesExtParse, CRAFT_RULE_EXT, modChecksOf, menuExtParse, zipUnpack, rulesScriptParse, rulesScriptWrite, rulesScriptCheck, rulesComplete, rulesVariables, condCheck, condRename, rulesWrite, ruleRefs, rulesRename, rulesBroken, datBlank, hmpFlat, datWaterGet, datWaterSet, datFlightInset, ENV_PATH, envParse, envWrite, recordInert, npcPathGet, npcSetPath, npcPathField, datRemoveIndexed, objectivesApply, objectiveTargets, isUnit, isTriggerArea, isFlightPath, WAVE_FIELDS, waveGet, waveSet, missionScript, scriptUnits, arrowWrite, arrowSources, unitDeath, levelUsedObjectiveSlots, levelUsedFlags, waveFlagFor, triggerWave, unitGate, unitGateSet, unitDeathSet, unitHasEvents, UNITS_ANY_LEVEL, UNITS_NEED_PATH, UNITS_OWN_PATH, UNIT_RECORD_FROM, MODEL_UNITS, PACKAGES_PATH, unitModelName, datAddDevastatorGuns, hmtRenameTextures, hmtRestoreNames, TRAIN_UNITS, datAddTrain, datRemoveUnit, trainOf, datAddEnemy, splineSetPoints, splinePoints, splineAt, splineCopy, splineNew, splineAttackEnd, splineSetAttackEnd, uniqueName, shotDestroyGet, shotDestroySet, shotSwapGet, shotSwapSet, shotRename, splineLoops, splineLoopBlocked, splineSetLoop, datRenameItem, triggerSetActivators, craftImportSlots, hobImportCraft, craftImportPlacement, craftClipBake, parseCutscene, cutsceneValue, createHistory, projectWrite, projectRead, crc32, writeCutscene, shotLinkGet, shotLinkSet, cutClips, cutDragClip, cutAddEvent, cutRemoveEvent, cutSetKey, cutRemoveKey, cutAddActor, cutRemoveActor, cutSetActorName, cutAddCamera, cutRemoveCamera, cutSetLength, cutBlank, cutSetChannelSpan, cutCurve, cutQuatToAxisAngle, cutAxisAngleToQuat, cutBakeActorPath, cutActorPose, cutBakeCameraPath, cutBakeCameraMode, cutShootAt, cutRetimeSnaps, cutUnbake, cutBake, cutBakedEvents, cutSyncModeSpans, CUT_LIMITS, CUT_ACTOR_EVENTS, cutMoveEvent, cutAnimateActor, CRAWL_PLANETS, CRAWL_PATH, crawlFontGaps, stockCrawls, crawlParse, crawlWrite, crawlFont, crawlLayout, encodeJpeg, CRAWL_STOCK_CAMERA, crawlCameraAt, gameFonts, glyphPackParse, glyphPackWrite, langChars, fontCovers, FONT_STRIP_HEADER, MOD_KINDS, modChangeKind, menuItemKey, menuItemKeys, menuLinks, exportPlan, menuParse, menuWrite, menuWriteSubset, menuMerge, menuLayout, menuFontGaps, NATIVE_MENUS, MENU_DEFAULT_BUTTONS, menuWithDefaults } = window.RS64;
const $ = (id) => document.getElementById(id);

function elem(tag, props, ...children) {
  const e = Object.assign(document.createElement(tag), props);

  e.append(...children);

  return e;
}

// A select of [value, label] pairs (values compared as strings), set to value when given; an entry can also be a ready element (an optgroup).
// missing: a value not in the list still shows, red, as `${value} ${missing}` (or missing(value)), so a dangling reference is visible instead of silently reset.
function selectOf(pairs, value, props, missing)
{
  const s = elem('select', props, ...pairs.map((p) => (p instanceof Node ? p : new Option(p[1], String(p[0])))));

  if (value != null && missing && ![...s.options].some((o) => o.value === String(value)))
  {
    s.append(Object.assign(new Option(typeof missing === 'function' ? missing(value) : `${value} ${missing}`, String(value)), { className: 'bad' }));
  }

  if (value != null)
    s.value = String(value);

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
    if (value == null)
      localStorage.removeItem(key);
    else
      localStorage.setItem(key, JSON.stringify(value));
  } catch {}
}

// File handles and the last ROM go in IndexedDB, which (unlike localStorage) holds them.
function idbKv(mode, fn) {
  return new Promise((res, rej) => {
    const open = indexedDB.open('rs64-explorer', 1);

    open.onupgradeneeded = () => open.result.createObjectStore('kv');
    open.onerror = () => rej(open.error);

    open.onsuccess = () => {
      const req = fn(open.result.transaction('kv', mode).objectStore('kv'));

      req.onsuccess = () => res(req.result);
      req.onerror = () => rej(req.error);
    };
  });
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
const GROUND_NPCS = new Set([0x0F, 0x1F, 0x25, 0x2B, 0x2D, 0x2E, 0x37, 0x3B, 0x3C, 0x3D, 0x46, 0x47, 0x50]);
const GROUND_SINK = new Set([0x2B, 0x2D, 0x2E]);

const SUBTYPE_MODEL = {
  0x0C: 'imp_stuff/tiefighter', 0x0D: 'imp_stuff/gun_turret', 0x0E: 'imp_stuff/prbdroid', 0x0F: 'imp_stuff/atat',
  0x12: 'imp_stuff/tieinter', 0x13: 'imp_stuff/tiebomber', 0x1E: 'imp_stuff/mis_turret', 0x1F: 'imp_stuff/atst',
  0x24: 'imp_stuff/shuttle', 0x25: 'imp_stuff/tankdroid', 0x28: 'imp_stuff/megaturret', 0x29: 'imp_stuff/tie_d',
  0x2A: 'gnrc_stuff/radardish', 0x2B: 'reb_stuff/r_transp', 0x2C: 'imp_stuff/hvyshut', 0x2D: 'imp_stuff/cargo01',
  0x2E: 'reb_stuff/r_combat', 0x32: 'gnrc_stuff/shield', 0x37: 'imp_stuff/wrld_dvstr', 0x3B: 'imp_stuff/trooper',
  0x3D: 'gnrc_stuff/civilian', 0x3E: 'imp_stuff/assboat', 0x40: 'imp_stuff/wskimmer', 0x41: 'reb_stuff/r_turret',
  0x42: 'imp_stuff/tal_turret', 0x43: 'gnrc_stuff/cloudcar', 0x47: 'reb_stuff/atpt_i', 0x48: 'imp_stuff/bosshut',
  0x4A: 'imp_stuff/pwrgen', 0x4C: 'gnrc_stuff/powerup', 0x4D: 'imp_stuff/raptor', 0x3C: 'reb_stuff/r_pilot', 0x50: 'imp_stuff/bsoldier',
  // Trains follow splines like NPCs; '#name' picks the object inside a multi-object HOB.
  0x2F: 'imp_stuff/train#tr_engine', 0x30: 'imp_stuff/train#tr_defense', 0x34: 'reb_stuff/r_train#r_tr_engine', 0x35: 'reb_stuff/r_train#r_tr_cargo',
};

// DAT item types; subtypes from rogue_squadron64/docs/dat_files/dat_file.md (several are educated guesses there).
const TYPES = {
  player: {
    color: 0xe8b84a,
    label: 'Player start / restarts',
    icon: 'tri',
    tip: 'Where the player begins and comes back after being shot down'
  },
  wing: { color: 0xe6edf7, label: 'Wingmen', icon: 'wing', tip: 'Where the wingmen start, such as Wedge and Rogue 10' },
  0: {
    color: 0xc9b38a,
    label: 'Other units / objects',
    icon: 'tri',
    tip: 'Placed units or objects the viewer has no model for'
  },
  cam: {
    color: 0xff79c6,
    label: 'Cameras',
    icon: 'cam',
    tip: 'Camera positions for intros, endings and camera shots'
  },
  // DAT type 2 is a generic point list; the subtype says what each point is.
  2: { color: 0xff6a3d, label: 'Lava vents', icon: 'circle', tip: 'Spots that erupt lava (Raid on Sullust)' },
  wdgun: {
    color: 0xb0b8c8,
    label: 'Devastator gun settings',
    icon: 'square',
    tip: 'Settings for the World Devastator gun turrets'
  },
  point: { color: 0x9ad0ff, label: 'Menu points', icon: 'circle', tip: 'Front-end spots: hangar camera starts and bridge scene points' },
  3: {
    color: 0x4ac0e8,
    label: 'Unassigned paths',
    icon: 'cross',
    tip: 'Flight paths nothing flies yet'
  },
  6: { color: 0x9aa3b5, label: 'Other regions', icon: 'zother', tip: 'A region whose purpose isn\'t known yet' },
  zhi: { color: 0x8a7cff, label: 'High-detail regions', icon: 'zhi', tip: 'Evens out small bumps in the ground here when the level loads, which also changes collision' },
  zlo: { color: 0x5b6bd8, label: 'Low-detail regions', icon: 'zlo', tip: 'Flattens the ground here into straight slopes when the level loads, which also changes collision' },
  zmus: { color: 0xf0a24a, label: 'Music region triggers', icon: 'zmus', tip: 'Entering the inner box changes the song, leaving the outer box brings the earlier song back' },
  zmtrig: {
    color: 0xffd36b,
    label: 'Music region triggers',
    icon: 'zmtrig',
    tip: 'The inner box: entering it sets the music region trigger off. The outer box moves with it'
  },
  zunit: { color: 0xff5f8f, label: 'Ground-unit areas', icon: 'zunit', tip: 'Troopers and civilians appear and walk around inside this area' },
  tvoice: {
    color: 0x6be0d0,
    label: 'Voice-line areas',
    icon: 'tvoice',
    tip: 'Flying in makes the mission play a voice line (the stock mission script picks which)'
  },
  tevent: {
    color: 0x5fd17a,
    label: 'Goal areas',
    icon: 'tevent',
    tip: 'Triggers when the player or a chosen unit comes close: it can complete a Reach an area objective, set a flag, play a cutscene or set off a rule'
  },
  tspawn: { color: 0xc3e65a, label: 'Wave areas', icon: 'tspawn', tip: 'Like a goal area, and also brings in the unit it names' },
  troam: { color: 0xd17aff, label: 'Roam areas', icon: 'troam', tip: 'A unit wanders inside this ball and turns back toward the middle when it strays out. Height counts, so keep the centre at ground level for ground units' },
  // Path events (fired when an NPC on a spline passes the node; func_800F3CB0), one kind per subtype.
  pspeed: { color: 0x7ee0ff, label: 'Speed changes', icon: 'bead', tip: 'The unit eases to a new speed here' },
  phold: { color: 0xffb347, label: 'Hold orders', icon: 'bead', tip: 'The unit stops here until a timer runs out and an objective lets it go on' },
  pattack: { color: 0xff3b30, label: 'Attack orders', icon: 'bead', tip: 'The unit picks a new attack target here (red dashed arrow)' },
  paction: {
    color: 0xff9ad5,
    label: 'Craft actions',
    icon: 'bead',
    tip: 'A craft action here: TIE bombers start a bombing run, shuttles land or fold their wings, wingmen play an effect'
  },
  pfly: {
    color: 0xc792ff,
    label: 'Fly-to orders',
    icon: 'bead',
    tip: 'The unit leaves its path here and flies toward a named object or the player (TIE fighters and the boss shuttle only)'
  },
  pweave: { color: 0x9be37a, label: 'Weave changes', icon: 'bead', tip: 'Changes how much the craft sways side to side from here on' },
  ppitch: { color: 0xf5e663, label: 'Landing pitch', icon: 'bead', tip: 'The craft tilts up to land, or levels out to take off, over a few seconds' },
  twaypoint: {
    color: 0xffe14a,
    label: 'Objective arrow points',
    icon: 'twaypoint',
    tip: 'The HUD arrow points at the nearest one whose objective allows it'
  },
  7: { color: 0x3f9a58, label: 'Other triggers', icon: 'diamond', tip: 'Trigger areas the game never reads' },
};

// 0x00: placed models with no unit behaviour (lamps, buildings); like 0x14 their model comes from the item, not the subtype.
const SUBTYPES = {
  0x00: 'Scenery',
  0x01: 'Player start',
  0x02: 'Restart point',
  0x03: 'Spline',
  0x04: 'LOD region (high detail)',
  0x05: 'LOD region (low detail)',
  0x06: 'Hangar placement',
  0x07: 'Scene placement',
  0x08: 'Scene spline',
  0x09: 'Menu point',
  0x0C: 'TIE fighter',
  0x0D: 'Gun turret',
  0x0E: 'Probe droid',
  0x0F: 'AT-AT',
  0x10: 'Music region',
  0x11: 'Proximity trigger',
  0x12: 'TIE interceptor',
  0x13: 'TIE bomber',
  0x14: 'Building / prop',
  0x16: 'Zone',
  0x17: 'Sphere trigger',
  0x18: 'Event trigger',
  0x19: 'Trigger',
  0x1A: 'Trigger',
  0x1B: 'Trigger',
  0x1C: 'Trigger',
  0x1E: 'Missile turret',
  0x1F: 'AT-ST',
  0x20: 'Spawn trigger',
  0x21: 'Stationary NPC spawn',
  0x22: 'Trigger',
  0x23: 'Trigger',
  0x24: 'Shuttle',
  0x25: 'Tank droid',
  0x26: 'Camera position',
  0x27: 'Wingman position',
  0x28: 'Mega turret',
  0x29: 'TIE (D)',
  0x2A: 'Radar dish',
  0x2B: 'Rebel transport',
  0x2C: 'Heavy shuttle',
  0x2D: 'Cargo',
  0x2E: 'Rebel combat',
  0x2F: 'Imperial train',
  0x30: 'Imperial train',
  0x31: 'Imperial train',
  0x32: 'Shield',
  0x33: 'Train pillar',
  0x34: 'Rebel train',
  0x35: 'Rebel train',
  0x36: 'Rebel train pillar',
  0x37: 'World Devastator',
  0x39: 'Music trigger area',
  0x3A: 'Trigger',
  0x3B: 'Stormtrooper',
  0x3C: 'Rebel pilot',
  0x3D: 'Civilian',
  0x3E: 'Assault boat',
  0x40: 'Wave skimmer',
  0x41: 'Rebel turret',
  0x42: 'Taloraan turret',
  0x43: 'Cloud car',
  0x44: 'Imperial platform',
  0x46: 'Moving NPC spawn',
  0x47: 'AT-PT',
  0x48: 'Boss hut',
  0x49: 'World Devastator gun',
  0x4A: 'Power generator',
  0x4B: 'Large building',
  0x4C: 'Power-up',
  0x4D: 'Heavy raptor shuttle',
  0x4E: 'Trigger',
  0x4F: 'Exhaust port',
  0x50: 'B soldier',
  0x51: 'Lava vent',
  0x52: 'Unnamed item',
};

// The player crafts' model files; a c_ file is the craft's cutscene copy.
const CRAFT_MODELS = {
  xwing: 'X-wing', c_xwg: 'X-wing', ywing: 'Y-wing', c_ywg: 'Y-wing', awing: 'A-wing', c_awg: 'A-wing', vwing: 'V-wing', c_vwg: 'V-wing',
  snowspeeder: 'Snowspeeder', c_spdr: 'Snowspeeder', falcon: 'Millennium Falcon', c_fal: 'Millennium Falcon', c_tie: 'TIE interceptor',
  t16: 'T-16 Skyhopper', c_t16: 'T-16 Skyhopper'
};

const UNIT_MODELS = new Map(Object.entries(SUBTYPE_MODEL).filter(([, p]) => !p.includes('#')).map(([s, p]) => ['data/' + p, SUBTYPES[s]]));

// What a model file draws, by the game's unit or craft name ('data/imp_stuff/atat_lo' -> 'AT-AT (low detail)'), or null if it has none.
function modelName(id)
{
  const file = id.split('/').pop();

  if (CRAFT_MODELS[file])
    return CRAFT_MODELS[file] + (file.startsWith('c_') ? ' (cutscene)' : '');

  const lod = ['_hi', '_lo', 'lo', 'l'].find((s) => id.endsWith(s) && UNIT_MODELS.has(id.slice(0, -s.length)));

  if (!lod)
    return UNIT_MODELS.get(id) ?? null;

  return UNIT_MODELS.get(id.slice(0, -lod.length)) + (lod === '_hi' ? '' : ' (low detail)');
}

// An icon from icons/<name>.svg drawn in color (default: the text colour around it). From file:// an <img> is the only way to load an SVG
// file, and it can't take the page's colours, so it sits clipped just out of its box while its drop shadow paints the shape in place.
const iconImg = (name, color) => `<span class="icon" aria-hidden="true"${color ? ` style="--icon-color: ${color}"` : ''}><img src="icons/${name}.svg" alt=""></span>`;

// A marker kind's legend glyph (icons/markers: trigger, zone and path event kinds each have their own), in the kind's colour.
const icon = (t) => iconImg('markers/' + t.icon, '#' + t.color.toString(16).padStart(6, '0'));

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
function applySun(light, w, h)
{
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

// The orbit zoom moves a share of the distance to the orbit point, so it crawls near that point and never passes it. Within 2% of the
// scene's size of it, a zoom in moves the camera and the orbit point forward together by a fixed step instead.
canvas.addEventListener('wheel', (e) => {
  if (e.deltaY >= 0 || !root || !controls.enabled)
    return;

  // A level's size is its map's (its sky dome reaches far past the ground).
  const map = selectedLevel ? mapSize() : [Infinity];

  root.userData.radius ??= Number.isFinite(map[0]) ? Math.max(...map) / 2 : new THREE.Box3().setFromObject(root).getBoundingSphere(new THREE.Sphere()).radius || 1;

  const near = root.userData.radius * 0.02;

  if (camera.position.distanceTo(controls.target) > near)
    return;

  const step = controls.target.clone().sub(camera.position).normalize().multiplyScalar(near * 0.3);

  camera.position.add(step);
  controls.target.add(step);
}, { capture: true, passive: true });

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

  // Drawn now: until the next loop frame the browser would stretch the old picture to the new size (a sidebar drag squashes it).
  if (frameLoopRunning)
    renderer.render(scene, camera);
};

let frameLoopRunning = false;

new ResizeObserver(resize).observe(canvas.parentElement);
addEventListener('resize', resize);
resize();

// ---------- camera: fly keys, framing, reset ----------
const keys = new Set();

addEventListener('keydown', (e) => {
  if (e.target.tagName === 'INPUT' || e.target.tagName === 'SELECT')
    return;

  if (e.code === 'KeyF' && !e.ctrlKey && !e.metaKey && !e.altKey && focusSelected())
    return;

  if (editKey(e))
    return;

  // The pose tool keys belong to the gizmo while a cutscene track is picked.
  if (cutEditing && cutTrack && ['KeyW', 'KeyE', 'KeyR', 'KeyX'].includes(e.code))
    return;

  keys.add(e.code);

  if (e.code === 'KeyR' && homeView)
    setView(homeView);
});

addEventListener('keyup', (e) => keys.delete(e.code));
addEventListener('blur', () => keys.clear());

// Orthographic view: a 1-degree lens from far back (parallax well under 1%), so picking, both gizmos and orbiting keep working on the one
// camera; toggling keeps the height seen at the target, and near/far follow the distance each frame (orthoClip).
const ORTHO_FOV = 1;
const PERSP_FOV = 50;
let orthoView = false;

function setOrtho(on)
{
  if (on === orthoView)
    return;

  const half = (fov) => Math.tan(THREE.MathUtils.degToRad(fov) / 2);
  const dist = camera.position.distanceTo(controls.target);
  const height = 2 * dist * half(camera.fov);
  const fov = on ? ORTHO_FOV : PERSP_FOV;

  camera.position.sub(controls.target).setLength(height / (2 * half(fov))).add(controls.target);
  camera.fov = fov;
  orthoView = on;

  // Back to perspective: frameBox's clip span around the (much nearer) target.
  if (!on) {
    const d = camera.position.distanceTo(controls.target);

    camera.near = d / 500;
    camera.far = d * 50;
  }

  orthoClip();
  camera.updateProjectionMatrix();
  $('viewOrtho').classList.toggle('on', on);
  $('viewOrtho').title = on ? 'Orthographic view: click for perspective (numpad 5)' : 'Perspective view: click for orthographic, which flattens the view for precise placement (numpad 5)';
  setEditIcon($('viewOrtho'), on ? 'camOrtho' : 'camPersp');
}

function orthoClip() {
  if (!orthoView)
    return;

  const d = camera.position.distanceTo(controls.target);

  camera.near = d * 0.5;
  camera.far = d * 2;
  camera.updateProjectionMatrix();
}

// Looks along an axis of what is shown (view space, +Y up; models face -Z after their Y-down flip), keeping the distance to the target.
// Straight up / down is offset a hair so the orbit keeps its up axis.
const VIEW_DIRS = { front: [0, 0, -1], back: [0, 0, 1], right: [1, 0, 0], left: [-1, 0, 0], top: [0, 1, 1e-4], bottom: [0, -1, 1e-4] };

function snapView(name)
{
  const d = camera.position.distanceTo(controls.target);

  camera.position.copy(controls.target).addScaledVector(new THREE.Vector3(...VIEW_DIRS[name]).normalize(), d);
  controls.update();
}

// The view gizmo (left edge): the six axis ends as seen from the camera, nearer ones on top; clicking one looks from that side. Positive ends
// are the labelled X / Y / Z bubbles, negative ones plain rings.
const NAV_AXES = [
  ['right', [1, 0, 0], 'X', '#e5534b'],
  ['top', [0, 1, 0], 'Y', '#57ab5a'],
  ['back', [0, 0, 1], 'Z', '#539bf5'],
  ['left', [-1, 0, 0], '', '#e5534b'],
  ['bottom', [0, -1, 0], '', '#57ab5a'],
  ['front', [0, 0, -1], '', '#539bf5']
];

const NAV_RADIUS = 24;
const _navQ = new THREE.Quaternion();
const _navV = new THREE.Vector3();
const SVG_NS = 'http://www.w3.org/2000/svg';

const navEnds = NAV_AXES.map(([name, , label, colour]) => {
  const g = document.createElementNS(SVG_NS, 'g');

  g.innerHTML = (label ? `<line x1="0" y1="0" stroke="${colour}" stroke-width="2"/>` : '')
    + `<circle r="${label ? 8 : 6}" fill="${label ? colour : 'var(--panel)'}" stroke="${colour}" stroke-width="2"/>`
    + (label ? `<text text-anchor="middle" dominant-baseline="central" font-size="10" font-weight="700" fill="#fff">${label}</text>` : '');
  g.style.cursor = 'pointer';
  g.append(Object.assign(document.createElementNS(SVG_NS, 'title'), { textContent: `View from ${name} (${{ front: 'numpad 1', back: 'Ctrl+numpad 1', right: 'numpad 3', left: 'Ctrl+numpad 3', top: 'numpad 7', bottom: 'Ctrl+numpad 7' }[name]})` }));
  g.addEventListener('click', () => root && !cut && snapView(name));

  return g;
});

$('viewAxes').append(...navEnds);

function updateViewNav()
{
  const nav = $('viewNav');

  nav.hidden = !root || !!cut || mode === 'audio' || mode === 'fonts' || mode === 'menus';

  if (nav.hidden)
    return;

  _navQ.copy(camera.quaternion).invert();

  const placed = NAV_AXES.map(([, dir], i) => {
    _navV.fromArray(dir).applyQuaternion(_navQ);

    return { i, x: _navV.x * NAV_RADIUS, y: -_navV.y * NAV_RADIUS, z: _navV.z };
  });

  for (const p of placed)
  {
    const g = navEnds[p.i];

    g.setAttribute('transform', `translate(${p.x.toFixed(1)} ${p.y.toFixed(1)})`);

    const line = g.querySelector('line');

    if (line) {
      line.setAttribute('x2', (-p.x).toFixed(1));
      line.setAttribute('y2', (-p.y).toFixed(1));
    }
  }

  // Farther ends (more negative view z) first.
  placed.sort((a, b) => a.z - b.z).forEach((p) => $('viewAxes').append(navEnds[p.i]));
}

// Blender's numpad: 1 front, 3 right, 7 top (Ctrl: the opposite side), 5 toggles orthographic.
addEventListener('keydown', (e) => {
  if (e.target.tagName === 'INPUT' || e.target.tagName === 'SELECT' || !root || cut)
    return;

  const views = { Numpad1: ['front', 'back'], Numpad3: ['right', 'left'], Numpad7: ['top', 'bottom'] };

  if (views[e.code])
    snapView(views[e.code][e.ctrlKey ? 1 : 0]);
  else if (e.code === 'Numpad5')
    setOrtho(!orthoView);
  else
    return;

  e.preventDefault();
});

const fwd = new THREE.Vector3();
const right = new THREE.Vector3();
const move = new THREE.Vector3();

// Unity-style boost: Shift starts at 3x and keeps accelerating while you keep moving (up to 40x); releasing Shift or stopping resets it.
let boost = 1;

function fly(dt)
{
  const shift = keys.has('ShiftLeft') || keys.has('ShiftRight');
  if (!keys.size) { boost = 1; return; }
  camera.getWorldDirection(fwd);
  right.crossVectors(fwd, camera.up).normalize();
  move.set(0, 0, 0);

  if (keys.has('KeyW'))
    move.add(fwd);

  if (keys.has('KeyS'))
    move.sub(fwd);

  if (keys.has('KeyD'))
    move.add(right);

  if (keys.has('KeyA'))
    move.sub(right);

  if (keys.has('KeyE'))
    move.y += 1;

  if (keys.has('KeyQ'))
    move.y -= 1;

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

function frameBox(box, dir = new THREE.Vector3(0.6, 0.5, 0.8))
{
  if (box.isEmpty())
    return;

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
function frameLevel(world, markers)
{
  world.updateMatrixWorld(true);

  const pts = [];
  const v = new THREE.Vector3();

  for (const o of world.children) {
    if (o === markers || o === sky)
      continue;

    if (o.name === 'terrain')
    {
      // The map itself, not the edge tiles repeated around it.
      o.geometry.computeBoundingBox();

      const { hmp } = o.userData.tiles;
      const g = o.geometry.boundingBox;
      const b = new THREE.Box3(new THREE.Vector3(0, g.min.y, 0), new THREE.Vector3(hmp.w * 2, g.max.y, hmp.h * 2)).applyMatrix4(o.matrixWorld);

      pts.push(b.min.clone(), b.max.clone(), b.getCenter(new THREE.Vector3()));
    }
    else {
      pts.push(o.getWorldPosition(v).clone());
    }
  }

  if (!pts.length)
    return frame(world);

  const q = (axis, f) => pts.map((p) => p[axis]).sort((a, b) => a - b)[Math.min(pts.length - 1, Math.floor(f * (pts.length - 1)))];
  const box = new THREE.Box3(new THREE.Vector3(q('x', 0.02), q('y', 0.02), q('z', 0.02)), new THREE.Vector3(q('x', 0.98), q('y', 0.98), q('z', 0.98)));

  // Small scenes (frontend bridge/starwars) place every model at one point, which gives an empty box; frame their geometry instead.
  if (pts.length < 20 || box.getSize(v).length() < 1) {
    box.makeEmpty();

    for (const o of world.children)
      if (o !== markers && o !== sky)
        box.expandByObject(o);
  }

  frameBox(box);
}

const clock = new THREE.Clock();
let spin = 0;
let sky = null;
let skyColor = null;
const skyPos = new THREE.Vector3();
const SKY_DROP = 38;

// The frame loop calls into every script, so it waits for the last one to load (index.html loads them in order).
const scriptsLoaded = new Promise((ok) => addEventListener('rs64-scripts-loaded', ok, { once: true }));

Promise.all([ready, scriptsLoaded]).then(() => renderer.setAnimationLoop(() => {
  const dt = Math.min(clock.getDelta(), 0.1);

  fly(dt);
  applyAnim(dt);
  applyExhaust(dt);
  applyLamps(dt);
  applyEffect(dt);
  lightsFxTick();
  lightsPreviewTick();

  // About 0.1 rad/s per slider step, roughly OrbitControls' old autoRotate pace.
  if (spin && !selectedLevel && root?.name === 'spin')
    root.rotation.y += spin * 0.1 * dt;

  controls.update();
  applyCutscene(dt);
  applyMedalEnv(cut ? cut.t * cut.data.length : clock.elapsedTime);
  sizeCorners();
  updateCutSfxPositions();
  updateFog();

  // The dome rim sits 38 units above its origin (x2 scale); drop it so the rim meets the horizon. It is drawn first without depth, so a
  // short far plane (a cutscene's fog distance) only shrinks it around the camera, as the game draws its sky past the terrain's clip.
  if (sky && sky.parent)
  {
    const s = Math.min(2, (camera.far * 0.9) / (sky.userData.reach * sky.parent.matrixWorld.getMaxScaleOnAxis()));

    sky.scale.setScalar(s);
    sky.position.copy(sky.parent.worldToLocal(skyPos.copy(camera.position))).y += SKY_DROP * s;
  }

  // Cutscenes drive the lens themselves.
  if (orthoView && cut)
    setOrtho(false);

  orthoClip();
  updateViewNav();
  renderer.render(scene, camera);
  frameLoopRunning = true;
}));

// In Objects / Events mode the box describes the hovered item, else the selection; otherwise the view's stats.
// The level's stats when viewing; while editing, the selected or hovered object's details in the side box instead.
function hud()
{
  const o = (editMode === 'objects' || editMode === 'events') && (editHovered ?? sel?.o);
  const rows = o ? editHudRows(o) : (editMode ? [] : Object.entries(stats));

  hudBox($('hud'), editMode ? [] : rows);
  hudBox($('selHud'), editMode ? rows : []);
}

function hudBox(box, rows)
{
  const text = rows.map(([k, v]) => `${k.padEnd(10)} ${typeof v === 'number' ? v.toLocaleString() : v}`).join('\n');

  if (box.textContent === text)
    return;

  box.textContent = text;

  // An explicit width from the text, so a longer row can animate the box (CSS can't transition to auto).
  const range = document.createRange();

  range.selectNodeContents(box);
  box.style.width = `${Math.ceil(range.getBoundingClientRect().width)}px`;
}

// ---------- materials ----------
const lodLevel = () => +$('optLod').value;

function setFilter(map)
{
  const mode = $('optFilter').value;
  const mips = mode === 'trilinear' || mode === 'aniso';

  map.magFilter = mode === 'nearest' ? THREE.NearestFilter : THREE.LinearFilter;
  map.minFilter = mips ? THREE.LinearMipmapLinearFilter : map.magFilter;
  map.generateMipmaps = mips;
  map.anisotropy = mode === 'aniso' ? 16 : 1;
  map.needsUpdate = true;
}

// Textures, materials and part geometry are cached; a filter change clears them all and rebuilds the view.
const texCache = new Map();
const matPool = new Map();
const partCache = new Map();
const artCache = new Map();

function clearCaches()
{
  for (const t of texCache.values())
    t.map?.dispose();

  for (const m of matPool.values())
    m.dispose();

  for (const p of partCache.values())
    p.geometry.dispose();

  for (const t of terrainCache.values()) { t.g.dispose(); t.map.dispose(); }

  for (const a of artCache.values())
    a.then((frames) => frames.forEach((f) => { f.map?.dispose(); f.modelMap?.dispose(); }));

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
    let map = null;
    let cutout = false;
    let blend = false;

    if (t && t.rgba)
    {
      map = new THREE.DataTexture(t.rgba, t.w, t.h, THREE.RGBAFormat);

      // Textures whose UVs never leave 0..1 are clamped, so tiled skies and panels don't bleed the opposite edge in at seams.
      map.wrapS = map.wrapT = clamp.has(matIndex) ? THREE.ClampToEdgeWrapping : THREE.RepeatWrapping;
      setFilter(map);

      // Soft alpha (glows, shockwaves) blends; on/off alpha (colour-keyed rails, walkways) is a cutout.
      let soft = 0;

      for (let i = 3; i < t.rgba.length; i += 4) {
        if (t.rgba[i] < 128)
          cutout = true;

        if (t.rgba[i] > 0 && t.rgba[i] < 255)
          soft++;
      }

      // Emissive alpha-format textures always blend, or opaque tiles of a set (centre of 1ade_moon) draw differently from their neighbours.
      blend = soft > t.rgba.length / 160 || (!!m.emissive && !!(t.flags & 0x800));
    }

    texCache.set(ck, {
      map,
      cutout,
      blend,
      emissive: !!m?.emissive,
      glow: !!m?.glow,
      index: matIndex,
      matName: m?.name ?? '',
      tex: t && { name: t.name, w: t.w, h: t.h, type: t.type, flags: t.flags, trans: t.trans }
    });
  }

  return texCache.get(ck);
}

// "game": models are prelit in the ROM and the game never lights them, so they are drawn unlit. "dynamic": lit by the level sun with shadows.
const gameLighting = () => $('optLight').value === 'game';

function threeMaterial(info, textured, wire)
{
  const game = gameLighting();
  const k = `${info.map ? info.map.id : 0}|${textured}|${wire}|${!!info.valpha}|${game}|${!!info.glow}`;

  if (!matPool.has(k))
  {
    const blend = (textured && info.blend) || !!info.valpha;

    // Glass (emissive, opaque texture made translucent by vertex alpha, e.g. sky_lv_01 on canopies) is a sphere-mapped reflection:
    // the models carry per-face normals for it, and the texture slides across the glass as the view turns.
    const env = textured && info.emissive && info.valpha && !info.blend;

    // A glowing material is drawn at full brightness in game, sun or shade.
    const Mat = env ? THREE.MeshMatcapNodeMaterial : (game || info.glow ? THREE.MeshBasicNodeMaterial : THREE.MeshLambertNodeMaterial);

    const mat = new Mat({
      ...(env ? { matcap: info.map } : { map: textured ? info.map : null }),
      vertexColors: true,
      side: THREE.DoubleSide,
      alphaTest: textured && info.cutout && !blend ? 0.5 : 0,
      transparent: blend,
      depthWrite: !blend,
      ...(env ? {} : { wireframe: wire }),
      // Glow effects add light: emissive soft-alpha textures (engine glows) and untextured translucent strips (exhaust, light shafts). Glass is a plain alpha blend.
      blending: blend && ((info.emissive && info.blend) || !info.map) ? THREE.AdditiveBlending : THREE.NormalBlending,
    });

    // three r170's node materials read vertex colour as RGB only, and the colour attribute's w reads back as 1, so the alpha has its own attribute.
    // Glass stores ~60% alpha but the game draws it nearly see-through; 25% is tuned by eye, not taken from the game's blend setup.
    if (env)
      mat.opacity = 0.25;
    else if (info.valpha)
      mat.opacityNode = new THREE.AttributeNode('valpha', 'float');

    if (!env && !blend && !info.glow)
      rtLit(mat);

    matPool.set(k, mat);
  }

  return matPool.get(k);
}

// One geometry per part (shared by every placement of it) with a group per material.
function partGeometry(part, model, key)
{
  const ck = key + ':' + part.offset;

  if (partCache.has(ck))
    return partCache.get(ck);

  const byMat = new Map();

  for (const f of part.faces) {
    if (!byMat.has(f.mat))
      byMat.set(f.mat, []);

    byMat.get(f.mat).push(f);
  }

  // vidx: the part vertex behind each buffer position, so effects that move part vertices (engine flicker) can rewrite them.
  const pos = [];
  const uv = [];
  const col = [];
  const groups = [];
  const vidx = [];
  const nidx = [];
  let tris = 0;

  for (const [mat, faces] of byMat)
  {
    const start = pos.length / 3;
    let valpha = false;

    // Faces without vertex colours are not prelit; the game lights them from the material, whose +4 value is the grey level (tow cable 0.2).
    const d = model.hmt.materials[mat]?.diffuse;
    const g = d > 0 ? Math.round(Math.min(d, 1) * 255) : 255;
    const unlit = ((g << 24) | (g << 16) | (g << 8) | 255) >>> 0;

    for (const f of faces)
    {
      const order = f.idx.length === 4 ? [0, 1, 2, 0, 2, 3] : [0, 1, 2];

      // Vertex alpha is opacity (canopy glass, exhaust, light shafts). On ordinary materials all-zero alpha means "unset" (~3.5k faces); on emissive ones (exhaust cones) zero really is see-through.
      // The medals store alpha 0xE0 but draw opaque (render mode 0x00552230; translucency needs mesh flags 0x20000|0x40000).
      const alphaSet = f.colors && !MEDAL_ENV[key] && (f.colors.some((c) => c & 255) || !!model.hmt.materials[mat]?.emissive);

      for (const k of order)
      {
        const vi = f.idx[k];

        pos.push(part.verts[vi * 3], part.verts[vi * 3 + 1], part.verts[vi * 3 + 2]);
        vidx.push(vi);
        nidx.push(f.flags & 4 && f.nidx ? f.nidx[k] : -1);
        uv.push(f.uvs ? f.uvs[k][0] : 0, f.uvs ? f.uvs[k][1] : 0);

        const c = f.colors ? (f.colors[k] ?? f.colors[0]) : unlit;
        const a = alphaSet ? (c & 255) / 255 : 1;

        if (a < 1)
          valpha = true;

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
function placementRotation(rot)
{
  // rotX, rotY, rotZ applied in that order with the game's signs: the hangar ship slots (stored as 180/a/180 or 0/a/0) then all face the
  // hangar centre; the old negated angles turned each one by 2a.
  return new THREE.Matrix4().makeRotationFromEuler(new THREE.Euler(...rot.map((d) => THREE.MathUtils.degToRad(d)), 'ZYX'));
}

// Node pose from the HOB rest values, or from an animation's rotation/translation when given (translation in vertex units).
const _one = new THREE.Vector3(1, 1, 1);
const _piv = new THREE.Matrix4();
const _zAxis = new THREE.Vector3(0, 0, 1);

function poseNode(g, n, q = null, t = null)
{
  const m = n.matrix;

  g.matrix.set(m[0], m[3], m[6], m[9], m[1], m[4], m[7], m[10], m[2], m[5], m[8], m[11], 0, 0, 0, 1);

  if (!q)
  {
    const [qw, qx, qy, qz] = n.quat;

    q = n.flags & 4 ? new THREE.Quaternion(qx, qy, qz, qw).normalize() : new THREE.Quaternion();
  }

  if (!t)
    t = n.flags & 8 ? new THREE.Vector3(...n.pos).multiplyScalar(256) : new THREE.Vector3();

  // Flag 2 = node scale at +0x5C (only the AT-ST root uses it: 0.0296).
  g.matrix.premultiply(new THREE.Matrix4().compose(t, q, n.flags & 2 ? new THREE.Vector3(...n.scale) : _one));

  // Flag 1 joints (AT-AT/AT-PT legs) keep vertices in whole-model space and bend about a pivot: T(pivot) R T(-pivot).
  if (n.flags & 1)
  {
    const p = new THREE.Vector3(...n.pivot).multiplyScalar(256);

    g.matrix.multiply(_piv.makeTranslation(-p.x, -p.y, -p.z)).premultiply(_piv.makeTranslation(p.x, p.y, p.z));
  }
}

function buildObject(obj, model, key, counts)
{
  const group = new THREE.Group();

  group.name = obj.name;
  group.userData.hobObject = true;

  const lodIndex = Math.min(lodLevel(), obj.lods.length - 1);
  const lod = obj.lods[lodIndex] || [];

  group.userData.lodSlot = obj.lodSlots?.[lodIndex] ?? 0;
  counts.lods = Math.max(counts.lods || 0, obj.lods.length);

  // Joints (moving nodes, such as wngrot_a) keep their names, so the inspect card and an exported .glb show what an import must use.
  const jointName = new Map([...(obj.nodeNames ?? [])].map(([name, index]) => [index, name]));

  const nodes = obj.nodes.map((n, i) => {
    const g = new THREE.Group();

    g.name = jointName.get(i) ?? '';
    g.matrixAutoUpdate = false;
    poseNode(g, n);

    (lod[i] || []).forEach((p, j) => {
      const part = partGeometry(p, model, key);
      const mesh = new THREE.Mesh(part.geometry, part.infos.map((info) => threeMaterial(info, $('optTex').checked, $('optWire').checked)));

      mesh.userData.infos = part.infos;
      mesh.userData.shared = true;

      // node and j (its place in the node's part list) name it in the game's mesh registry (hobMeshKey), for light presets.
      mesh.userData.hob = { part: p, vidx: part.vidx, node: i, j };
      mesh.name = p.name;

      const envK = MEDAL_ENV[key]?.[i];

      if (envK && p.normals)
      {
        mesh.userData.env = { k: envK, ending: key.endsWith('end_medal'), nidx: part.nidx };
        envMeshes.add(mesh);

        // The file UVs stay in 0..1 (so the map was set to clamp) but the env map's reach k * 2.
        for (const info of part.infos)
          if (info.map && info.map.wrapS !== THREE.RepeatWrapping) {
            info.map.wrapS = info.map.wrapT = THREE.RepeatWrapping;
            info.map.needsUpdate = true;
          }
      }

      counts.tris += part.tris;
      counts.verts += part.verts;
      counts.parts++;
      g.add(mesh);
    });

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
const _em = new THREE.Matrix4();
const _enm4 = new THREE.Matrix4();
const _enm = new THREE.Matrix3();
const _eP = new THREE.Vector3();
const _eN = new THREE.Vector3();
const _eE = new THREE.Vector3();

function applyMedalEnv(time) {
  for (const mesh of envMeshes)
  {
    let root = mesh;

    while (root.parent && !root.userData.hobObject)
      root = root.parent;

    let top = root;

    while (top.parent)
      top = top.parent;

    if (top !== scene) {
      if (!mesh.parent)
        envMeshes.delete(mesh);

      continue;
    }

    const { k, ending, nidx } = mesh.userData.env;
    const part = mesh.userData.hob.part;
    const vidx = mesh.userData.hob.vidx;

    // shortcut: the game's angle counters are taken as 30 per second (end_medal multiplies 0x800B0B2C by 30); the menu medal's E is in its model units.
    const d = THREE.MathUtils.degToRad(time * 30);

    if (ending)
      _eE.set(50 * Math.cos(1.1 * d), 50 * Math.sin(1.2 * d), 10 * Math.sin(1.3 * d));
    else
      _eE.set(0.9 * Math.cos(1.1 * d), 0.8 * Math.sin(1.3 * d), 6 + 0.5 * Math.sin(d));

    // In the ceremony M is the medal's cutscene world matrix (*(0x800B1900) + 0x21DC), in world units (the mesh's 1/256 included);
    // elsewhere object space, the node chain below the HOB object (vertex units, so the game's /256 applies).
    let space = root;

    if (ending && cut)
      while (space.parent && space.parent !== scene)
        space = space.parent;

    _em.copy(space.matrixWorld).invert().multiply(mesh.matrixWorld);

    if (space !== root)
      _em.premultiply(_enm4.makeScale(256, 256, 256));

    _enm.getNormalMatrix(_em);

    const uv = mesh.geometry.attributes.uv;
    const n = part.normals;
    const v = part.verts;

    for (let j = 0; j < nidx.length; j++)
    {
      const ni = nidx[j];

      if (ni < 0)
        continue;

      _eN.set(n[ni * 3], n[ni * 3 + 1], n[ni * 3 + 2]).applyMatrix3(_enm).normalize();

      const vi = vidx[j];

      _eP.set(v[vi * 3], v[vi * 3 + 1], v[vi * 3 + 2]).applyMatrix4(_em).multiplyScalar(1 / 256).sub(_eE).normalize();
      _eP.addScaledVector(_eN, -2 * _eN.dot(_eP));
      uv.setXY(j, k * (1 - _eP.x), k * (1 - _eP.z));
    }

    uv.needsUpdate = true;

    const tint = MEDAL_TINT[$('optMedal').value] ?? [255, 255, 255];

    // In the ceremony the tint's alpha (tickCutsceneCameraFromTimeline) fades the medal in over frames 170-200 and out over 390-420.
    const f = cut ? cut.t * cut.data.frames : 300;
    const alpha = ending ? THREE.MathUtils.clamp(Math.min((f - 170) / 30, (420 - f) / 30), 0, 1) : 1;

    for (const m of [mesh.material].flat()) {
      if (m.map || m.matcap)
        m.color.setRGB(tint[0] / 255, tint[1] / 255, tint[2] / 255);

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
let anim = null;
let animClock = 0;
let spriteRestore = null;

// Sprite-animated NPCs swap their texture per frame. 12 fps is the rate the spawn code (0x80065260) passes for the 10-frame sets.
const SPRITE_FPS = 12;

const SPRITE_ANIMS = {
  'gnrc_stuff/civilian': ['civ_anm_run'],
  'imp_stuff/trooper': ['trp_anm_run', 'trp_anm_sht', 'blk_anm_run'],
  'reb_stuff/r_pilot': ['r_pilot_anm']
};

const _qa = new THREE.Quaternion();
const _qb = new THREE.Quaternion();

function sampleTrack(times, phase) {
  let k = 0;

  while (k < times.length - 1 && times[k + 1] <= phase)
    k++;

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
  [
    /pl_crafts\/awing$/,
    [[
      'bank_sweep',
      2,
      stickSweep,
      [
        ['f_trns_l', 'y', (d) => d.x * -10.125],
        ['f_trns_r', 'y', (d) => d.x * -10.125],
        ['rot_a', 'y', (d) => d.x * -15.75],
        ['rot_b', 'y', (d) => d.x * -15.75]
      ]
    ]]
  ],
  [/pl_crafts\/(vwing|koelsch|rmobil)$/, [['bank_sweep', 2, stickSweep, [['f_trns_l', 'x', (d) => 45 * d.x], ['f_trns_r', 'x', (d) => -45 * d.x]]]]],
  // botrns: a hold timer 0..0.5 s -> -180 * t deg; uptrns: the turned-toward side eases up to 45 deg.
  [
    /pl_crafts\/snowspeeder$/,
    [
      ['brake_left', 1, (p) => ({ t: 0.5 * (1 - Math.abs(2 * p - 1)) }), [['botrns_l', 'x', (d) => -180 * d.t]]],
      ['brake_right', 1, (p) => ({ t: 0.5 * (1 - Math.abs(2 * p - 1)) }), [['botrns_r', 'x', (d) => -180 * d.t]]],
      // uptrns are plain nodes with a rest orientation (flags 12): the generic sign flip turned them the wrong way, so their angle is pre-negated.
      [
        'bank_sweep',
        2,
        stickSweep,
        [
          ['uptrns_l', 'x', (d) => (d.x < 0 ? -45 * (1 - Math.cos((-d.x * Math.PI) / 2)) : 0)],
          ['uptrns_r', 'x', (d) => (d.x > 0 ? -45 * (1 - Math.cos((d.x * Math.PI) / 2)) : 0)]
        ]
      ],
    ]
  ],
  [/gnrc_stuff\/powerup$/, [['spin', 4, (p) => ({ a: 360 * p, b: Math.sin(8 * Math.PI * p) }), [['rot_a', 'y', (d) => d.a, (d) => 0.5 * d.b]]]]],
  // Radar dish at speed scalar 1 (30 deg/s); its target aiming is not a clip.
  [/gnrc_stuff\/radardish$/, [['spin', 12, (p) => ({ a: 360 * p }), [['dish', 'y', (d) => d.a]]]]],
];

const _axes = { x: new THREE.Vector3(1, 0, 0), y: new THREE.Vector3(0, 1, 0), z: _zAxis };

// The game rebuilds the node matrix with the angle added in node space. Like the S-foils, a plain node takes it on its matrix with the
// sign flipped (row- vs column-vector rotation); a pivot (flag 1) node takes it inside the pivot with the game's sign (as the shuttle).
function poseProc(g, n, axis, deg, dy = 0)
{
  const r = new THREE.Quaternion().setFromAxisAngle(_axes[axis], THREE.MathUtils.degToRad(n.flags & 1 ? deg : -deg));

  if (n.flags & 1)
  {
    const [qw, qx, qy, qz] = n.quat;

    poseNode(g, n, (n.flags & 4 ? new THREE.Quaternion(qx, qy, qz, qw).normalize() : new THREE.Quaternion()).multiply(r));
  }
  else
  {
    poseNode(g, n);
    g.matrix.multiply(new THREE.Matrix4().makeRotationFromQuaternion(r));
  }

  if (dy)
    g.matrix.premultiply(new THREE.Matrix4().makeTranslation(0, dy * 256, 0));
}

// The other shuttle models carry the same wngrot hinge nodes, so they take the traced shuttle rule.
const foilKind = (id) => (/xwing|xwng/.test(id) ? 'xwing' : (/imp_stuff\/(shuttle|hvyshut|bosshut)/.test(id) ? 'shuttle' : null));

function setupFoils(id, objGroup)
{
  const rig = objGroup?.userData.rig;
  const names = objGroup && modelCache.get(id)?.hob.objects.find((o) => o.name === objGroup.name)?.nodeNames;
  const a = names?.get('wngrot_a');
  const b = names?.get('wngrot_b');
  const kind = foilKind(id);

  foils = kind && rig && a != null && b != null ? { rig, a, b, kind } : null;
  $('foilsRow').hidden = !foils;

  if (foils)
    $('foilsLabel').textContent = kind === 'shuttle' ? 'Wings lowered' : 'S-foils closed';

  applyFoils();
}

// Engine flicker (func_80053C04 setup, func_80053F7C per frame): each frame one of eng_a/eng_b (alternating) gets its leading vertices'
// z set to rest z - rand() % N, walking from vertex 0 while z stays within 16 of vertex 0's; N = t * 1024 + 1 with
// t = max(0, (speed + 0.5 - lo) / (hi - lo)). Per-craft (lo, hi) from the game's table.
const EXHAUST_RANGE = {
  awing: [3, 5.625],
  vwing: [3, 4.5],
  xwing: [1.875, 3.75],
  ywing: [2.25, 3.375],
  snowspeeder: [2.625, 3.5625],
  falcon: [2.25, 5.625],
  tie_inter: [2.625, 4.5],
  koelsch: [2.25, 6],
  t16: [2.25, 6]
};

let exhaust = null;
let exhaustClock = 0;
let exhaustParity = 0;

function setupExhaust(objGroup) {
  if (exhaust)
    for (const e of exhaust.engines)
      writeEngineZ(e, (i) => e.rest[i]);

  const range = objGroup && EXHAUST_RANGE[objGroup.name];
  const engines = [];

  objGroup?.traverse((o) => {
    const h = o.userData.hob;

    if (!h || !/^eng_[ab]$/.test(o.name))
      return;

    // Rest z of the leading run of vertices the game rewrites.
    const v = h.part.verts;
    const rest = [v[2]];

    for (let i = 1; i < 16 && i * 3 + 2 < v.length && v[i * 3 + 2] - v[2] < 16; i++)
      rest.push(v[i * 3 + 2]);

    engines.push({ mesh: o, vidx: h.vidx, rest, name: o.name });
  });

  engines.sort((a, b) => a.name.localeCompare(b.name));
  exhaust = range && engines.length ? { engines, range } : null;
  $('speedRow').hidden = !exhaust;
}

function writeEngineZ(e, zOf) {
  const pos = e.mesh.geometry.attributes.position;

  for (let j = 0; j < e.vidx.length; j++)
    if (e.vidx[j] < e.rest.length)
      pos.setZ(j, zOf(e.vidx[j]));

  pos.needsUpdate = true;
}

function applyExhaust(dt)
{
  if (!exhaust)
    return;

  exhaustClock += dt;

  if (exhaustClock < 1 / 30)
    return;

  exhaustClock = 0;

  const [lo, hi] = exhaust.range;
  const speed = +$('optSpeed').value;
  const n = Math.floor(Math.max(0, (speed + 0.5 - lo) / (hi - lo)) * 1024 + 1);
  const e = exhaust.engines[exhaustParity++ & 1] ?? exhaust.engines[0];

  writeEngineZ(e, (i) => e.rest[i] - Math.floor(Math.random() * n));
}

function applyFoils(v = +$('optFoils').value)
{
  if (!foils)
    return;

  // X-wing: the game writes the angle into the row-vector node matrix and three's column-vector rotation runs the other way.
  // Shuttle: the angle goes inside the pivot, where the game's sign carries over unflipped (wings fold down outward).
  const deg = foils.kind === 'shuttle' ? 55 * (1 - Math.cos(v * Math.PI)) : (1 - v) * 10;

  for (const [i, sign] of [[foils.a, 1], [foils.b, -1]])
  {
    const g = foils.rig.nodes[i];
    const q = new THREE.Quaternion().setFromAxisAngle(_zAxis, THREE.MathUtils.degToRad(sign * deg));

    // The shuttle hinges are flag-1 pivot nodes, so the rotation goes inside the pivot; the X-wing's go on the node matrix.
    if (foils.kind === 'shuttle')
      poseNode(g, foils.rig.src[i], q);
    else
    {
      poseNode(g, foils.rig.src[i]);
      g.matrix.multiply(new THREE.Matrix4().makeRotationFromQuaternion(q));
    }
  }
}

function poseProcAt(a, phase) {
  const d = a.proc[2](phase);

  for (const [i, axis, angle, dy] of a.nodes)
    poseProc(a.rig.nodes[i], a.rig.src[i], axis, angle(d), dy?.(d));
}

function applyAnim(dt) {
  if (anim?.sprite)
  {
    animClock += dt * +$('optAnimSpeed').value;

    const f = anim.sprite[Math.floor(animClock * SPRITE_FPS) % anim.sprite.length];

    if (anim.map.image.data !== f.rgba) {
      anim.map.image.data = f.rgba;
      anim.map.needsUpdate = true;
    }

    return;
  }

  if (anim?.foil)
  {
    // Drives the wings slider, so the slider shows where the sweep is.
    animClock += dt * +$('optAnimSpeed').value;

    const [from, to] = anim.foil;
    const v = from + (to - from) * ((animClock / anim.seconds) % 1);

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

  if (!anim || !anim.rig)
    return;

  animClock += dt * +$('optAnimSpeed').value;
  poseTracks(anim.rig, anim.data, (animClock / anim.seconds) % 1);
}

// Poses a skeletal animation at a phase (0..1 over the clip): one track per rig node after the root.
function poseTracks(rig, data, phase) {
  data.tracks.forEach((tr, i) => {
    const g = rig.nodes[i + 1];
    const n = rig.src[i + 1];

    if (!tr || !g)
      return;

    const [a, b, f] = sampleTrack(tr.times, phase);
    let q = null;
    let t = null;

    if (tr.rot)
      q = new THREE.Quaternion().slerpQuaternions(_qa.set(...tr.rot[a]), _qb.set(...tr.rot[b]), f).normalize();

    if (tr.pos)
      t = new THREE.Vector3(...tr.pos[a]).lerp(new THREE.Vector3(...tr.pos[b]), f);

    poseNode(g, n, q, t);
  });
}

function applyMaterialOptions() {
  if (!root)
    return;

  root.traverse((o) => {
    if (o.isMesh && o.userData.infos)
      o.material = o.userData.infos.map((i) => threeMaterial(i, $('optTex').checked, $('optWire').checked));

    if (o.isMesh && o.userData.terrain)
    {
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

// Named objects after a verb: one in quotes on the line, several as a bulleted list under "verb:".
const listed = (verb, names) => (names.length === 1 ? `${verb} '${names[0]}'` : `${verb}:` + names.map((n) => `
• '${n}'`).join(''));

const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
const TEX_TYPES = ['CI4', 'CI8', 'RGB16', 'RGB(A)32', 'Gray 4/8', 'Gray 8'];

// A condition in words ("When 'X' is entered", from gateWords) as a card line: "Only once 'X' is entered".
// A condition (gateWords: "When 'X' is entered") joined onto what sets the item off: ", after 'X' is entered" ('' with none). "after", not
// "once": the item waits for it, it does not fire only one time.
const afterWords = (w) => (w ? ', ' + w.replace(/^(When|After) /, 'after ') : '');

// What an item does, in words; trigger targets and voice lines are filled in by showLevel.
function describe(it)
{
  if (it.type === 6)
  {
    let s = (TYPES[markerKind(it)]?.tip ?? 'Zone').replaceAll('; ', ';\n');

    if (/LevelSett|LevelDefa/.test(it.name))
      s += ". Sits off the map; likely the level's default music settings";

    if (it.linkedFrom && !it.label)
      s += '. ' + listed('Used by', it.linkedFrom);

    return s;
  }

  if (it.type === 3)
    return it.users?.length ? listed('Path followed by', it.users) : (it.pathKind ? `Path with no NPC naming it; grouped with ${TYPES[it.pathKind].label.toLowerCase()} by its name` : 'Path that no NPC names (a level script may switch an NPC onto it)');

  if (it.type === 7)
  {
    const lines = [];

    if (it.radius > 0 && (it.sub === 0x11 || it.sub === 0x17))
    {
      // npcProbeDriod / func_800F4E74: the NPC wanders inside the radius (probe droids subtract their own size) and heads back to the centre when outside.
      lines.push(`Roam area${it.targets?.length ? ' for ' + it.targets.join(', ') : ''}: wanders within ${it.radius.toFixed(1)} units of the centre and turns back when it strays out`);

      return lines.join('\n');
    }

    if (it.waypoint)
    {
      const w = it.waypoint;

      lines.push(`The HUD arrow points at the nearest active one of these${w.follow ? `, following ${w.follow}` : ''}`);

      return lines.join('\n');
    }

    if (it.pathEvent)
    {
      // func_800F3CB0 writes 0x1B/0x1C/0x23 into the path follower; 0x1A/0x22/0x4E/0x20 are channel-9 orders to the NPC's own handler.
      const e = it.pathEvent;
      const sc = (x) => +x.toFixed(2);
      const w = e.w;
      const fl = e.fl;

      lines.push(`When an NPC passes this point${afterWords(it.needWords)}.`);

      // 0x22 hold (func_800F9AB8 + func_800F410C): stop while the timer runs, flag w[1] != w[2], or counter w[3] < w[4].
      const hold = () => {
        const until = [w[1] && `flag #${w[1]} = ${w[2]}`, w[3] && `counter #${w[3]} ≥ ${w[4]}`].filter(Boolean);
        const time = fl[0] >= 9000 ? 'Stops here for good' : (fl[0] > 0 ? `Stops here for ${sc(fl[0])} s` : 'Stops here');

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

      if (what)
        lines.push(what);

      const act = [
        e.addCounter && `counter #${e.addCounter} ${e.amount >= 0 ? '+' : '−'}= ${Math.abs(e.amount)}`,
        e.setFlag && `flag #${e.setFlag} = ${e.value}`
      ].filter(Boolean);

      if (act.length)
        lines.push('Also ' + act.join(', '));

      return lines.join('\n');
    }

    if (it.activators)
    {
      // func_800AA6D0 / func_80065980: an activator entering/leaving the area adjusts mission flags and counters,
      // which the level's own script (lvN_checkComplexObjectives) reacts to.
      const who = it.activators.map((a) => (a === 'ply' ? 'the player' : `'${a}'`));
      const near = it.cylinder ? 'flies over it' : `comes within ${it.radius.toFixed(1)} units`;

      const after = afterWords(it.needWords);

      lines.push(who.length === 1 ? `Triggers when ${who[0]} ${near}${after}` : `Triggers when one of these ${near}${after}:` + who.map((w) => `\n• ${w}`).join(''));

      // Enter/exit flags are just set / cleared, so only counter changes are worth listing.
      const act = (b) => {
        const c = `'counter #${b.counter}'`;

        if (!b.counter)
          return '';

        if (Math.abs(b.add) === 1)
          return `${b.add > 0 ? 'Increments' : 'Decrements'} ${c}`;

        return b.add >= 0 ? `Adds ${b.add} to ${c}` : `Subtracts ${-b.add} from ${c}`;
      };

      if (act(it.onEnter))
        lines.push('On enter: ' + act(it.onEnter));

      if (act(it.onExit))
        lines.push('On exit: ' + act(it.onExit));
    }
    else if (it.targets?.length) {
      lines.push('Linked to ' + it.targets.join(', '));
    }

    // Many voice ids are empty slots (no recording, a blank subtitle), lines never recorded or cut.
    if (it.voice?.trim())
      lines.push(`Name points to voice line ${it.voiceId}: "${it.voice}"`);
    else if (it.voice != null)
      lines.push(`Name points to voice line ${it.voiceId} (empty)`);

    return lines.join('\n') || (SUBTYPES[it.sub] ?? 'Trigger') + ' (no activators or actions decoded for this subtype yet)';
  }

  return SUBTYPES[it.sub] ?? 'subtype ' + hex(it.sub);
}

// The item's category and subtype names; one when the category is just the subtype's plural.
function kindParts(it)
{
  const cat = TYPES[markerKind(it)]?.label ?? 'Type ' + it.type;
  const sub = SUBTYPES[it.sub] ?? hex(it.sub);

  return cat.toLowerCase().replace(/s$/, '') === sub.toLowerCase() ? [sub] : [cat, sub];
}

// The wiki's level reference has a page per legend group; path events extend the Paths page.
const EVENT_HELP_PAGES = {
  'Starts and cameras': 'Level-Starts-and-Cameras',
  Paths: 'Level-Paths',
  'Trigger areas': 'Level-Trigger-Areas',
  'Path events': 'Level-Paths',
  Regions: 'Level-Regions'
};

// A marker kind's section there, by its legend label (GitHub's heading anchors). Every per-unit path row is the same thing, so they share one section.
function eventHelpOfKind(kind)
{
  const k = legendKindOf(kind);
  const isPath = k === '3' || k.startsWith('path:');
  const group = isPath ? 'Paths' : LEGEND_GROUPS.find(([, kinds]) => kinds?.includes(k))?.[0];
  const label = k.startsWith('path:') ? 'Flight paths' : TYPES[k]?.label;

  return group && label ? `${GUIDE_URL(EVENT_HELP_PAGES[group])}#${label.toLowerCase().replace(/[^a-z0-9 -]/g, '').replaceAll(' ', '-')}` : null;
}

const eventHelpUrl = (it) => eventHelpOfKind(String(markerKind(it)));

function eventHelpLink(it)
{
  const a = elem('a', { className: 'eventHelp', href: eventHelpUrl(it), target: '_blank', rel: 'noopener', title: 'What this is and how to use it' });

  a.setAttribute('aria-label', a.title);
  a.innerHTML = iconImg('help');

  return a;
}

function itemRows(it)
{
  // A mission camera turns to face its target every frame; its stored rotation only counts until the target appears, and the game never reads its scale.
  const camera = it.type === 0 && it.sub === 0x26;
  const aims = camera && it.model && it.model !== '-';

  return [
    ['What it is', esc(describe(it)).replaceAll('\n', '<br>')],
    ['Kind', kindParts(it).join(' · ')],
    it.model && (!camera || aims) && [aims ? 'Aims at' : 'Reference', esc(it.model)],
    [
      it.recordPos ? 'Position (node)' : 'Position',
      fmt(it.pos)
    ],
    it.recordPos && ['Record position', fmt(it.recordPos)],
    it.rot && [aims ? 'Rotation° until its target appears' : 'Rotation°', fmt(it.rot)],
    it.scale && !camera && ['Scale', fmt(it.scale)],
    it.size && [
      'Footprint',
      it.size[0].toFixed(1) + ' × ' + it.size[2].toFixed(1)
    ],
    it.points && ['Spline points', it.points.length]
  ];
}

// ---------- level markers ----------
const markerMat = new Map();

const lineMat = (type, opacity = 1) => {
  const k = type + '|' + opacity + '|' + TYPES[type]?.color;

  if (!markerMat.has(k))
    markerMat.set(k, new THREE.LineBasicMaterial({ color: TYPES[type]?.color ?? 0xffffff, transparent: opacity < 1, opacity, fog: false }));

  return markerMat.get(k);
};

const roamMat = new THREE.LineDashedMaterial({ color: 0xd17aff, dashSize: 0.9, gapSize: 0.6, fog: false });
const attackLineMat = new THREE.LineDashedMaterial({ color: 0xff3b30, dashSize: 0.5, gapSize: 0.3, fog: false });
const attackHeadMat = new THREE.MeshBasicMaterial({ color: 0xff3b30, fog: false });

const meshMat = (type) => {
  const k = 'm' + type + '|' + TYPES[type]?.color;

  if (!markerMat.has(k))
    markerMat.set(k, new THREE.MeshBasicMaterial({ color: TYPES[type]?.color ?? 0xffffff, wireframe: true, fog: false }));

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
  cross: new THREE.BufferGeometry().setFromPoints([
    [-0.6, 0, 0],
    [0.6, 0, 0],
    [0, -0.6, 0],
    [0, 0.6, 0],
    [0, 0, -0.6],
    [0, 0, 0.6]
  ].map((p) => new THREE.Vector3(...p))),
};

// 3D marker per trigger kind, matching the legend: ball = voice line, cube = spawn, down cone = HUD arrow target, small ball = roam area.
const TRIGGER_GEO = { tvoice: GEO.ball, tspawn: GEO.cube, twaypoint: GEO.downCone, troam: new THREE.IcosahedronGeometry(0.2, 0) };

// The ring's plane faces along the path at the node, so it reads as a bead threaded on the spline. Rotated per marker in the geometry
// (not the mesh) so the marker's link lines keep their positions.
const beadGeometry = (tangent) => (tangent ? GEO.bead.clone().applyQuaternion(new THREE.Quaternion().setFromUnitVectors(new THREE.Vector3(0, 0, 1), tangent)) : GEO.bead);

const BEAD_RADIUS = 0.44;
const _beadAt = new THREE.Vector3();
const _beadScale = new THREE.Vector3();

// A bead is a ring with its path running through the hole: it is hit anywhere inside its outer edge.
function beadRaycast(raycaster, hits)
{
  const r = BEAD_RADIUS * Math.abs(this.getWorldScale(_beadScale).x);

  this.getWorldPosition(_beadAt);

  if (raycaster.ray.distanceSqToPoint(_beadAt) <= r * r)
    hits.push({ distance: raycaster.ray.origin.distanceTo(_beadAt), point: _beadAt.clone(), object: this });
}

// Stretches a link line (and an attack order's arrowhead) from its marker, now at origin, to the point it links to.
function aimLink(link, origin)
{
  const end = new THREE.Vector3(...link.userData.to).sub(origin);
  const g = link.geometry;
  const head = link.children[0];

  g.attributes.position.setXYZ(1, end.x, end.y, end.z);
  g.attributes.position.needsUpdate = true;
  g.computeBoundingSphere();

  if (!head)
    return;

  g.attributes.lineDistance.setX(1, end.length());
  g.attributes.lineDistance.needsUpdate = true;
  end.normalize();
  head.position.set(...g.attributes.position.array.slice(3, 6)).addScaledVector(end, -0.3);
  head.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0), end);
}

// A line whose points change while it is shown (drag outlines). The WebGPU renderer keeps the vertex buffer a line was first drawn with,
// and a swapped or empty geometry fails the whole frame, so the buffer is allocated once and rewritten in place.
const OUTLINE_POINTS = 4200;

function outlineLine(mat)
{
  const g = new THREE.BufferGeometry().setAttribute('position', new THREE.BufferAttribute(new Float32Array(OUTLINE_POINTS * 3), 3));
  const line = new THREE.Line(g, mat);

  g.setDrawRange(0, 1);
  line.frustumCulled = false;
  line.raycast = () => {};

  return line;
}

function setOutline(line, pts)
{
  const a = line.geometry.attributes.position;
  const n = Math.min(pts.length, OUTLINE_POINTS);

  for (let i = 0; i < n; i++)
    a.setXYZ(i, pts[i].x, pts[i].y, pts[i].z);

  a.needsUpdate = true;
  line.geometry.setDrawRange(0, Math.max(n, 1));
}

// zoneSteps: a zone's grid steps per side (drapeZone keeps the count it was built with, so its vertex buffers keep their size).
function makeMarker(it, zoneSteps = null) {
  const [x, y, z] = it.pos;
  let mk;

  if (it.type === 6)
  {
    // Zones are 2D footprints from pos to pos + size (X/Z); height is ignored (npcGroundTroopUpdate scatters units over
    // [pos, pos+size] and takes Y from the terrain). Drawn as an outline + see-through fill draped on the ground.
    const kind = markerKind(it);
    const rot = placementRotation(it.rot);
    const sx = it.size[0];
    const sz = it.size[2];
    const n = zoneSteps ?? Math.max(2, Math.ceil(Math.max(Math.abs(sx), Math.abs(sz)) / 0.5));

    const at = (u, v) => {
      const p = new THREE.Vector3(u * sx, 0, v * sz).applyMatrix4(rot);

      return new THREE.Vector3(p.x, levelHeight(x + p.x, z + p.z) - y - 0.15, p.z);
    };

    const ring = [];

    for (const [u0, v0, u1, v1] of [[0, 0, 1, 0], [1, 0, 1, 1], [1, 1, 0, 1], [0, 1, 0, 0]])
    {
      for (let i = 0; i < n; i++)
        ring.push(at(u0 + (u1 - u0) * i / n, v0 + (v1 - v0) * i / n));
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
  }
  else if (it.type === 3) {
    mk = new THREE.LineSegments(GEO.cross, lineMat(markerKind(it)));

    if (it.points?.length > 1)
    {
      const pts = it.points.map((p) => new THREE.Vector3(p[0] - x, p[1] - y, p[2] - z));
      const curve = new THREE.CatmullRomCurve3(pts, false, 'centripetal');
      const line = new THREE.Line(new THREE.BufferGeometry().setFromPoints(curve.getPoints(pts.length * 12)), lineMat(markerKind(it)));

      line.userData.item = it;
      mk.add(line);
    }
  } else if (it.type === 0)
  {
    const cam = markerKind(it) === 'cam';

    mk = new THREE.Mesh(cam ? GEO.camera : GEO.cone, meshMat(markerKind(it)));

    if (it.rot)
      mk.quaternion.setFromRotationMatrix(placementRotation(it.rot));
  }
  else
  {
    mk = new THREE.Mesh(it.pathEvent ? beadGeometry(it.nodeTangents?.[0]) : TRIGGER_GEO[markerKind(it)] ?? (it.type === 7 ? GEO.diamond : GEO.ball), meshMat(markerKind(it)));

    if (it.pathEvent)
      mk.raycast = beadRaycast;

    // The real trigger area: a wire sphere, or for cylinder triggers a ring draped on the ground.
    if (it.radius > 0)
    {
      let area;

      if (it.cylinder)
      {
        const pts = [];

        for (let k = 0; k <= 64; k++)
        {
          const a = (k / 64) * Math.PI * 2;
          const px = Math.cos(a) * it.radius;
          const pz = Math.sin(a) * it.radius;

          pts.push(new THREE.Vector3(px, levelHeight(x + px, z + pz) - y - 0.15, pz));
        }

        area = new THREE.Line(new THREE.BufferGeometry().setFromPoints(pts), lineMat(markerKind(it), 0.7));
      }
      else if (markerKind(it) === 'troam' && it.radius ** 2 > (levelHeight(x, z) - y) ** 2)
      {
        // Where a roam ball meets the ground: a dashed ring draped on it, the part ground units can walk. A ball clear of the ground draws as a sphere.
        const walk = Math.sqrt(it.radius ** 2 - (levelHeight(x, z) - y) ** 2);
        const pts = [];

        for (let k = 0; k <= 96; k++)
        {
          const px = Math.cos((k / 96) * Math.PI * 2) * walk;
          const pz = Math.sin((k / 96) * Math.PI * 2) * walk;

          pts.push(new THREE.Vector3(px, levelHeight(x + px, z + pz) - y - 0.15, pz));
        }

        area = new THREE.Line(new THREE.BufferGeometry().setFromPoints(pts), roamMat);
        area.computeLineDistances();
      }
      else
      {
        area = new THREE.LineSegments(new THREE.WireframeGeometry(new THREE.IcosahedronGeometry(it.radius, 1)), lineMat(markerKind(it), 0.25));

        // Hovered or selected, the ball fills in (stronger toward its edge) so its size reads at a glance; editHighlight shows it.
        const fill = new THREE.Mesh(new THREE.SphereGeometry(it.radius, 48, 24), areaFillMat(markerKind(it)));

        fill.visible = false;
        fill.raycast = () => {};
        fill.renderOrder = 2;
        mk.add(fill);
        mk.userData.areaFill = fill;
      }

      area.userData.item = it;
      area.userData.triggerArea = true;
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
const triggerKind = (it) => (it.sub === 0x11 || it.sub === 0x17 ? 'troam' : (it.sub === 0x19 ? 'tspawn' : (it.sub === 0x3A ? 'twaypoint' : (it.pathEvent ? PATH_KINDS[it.sub] : (it.sub !== 0x18 ? 7 : (/(?<![a-z])LINE_\d|say/i.test(it.name) ? 'tvoice' : 'tevent'))))));
const markerKind = (it) => (it.type === 2 && it.sub !== 0x51 ? 'point' : (it.type === 3 && it.pathKind ? it.pathKind : (it.type === 0 ? SPAWN_KINDS[it.sub] ?? 0 : (it.type === 6 ? ZONE_KINDS[it.sub] ?? 6 : (it.type === 7 ? triggerKind(it) : it.type)))));

// Game space is Y-down, so lifting a spline means subtracting from its y.
function applySpread(markers)
{
  const step = +$('optSpread').value;

  for (const m of markers.children)
    if (m.userData.spreadRank != null)
      m.position.y = m.userData.item.pos[1] - m.userData.spreadRank * step;
}

// Terrain height (game Y-down) at world x/z, set per level; 0 when the level has no terrain.
let levelHeight = () => 0;
const zoneFill = new Map();

function zoneFillMat(kind) {
  if (!zoneFill.has(kind))
    zoneFill.set(kind, new THREE.MeshBasicMaterial({
      color: TYPES[kind]?.color ?? 0xffffff,
      transparent: true,
      opacity: 0.18,
      depthWrite: false,
      side: THREE.DoubleSide,
      fog: false
    }));

  return zoneFill.get(kind);
}

// A trigger ball's fill: faint where it faces the camera, stronger toward the rim, so its size and shape read as a solid ball.
const areaFill = new Map();

function areaFillMat(kind) {
  if (!areaFill.has(kind))
  {
    const m = new THREE.MeshBasicNodeMaterial({ color: TYPES[kind]?.color ?? 0xffffff, transparent: true, depthWrite: false, fog: false });
    const rim = THREE.normalView.dot(THREE.positionViewDirection).abs().oneMinus().pow(2);

    m.opacityNode = THREE.float(0.06).add(rim.mul(0.5));
    areaFill.set(kind, m);
  }

  return areaFill.get(kind);
}

const hiddenTypes = new Set();

// A music region's inner and outer boxes keep their own colours but share one legend row.
const legendKindOf = (k) => (k === 'zmtrig' ? 'zmus' : k);

function applyMarkerVisibility(markers)
{
  // The selection's edit handle also sits in the group; only items' markers follow the legend.
  const items = markers.children.filter((m) => m.userData.item);
  const kinds = new Set(items.map((m) => legendKindOf(String(markerKind(m.userData.item)))));

  for (const m of items)
    m.visible = !hiddenTypes.has(legendKindOf(String(markerKind(m.userData.item))));

  // The master checkbox mirrors the per-kind toggles: off when all are hidden, indeterminate when some are.
  const hidden = [...kinds].filter((k) => hiddenTypes.has(k)).length;

  $('optMarkers').checked = hidden < kinds.size;
  $('optMarkers').indeterminate = hidden > 0 && hidden < kinds.size;
}

const LEGEND_GROUPS = [
  ['Starts and cameras', ['player', 'wing', '0', 'cam', 'point', 'wdgun']],
  // Paths get one row per NPC kind found on the level (path:*), then unassigned splines.
  ['Paths', null],
  ['Trigger areas', ['tevent', 'tspawn', 'tvoice', 'twaypoint', '7', '2']],
  ['Path events', ['pspeed', 'phold', 'pattack', 'paction', 'pfly', 'pweave', 'ppitch']],
  ['Regions', ['zhi', 'zlo', 'zmus', 'zunit', 'troam', '6']],
];

function renderLegend(markers)
{
  const counts = {};

  // A paired inner box is half of an item the outer box already counts.
  for (const m of markers.children)
  {
    if (m.userData.item.musicRegion)
      continue;

    const k = legendKindOf(markerKind(m.userData.item));

    counts[k] = (counts[k] || 0) + 1;
  }

  const el = $('legendRows');

  el.innerHTML = '';

  // Each group header toggles all of its rows; it shows as indeterminate when only some are on.
  for (const [group, types] of LEGEND_GROUPS)
  {
    const present = (types ?? [...Object.keys(counts).filter((k) => k.startsWith('path:')).sort(), '3']).filter((t) => counts[t]);

    if (!present.length)
      continue;

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
        if (headBox.checked)
          hiddenTypes.delete(t);
        else
          hiddenTypes.add(t);
      }

      rows.forEach((r) => (r.checked = headBox.checked));
      applyMarkerVisibility(markers);
    };

    el.append(head);

    for (const type of present)
    {
      const info = TYPES[type];
      const row = elem('label');

      row.className = 'sub';
      row.title = info.tip;
      row.innerHTML = `<input type="checkbox" ${hiddenTypes.has(type) ? '' : 'checked'}>${icon(info)}<span>${info.label}</span><em>${counts[type]}</em>`;

      const box = row.querySelector('input');

      rows.push(box);

      box.onchange = () => {
        if (box.checked)
          hiddenTypes.delete(type);
        else
          hiddenTypes.add(type);

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

const isUnderSky = (o) => {
  for (; o; o = o.parent)
    if (o === sky)
      return true;

  return false;
};

function pick(e, targets)
{
  const r = canvas.getBoundingClientRect();

  pointer.set(((e.clientX - r.left) / r.width) * 2 - 1, -((e.clientY - r.top) / r.height) * 2 + 1);
  raycaster.setFromCamera(pointer, camera);

  const hits = raycaster.intersectObjects(targets, true).filter((h) => {
    for (let o = h.object; o; o = o.parent)
      if (!o.visible)
        return false;

    return !isUnderSky(h.object);
  });

  // Zones (outline and see-through fill) are the last pick: anything else under the cursor wins.
  const first = hits.find((h) => !h.object.userData.zone) ?? hits[0];

  // A path event wins over the path it sits on.
  if (first && hitItem(first)?.type === 3)
    return hits.find((h) => h.object.raycast === beadRaycast) ?? first;

  // Where a high and a low detail region overlap the game uses the high one.
  return (first && isLowLod(hitItem(first)) && hits.find((h) => isHighLod(hitItem(h)))) || first;
}

const isHighLod = (it) => it?.type === 6 && it.sub === 0x04;
const isLowLod = (it) => it?.type === 6 && it.sub === 0x05;

function hitItem(hit)
{
  let o = hit.object;

  while (o && !o.userData.item)
    o = o.parent;

  return o?.userData.item;
}

const visibleMarkers = () => {
  const m = root && root.getObjectByName('markers');

  return m && m.visible ? m.children.filter((c) => c.visible) : [];
};

canvas.addEventListener('pointermove', (e) => {
  const tip = $('tip');

  // The right-click card takes over while it is open.
  if (editHover(e))
    return;

  const hit = !e.buttons && $('info').hidden && pick(e, editMode === 'events' ? editTargets() : visibleMarkers());
  if (!hit) { tip.hidden = true; return; }
  const it = hit.object.userData.item;
  let what = describe(it);

  // Splines often run on top of each other; name every other one under the cursor too.
  if (it.type === 3)
  {
    const others = new Set(raycaster.intersectObjects(visibleMarkers(), true).map((h) => h.object.userData.item).filter((o) => o && o.type === 3 && o !== it).map((o) => o.name));

    if (others.size)
      what += '\nAlso here: ' + [...others].join(', ');
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

function inspect(hit)
{
  const o = hit.object;

  if (o.userData.crawlBackdrop)
    return crawlBackdropCard();

  if (o.userData.item)
    return {
      title: o.userData.item.name || o.userData.item.label || '(unnamed)',
      rows: itemRows(o.userData.item),
      // The raw record ids, for matching the game's memory or the decomp; Kind names the same thing for a modder.
      tip: `DAT type ${o.userData.item.type}, subtype ${hex(o.userData.item.sub)}, record ${o.userData.item.index} at ${hex(o.userData.item.offset)}`,
      help: o.userData.item,
      place: o.userData.item,
      voiceIds: triggerVoiceIds(o.userData.item, selected?.split('/').pop()).filter((v) => audioBank?.byIndex.get(audioBank.voiceMap[v]))
    };

  if (o.userData.tiles)
  {
    const { hmp, texIds } = o.userData.tiles;
    const p = o.worldToLocal(hit.point.clone());
    const tx = Math.min(hmp.w - 1, Math.max(0, Math.floor(p.x / 2)));
    const ty = Math.min(hmp.h - 1, Math.max(0, Math.floor(p.z / 2)));
    const ti = hmp.map[ty * hmp.w + tx];
    const tile = hmp.tiles[ti];

    if (!tile)
      return null;

    const light = lightsLevel && [[
      'Add light here',
      'A ray-traced light just above this spot, for this level (a light preset in the mod\'s roguesq_lights.json)',
      () => lightsAddLevel(hit)
    ]];

    return {
      title: `Terrain tile ${tx}, ${ty}`,
      actions: light || [],
      rows: [
        ['Tile record', ti],
        ['Texmap id', tile.tex],
        ['Bank texture', texIds[tile.tex] ?? '-'],
        ['Flags', hex(tile.flags)],
        ['Height range', `${Math.min(...tile.hts)} .. ${Math.max(...tile.hts)} (x${+hmp.heightScale.toFixed(3)})`]
      ]
    };
  }

  const info = o.userData.infos?.[hit.face?.materialIndex ?? 0];

  if (!info)
    return null;

  let obj = o;

  while (obj.parent && obj.parent !== root && !obj.userData.hobObject)
    obj = obj.parent;

  let placed = o;

  while (placed && !placed.userData.source)
    placed = placed.parent;

  const t = info.tex;
  const editId = placed?.userData.source?.id ?? (selectedLevel ? null : selected);
  let alpha = 'opaque';

  if (info.valpha)
    alpha = 'blended (vertex alpha)';
  else if (info.blend)
    alpha = 'blended (texture alpha)';
  else if (info.cutout)
    alpha = 'cutout (alpha test 0.5)';

  const light = !selectedLevel && o.userData.hob && [
    [
      'Add light here',
      'A ray-traced light at this point that rides on this part in game (a light preset for the part, in the mod\'s roguesq_lights.json)',
      () => lightsAdd(hit)
    ],
    [
      info.glow ? 'Stop it glowing' : 'Make it glow',
      info.glow ? `Material ${info.matName || '#' + info.index} goes back to its own shading` : `Every part using material ${info.matName || '#' + info.index} draws at full brightness in game, lit or in shade, and with ray tracing on casts no shadow and shows in reflections`,
      () => setMaterialGlow(selected, info.index, !info.glow)
    ]
  ];

  return {
    title: `${obj.name || 'model'} / ${o.name || 'part'}`,
    open: placed?.userData.source,
    edit: t && editId && { id: editId, tex: t.name },
    actions: light || [],
    rows: [
      ['Material', `#${info.index} ${esc(info.matName)}`],
      ['Texture', t ? `${esc(t.name)} ${t.w}x${t.h}` : 'none (vertex colour only)'],
      t && ['Format', `${TEX_TYPES[t.type] ?? 'type ' + t.type}, flags ${hex(t.flags)}`],
      t && ['Wrap', info.map?.wrapS === THREE.ClampToEdgeWrapping ? 'clamp (UVs stay in 0..1)' : 'repeat'],
      info.emissive && info.valpha && !info.blend && t && ['Shading', 'sphere-mapped reflection (glass)'],
      ['Alpha', alpha],
      ['Part triangles', o.geometry.attributes.position.count / 3],
      o.parent?.name && ['Joint', `${o.parent.name} (moves; a mesh named this in an imported model moves with it)`]
    ]
  };
}

let backTo = null;

function showInfo(card, x, y)
{
  const el = $('info');
  if (!card) { el.hidden = true; return; }
  el.innerHTML = `<b${card.tip ? ` title="${esc(card.tip)}"` : ''}>${esc(card.title)}</b>` + card.rows.filter(Boolean).map(([k, v]) => `<span>${k}</span><span>${v}</span>`).join('');

  if (card.help && eventHelpUrl(card.help))
    el.append(eventHelpLink(card.help));

  if (card.open) {
    const btn = elem('button');

    btn.textContent = `Open ${card.open.name} in model view`;

    btn.onclick = () => {
      el.hidden = true;
      rememberLevel(true);
      $('tabModels').click();
      showModel(card.open.id, card.open.objIndex);
    };

    el.append(btn);
  }

  if (card.edit)
  {
    const dl = elem('button');

    dl.textContent = 'Download texture PNG';
    dl.onclick = () => downloadTexture(card.edit.id, card.edit.tex);

    const rep = elem('button');

    rep.textContent = 'Replace texture from image…';
    rep.title = 'Scaled to the texture size and converted to its format';
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

  if (card.voiceIds?.length && audioBank)
  {
    const btn = elem('button');

    btn.textContent = `Play voice line${card.voiceIds.length > 1 ? 's' : ''} ${card.voiceIds.join(', ')}`;
    btn.onclick = () => playVoiceIds(card.voiceIds);
    el.append(btn);
  }
  else if (card.place?.type === 7 && (card.place.onEnter || card.place.pathEvent))
  {
    const note = elem('small', { className: 'shotNote' });

    el.append(note);
    triggerEffect(card.place, selected).then((t) => (t.text ? Object.assign(note, { textContent: t.text, title: t.tip ?? '' }) : note.remove()));
  }

  const r = canvas.getBoundingClientRect();

  el.style.left = Math.min(x, r.width - 390) + 'px';
  el.style.top = Math.min(y, r.height - 220) + 'px';
  el.hidden = false;
}

// What a trigger that plays no line does, in words ({ text, tip }: the tip names the flags and counters): who reads the ones it writes (the level script, or DAT items gated on them:
// units and restart points by flag +0x60 / counter +0x64 (isDatItemConditionMet), other triggers, objective arrows).
async function triggerEffect(it, dir)
{
  const p = it.pathEvent;
  const acts = [it.onEnter, it.onExit, p && { counter: p.addCounter, flag: p.setFlag }].filter(Boolean);
  const flags = [...new Set(acts.map((a) => a.flag).filter(Boolean))];
  const counters = [...new Set(acts.map((a) => a.counter).filter(Boolean))];

  const subject = p ? 'An NPC passing it' : (it.onExit?.flag || it.onExit?.counter ? 'Entering or leaving it' : 'Entering it');

  // A path event's own work is its order to the NPC (hold, attack); only the flags and counters it sets need a note.
  if (!flags.length && !counters.length)
    return { text: p ? '' : `${subject} does nothing.` };

  const reads = new Set(globalThis.RS64_TRIGGER_VOICES?.[(modLevelOf(dir) ? 'lv_' + modLevelOf(dir).template.toString(36) : dir.split('/').pop())]?.reads ?? []);
  const dv = await rom.read(dir + '/dat');
  const brings = [];
  const starts = [];

  for (const x of parseDAT(dv).items)
  {
    if (x === it || x.offset === it.offset)
      continue;

    const size = dv.getUint32(x.offset + 8);
    const gate = x.type === 0 && size > 0x6C ? { flag: dv.getUint32(x.offset + 0x60), counter: dv.getUint32(x.offset + 0x64) } : null;
    const uses = [
      gate,
      x.requires,
      x.waypoint && { flag: x.waypoint.flag },
      x.pathEvent && { flag: x.pathEvent.flag, counter: x.pathEvent.counter }
    ].filter(Boolean);

    if (uses.some((u) => flags.includes(u.flag) || counters.includes(u.counter)))
      (gate && isUnit(x) ? brings : starts).push(x.name ? { name: x.name } : { kind: TYPES[markerKind(x)]?.label ?? 'item' });
  }

  const what = [...flags.map((f) => `flag ${f}`), ...counters.map((c) => `counter ${c}`)].join(' and ');
  const script = [...flags.map((f) => 'b' + f), ...counters.map((c) => 'c' + c)].some((k) => reads.has(k));

  const tip = `It sets ${what}`;

  // Units it brings into the level, then what else it switches on (objective arrows, path events, other triggers), then the script.
  // Unnamed things (path events, mostly) only say what kind they are, so they count up by kind instead of listing the same word again.
  const kinds = [...Map.groupBy(starts.filter((x) => x.kind), (x) => x.kind)].map(([k, l]) => `${l.length} ${k.toLowerCase()}`);
  const named = starts.filter((x) => x.name).map((x) => x.name);

  const parts = [
    brings.length && listed('spawns', brings.map((x) => x.name ?? x.kind)),
    named.length && listed('switches on', named),
    kinds.length && `switches on ${kinds.join(', ')}`,
    script && 'sets off something in the mission script (an objective, camera cut or spawn)'
  ].filter(Boolean);

  return parts.length ? {
    text: `${subject} ${parts.join('\nand ')}`,
    tip
  } : {
    text: `${subject} does nothing: nothing in the level or its mission script waits for it.`,
    tip
  };
}

// A press that barely moves is a click; anything longer is an orbit drag.
const downAt = {};
canvas.addEventListener('pointerdown', (e) => (downAt[e.button] = [e.clientX, e.clientY]));
const wasClick = (e, button) => downAt[button] && Math.hypot(e.clientX - downAt[button][0], e.clientY - downAt[button][1]) <= 4;

canvas.addEventListener('pointerup', (e) => {
  if (levelCompose && e.button === 0 && wasClick(e, 0))
    return levelCompose.shoot();

  if (e.button !== 0 || !wasClick(e, 0) || sculptActive() || lightsClick(e) || editClick(e) || cutPickActor(e))
    return;

  const hit = pick(e, visibleMarkers());
  const r = canvas.getBoundingClientRect();
  const card = hit && inspect(hit);

  // A click on what the open card describes closes it.
  if (infoOpenAtPress && card && card.title === infoTitle)
  {
    $('info').hidden = true;
    infoTitle = null;

    return;
  }

  infoTitle = card?.title ?? null;
  showInfo(card, e.clientX - r.left + 12, e.clientY - r.top + 12);
});

let infoTitle = null;

// Whether a card was open at the press: the press itself closes it (the camera controls start), so its state is read before them.
let infoOpenAtPress = false;

canvas.addEventListener('pointerdown', () => (infoOpenAtPress = !$('info').hidden), true);

canvas.addEventListener('contextmenu', (e) => {
  e.preventDefault();

  if (levelCompose)
    return;

  if (!root || !wasClick(e, 2))
    return;

  let title = editPickList(e);

  if (!title)
  {
    const r = canvas.getBoundingClientRect();

    // In a crawl the backdrop is picked first: it is drawn behind everything but sits past the stars plane.
    const hit = (cut?.crawl?.backdrop?.visible && pick(e, [cut.crawl.backdrop])) || pick(e, [root]);
    const card = hit && inspect(hit);

    title = card?.title ?? null;
    showInfo(card, e.clientX - r.left + 12, e.clientY - r.top + 12);
  }

  // A right-click on what the open card already shows closes it; anywhere else opens that spot's card.
  if (infoOpenAtPress && title && title === infoTitle)
  {
    $('info').hidden = true;
    title = null;
  }

  infoTitle = title;
});

function setRoot(obj) {
  endCutscene();
  select(null);

  if (!keepPlacing)
    placing = null;

  $('legend').hidden = true;
  $('tip').hidden = true;
  $('info').hidden = true;

  if (root) {
    scene.remove(root);

    root.traverse((o) => {
      if ((o.isMesh || o.isLine) && !o.userData.shared && !Object.values(GEO).includes(o.geometry))
        o.geometry.dispose();
    });
  }

  root = obj;
  anim = null;
  setupExhaust(null);
  lamps.forEach((l) => l.sprite.material.dispose());
  lamps = [];
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
let rom = null;
let romBytes = null;
let mode = 'models';
let selected = null;
let selectedLevel = false;
let selectedObj = -1;
const modelCache = new Map();

