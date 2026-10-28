'use strict';
// ---------- UI wiring ----------
$('themeBtn').onclick = () => {
  const now = document.documentElement.dataset.theme ?? (matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark');
  document.documentElement.dataset.theme = now === 'light' ? 'dark' : 'light';
  store('rs64.theme', document.documentElement.dataset.theme);
  syncThemeBg();
};
$('romfile').onchange =(e) => e.target.files[0] && openFile(e.target.files[0]);
const drop = $('drop');
// The drop box doubles as a button for the file dialog.
const dropBox = drop.querySelector('.box');
dropBox.onclick = () => $('romfile').click();
dropBox.onkeydown = (e) => (e.key === "Enter" || e.key === " ") && (e.preventDefault(), $('romfile').click());
addEventListener('dragover', (e) => { e.preventDefault(); drop.classList.add('over'); });
addEventListener('dragleave', () => drop.classList.remove('over'));
addEventListener('drop', (e) => {
  e.preventDefault();
  drop.classList.remove('over');
  if (e.dataTransfer.files[0]) openFile(e.dataTransfer.files[0]);
});
$('filter').oninput = () => {
  $('filterClear').hidden = !$('filter').value;
  renderList();
};
$('filterClear').onclick = () => {
  $('filter').value = '';
  $('filter').oninput();
  // Keep the current model/level in view in the full list.
  $('list').querySelector('.row.sel')?.scrollIntoView({ block: 'center' });
  $('filter').focus();
};
const setMode = (m) => {
  mode = m;
  for (const [id, k] of [['tabModels', 'models'], ['tabLevels', 'levels'], ['tabEffects', 'effects'], ['tabAudio', 'audio'], ['tabFonts', 'fonts'], ['tabMenus', 'menus']]) $(id).classList.toggle('on', k === m);
  // Over a cutscene being edited the Audio list leaves it on screen, so a row can be dragged onto its Audio lane; picking a row opens the player.
  const overCut = m === 'audio' && cutEditing && !!cut?.file;
  document.body.classList.toggle('audio', m === 'audio' && !overCut);
  document.body.classList.toggle('fonts', m === 'fonts' || m === 'menus');
  $('audioPanel').hidden = m !== 'audio' || overCut || !audioSel || !!musicSel;
  $('musicPanel').hidden = m !== 'audio' || overCut || !musicSel;
  $('fontPanel').hidden = m !== 'fonts' || !fontSel;
  $('menuPanel').hidden = m !== 'menus' || !menuSel;
  if (m !== 'audio') stopAudio();
  // lights.js loads after this script.
  if (typeof lightsPanel === 'function') lightsPanel();
  renderList();
};
$('tabFonts').onclick = () => setMode('fonts');
$('tabMenus').onclick = () => menuRefreshPages().then(() => setMode('menus'));
$('tabAudio').onclick = () => setMode('audio');
$('audCat').onchange = $('audLevel').onchange = renderList;
$('viewOrtho').onclick = () => root && !cut && setOrtho(!orthoView);
$('tabModels').onclick = () => setMode('models');
$('tabLevels').onclick = () => setMode('levels');
$('tabEffects').onclick = () => setMode('effects');
addEventListener('beforeunload', (e) => unsavedEdits() && e.preventDefault());
$('optFxFps').oninput = () => ($('fxFpsVal').textContent = $('optFxFps').value);
$('optFxAdd').onchange = refresh;
$('optFxBg').oninput = () => {
  fxBg.set($('optFxBg').value);
  if (selectedEffect) scene.background = fxBg;
};
$('optFxSize').oninput = () => ($('fxSizeVal').textContent = $('optFxSize').value);
$('optFxSize').onchange = refresh;
$('optFxCraft').onchange = refresh;
$('optFxSpeed').oninput = () => ($('fxSpeedVal').textContent = $('optFxSpeed').value + 'x');
$('optFxMode').onchange = refresh;
// New odds apply from the next take.
// The rolls happen at spawn, so a new chance restarts the take.
$('optChunkPct').oninput = () => ($('chunkPctVal').textContent = $('optChunkPct').value);
$('optPlumePct').oninput = () => ($('plumePctVal').textContent = $('optPlumePct').value);
$('optChunkPct').onchange = $('optPlumePct').onchange = () => effect && timelineStart(effect);
$('fxPlay').onclick = () => {
  if (!effect?.step) return;
  // Play at the end of a finished take starts it over.
  if (!TL.playing && TL.time >= TL.dur) seekTimeline(0);
  TL.playing = !TL.playing;
  syncTimeline();
};
$('fxStop').onclick = () => {
  if (!effect?.step) return;
  TL.playing = false;
  seekTimeline(0);
};
$('fxTime').oninput = () => {
  if (!effect?.step) return;
  TL.playing = false;
  seekTimeline(+$('fxTime').value);
};
$('optFxBurst').oninput = () => ($('fxBurstVal').textContent = $('optFxBurst').value);
$('optLod').oninput = () => {
  $('lodVal').textContent = lodLevel();
  refresh();
};
$('optTerrainLod').oninput = () => {
  $('terrainLodVal').textContent = $('optTerrainLod').value;
  if (selectedLevel) refresh();
};
$('btnBack').onclick = async () => {
  const b = backTo;
  $('tabLevels').click();
  await showLevel(b.level, true);
  setView(b.view);
};
$('optTex').onchange = () => {
  $('optFilter').disabled = !$('optTex').checked;
  applyMaterialOptions();
};
// Sampler settings don't reliably update on live WebGPU textures, so rebuild them.
$('optFilter').onchange = () => {
  clearCaches();
  refresh();
};
$('optWire').onchange = applyMaterialOptions;
$('optAnim').onchange = () => {
  animClock = 0;
  // Rebuilding restores the rest pose when going back to "None", and rebinds the chosen animation otherwise.
  refresh();
};
$('optAnimSpeed').oninput = () => ($('animSpeedVal').textContent = $('optAnimSpeed').value + 'x');
$('optSpeed').oninput = () => ($('speedVal').textContent = $('optSpeed').value);
// The ceremony's events and length depend on the tier, so a change rebuilds it.
$('optMedal').onchange = () => cut?.ceremony && reopenCutscene();
$('optCutCraft').onchange = () => cut && reopenCutscene();
$('optFoils').oninput = () => { $('foilsVal').textContent = Math.round(+$('optFoils').value * 100) + '%'; applyFoils(); };
$('optSpin').oninput = () => {
  const v = +$('optSpin').value;
  $('spinVal').textContent = v;
  spin = v;
};
// Item markers is the master switch for the legend's per-kind toggles: ticking shows every kind, unticking hides them all.
$('optMarkers').onchange = (e) => {
  const m = root && root.getObjectByName('markers');
  if (!m) return;
  for (const c of m.children) {
    const k = String(markerKind(c.userData.item));
    if (e.target.checked) hiddenTypes.delete(k);
    else hiddenTypes.add(k);
  }
  renderLegend(m);
};
$('optSky').onchange = (e) => {
  if (sky) sky.visible = e.target.checked;
  scene.background = e.target.checked && skyColor ? skyColor : bgColor;
};
// Lighting mode swaps material types, so rebuild; shadows only darken anything in dynamic mode.
$('optLight').onchange = () => {
  sun.shadow.intensity = gameLighting() ? 0 : 1;
  refresh();
};
$('optFog').onchange = () => {
  syncFogControls();
  applyFog();
};
$('optSpread').oninput = () => {
  $('spreadVal').textContent = $('optSpread').value;
  const m = root?.getObjectByName('markers');
  if (m) applySpread(m);
};
$('optFogDist').oninput = () => {
  $('fogVal').textContent = $('optFogDist').value + 'x';
  applyFog();
};
// The shown model's skeletal animations as clips: each track key is posed with poseNode (the viewer's own rule, pivots included) and
// split into position/quaternion/scale. Sprite-sheet animations have no node motion and are skipped.
function animClips() {
  let rig = null;
  root.traverse((o) => (rig ??= o.userData.rig));
  if (!rig || $('animRow').hidden) return [];
  const tmp = new THREE.Object3D(), p = new THREE.Vector3(), q = new THREE.Quaternion(), s = new THREE.Vector3();
  // Wing sweeps: 17 keys over 1 s, posed by applyFoils on the real hinge nodes, then the slider pose is put back.
  const foilClips = !foils ? [] : ($('optAnim').found ?? []).filter((f) => f.foil).map((f) => {
    const times = [], keys = { [foils.a]: [[], [], []], [foils.b]: [[], [], []] };
    for (let k = 0; k <= 16; k++) {
      applyFoils(f.foil[0] + (f.foil[1] - f.foil[0]) * (k / 16));
      times.push(k / 16);
      for (const i of [foils.a, foils.b]) {
        foils.rig.nodes[i].matrix.decompose(p, q, s);
        keys[i][0].push(...p.toArray());
        keys[i][1].push(...q.toArray());
        keys[i][2].push(...s.toArray());
      }
    }
    applyFoils();
    return new THREE.AnimationClip(f.name, 1, [foils.a, foils.b].flatMap((i) => {
      const id = foils.rig.nodes[i].uuid;
      return [new THREE.VectorKeyframeTrack(id + '.position', times, keys[i][0]), new THREE.QuaternionKeyframeTrack(id + '.quaternion', times, keys[i][1]), new THREE.VectorKeyframeTrack(id + '.scale', times, keys[i][2])];
    }));
  });
  // Code-driven clips: 33 keys over the clip, posed on the real nodes, then those nodes go back to rest.
  const procClips = ($('optAnim').found ?? []).filter((f) => f.proc).map((f) => {
    const a = { proc: f.proc, rig, nodes: f.nodes }, idx = [...new Set(f.nodes.map(([i]) => i))], times = [], keys = new Map(idx.map((i) => [i, [[], [], []]]));
    for (let k = 0; k <= 32; k++) {
      poseProcAt(a, k / 32);
      times.push((k / 32) * f.proc[1]);
      for (const i of idx) {
        rig.nodes[i].matrix.decompose(p, q, s);
        keys.get(i)[0].push(...p.toArray());
        keys.get(i)[1].push(...q.toArray());
        keys.get(i)[2].push(...s.toArray());
      }
    }
    for (const i of idx) poseNode(rig.nodes[i], rig.src[i]);
    return new THREE.AnimationClip(f.name, f.proc[1], idx.flatMap((i) => {
      const id = rig.nodes[i].uuid, [P, Q, S] = keys.get(i);
      return [new THREE.VectorKeyframeTrack(id + '.position', times, P), new THREE.QuaternionKeyframeTrack(id + '.quaternion', times, Q), new THREE.VectorKeyframeTrack(id + '.scale', times, S)];
    }));
  });
  return foilClips.concat(procClips,($('optAnim').found ?? []).filter((f) => f.data && f.data.tracks.length === rig.nodes.length - 1).map((f) => {
    const seconds = (f.data.lengthMs || 2000) / 1000, tracks = [];
    f.data.tracks.forEach((tr, i) => {
      const g = rig.nodes[i + 1], n = rig.src[i + 1];
      if (!tr || !g || !tr.times.length) return;
      const times = [], P = [], Q = [], S = [];
      tr.times.forEach((ph, k) => {
        // Keys must strictly increase; a repeated phase keeps the first key.
        if (times.length && ph * seconds <= times[times.length - 1]) return;
        poseNode(tmp, n, tr.rot ? new THREE.Quaternion(...tr.rot[k]).normalize() : null, tr.pos ? new THREE.Vector3(...tr.pos[k]) : null);
        tmp.matrix.decompose(p, q, s);
        times.push(ph * seconds);
        P.push(...p.toArray());
        Q.push(...q.toArray());
        S.push(...s.toArray());
      });
      tracks.push(new THREE.VectorKeyframeTrack(g.uuid + '.position', times, P), new THREE.QuaternionKeyframeTrack(g.uuid + '.quaternion', times, Q), new THREE.VectorKeyframeTrack(g.uuid + '.scale', times, S));
    });
    return new THREE.AnimationClip(f.name, seconds, tracks);
  }));
}
$('btnExport').onclick = async () => {
  if (!root) return;
  const btn = $('btnExport');
  btn.disabled = true;
  try {
    const animations = animClips();
    // glTF cannot animate a node stored as a matrix, so animated exports write each node's transform as TRS (rig nodes keep theirs in .matrix).
    if (animations.length) root.traverse((o) => !o.matrixAutoUpdate && o.matrix.decompose(o.position, o.quaternion, o.scale));
    const glb = await new GLTFExporter().parseAsync(root, { binary: true, onlyVisible: true, animations, trs: animations.length > 0 });
    saveBlob(new Blob([glb], { type: 'model/gltf-binary' }), selected.split('/').pop() + (selectedLevel ? '' : '_lod' + lodLevel()) + '.glb');
  } finally {
    btn.disabled = false;
  }
};

// ---- Mod edits ----
// Edited assets are changed in place in rom.read's cache; the originals are kept to diff against. Export writes one BPS patch per asset,
// which the game applies to the player's own copy at startup (<mod>/assets/<path>.bps, the path without "data/").
// edits keeps each touched asset's original; dirty is the ones that differ from it now (undo can bring one back to the original).
const edits = new Map(), dirty = new Set();
// The edited game assets a mod exports (the explorer/ authoring sidecars stay in the project).
const modDirty = () => [...dirty].filter((p) => !p.startsWith('explorer/'));
const virtualAsset = (p) => p.startsWith('explorer/') || p.startsWith('mod/');
const sameBytes = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
let history = createHistory();
function updateModButton() {
  const n = modDirty().length;
  $('btnMod').disabled = !n;
  $('btnPlay').disabled = !rom;
  $('btnModLabel').textContent = n ? `Export mod (${n} edited asset${n > 1 ? 's' : ''})` : 'Export mod';
  $('btnUndo').disabled = !history.at;
  $('btnRedo').disabled = history.at >= history.steps.length;
}
function markDirty(path, now) {
  if (sameBytes(edits.get(path), now)) dirty.delete(path);
  else dirty.add(path);
}
// change() edits the bytes in place, or returns a new array when the asset changes size. Every edit is one history commit; wrap a
// gesture that makes several in historyGroup so it undoes as one step.
async function editAsset(path, change, label) {
  // A virtual asset (explorer/ authoring sidecars, mod/ files written raw at the mod root) starts out empty.
  const dv = virtualAsset(path) ? await rom.read(path).catch(() => new DataView(new ArrayBuffer(0))) : await rom.read(path);
  const bytes = u8(dv);
  if (!edits.has(path)) edits.set(path, bytes.slice());
  const before = bytes.slice(), replaced = change(bytes), now = replaced ?? bytes;
  if (replaced) rom.write(path, replaced);
  if (sameBytes(before, now)) return;
  history.commit(path, before, now.slice(), label);
  markDirty(path, now);
  updateModButton();
}
// Text assets (camera_shots.txt, levels.json, rules.json, the explorer's JSON sidecars) read as "" when missing.
const virtualText = async (path) => new TextDecoder().decode(await rom.read(path).then(u8, () => new Uint8Array(0)));
const editText = (path, text, label) => editAsset(path, () => new TextEncoder().encode(text), label);
// fallback() is the value for a missing or unreadable file.
async function readJson(path, fallback) {
  try {
    const text = await virtualText(path);
    return text ? JSON.parse(text) : fallback();
  } catch {
    return fallback();
  }
}
const writeJson = (path, value, label, indent) => editText(path, JSON.stringify(value, null, indent), label);
async function historyGroup(label, fn) {
  history.begin(label);
  try {
    return await fn();
  } finally {
    history.end();
    updateModButton();
  }
}
// Undo / redo: write the stored bytes back, then rebuild whatever shows them (cutscene, level with its terrain, or model).
async function applyHistory(changes) {
  if (!changes.length) return;
  for (const { path, bytes } of changes) {
    rom.write(path, bytes.slice());
    markDirty(path, bytes);
  }
  updateModButton();
  if (changes.some((c) => c.path === MOD_LEVELS)) await applyModLevels();
  rulesChanged();
  crawlsChanged();
  if (mode === 'fonts' && fontSel) showFont(fontSel);
  if (mode === 'menus' && menuSel) showMenu(menuSel);
  await refreshView();
}
async function refreshView() {
  modelCache.clear();
  clearCaches();
  if (cut?.crawl) await showCrawl(cut.crawl.which, { t: cut.t, playing: cut.playing });
  else if (cut && selected?.startsWith('cut:')) await reopenCutscene();
  else if (selectedLevel) await reloadLevel(rom.assets.has(selected + '/dat') ? selected : 'data/level/lv_0');
  else if (selected) await showModel(selected, selectedObj, true);
}
// ---- Project files: every edit plus the undo history, as patches against the ROM's assets (projectWrite / projectRead) ----
let romCrc = 0, projectHandle = null;
// The history step the last save or open left us at; any other step means unsaved edits.
let savedStep = null;
const unsavedEdits = () => (history.steps[history.at - 1] ?? null) !== savedStep;
async function saveProject() {
  if (!rom) return;
  const current = new Map();
  for (const p of edits.keys()) {
    const dv = await rom.read(p);
    current.set(p, u8(dv).slice());
  }
  const blob = new Blob([projectWrite({ romCrc, originals: edits, current, history })], { type: 'application/octet-stream' });
  if (window.showSaveFilePicker) {
    try {
      projectHandle ??= await showSaveFilePicker({ suggestedName: 'my-rs64-project.rs64proj', types: [{ description: 'RS64 Explorer project', accept: { 'application/octet-stream': ['.rs64proj'] } }] });
      const w = await projectHandle.createWritable();
      await w.write(blob);
      await w.close();
      savedStep = history.steps[history.at - 1] ?? null;
      return;
    } catch (e) {
      if (e.name === 'AbortError') return;
      projectHandle = null;
    }
  }
  saveBlob(blob, 'my-rs64-project.rs64proj');
  savedStep = history.steps[history.at - 1] ?? null;
}
async function openProject() {
  if (!rom) return;
  // The file-system picker (Chrome, Edge) gives a handle, so Save writes back to the opened file.
  let file = null, handle = null;
  if (window.showOpenFilePicker) {
    try {
      [handle] = await showOpenFilePicker({ types: [{ description: 'RS64 Explorer project', accept: { 'application/octet-stream': ['.rs64proj'] } }] });
      file = await handle.getFile();
    } catch (e) {
      if (e.name === 'AbortError') return;
    }
  }
  file ??= await pickImage('.rs64proj');
  if (!file) return;
  const original = async (p) => {
    if (edits.has(p)) return edits.get(p).slice();
    if (virtualAsset(p)) return new Uint8Array(0);
    const dv = await rom.read(p);
    return u8(dv).slice();
  };
  let loaded;
  try {
    loaded = await projectRead(new Uint8Array(await file.arrayBuffer()), romCrc, original);
  } catch (e) {
    alert(e.message);
    return;
  }
  // A mod level's files are aliases of its template's: they must exist before their originals are read.
  applyModLevelsText(new TextDecoder().decode(loaded.current.get(MOD_LEVELS) ?? new Uint8Array(0)));
  const originals = new Map();
  for (const p of new Set([...edits.keys(), ...loaded.current.keys(), ...loaded.history.steps.flatMap((s) => s.changes.map((c) => c.path))])) originals.set(p, await original(p));
  edits.clear();
  dirty.clear();
  for (const [p, orig] of originals) {
    edits.set(p, orig);
    const now = loaded.current.get(p) ?? orig;
    rom.write(p, now.slice());
    markDirty(p, now);
  }
  history = loaded.history;
  savedStep = history.steps[history.at - 1] ?? null;
  projectHandle = handle;
  updateModButton();
  await refreshView();
}
addEventListener('keydown', (e) => {
  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 's') {
    e.preventDefault();
    saveProject();
  }
});
const undoEdit = () => applyHistory(history.undo());
const redoEdit = () => applyHistory(history.redo());
addEventListener('keydown', (e) => {
  if (!(e.ctrlKey || e.metaKey) || e.target.closest?.('input,select,textarea')) return;
  const k = e.key.toLowerCase();
  if (k === 'z' && !e.shiftKey) undoEdit();
  else if (k === 'y' || (k === 'z' && e.shiftKey)) redoEdit();
  else return;
  e.preventDefault();
});
async function findTexture(id, name) {
  const t = (await loadModel(id)).hmt.textures.find((x) => x.name === name);
  if (!t) throw new Error(`No texture ${name} in ${id}`);
  return t;
}
async function downloadTexture(id, name) {
  const t = await findTexture(id, name);
  const c = elem('canvas');
  c.width = t.w;
  c.height = t.h;
  c.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(t.rgba), t.w, t.h), 0, 0);
  c.toBlob((b) => saveBlob(b, `${name}.png`), 'image/png');
}
function pickImage(accept = 'image/*') {
  return new Promise((resolve) => {
    const input = elem('input');
    input.type = 'file';
    input.accept = accept;
    input.onchange = () => resolve(input.files[0] || null);
    input.click();
  });
}
// A question with one button per answer; resolves to the picked value, or null when closed with Cancel or Escape.
function askChoice(question, answers) {
  const d = elem('dialog', { className: 'panel levelDialog' }), row = elem('div', { className: 'actions' });
  return new Promise((res) => {
    for (const [value, label, tip] of answers) {
      const b = elem('button', { textContent: label, title: tip ?? '' });
      b.onclick = () => {
        res(value);
        d.close();
      };
      row.append(b);
    }
    const cancel = elem('button', { textContent: 'Cancel' });
    cancel.onclick = () => d.close();
    row.append(cancel);
    d.onclose = () => {
      d.remove();
      res(null);
    };
    d.append(elem('b', { textContent: question }), ...answers.map(([, label, tip]) => elem('small', { textContent: `${label}: ${tip}` })), row);
    document.body.append(d);
    d.showModal();
  });
}
// Replaces a player craft's geometry with a .glb (hobImportCraft). Each of the model's textures takes one of the craft's free texture
// slots, the most used texture the largest slot; untextured faces share a white slot.
// shortcut: textures beyond the craft's free slots reuse the last slot; pack them into fewer images (an atlas) if that shows.
async function importCraftModel() {
  const id = selected;
  if (selectedLevel || !id?.startsWith('data/pl_crafts/') || id.endsWith('/koelsch')) {
    alert('Open a player craft in the Models tab first (the Naboo Starfighter is encrypted and cannot be changed yet).');
    return;
  }
  const file = await pickImage('.glb,model/gltf-binary');
  if (!file) return;
  const plusZ = await askChoice(`Which way does the nose of ${file.name} point?`, [[true, '+Z', 'The usual glTF export (Blender\'s default): turned to face the game\'s way'], [false, '-Z', 'Already faces the game\'s way: kept as it is']]);
  if (plusZ == null) return;
  busy('Importing model...');
  try {
    const { GLTFLoader } = await import('three/addons/loaders/GLTFLoader.js');
    const gltf = await new GLTFLoader().parseAsync(await file.arrayBuffer(), '');
    gltf.scene.updateMatrixWorld(true);
    const model = await loadModel(id), slots = craftImportSlots(model.hob, model.hmt);
    if (!slots.length) throw new Error('This craft has no texture slot that can take the import');
    const tris = [], v = new THREE.Vector3();
    gltf.scene.traverse((o) => {
      if (!o.isMesh) return;
      // A mesh (or a parent) named after a craft node, e.g. wngrot_a, goes into that node's part so it keeps animating.
      let node = '';
      for (let p = o; p && !node; p = p.parent) if (model.hob.objects[0].nodeNames.has(p.name)) node = p.name;
      const g = o.geometry, pos = g.attributes.position, uv = g.attributes.uv, col = g.attributes.color, idx = g.index;
      const mats = Array.isArray(o.material) ? o.material : [o.material];
      const groups = g.groups.length ? g.groups : [{ start: 0, count: idx ? idx.count : pos.count, materialIndex: 0 }];
      for (const gr of groups) {
        const m = mats[gr.materialIndex] || mats[0], mc = m.color ?? { r: 1, g: 1, b: 1 };
        for (let i = gr.start; i + 2 < gr.start + gr.count; i += 3) {
          const t = { p: [], uv: [], rgba: [], node, map: m.map ?? null };
          for (let k = 0; k < 3; k++) {
            const vi = idx ? idx.getX(i + k) : i + k;
            v.fromBufferAttribute(pos, vi).applyMatrix4(o.matrixWorld);
            t.p.push([v.x, v.y, v.z]);
            t.uv.push(uv ? [uv.getX(vi), uv.getY(vi)] : [0.5, 0.5]);
            const c = [mc.r, mc.g, mc.b].map((x, a) => Math.round(255 * Math.min(1, x * (col ? col.getComponent(vi, a) : 1))));
            t.rgba.push(((c[0] << 24) | (c[1] << 16) | (c[2] << 8) | 0xff) >>> 0);
          }
          tris.push(t);
        }
      }
    });
    if (!tris.length) throw new Error('That file has no meshes');
    const uses = new Map();
    for (const t of tris) uses.set(t.map, (uses.get(t.map) || 0) + 1);
    const ranked = [...uses.keys()].sort((a, b) => uses.get(b) - uses.get(a));
    const slotOf = new Map(ranked.map((m, i) => [m, slots[Math.min(i, slots.length - 1)]]));
    for (const t of tris) t.mat = slotOf.get(t.map).mat;
    const dv = await rom.read(id + '_HOB');
    const out = hobImportCraft(u8(dv), model.hob, tris, { mat: slots[0].mat, plusZ });
    const images = ranked.slice(0, slots.length).map((map) => {
      const { tex } = slotOf.get(map), c = elem('canvas');
      c.width = tex.w;
      c.height = tex.h;
      const ctx = c.getContext('2d');
      ctx.fillStyle = '#fff';
      ctx.fillRect(0, 0, tex.w, tex.h);
      if (map?.image) ctx.drawImage(map.image, 0, 0, tex.w, tex.h);
      return [tex, ctx.getImageData(0, 0, tex.w, tex.h).data];
    });
    await editAsset(id + '_HOB', () => out);
    await editAsset(id + '_HMT', (bytes) => { for (const [tex, rgba] of images) encodeTexture(bytes, tex, rgba, model.hmt.textures); });
    modelCache.delete(id);
    clearCaches();
    await showModel(id, selectedObj, true);
  } catch (e) {
    alert(e.message);
  } finally {
    busy(null);
  }
}
$('btnImport').onclick = importCraftModel;
async function replaceTexture(id, name) {
  if (id.endsWith('/koelsch')) {
    alert('The Naboo Starfighter textures are encrypted in the ROM and cannot be patched yet.');
    return;
  }
  const file = await pickImage();
  if (!file) return;
  try {
    const t = await findTexture(id, name), textures = (await loadModel(id)).hmt.textures;
    const c = elem('canvas');
    c.width = t.w;
    c.height = t.h;
    const g = c.getContext('2d');
    g.drawImage(await createImageBitmap(file), 0, 0, t.w, t.h);
    const rgba = g.getImageData(0, 0, t.w, t.h).data;
    await editAsset(id + '_HMT', (bytes) => encodeTexture(bytes, t, rgba, textures));
    modelCache.delete(id);
    clearCaches();
    if (selectedLevel) await showLevel(selected, true);
    else await showModel(selected, selectedObj, true);
  } catch (e) {
    alert(e.message);
  }
}
// ---- Terrain sculpting ----
// The level's HMP is edited as one continuous height grid (hmpGrid) and re-tiled by writeHMP; the terrain mesh is rebuilt while
// dragging and the new HMP is committed as an edit when the stroke ends. Heights are stored game Y-down, so raising subtracts.
let sculpt = null;
const sculptActive = () => $('optSculpt').checked && selectedLevel && !!sculpt;
async function loadSculpt() {
  sculpt = null;
  $('sculptInfo').textContent = '';
  if (!$('optSculpt').checked || !selectedLevel || !rom.assets.has(selected + '/hmp')) {
    controls.mouseButtons.LEFT = THREE.MOUSE.ROTATE;
    return;
  }
  const dir = selected, dv = await rom.read(dir + '/hmp');
  const base = u8(dv).slice();
  const [texIds, bank] = await Promise.all([rom.read(stockDir(dir) + '_TEX').then(parseTexTable), rom.read('data/level/tex')]);
  sculpt = { dir, base, grid: hmpGrid(parseHMP(dv)), texIds, bank, undo: [], tex: null, bytes: null };
  sculpt.committed = sculpt.grid.hts.slice();
  controls.mouseButtons.LEFT = null;
  sculptStatus();
}
function sculptStatus(extra = '') {
  if (!sculpt) return;
  const t = sculpt.tex == null ? 'none (use Pick texture)' : `#${sculpt.tex}`;
  $('sculptInfo').textContent = `Paint texture: ${t}${extra ? ' · ' + extra : ''}`;
}
function sculptHit(e) {
  const terrain = root?.getObjectByName('terrain');
  const hit = terrain && pick(e, [terrain]);
  if (!hit || hit.object !== terrain) return null;
  const p = terrain.worldToLocal(hit.point.clone());
  return { gx: p.x * 2, gy: p.z * 2 };
}
function brush({ gx, gy }) {
  const g = sculpt.grid, r = +$('sculptRadius').value, s = +$('sculptStrength').value, tool = $('sculptTool').value;
  const cell = (x, y) => Math.min(g.h - 1, Math.max(0, Math.floor(y / 4))) * g.w + Math.min(g.w - 1, Math.max(0, Math.floor(x / 4)));
  if (tool === 'pick') {
    sculpt.tex = g.tex[cell(gx, gy)];
    sculptStatus();
    return false;
  }
  if (tool === 'paint') {
    if (sculpt.tex == null) return false;
    for (let ty = 0; ty < g.h; ty++) for (let tx = 0; tx < g.w; tx++) {
      if (Math.hypot(tx * 4 + 2 - gx, ty * 4 + 2 - gy) <= r) g.tex[ty * g.w + tx] = sculpt.tex;
    }
    return true;
  }
  const src = tool === 'smooth' ? g.hts.slice() : null;
  const at = (x, y) => src[Math.min(g.H - 1, Math.max(0, y)) * g.W + Math.min(g.W - 1, Math.max(0, x))];
  for (let y = Math.max(0, Math.floor(gy - r)); y <= Math.min(g.H - 1, Math.ceil(gy + r)); y++) {
    for (let x = Math.max(0, Math.floor(gx - r)); x <= Math.min(g.W - 1, Math.ceil(gx + r)); x++) {
      const d = Math.hypot(x - gx, y - gy);
      if (d > r) continue;
      let w = 1 - d / r;
      w = w * w * (3 - 2 * w);
      const i = y * g.W + x;
      if (tool === 'raise') g.hts[i] -= s * w;
      else if (tool === 'lower') g.hts[i] += s * w;
      else if (tool === 'flatten') g.hts[i] += (sculpt.flat - g.hts[i]) * Math.min(1, s * w * 0.1);
      else g.hts[i] += ((at(x - 1, y) + at(x + 1, y) + at(x, y - 1) + at(x, y + 1)) / 4 - g.hts[i]) * Math.min(1, s * w * 0.1);
      g.hts[i] = Math.max(-128, Math.min(127, g.hts[i]));
    }
  }
  return true;
}
// Re-tiles the grid and swaps the terrain mesh's geometry; returns the new HMP bytes (null when it does not fit).
function rebuildTerrain() {
  let bytes;
  try {
    bytes = writeHMP(sculpt.base, sculpt.grid);
  } catch (err) {
    sculptStatus(err.message);
    return null;
  }
  const hmp = parseHMP(new DataView(bytes.buffer)), terrain = root?.getObjectByName('terrain');
  if (!terrain) return bytes;
  // A new mesh rather than a geometry swap: the WebGPU renderer keeps drawing a mesh's first geometry.
  const t = buildTerrain(hmp, sculpt.texIds, sculpt.bank);
  const material = terrain.material.clone();
  if (material.map) material.map = t.map;
  const mesh = new THREE.Mesh(t.g, material);
  mesh.name = 'terrain';
  mesh.userData = { ...terrain.userData, terrain: t.map, tiles: { hmp, texIds: sculpt.texIds } };
  mesh.position.copy(terrain.position);
  mesh.quaternion.copy(terrain.quaternion);
  mesh.scale.copy(terrain.scale);
  terrain.parent.add(mesh);
  terrain.removeFromParent();
  if (sculpt.built) {
    terrain.geometry.dispose();
    terrain.material.dispose();
    terrain.userData.terrain?.dispose();
  }
  sculpt.built = true;
  texCache.set('terrain', { map: t.map });
  sculptStatus(`${hmp.tiles.length} of ${0x1FFF} tiles`);
  return bytes;
}
// Ground height (game units, Y-down) at world x/z, bilinear on a grid's 0.5-unit vertex spacing.
function groundAt(hts, g, scale, wx, wz) {
  const fx = Math.min(g.W - 1, Math.max(0, wx * 2)), fz = Math.min(g.H - 1, Math.max(0, wz * 2));
  const x0 = Math.min(g.W - 2, Math.floor(fx)), z0 = Math.min(g.H - 2, Math.floor(fz)), tx = fx - x0, tz = fz - z0, at = (x, z) => hts[z * g.W + x];
  return ((at(x0, z0) * (1 - tx) + at(x0 + 1, z0) * tx) * (1 - tz) + (at(x0, z0 + 1) * (1 - tx) + at(x0 + 1, z0 + 1) * tx) * tz) * scale;
}
// Placed objects (DAT type 0) keep their height above the ground: each one moves by however much the ground under it moved since
// the last commit (undo moves them back). NPCs that the game drops onto the terrain itself are moved too, which is harmless.
async function followGround(before) {
  const g = sculpt.grid, scale = new DataView(sculpt.base.buffer).getFloat32(0x10);
  const dat = parseDAT(await rom.read(sculpt.dir + '/dat'));
  const moves = [];
  for (const it of dat.items) {
    if (it.type !== 0) continue;
    const d = groundAt(g.hts, g, scale, it.pos[0], it.pos[2]) - groundAt(before, g, scale, it.pos[0], it.pos[2]);
    if (Math.abs(d) > 1e-4) moves.push([it, d]);
  }
  if (!moves.length) return;
  await editAsset(sculpt.dir + '/dat', (bytes) => {
    const dv = dvOf(bytes);
    for (const [it, d] of moves) dv.setFloat32(it.offset + 0x14, it.pos[1] + d);
  });
  const byOffset = new Map(moves.map(([it, d]) => [it.offset, d]));
  root?.traverse((o) => {
    const it = o.userData.placedItem ?? (o.parent?.name === 'markers' && o.userData.item);
    const d = it && byOffset.get(it.offset);
    if (!d) return;
    if (o.matrixAutoUpdate) o.position.y += d;
    else { o.matrix.elements[13] += d; o.matrixWorldNeedsUpdate = true; }
    it.pos[1] += d;
  });
}
async function commitTerrain(bytes) {
  if (!bytes) return;
  await historyGroup('Terrain', async () => {
    await editAsset(sculpt.dir + '/hmp', () => bytes);
    for (const k of [...terrainCache.keys()]) if (k.startsWith(sculpt.dir + '|')) terrainCache.delete(k);
    if ($('optFollow').checked) await followGround(sculpt.committed);
  });
  sculpt.committed = sculpt.grid.hts.slice();
}
// Heightmap as an 8-bit grey PNG, one pixel per grid point (the 256 stored height steps); white is high (game Y is down).
function exportHeightmap() {
  const g = sculpt.grid, c = elem('canvas');
  c.width = g.W;
  c.height = g.H;
  const img = new ImageData(g.W, g.H);
  g.hts.forEach((v, i) => {
    const p = 127 - Math.round(v);
    img.data.set([p, p, p, 255], i * 4);
  });
  c.getContext('2d').putImageData(img, 0, 0);
  c.toBlob((b) => saveBlob(b, `${sculpt.dir.split('/').pop()}_heightmap.png`), 'image/png');
}
async function importHeightmap() {
  const file = await pickImage();
  if (!file || !sculpt) return;
  const g = sculpt.grid, c = elem('canvas');
  c.width = g.W;
  c.height = g.H;
  const ctx = c.getContext('2d');
  ctx.imageSmoothingQuality = 'high';
  ctx.drawImage(await createImageBitmap(file), 0, 0, g.W, g.H);
  const px = ctx.getImageData(0, 0, g.W, g.H).data;
  sculpt.undo.push({ hts: g.hts.slice(), tex: g.tex.slice() });
  for (let i = 0; i < g.hts.length; i++) g.hts[i] = 127 - (px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2]) / 3;
  const bytes = rebuildTerrain();
  if (!bytes) {
    const u = sculpt.undo.pop();
    g.hts.set(u.hts);
    g.tex.set(u.tex);
    rebuildTerrain();
    alert('That heightmap needs more distinct terrain tiles than the level can hold. A smoother image uses fewer.');
    return;
  }
  await commitTerrain(bytes);
}
let stroke = null;
canvas.addEventListener('pointerdown', (e) => {
  if (e.button !== 0 || !sculptActive()) return;
  const h = sculptHit(e);
  if (!h) return;
  canvas.setPointerCapture(e.pointerId);
  const g = sculpt.grid;
  sculpt.undo.push({ hts: g.hts.slice(), tex: g.tex.slice() });
  if (sculpt.undo.length > 30) sculpt.undo.shift();
  sculpt.flat = g.hts[Math.round(h.gy) * g.W + Math.round(h.gx)];
  stroke = { last: h, dirty: brush(h), queued: false };
  if (stroke.dirty) stroke.bytes = rebuildTerrain();
});
canvas.addEventListener('pointermove', (e) => {
  if (!stroke || !sculptActive()) return;
  stroke.last = sculptHit(e) ?? stroke.last;
  if (stroke.queued) return;
  stroke.queued = true;
  requestAnimationFrame(() => {
    if (!stroke) return;
    stroke.queued = false;
    if (brush(stroke.last)) { stroke.dirty = true; stroke.bytes = rebuildTerrain(); }
  });
});
canvas.addEventListener('pointerup', async (e) => {
  if (!stroke || e.button !== 0) return;
  const s = stroke;
  stroke = null;
  if (!s.dirty) return;
  if (!s.bytes) {
    // Over the tile limit: drop the stroke.
    const u = sculpt.undo.pop();
    sculpt.grid.hts.set(u.hts);
    sculpt.grid.tex.set(u.tex);
    rebuildTerrain();
    return;
  }
  await commitTerrain(s.bytes);
});
const needSculpt = (fn) => () => (sculpt ? fn() : ($('sculptInfo').textContent = 'This level has no terrain to edit.'));
$('btnHeightOut').onclick = needSculpt(exportHeightmap);
$('btnHeightIn').onclick = needSculpt(importHeightmap);
for (const id of ['sculptRadius', 'sculptStrength']) $(id).oninput = () => ($(id + 'Val').textContent = $(id).value);

// A level's cutscenes are picked from its Scene panel: intro / extro (cutscene N plays over lv_N), the ending ceremony (id3_special, which
// the medal screen plays on level 3, stage 2), and the in-mission camera placements (subtype 0x26) the mission script cuts to by name.
let cutLevel = null;
function fillCutPick(dir) {
  cutLevel = dir;
  const ml = modLevelOf(dir), n = parseInt(dir.split('/lv_')[1], 36), files = [], cams = [];
  for (const [kind, label] of [['intro', 'Intro'], ['extro', 'Extro'], ['special', 'Ending ceremony']]) {
    const path = ml ? `data/cuts/${ml.folder}_${kind}` : `data/cuts/id${n}_${kind}`;
    if ((ml || n <= 15) && rom.assets.has(path)) files.push([`cut:${path}`, label]);
  }
  const camItems = (root?.getObjectByName('markers')?.children ?? []).map((m) => m.userData.item).filter((it) => it.type === 0 && it.sub === 0x26);
  for (const it of camItems) cams.push([`cam:${it.index}`, it.name || 'camera #' + it.index]);
  const opt = ([v, l]) => `<option value="${esc(v)}">${esc(l)}</option>`;
  // The level's opening crawl plays before its intro (stock chapter levels and mod levels; see levelCrawl).
  let seqsNow = [], crawlOpt = null;
  const fill = (seqs = seqsNow) => {
    seqsNow = seqs;
    const cuts = [...(crawlOpt ? [crawlOpt] : []), ...files];
    $('cutPick').innerHTML = opt(['', 'None (Level Editor)']) + (cuts.length ? `<optgroup label="Cutscenes">${cuts.map(opt).join('')}</optgroup>` : '')
      + (seqs.length ? `<optgroup label="In-mission sequences">${seqs.map(opt).join('')}</optgroup>` : '')
      + (cams.length ? `<optgroup label="In-mission cameras">${cams.map(opt).join('')}</optgroup>` : '');
    if (cut?.crawl) $('cutPick').value = 'crawl';
    $('cutPickHead').hidden = $('cutPickRow').hidden = !cuts.length && !cams.length;
  };
  fill([]);
  levelCrawl(dir).then((lc) => {
    if (cutLevel !== dir || !lc) return;
    crawlOpt = ['crawl', 'Opening crawl'];
    fill();
  });
  // The sequences need the camera records' next-item links, read from the level file.
  if (camItems.length) rom.read(dir + '/dat').then((dv) => {
    if (cutLevel !== dir) return;
    const recs = camItems.map((it) => camRecord(it, dv)), byName = new Map(recs.map((r) => [r.it.name, r])), linked = new Set(recs.map((r) => r.next));
    fill(recs.filter((r) => !linked.has(r.it.name)).map((r) => {
      const chain = seqChain(r, byName);
      const unused = SEQ_UNUSED.has(`${dir.split('/').pop()}:${r.it.name}`) ? ', unused' : '';
      return [`seq:${r.it.index}`, chain.length > 1 || unused ? `${r.it.name} (${chain.length > 1 ? chain.length + ' shots' : ''}${chain.length > 1 ? unused : unused.slice(2)})` : r.it.name];
    }));
  });
}
// In-mission camera sequences (func_800C7884 from the level script -> func_800FB2F8; traced 2026-10-19). A camera item (DAT 0/0x26) holds
// for +0x60 seconds; +0x64 / +0x68 are item-relative offsets to its target's and the next item's names ("-" for none). With a spawned
// target the camera stays put and aims at it every frame (func_8004E140: yaw atan2(-dx, dz), pitch asin(dy / |d|), no roll); without one
// it keeps the item's rotation. The next item is another camera (an instant cut), a restart point or nothing, which ends the sequence.
// The view letterboxes to 170 of 224 lines with the FOV going 65 -> 49.33 (65 * 170 / 224).
const datStr = (dv, at) => {
  let s = '';
  for (let c; at < dv.byteLength && (c = dv.getUint8(at)); at++) s += String.fromCharCode(c);
  return s;
};
function camRecord(it, dv) {
  const o = it.offset, name = (k) => {
    const rel = dv.getUint32(o + k);
    // Item-relative and 32-bit: a name earlier in the file is reached by wrapping.
    return rel ? datStr(dv, (o + rel) % 0x100000000) : '-';
  };
  return { it, hold: dv.getFloat32(o + 0x60), target: name(0x64), next: name(0x68) };
}
function seqChain(first, byName) {
  const chain = [first];
  for (let r = byName.get(first.next); r && !chain.includes(r) && chain.length < 16; r = byName.get(r.next)) chain.push(r);
  return chain;
}
// Each sequence's speech and music, from the level scripts (playObjectiveVoiceLine2 ids in call order; func_800C7D98 cue n -> song
// D_8010B304[n]: 0 0x35, 3 0x59, 4 0x3C, 6 0x39, 7 0x3B), keyed by level and starting camera. lv_g's lines are its all-three-saved win and
// its usual loss (others: win 385 alone; loss 58 + 323, or 619 + 58). lv_2 Camera_01-03 and lv_9 Camera_Gen have none.
// No script starts these two; they are in the level files only.
const SEQ_UNUSED = new Set(['lv_4:cutscene_disable_yacht', 'lv_7:cam_3']);
const SEQ_SONGS = { 0: 0x35, 3: 0x59, 4: 0x3c, 6: 0x39, 7: 0x3b };
const SEQ_AUDIO = {
  'lv_0:CameraPosition_01': [[552], 4], 'lv_2:Camera_04': [[285, 483], 3], 'lv_2:Camera_05': [[285, 483], 3], 'lv_2:Camera_06': [[285, 483], 3],
  'lv_3:Camera_Solo_New': [[488, 486, 342, 141, 1, 317, 142]], 'lv_3:Camera_Gvt_Bldg': [[90, 565]], 'lv_3:Camera_CM_1': [[4, 566], 3], 'lv_3:Camera_HL_Shuttle': [[5]],
  'lv_4:Cut_Camera': [[568, 48, 322], 3], 'lv_5:Camera1': [[14, 15, 327, 574, 328, 220, 329, 675], 0],
  'lv_7:cam_2': [[138, 282]], 'lv_7:cam_1': [[678, 139]], 'lv_7:cam_1c': [[678, 138]], 'lv_7:Camera_Shuttle_01': [[], 3], 'lv_7:Wedge_Cam_1': [[573, 341, 137, 259, 342, 329]],
  'lv_8:camera1': [[615], 6], 'lv_b:HeavyLifterShuttleCamera': [[586, 364], 0], 'lv_b:FacilityCamera': [[541], 3],
  'lv_c:City_Cam_01': [[465], 4], 'lv_c:Camera_AT_ST': [[714], 3], 'lv_c:City_Cam_02': [[677], 4], 'lv_d:Camera_01': [[245, 370], 3], 'lv_e:MOFF_SEERDON_CAMERA': [[471], 4],
  'lv_h:Cam_01': [[141, 317, 1, 2, 3, 319]], 'lv_g:Camera_Win': [[386, 595, 328]], 'lv_g:Camera_Lose': [[58]], 'lv_i:camera_fail': [[17, 6]], 'lv_i:camera_win_cut_1': [[702, 703], 7], 'lv_i:Camera_on_ATAT': [[294, 295, 311], 4],
  'lv_i:Camera_on_Rogue': [[701, 282, 283], 3],
};
// A sequence as cutscene data (the shape parseCutscene returns), sampled at 30 Hz, so it plays with the cutscene viewer's timeline, audio
// and effects: one camera entry per shot with type-4 cuts between them, wingman targets as model actors flying their splines, voice lines
// (type 0, at the speech queue's times) and the music sting (type 1 with its song id).
async function buildSequenceData(dir, startIndex) {
  const dv = await rom.read(dir + '/dat'), items = parseDAT(dv).items, fps = 30;
  const byName = new Map(items.filter((it) => it.type === 0 && it.sub === 0x26).map((it) => [it.name, camRecord(it, dv)]));
  const first = [...byName.values()].find((r) => r.it.index === startIndex), chain = seqChain(first, byName).slice(0, 6);
  // The hold counts down by the 30 Hz frame dt and the cut (or teardown) is dispatched at the top of the next frame
  // (updateEffectObjectOrientationQuat, runInMissionFrame), so each shot lasts its hold rounded up to whole frames, plus one.
  const holds = chain.map((r) => (Math.ceil(Math.max(0, r.hold) * fps - 1e-4) + 1) / fps), length = holds.reduce((a, b) => a + b, 0), frames = Math.max(2, Math.round(length * fps));
  // Path-following NPCs (initActorAsType1, ticked by executeActorType1Effect): record offsets of speed, start (seconds of travel), lift
  // (A0, A4) and the roll gain per handler; ground = flag 4 (Y on the terrain, less 0.25 * sink), normal = flag 0x10 (up eased to the
  // terrain normal at 30 deg/s), level = record word that sets flag 8 (no pitch), walk = seconds per walk cycle = walk * scale / speed.
  // Shuttles' wings stand at 110 deg when +0xB0 is set. shortcut: the shuttle pitch-over (+0xB4), TIE speeds above difficulty 0, holds
  // (path event 0x22) and links to a next spline at the path end are not followed; a path ends where it ends. No sequence target reaches
  // its path end (the furthest gets 0.91 along) and no sequence shuttle sets +0xB4.
  const SHUTTLE = { speed: 0x90, start: 0x9C, lift: 0x94, liftRot: 0x98, gain: 0.5, level: 0xB8, wings: 0xB0 };
  const TIE = { speed: 0x90, start: 0xA0, lift: 0x98, liftRot: 0x9C, gain: 2 }, GROUND = { speed: 0x90, start: 0x9C, lift: 0x94, liftRot: 0x98, gain: 0 };
  const PATH_NPCS = { 0x27: { speed: 0x98, start: 0xA8, lift: 0xA0, liftRot: 0xA4, gain: 1.5 }, 0x24: SHUTTLE, 0x48: SHUTTLE, 0x12: TIE, 0x13: TIE,
    0x1F: { ...GROUND, ground: true, walk: 2 }, 0x0F: { ...GROUND, ground: true, walk: 26.4 }, 0x2E: { ...GROUND, ground: true, normal: true, sink: 0xA0 }, 0x2F: GROUND };
  const targets = new Map();
  for (const r of chain) {
    const it = r.target !== '-' && items.find((i) => i.type === 0 && i.name === r.target);
    if (!it || targets.has(it.name)) continue;
    const path = it.model && items.find((s) => s.type === 3 && s.name === it.model && s.points?.length > 1), kind = path && PATH_NPCS[it.sub];
    // A uniform Catmull-Rom over the points, its parameter advancing speed / path length per second from start * speed / length (0 past 1).
    // shortcut: the path length (path +0x24) is taken as the curve's arc length.
    const curve = kind && new THREE.CatmullRomCurve3(path.points.map((p) => new THREE.Vector3(...p)), false, 'catmullrom', 0.5);
    const f = (k) => dv.getFloat32(it.offset + k), len = curve ? curve.getLength() : 0, speed = kind ? f(kind.speed) : 0;
    const t0 = kind && len ? (f(kind.start) * speed) / len : 0;
    targets.set(it.name, { it, kind, curve, len, speed, t0: t0 > 1 ? 0 : t0, rest: new THREE.Vector3(...(path ? path.points[0] : it.pos)) });
  }
  const pathU = (tg, sec) => 0.999 * THREE.MathUtils.clamp(tg.t0 + (tg.speed * sec) / tg.len, 0, 1) + 0.001;
  const at = (tg, sec) => (tg.curve ? tg.curve.getPoint(pathU(tg, sec)) : tg.rest.clone());
  const keys = (vals) => {
    const out = new Float32Array(vals.length * 3), n = vals.length - 1;
    vals.forEach((v, k) => out.set([v, k < n ? (vals[k + 1] - v) * n : 0, k / n], k * 3));
    return out;
  };
  const axisAngleOf = (m, prev) => {
    const q = new THREE.Quaternion().setFromRotationMatrix(m);
    if (prev && q.dot(prev) < 0) q.set(-q.x, -q.y, -q.z, -q.w);
    const ang = 2 * Math.acos(THREE.MathUtils.clamp(q.w, -1, 1)), s = Math.sqrt(Math.max(1e-12, 1 - q.w * q.w));
    return { q, axis: ang < 1e-6 ? [0, 1, 0] : [q.x / s, q.y / s, q.z / s], deg: THREE.MathUtils.radToDeg(ang) };
  };
  // Moving targets as model actors: wingmen by record +0x8C (npcWingmanUpdate: 0 wmxwng .. 7 t16, else wmxwng), the rest with the model the
  // level shows for them; position x 4 (the format's x 0.25), scale input so ch3 * 0.2222 gives the item scale over the model's divisor,
  // and the path actor's orientation (buildPositionAndCallTransformHelper): Ry(yaw) Rx(pitch) Rz(roll) from the tangent P(u) - P(u - 0.0009),
  // roll eased per frame as (roll + gain * yaw change) * 0.9375; the lift moves it 0.25 * A0 off the path, turned -A4 deg about forward.
  // Random parts are rolled once per load and baked, so scrubbing replays them: a walker's starting walk phase |(rand & 127) - 64| / 64
  // (npcAtStUpdate / npcAtAtUpdate) and the wingman weave below.
  const WINGMEN = ['wmxwng', 'wmywng', 'wmawng', 'wmvwng', 'wmspdr', 'wmfalc', 'tie', 't16'], homes = await buildObjectIndex();
  const holders = [];
  root?.traverse((h) => h.userData.placedItem && h.userData.source && holders.push(h));
  const actors = [], foilEvents = [], hidden = [], R = THREE.MathUtils.degToRad, D = THREE.MathUtils.radToDeg, Y = new THREE.Vector3(0, 1, 0);
  for (const tg of [...targets.values()].filter((t) => t.curve)) {
    const o = tg.it.offset, K = tg.kind, wingman = tg.it.sub === 0x27, i = actors.length;
    const holder = !wingman && holders.find((h) => h.userData.placedItem === tg.it || h.userData.placedItem.name === tg.it.name);
    let name, src = null, obj;
    if (holder) {
      src = holder.userData.source;
      name = src.name;
      obj = (await loadModel(src.id)).hob.objects[src.objIndex];
      hidden.push(tg.it.name);
    } else {
      const wk = dv.getUint32(o + 0x8c);
      name = wingman && homes.get(WINGMEN[wk]) ? WINGMEN[wk] : 'wmxwng';
      const m = await loadModel(homes.get(name));
      obj = m.hob.objects.find((h) => h.name === name) ?? m.hob.objects[0];
    }
    const f = (k) => dv.getFloat32(o + k), scale = tg.it.scale[0] || 0.25, lift = f(K.lift), liftRot = f(K.liftRot), sink = K.sink ? f(K.sink) : 0;
    const level = K.level && dv.getUint32(o + K.level), ch = [...Array(11)].map(() => []), up = Y.clone();
    let prevYaw = null, roll = 0, prevQ = null;
    // Wingman weave (computeRandomizedOrientationFrame 0x800F34DC, record +0x9C): an eased random lift e and side angle a, each retargeted
    // as the last ease ends, push the craft off the path about its forward axis and turn its nose toward the push. Its own seed per load.
    // shortcut: quaternions are taken as unit (the game's axis is not renormalised before orthonormalizeFrame).
    let seed = ((Math.random() * 2 ** 32) >>> 0) | 1;
    const rand = () => ((seed = Math.imul(seed, 0xA8351D63) >>> 0) >>> 6) & 0x7fff, fmod = (a, m) => a - Math.trunc(a / m) * m;
    const wv = wingman && f(0x9c) ? { amp0: f(0x9c) * 0.25, amp: 0, rate: 0, e: 0.5, eT: 0.5, eTimer: 0, eK: 0, eD: 0, e0: 0.5,
      a: 0, aT: 0, aTimer: 0, aK: 0, aD: 0, a0: 0, prevP: new THREE.Vector3(), q: new THREE.Quaternion(), last: null } : null;
    for (let k = 0; k <= frames; k++) {
      const sec = k / fps, u = pathU(tg, sec), p = tg.curve.getPoint(u), d = p.clone().sub(tg.curve.getPoint(u - 0.0009));
      const yaw = D(Math.atan2(d.x, d.z)), pitch = level ? 0 : D(Math.atan2(-d.y, Math.hypot(d.x, d.z)));
      if (prevYaw != null) roll = (roll + K.gain * (THREE.MathUtils.euclideanModulo(yaw - prevYaw + 180, 360) - 180)) * 0.9375;
      prevYaw = yaw;
      const mat = new THREE.Matrix4().makeRotationY(R(yaw)).multiply(new THREE.Matrix4().makeRotationX(R(pitch))).multiply(new THREE.Matrix4().makeRotationZ(R(roll)));
      if (lift > 0) p.add(new THREE.Vector3(0, 0.25 * lift, 0).applyAxisAngle(new THREE.Vector3(0, 0, 1).transformDirection(mat), R(-liftRot)));
      if (wv) {
        const dt = 1 / fps, sp = tg.speed, kk = sp * 0.25, w = wv;
        if (w.amp !== w.amp0) {
          w.rate ||= Math.abs(w.amp0 - w.amp) / 2;
          w.amp += Math.sign(w.amp0 - w.amp) * w.rate * dt;
          if (Math.abs(w.amp0 - w.amp) < 0.01) [w.amp, w.rate] = [w.amp0, 0];
        }
        if ((w.eTimer -= dt) <= 0) {
          const s = Math.abs(((rand() & 0x7f) - 64) / 160) + 0.1;
          w.eT = w.eD > 0 ? Math.max(w.eT - s, 0) : Math.min(w.eT + s, 1);
          [w.eTimer, w.eK, w.e0, w.eD] = [120, 0, w.e, w.eT - w.e];
        }
        if (w.e === w.eT) w.eTimer = 0;
        else if ((w.eK += 0.05 * kk) >= 1) [w.e, w.eTimer] = [w.eT, 0];
        else w.e = w.e0 + w.eD * 0.5 * (1 - Math.cos(Math.PI * w.eK));
        if ((w.aTimer -= dt) <= 0) {
          const mag = Math.abs(((rand() & 0x7f) - 64) / 0.376471) + 10;
          w.aT = fmod(w.a + (w.aD < 0 ? mag : -mag) * kk + 360, 360);
          let dd = Math.abs(w.aT - w.a);
          if (dd > 180) dd = 360 - dd;
          const x = fmod(w.a + dd, 360);
          [w.aTimer, w.aK, w.a0, w.aD] = [120, 0, w.a, x < w.aT - 1 || x > w.aT + 1 ? -dd : dd];
        }
        if (w.a === w.aT) w.aTimer = 0;
        else if ((w.aK += 0.022) >= 1) [w.a, w.aTimer] = [w.aT, 0];
        else w.a = w.a0 + w.aD * 0.5 * (1 - Math.cos(Math.PI * w.aK));
        w.a = fmod(w.a, 360);
        const xr = roll * 2 * kk, fwd = new THREE.Vector3(0, 0, 1).transformDirection(mat);
        const rise = xr < 90 ? Math.cos(Math.PI * w.e) * w.amp * 0.25 * Math.cos(R(xr)) * kk : 0;
        const off = new THREE.Vector3(0, rise, 0).applyAxisAngle(fwd, R(-w.a));
        const vel = w.last ? p.clone().sub(w.last).multiplyScalar(fps) : fwd.clone().multiplyScalar(sp);
        w.last = p.clone();
        p.add(off);
        const P = vel.add(off), Dv = P.clone().sub(w.prevP);
        w.prevP.copy(P);
        if (P.length() > 1e-5 && Dv.length() > 1e-5) {
          P.normalize();
          const axis = Dv.normalize().cross(fwd), ang = D(Math.acos(THREE.MathUtils.clamp(P.dot(fwd), -1, 1))) * kk;
          if (axis.lengthSq() > 1e-12) {
            let q = new THREE.Quaternion().setFromAxisAngle(axis.normalize(), R(-ang));
            if (ang > 10) q = w.q.clone().slerp(q, 10 / ang);
            w.q.copy(q);
            mat.premultiply(new THREE.Matrix4().makeRotationFromQuaternion(q));
          }
        }
      }
      if (K.ground) p.y = levelHeight(p.x, p.z) - 0.25 * sink;
      if (K.normal) {
        const e = 0.25, n = new THREE.Vector3(levelHeight(p.x - e, p.z) - levelHeight(p.x + e, p.z), 2 * e, levelHeight(p.x, p.z - e) - levelHeight(p.x, p.z + e)).normalize();
        const step = Math.min(up.angleTo(n), R(30) / fps);
        if (step > 1e-6) up.applyAxisAngle(new THREE.Vector3().crossVectors(up, n).normalize(), step);
        mat.premultiply(new THREE.Matrix4().makeRotationFromQuaternion(new THREE.Quaternion().setFromUnitVectors(Y.clone().transformDirection(mat), up)));
      }
      (tg.pts ??= []).push(p.clone());
      const a = axisAngleOf(mat, prevQ);
      prevQ = a.q;
      [p.x * 4, p.y * 4, p.z * 4, scale / (obj.scaleDiv || 1) / (cutAsset(name).scale || 1) / 0.2222, ...a.axis, a.deg, 0, 0, 0].forEach((v, c) => ch[c].push(v));
    }
    // npcWingmanResetState: the X-wing's S-foils start open (10 deg) when record +0xB4 is set, else closed; only +0x8F == 0 updates them.
    if (wingman && dv.getUint32(o + 0x8c) === 0 && dv.getUint32(o + 0xb4) && !dv.getUint8(o + 0x8f)) foilEvents.push({ type: 11, frame: 0, p: [i, 190, 1, 0] });
    if (K.wings && dv.getUint32(o + K.wings)) foilEvents.push({ type: 11, frame: 0, p: [i, 290, 1, 0] });
    // Walk cycle (npcAtStUpdate / npcAtAtUpdate): one cycle per walk * scale / speed seconds, as a type-13 rate over the whole sequence.
    if (K.walk && tg.speed > 0) foilEvents.push({ type: 13, frame: 0, p: [i, (length * tg.speed * 10) / (K.walk * scale), 0, 0] });
    actors.push({ name, src, anim: K.walk && src && src.id.replace(/[^/]*$/, '') + src.id.split('/').pop().split('_')[0] + '_walk', phase0: K.walk ? Math.abs(((Math.random() * 128) | 0) - 64) / 64 : 0, flags: 0xff, model: true, animated: true, parent: -1, ch: ch.map(keys), value: null });
  }
  // Shots: fixed at the item; aimed at the target (func_8004E140, no roll; +Z ahead and +Y down in game space) or the item's rotation.
  let t0 = 0;
  const cameras = chain.map((r, i) => {
    const tg = targets.get(r.target), pos = new THREE.Vector3(...r.it.pos), ch = [...Array(8)].map(() => []);
    let prev = null;
    for (let k = 0; k <= frames; k++) {
      const sec = THREE.MathUtils.clamp(k / fps, t0, t0 + holds[i]);
      let m;
      if (tg) {
        const z = (tg.pts ? tg.pts[Math.round(sec * fps)].clone() : at(tg, sec)).sub(pos).normalize(), y = new THREE.Vector3(0, 1, 0).addScaledVector(z, -z.y).normalize();
        m = new THREE.Matrix4().makeBasis(new THREE.Vector3().crossVectors(y, z), y, z);
      } else m = placementRotation(r.it.rot);
      const a = axisAngleOf(m, prev);
      prev = a.q;
      [pos.x * 4, pos.y * 4, pos.z * 4, ...a.axis, a.deg, 65 * 170 / 224].forEach((v, c) => ch[c].push(v));
    }
    t0 += holds[i];
    return ch.map(keys);
  });
  // Cuts, then speech and the sting.
  const events = [...foilEvents];
  t0 = 0;
  chain.forEach((r, i) => {
    events.push({ type: 4, frame: Math.round(t0 * fps), p: [i, 0, 0, 0] });
    t0 += holds[i];
  });
  const [voices = [], cue] = SEQ_AUDIO[`${dir.split('/').pop()}:${first.it.name}`] ?? [];
  let voiceRow = null;
  if (audioBank && voices.length) {
    voiceRow = { key: `seq:${dir}:${startIndex}`, kind: 'cut', rate: 16000, length, parts: voices.map((id) => ({ row: audioBank.byIndex.get(audioBank.voiceMap[id]), at: 0, voiceId: id })).filter((p) => p.row) };
    for (const p of cutSchedule(voiceRow)) events.push({ type: 0, frame: Math.round(p.at * fps), p: [p.voiceId, 0, 0, 0] });
  }
  if (cue != null && SEQ_SONGS[cue]) events.push({ type: 1, frame: 0, p: [0, 0x7f, 0, 0], song: SEQ_SONGS[cue] });
  events.sort((a, b) => a.frame - b.frame);
  // seq: the camera records behind the shots, for editing (hold +0x60, target +0x64, position +0x10, rotation +0x1C).
  const seq = { dir, shots: chain.map((r) => ({ off: r.it.offset, name: r.it.name, target: r.target, next: r.next, hold: r.hold })) };
  return { name: first.it.name, frames, length, fps, events, cameras, camera: cameras[0], actors, voiceRow, hidden, seq, picAspect: (4 / 3) * (224 / 170) };
}
// Looks through a camera placement: game cameras look down +Z (the marker pyramid opens that way).
function lookThroughCamera(index) {
  const m = root?.getObjectByName('markers')?.children.find((c) => c.userData.item.type === 0 && c.userData.item.index === index);
  if (!m) return;
  const it = m.userData.item;
  root.updateMatrixWorld(true);
  const p = root.localToWorld(new THREE.Vector3(...it.pos));
  const d = new THREE.Vector3(0, 0, 1).applyMatrix4(placementRotation(it.rot)).transformDirection(root.matrixWorld);
  camera.position.copy(p);
  controls.target.copy(p).addScaledVector(d, 5);
  controls.update();
}
$('cutPick').onchange = async () => {
  const v = $('cutPick').value;
  if (v === 'crawl') return showCrawl(cutLevel);
  if (v.startsWith('cut:')) return showCutscene(v.slice(4));
  if (v.startsWith('seq:')) return showCutscene(`seq:${cutLevel}:${v.slice(4)}`);
  if (cut) {
    await showLevel(cutLevel, true);
    $('cutPick').value = v;
  }
  if (v.startsWith('cam:')) lookThroughCamera(+v.slice(4));
};
