'use strict';
// WebMCP: tools an AI agent in the browser can call (registerTool, where the browser has it). Each answers JSON text (rs64_view_capture an image). The edit tools go through
// the page's own edit functions, so each edit is one step in its undo history (rs64_history undoes it) and is saved with the project as usual.
const WEBMCP_LIMIT = 200;
const vec3 = { type: 'array', items: { type: 'number' }, minItems: 3, maxItems: 3 };
const mcpNeedLevel = () => {
  mcpNeedRom();
  if (!selectedLevel || cut) throw new Error('No level open (or a cutscene is open over it): rs64_open a level first');
};
const mcpNeedCut = () => {
  mcpNeedRom();
  if (!cut?.file || cut.seq || cut.crawl) throw new Error('No cutscene open: rs64_open a cutscene first (sequences and opening crawls are not edited here)');
};
// A level object by DAT type and index; the first view of it (a placed model before its marker).
const mcpObject = (type, index) => {
  const hit = editObjects().find(([, x]) => x.type === type && x.index === index);
  if (!hit) throw new Error(`No object of type ${type} index ${index} in this level: see rs64_level_objects`);
  return hit;
};
// A cutscene track: a camera, or an actor with a model (as the page's track list offers).
const mcpTrack = (actor, camera) => {
  if (camera != null) {
    if (!cut.file.cameras[camera]) throw new Error(`No camera ${camera} in this cutscene`);
    return { cam: camera };
  }
  if (!cut.file.actors[actor]?.model) throw new Error(`No actor ${actor} with a model: give actor (an index from rs64_cutscene) or camera`);
  return { actor };
};
// The open level's terrain as the Terrain edit mode holds it (a brush stroke switches to that mode, as it would by hand); reading
// alone uses a parse of the level's HMP and leaves the mode as it is.
const mcpTerrainScale = (s) => new DataView(s.base.buffer, s.base.byteOffset).getFloat32(0x10);
const mcpSculptLoad = async (read = false) => {
  mcpNeedLevel();
  if (!rom.assets.has(selected + '/hmp')) throw new Error('This level has no terrain');
  if (sculpt?.dir === selected) return sculpt;
  if (read) {
    const base = u8(await rom.read(selected + '/hmp')).slice();
    return { base, grid: hmpGrid(parseHMP(dvOf(base))) };
  }
  setEditMode('terrain');
  await loadSculpt();
  if (!sculpt) throw new Error('The terrain could not be loaded');
  return sculpt;
};
const mcpRulesLevel = (level) => {
  const dir = level ?? (selectedLevel && !cut ? selected : null);
  if (!dir || !rom.assets.has(dir + '/dat')) throw new Error(`No level ${dir ?? '(none open)'}: use rs64_list with kind levels`);
  return dir;
};
const mcpEditMenu = () => {
  if (mode !== 'menus') setMode('menus');
};
const mcpRound = (v) => v.map((n) => +n.toFixed(2));
const mcpNeedRom = () => {
  if (!rom) throw new Error('No ROM loaded: open the Rogue Squadron (USA v1.0) ROM in the page first');
};
const mcpLevels = () => [...rom.assets.values()].filter((a) => a.name === 'dat').map((a) => {
  const ml = modLevelOf(a.dir), lv = a.dir.split('/').pop();
  return { id: a.dir, name: ml ? ml.title : /^lv_/.test(lv) ? lvName(lv) : lv, mod: !!ml };
});
const mcpItem = (it) => ({ type: it.type, index: it.index, sub: it.sub, kind: TYPES[markerKind(it)]?.label ?? String(markerKind(it)), name: it.name || null,
  model: it.model || null, pos: mcpRound(it.pos) });
const mcpEvent = (e, i) => ({ index: i, kind: TL_KIND[e.type] ?? `type ${e.type}`, type: e.type, frame: e.frame, params: e.p });

const WEBMCP_TOOLS = [
  {
    name: 'rs64_status',
    description: 'What the RS64 Explorer has open: whether a ROM is loaded, the list tab, the open level, cutscene, model, audio entry or menu, edit modes, and whether there are unsaved edits.',
    inputSchema: { type: 'object', properties: {} },
    run: () => ({ rom: !!rom, tab: mode, level: selectedLevel && !String(selected).startsWith('cut:') ? selected : null, model: !selectedLevel && mode === 'models' ? selected : null,
      cutscene: cut?.file ? lastCutPath : null, cutsceneEditing: !!cut?.file && cutEditing, levelEditMode: editMode, audio: audioSel ?? musicSel?.key ?? null,
      menu: menuSel, unsavedEdits: rom ? unsavedEdits() : false }),
  },
  {
    name: 'rs64_list',
    description: 'Lists what the ROM (plus the project\'s mod edits) holds: levels (DAT folders, mod levels included), cutscenes, models (HOB files), audio (voice, music, sound effects) or menus (the game\'s and the project\'s pages). Optional filter matches names case-insensitively.',
    inputSchema: { type: 'object', properties: { kind: { type: 'string', enum: ['levels', 'cutscenes', 'models', 'audio', 'menus'] }, filter: { type: 'string' },
      limit: { type: 'integer', minimum: 1, maximum: 1000 } }, required: ['kind'] },
    run: ({ kind, filter = '', limit = WEBMCP_LIMIT }) => {
      mcpNeedRom();
      const q = filter.toLowerCase(), keep = (...s) => !q || s.some((x) => String(x ?? '').toLowerCase().includes(q));
      const all = kind === 'levels' ? mcpLevels()
        : kind === 'cutscenes' ? [...rom.assets.keys()].filter((k) => k.startsWith('data/cuts/')).sort().map((id) => ({ id }))
        : kind === 'models' ? [...rom.assets.values()].filter((a) => a.name.endsWith('_HOB') && a.size > 8).map((a) => ({ id: a.path.replace(/_HOB$/, '') }))
        : kind === 'audio' ? [...(audioBank?.voice ?? []), ...(audioBank?.music ?? []), ...(audioBank?.sfx ?? [])].map((r) => ({ id: r.key, kind: r.kind, label: r.label, category: r.cat }))
        : [...Object.keys(NATIVE_MENUS).map((id) => ({ id, game: true })), ...menuPageCache.map((p) => ({ id: p.page, game: false, from: p.from }))];
      const hits = all.filter((x) => keep(x.id, x.name, x.label));
      return { total: hits.length, items: hits.slice(0, limit) };
    },
  },
  {
    name: 'rs64_level_objects',
    description: 'The objects and event markers in a level\'s DAT (with the project\'s edits): type, index, kind, name, model and position. Defaults to the open level; optional type filters by DAT record type (0 units, 2 points, 3 paths, 6 zones, 7 triggers).',
    inputSchema: { type: 'object', properties: { level: { type: 'string', description: 'A level id from rs64_list kind levels, e.g. data/level/lv_0' }, type: { type: 'integer' },
      limit: { type: 'integer', minimum: 1, maximum: 2000 } } },
    run: async ({ level, type, limit = WEBMCP_LIMIT }) => {
      mcpNeedRom();
      const dir = level ?? (selectedLevel ? selected : null);
      if (!dir || !rom.assets.has(dir + '/dat')) throw new Error(`No level ${dir ?? '(none open)'}: use rs64_list with kind levels`);
      const items = parseDAT(await rom.read(dir + '/dat')).items.filter((it) => type == null || it.type === type);
      return { level: dir, total: items.length, items: items.slice(0, limit).map(mcpItem) };
    },
  },
  {
    name: 'rs64_cutscene',
    description: 'A cutscene file (with the project\'s edits): length, frame rate, actors (name, model, parent) and events (kind, frame, parameters). Defaults to the open cutscene.',
    inputSchema: { type: 'object', properties: { path: { type: 'string', description: 'A cutscene from rs64_list kind cutscenes, e.g. data/cuts/id0_extro' } } },
    run: async ({ path }) => {
      mcpNeedRom();
      const f = path ? (rom.assets.has(path) ? parseCutscene(await rom.read(path)) : null) : cut?.file;
      if (!f) throw new Error(path ? `No cutscene ${path}` : 'No cutscene open: pass a path');
      return { path: path ?? lastCutPath, frames: f.frames, fps: f.fps, seconds: +(f.frames / f.fps).toFixed(2),
        actors: f.actors.map((a, i) => ({ index: i, name: a.name, model: !!a.model, parent: a.parent })), events: f.events.map(mcpEvent) };
    },
  },
  {
    name: 'rs64_selection',
    description: 'What is selected in the editor: the level object (its record fields) or the cutscene event or track.',
    inputSchema: { type: 'object', properties: {} },
    run: () => {
      const ev = cut?.file && cutSelEvent != null ? cut.file.events[cutSelEvent] : null;
      return { levelObject: sel?.it ? { ...mcpItem(sel.it), rot: sel.it.rot && mcpRound(sel.it.rot), scale: sel.it.scale && mcpRound(sel.it.scale) } : null,
        cutsceneEvent: ev ? mcpEvent(ev, cutSelEvent) : null,
        cutsceneTrack: cutTrack?.actor != null ? { actor: cutTrack.actor, name: cut.file.actors[cutTrack.actor]?.name } : cutTrack?.cam != null ? { camera: cutTrack.cam } : null };
    },
  },
  {
    name: 'rs64_menu',
    description: 'A menu as the game shows it with the project\'s menu mod merged in: its title and rows in order (label, whether it is the game\'s own entry or a mod button, the button id and type). menu is a name from rs64_list kind menus.',
    inputSchema: { type: 'object', properties: { menu: { type: 'string' } }, required: ['menu'] },
    run: async ({ menu }) => {
      mcpNeedRom();
      const doc = menuWithDefaults((await readMenu()).doc), native = NATIVE_MENUS[menu];
      if (!native && !menuPagesOf(doc).some((b) => b.page === menu)) throw new Error(`No menu ${menu}: use rs64_list with kind menus`);
      const rows = menuLayout(doc, menu, native?.entries ?? [], () => true, native?.max ?? 16).rows;
      return { menu, title: menuText(doc.titles?.[menu]?.label ?? native?.title ?? menuPagesOf(doc).find((b) => b.page === menu)?.title ?? ''),
        rows: rows.map((r) => (r.native ? { game: true, alias: r.alias ?? null, label: menuText(r.entry.label) } : { game: false, id: r.id, type: r.button.type ?? 'action', label: menuText(r.button.label ?? r.button.label_off ?? '') })) };
    },
  },
  {
    name: 'rs64_open',
    edit: false,
    description: 'Opens something in the explorer, as clicking it in the list does: a level (then edit it with the rs64_level_* tools), a cutscene (rs64_cutscene_*; editing is switched on), a model, or a menu (rs64_menu_*). Ids come from rs64_list.',
    inputSchema: { type: 'object', properties: { kind: { type: 'string', enum: ['level', 'cutscene', 'model', 'menu'] }, id: { type: 'string' },
      object: { type: 'integer', description: 'For a model package (opkg), the object index' } }, required: ['kind', 'id'] },
    run: async ({ kind, id, object }) => {
      mcpNeedRom();
      if (kind === 'level') {
        if (!rom.assets.has(id + '/dat')) throw new Error(`No level ${id}`);
        setMode('levels');
        await showLevel(id);
      } else if (kind === 'cutscene') {
        if (!rom.assets.has(id)) throw new Error(`No cutscene ${id}`);
        await showCutscene(id);
        if (!cutEditing) $('cutEdit').click();
      } else if (kind === 'model') {
        if (!rom.assets.has(id + '_HOB')) throw new Error(`No model ${id}`);
        setMode('models');
        await showModel(id, object ?? -1);
      } else {
        await menuRefreshPages();
        if (!NATIVE_MENUS[id] && !menuPageCache.some((p) => p.page === id)) throw new Error(`No menu ${id}: use rs64_list with kind menus`);
        setMode('menus');
        await showMenu(id);
      }
      return WEBMCP_TOOLS[0].run();
    },
  },
  {
    name: 'rs64_level_add_object',
    edit: true,
    description: 'Adds an object to the open level as a copy of one already in it (copyType + copyIndex from rs64_level_objects), or of the first object using a model the level loads (model, a model id), at pos. Returns the new object.',
    inputSchema: { type: 'object', properties: { copyType: { type: 'integer' }, copyIndex: { type: 'integer' }, model: { type: 'string' }, pos: vec3 }, required: ['pos'] },
    run: async ({ copyType, copyIndex, model, pos }) => {
      mcpNeedLevel();
      let src;
      if (model) {
        const h = root.children.find((o) => o.userData.placedItem && o.userData.source?.id === model);
        if (!h) throw new Error(`No object in this level uses ${model}, so the level does not load it`);
        src = h.userData.placedItem;
      } else {
        if (copyType == null || copyIndex == null) throw new Error('Give copyType and copyIndex, or model');
        src = mcpObject(copyType, copyIndex)[1];
      }
      if (src.pathEvent) throw new Error('A path event is added on its spline in the page, not copied');
      await addItem(src, pos);
      return { added: sel?.it ? mcpItem(sel.it) : null };
    },
  },
  {
    name: 'rs64_level_set_object',
    edit: true,
    description: 'Moves, rotates or scales an object in the open level (type + index from rs64_level_objects). rot is the record\'s rotation in degrees about X, Y, Z (applied in ZYX order), scale is used by placed models. A spline-following object moves with its spline instead.',
    inputSchema: { type: 'object', properties: { type: { type: 'integer' }, index: { type: 'integer' }, pos: vec3, rot: vec3, scale: vec3 }, required: ['type', 'index'] },
    run: async ({ type, index, pos, rot, scale }) => {
      mcpNeedLevel();
      const [o] = mcpObject(type, index);
      select(o);
      if (sel.fixed || sel.pathNode) throw new Error(`This object can't be moved on its own${sel.fixed ?? ': it sits on a spline'}`);
      const it = sel.it;
      if (pos) {
        const d = pos.map((v, a) => v - it.pos[a]);
        it.points?.forEach((pt) => d.forEach((v, a) => (pt[a] += v)));
        it.pos = pos.slice();
      }
      if (rot && it.rot) it.rot = rot.slice();
      if (scale && Array.isArray(it.scale)) it.scale = scale.slice();
      itemToProxy();
      showItemChange();
      await commitSel();
      return { object: mcpItem(sel?.it ?? it) };
    },
  },
  {
    name: 'rs64_level_delete_object',
    edit: true,
    description: 'Deletes an object (type 0, 6 or 7) from the open level. Refused when the mission\'s script names it; when other items mention it, refused unless force is true.',
    inputSchema: { type: 'object', properties: { type: { type: 'integer' }, index: { type: 'integer' }, force: { type: 'boolean' } }, required: ['type', 'index'] },
    run: async ({ type, index, force = false }) => {
      mcpNeedLevel();
      const it = mcpObject(type, index)[1], dir = selected;
      if (!canDelete(it)) throw new Error('Only units, zones and triggers can be deleted');
      if (it.name && nameUses(romBytes.subarray(0, BLOB_START), it.name, 0, 0)) throw new Error(`"${it.name}" is named by the mission's script, so deleting it could break the mission: move it away instead`);
      const dv = await rom.read(dir + '/dat'), refs = it.name ? nameUses(u8(dv), it.name, it.offset, it.offset + dv.getUint32(it.offset + 8)) : 0;
      if (refs && !force) throw new Error(`Other items in this level mention "${it.name}" (${refs}x: triggers, paths or areas); pass force to delete it anyway`);
      await editAsset(dir + '/dat', (b) => (it.type === 0 ? datRemoveItem(b, 0, it.index) : datRemoveIndexed(b, it.type, it.index)));
      await reloadLevel(dir);
      return { deleted: mcpItem(it) };
    },
  },
  {
    name: 'rs64_cutscene_add_actor',
    edit: true,
    description: 'Adds an actor to the open cutscene: a model id from rs64_list kind models, or an actor name the cutscenes already use. It starts where the first actor is (or in front of the camera) at the stock scale for that model.',
    inputSchema: { type: 'object', properties: { model: { type: 'string' } }, required: ['model'] },
    run: async ({ model }) => {
      mcpNeedCut();
      await cutScanPathModels();
      const name = cutModelNames().includes(model) ? model : await cutActorNameFor(model);
      if (!name) throw new Error(`Cutscenes can't load ${model}`);
      const before = cut.file.actors.length;
      await cutAddActorNamed(name);
      if (cut.file.actors.length === before) throw new Error($('cutStatus').textContent || 'The actor was not added');
      return { actor: { index: cut.file.actors.length - 1, name: cut.file.actors.at(-1).name } };
    },
  },
  {
    name: 'rs64_cutscene_add_event',
    edit: true,
    description: 'Adds an event to the open cutscene at a frame (or seconds): type is the event kind (0 voice line, 1 music, 2 music stop, 3 sound, 5 effect, 6 appear, 7 disappear, 8 weapon fire, ... as rs64_cutscene lists them) with up to 4 parameters (p0 is the voice id, song cue, sound slot or actor index).',
    inputSchema: { type: 'object', properties: { type: { type: 'integer' }, frame: { type: 'integer' }, seconds: { type: 'number' },
      params: { type: 'array', items: { type: 'integer' }, maxItems: 4 } }, required: ['type'] },
    run: async ({ type, frame, seconds, params = [] }) => {
      mcpNeedCut();
      if (!(type in TL_KIND)) throw new Error(`Unknown event type ${type}: ${Object.entries(TL_KIND).map(([k, v]) => `${k} ${v}`).join(', ')}`);
      const f = cut.file, at = THREE.MathUtils.clamp(Math.round(frame ?? (seconds ?? cut.t * f.frames / f.fps) * f.fps), 0, f.frames);
      let i = -1;
      if (!(await editCutFile('Add ' + TL_KIND[type], (g) => (i = cutAddEvent(g, { type, frame: at, p: [0, 1, 2, 3].map((k) => (params[k] ?? 0) >>> 0) }))))) throw new Error($('cutStatus').textContent || 'The event was not added');
      selectCutEvent(i);
      return { event: mcpEvent(cut.file.events[i], i) };
    },
  },
  {
    name: 'rs64_cutscene_delete_event',
    edit: true,
    description: 'Deletes an event (index from rs64_cutscene) from the open cutscene. Events a camera mode or a shot sets are changed there, not deleted.',
    inputSchema: { type: 'object', properties: { index: { type: 'integer' } }, required: ['index'] },
    run: async ({ index }) => {
      mcpNeedCut();
      const ev = cut.file.events[index];
      if (!ev) throw new Error(`No event ${index}`);
      if (cutBakedEvents(cut.file, cutAuth).includes(ev)) throw new Error('This event is set by a camera mode or a shot: change it there');
      selectCutEvent(null);
      await editCutFile('Delete ' + (TL_KIND[ev.type] ?? 'event'), (f) => cutRemoveEvent(f, index));
      return { deleted: mcpEvent(ev, index) };
    },
  },
  {
    name: 'rs64_menu_add_button',
    edit: true,
    description: 'Adds a button to a menu in the project\'s menu mod (docs/guides/adding-menus-and-buttons.md has every field): menu, then fields such as type (action, toggle, cycle, slider, submenu, label), label, action / toggle / slider key, before / after / replace (a native alias or button id), x, y, requires. Returns its id.',
    inputSchema: { type: 'object', properties: { menu: { type: 'string' }, fields: { type: 'object' } }, required: ['menu'] },
    run: async ({ menu, fields = {} }) => {
      mcpNeedRom();
      mcpEditMenu();
      const { id: _, menu: __, ...extra } = fields;
      await addMenuButton(menu, extra);
      return { id: menuPick, menu };
    },
  },
  {
    name: 'rs64_menu_set_button',
    edit: true,
    description: 'Changes fields of a button in the project\'s menu mod by id (from rs64_menu): set maps field names to values, null removes a field. A built-in button (quit, pause_quit, language) is copied into the mod on its first change.',
    inputSchema: { type: 'object', properties: { id: { type: 'string' }, set: { type: 'object' } }, required: ['id', 'set'] },
    run: async ({ id, set }) => {
      mcpNeedRom();
      mcpEditMenu();
      const { doc } = await readMenu();
      if (!doc.buttons.some((b) => b.id === id) && !MENU_DEFAULT_BUTTONS.some((b) => b.id === id)) throw new Error(`No button ${id}`);
      await editMenuButton(id, (b) => Object.entries(set).forEach(([k, v]) => k !== 'id' && (v === null ? delete b[k] : (b[k] = v))), `${id}: ${Object.keys(set).join(', ')}`);
      return { button: (await readMenu()).doc.buttons.find((b) => b.id === id) };
    },
  },
  {
    name: 'rs64_menu_delete_button',
    edit: true,
    description: 'Removes a button from the project\'s menu mod by id. A built-in button falls back to the game\'s default.',
    inputSchema: { type: 'object', properties: { id: { type: 'string' } }, required: ['id'] },
    run: async ({ id }) => {
      mcpNeedRom();
      mcpEditMenu();
      const { doc } = await readMenu();
      if (!doc.buttons.some((b) => b.id === id)) throw new Error(`No button ${id} in the mod's menu file`);
      await writeMenu({ ...doc, buttons: doc.buttons.filter((b) => b.id !== id) }, `Delete ${id}`);
      if (menuSel) showMenu(menuSel);
      return { deleted: id };
    },
  },
  {
    name: 'rs64_history',
    edit: true,
    description: 'Undoes or redoes the last edit (any edit in the page, the agent\'s or the user\'s), as the Undo / Redo buttons do.',
    inputSchema: { type: 'object', properties: { action: { type: 'string', enum: ['undo', 'redo'] } }, required: ['action'] },
    run: async ({ action }) => {
      mcpNeedRom();
      const can = action === 'undo' ? history.at > 0 : history.at < history.steps.length;
      if (!can) throw new Error(`Nothing to ${action}`);
      await (action === 'undo' ? undoEdit() : redoEdit());
      return { steps: history.at, of: history.steps.length };
    },
  },
  {
    name: 'rs64_view_capture',
    edit: false,
    description: 'A picture of what the explorer shows: the 3D view (model, level, or cutscene at its current frame) or, in the Menus tab, the menu preview at the game\'s 640 x 480. Returns a JPEG no wider than maxWidth (default 960).',
    inputSchema: { type: 'object', properties: { maxWidth: { type: 'integer', minimum: 160, maximum: 1920 } } },
    run: async ({ maxWidth = 960 }) => {
      mcpNeedRom();
      let src, w, h;
      const box = mode === 'menus' && !$('menuPanel').hidden ? document.querySelector('#menuPanel .menuPreview') : null;
      if (box) {
        // The preview is HTML: each line is its own canvas, drawn here where it sits on the 640 x 480 screen.
        const r = box.getBoundingClientRect(), k = 640 / r.width;
        src = Object.assign(elem('canvas'), { width: 640, height: 480 });
        const g = src.getContext('2d');
        g.fillStyle = '#000';
        g.fillRect(0, 0, 640, 480);
        for (const c of box.querySelectorAll('canvas')) {
          const cr = c.getBoundingClientRect();
          g.drawImage(c, (cr.left - r.left) * k, (cr.top - r.top) * k, cr.width * k, cr.height * k);
        }
        [w, h] = [640, 480];
      } else {
        if (document.body.classList.contains('audio') || document.body.classList.contains('fonts')) throw new Error('No 3D view on screen in this tab: rs64_open a model, level or cutscene, or open a menu');
        // WebGPU keeps the canvas image only until the frame is handed off, so a frame is drawn and copied in one go.
        renderer.render(scene, camera);
        [src, w, h] = [canvas, canvas.width, canvas.height];
      }
      const s = Math.min(1, maxWidth / w), out = Object.assign(elem('canvas'), { width: Math.round(w * s), height: Math.round(h * s) });
      out.getContext('2d').drawImage(src, 0, 0, out.width, out.height);
      const data = out.toDataURL('image/jpeg', 0.85).split(',')[1];
      return { mcpContent: [{ type: 'image', data, mimeType: 'image/jpeg' }, { type: 'text', text: JSON.stringify({ width: out.width, height: out.height, showing: box ? `menu ${menuSel}` : cut?.file ? `cutscene ${lastCutPath} at ${(cut.t * cut.data.length).toFixed(2)} s` : selected }) }] };
    },
  },
  {
    name: 'rs64_cutscene_keys',
    edit: false,
    description: 'The animation keys of a track in the open cutscene: an actor (index from rs64_cutscene) or a camera. Lists each key\'s time in seconds and every channel\'s value there (actor: x y z, scale, rotation axis x y z and angle, pivot x y z; camera: x y z, axis x y z, angle, FOV). snapshots: true when a camera\'s keys are baked from view snapshots and can only be changed in the page.',
    inputSchema: { type: 'object', properties: { actor: { type: 'integer' }, camera: { type: 'integer' } } },
    run: ({ actor, camera }) => {
      mcpNeedCut();
      const f = cut.file, tr = mcpTrack(actor, camera), names = CUT_CH_NAMES[tr.cam != null ? 'cam' : 'actor'];
      const static_ = tr.actor != null && !f.actors[tr.actor].animated;
      const times = static_ ? [0] : cutKeyTimes(f, tr), chs = static_ ? null : cutTrackChannels(f, tr), half = 0.5 / f.frames;
      // A channel's stored key where it has one at that time (the sampled curve, as the game plays it, sits slightly off it).
      const stored = (c, t, v) => {
        const keys = chs?.[c];
        for (let j = 0; keys && j < keys.length / 3; j++) if (Math.abs(keys[j * 3 + 2] - t) < half) return keys[j * 3];
        return v;
      };
      return { track: tr, length: f.length, animated: !static_, snapshots: cutSnaps(tr).length > 0, channels: names,
        keys: times.map((t) => ({ seconds: +(t * f.length).toFixed(3), values: Object.fromEntries(cutTrackValues(f, tr, t).map((v, c) => [names[c], +stored(c, t, v).toFixed(4)])) })) };
    },
  },
  {
    name: 'rs64_cutscene_set_key',
    edit: true,
    description: 'Sets an actor\'s or camera\'s key at a time (seconds) in the open cutscene: values maps channel names (see rs64_cutscene_keys) to numbers; channels left out keep the value they have there. A key that does not exist is added. A static actor becomes animated first. To animate in level terms instead: levelPos puts a top-level actor (or a camera) at a level position (the coordinates rs64_level_objects uses; channels x y z are those times 4), and rotation sets the rotation as degrees about X, Y, Z (the order level objects use, ZYX) instead of axis + angle.',
    inputSchema: { type: 'object', properties: { actor: { type: 'integer' }, camera: { type: 'integer' }, seconds: { type: 'number', minimum: 0 }, values: { type: 'object' }, levelPos: vec3, rotation: vec3 }, required: ['seconds'] },
    run: async ({ actor, camera, seconds, values = {}, levelPos, rotation }) => {
      mcpNeedCut();
      const f = cut.file, tr = mcpTrack(actor, camera), names = CUT_CH_NAMES[tr.cam != null ? 'cam' : 'actor'];
      if (cutSnaps(tr).length) throw new Error('This camera\'s keys are baked from view snapshots: change them in the page (fly the view, press V)');
      values = { ...values };
      if (levelPos) {
        if (tr.actor != null && f.actors[tr.actor].parent >= 0) throw new Error('levelPos is for a top-level actor: a child actor\'s x y z are in its parent\'s space');
        [values.x, values.y, values.z] = levelPos.map((v) => v * 4);
      }
      if (rotation) {
        const q = new THREE.Quaternion().setFromEuler(new THREE.Euler(...rotation.map((d) => THREE.MathUtils.degToRad(d)), 'ZYX'));
        const w = THREE.MathUtils.clamp(q.w, -1, 1), sin = Math.sqrt(1 - w * w), axis = sin < 1e-6 ? [0, 1, 0] : [q.x / sin, q.y / sin, q.z / sin];
        [values['axis x'], values['axis y'], values['axis z'], values.angle] = [...axis, THREE.MathUtils.radToDeg(2 * Math.acos(w))];
      }
      if (!Object.keys(values).length) throw new Error('Give values, levelPos or rotation');
      const bad = Object.keys(values).filter((k) => !names.includes(k) || !Number.isFinite(values[k]));
      if (bad.length) throw new Error(`Unknown channel or not a number: ${bad.join(', ')}; channels are ${names.join(', ')}`);
      const t = THREE.MathUtils.clamp(seconds / f.length, 0, 1), now = tr.actor != null && !f.actors[tr.actor].animated ? [...f.actors[tr.actor].value] : cutTrackValues(f, tr, t);
      const ok = await editCutFile('Key value', (g) => {
        if (tr.actor != null) cutAnimateActor(g, tr.actor);
        names.forEach((n, ch) => cutSetKey(g, { ...tr, ch }, t, values[n] ?? now[ch]));
      });
      if (!ok) throw new Error($('cutStatus').textContent || 'The key was not set');
      return WEBMCP_TOOLS.find((x) => x.name === 'rs64_cutscene_keys').run({ actor, camera });
    },
  },
  {
    name: 'rs64_cutscene_delete_key',
    edit: true,
    description: 'Removes the keys at a time (seconds) from an actor\'s or camera\'s track in the open cutscene. The key at 0 s starts the track and stays.',
    inputSchema: { type: 'object', properties: { actor: { type: 'integer' }, camera: { type: 'integer' }, seconds: { type: 'number' } }, required: ['seconds'] },
    run: async ({ actor, camera, seconds }) => {
      mcpNeedCut();
      const f = cut.file, tr = mcpTrack(actor, camera);
      if (cutSnaps(tr).length) throw new Error('This camera\'s keys are baked from view snapshots: change them in the page');
      const t = cutKeyTimes(f, tr).find((x) => Math.abs(x * f.length - seconds) < 0.5 / f.fps);
      if (t == null) throw new Error(`No key at ${seconds} s: see rs64_cutscene_keys`);
      if (t === 0) throw new Error('The key at 0 s starts the track and stays');
      await editCutFile('Delete key', (g) => cutRetimeKeys(g, tr, t, null));
      return WEBMCP_TOOLS.find((x) => x.name === 'rs64_cutscene_keys').run({ actor, camera });
    },
  },
  {
    name: 'rs64_mod_level_new',
    edit: true,
    description: 'Makes a new mod level from a stock level (from, e.g. data/level/lv_0): a copy that plays that level\'s mission script, or with blank true only a player start on flat ground with that level\'s look (then give it objectives with rs64_mod_level_set). Opens it.',
    inputSchema: { type: 'object', properties: { from: { type: 'string' }, title: { type: 'string', minLength: 1 }, blank: { type: 'boolean' } }, required: ['from', 'title'] },
    run: async ({ from, title, blank = false }) => {
      mcpNeedRom();
      if (!/^data\/level\/lv_[0-9a-z]+$/.test(from) || !rom.assets.has(from + '/dat') || modLevelOf(from)) throw new Error(`${from} is not a stock level: use rs64_list with kind levels`);
      setMode('levels');
      const made = await newModLevel(from, blank, title);
      if (!made) throw new Error('The level was not made');
      return { level: 'data/level/' + made.folder, settings: modLevelOf('data/level/' + made.folder) };
    },
  },
  {
    name: 'rs64_mod_level_get',
    edit: false,
    description: 'A mod level\'s settings as levels.json holds them (docs/guides/modding.md "New levels"): title, description, voice, craft, objectives, medals, music, crawl and the rest. Defaults to the open level.',
    inputSchema: { type: 'object', properties: { level: { type: 'string' } } },
    run: ({ level }) => {
      mcpNeedRom();
      const dir = level ?? (selectedLevel ? selected : null), l = dir && modLevelOf(dir);
      if (!l) throw new Error(`${dir ?? 'No level open'}: not a mod level (stock levels have no settings here; make one with rs64_mod_level_new)`);
      return { level: dir, settings: l };
    },
  },
  {
    name: 'rs64_mod_level_set',
    edit: true,
    description: 'Changes a mod level\'s settings: set maps levels.json fields to values (null removes a field), e.g. { "title": "...", "objectives": [...], "medals": {...} }. id, folder and template can\'t change. Defaults to the open level.',
    inputSchema: { type: 'object', properties: { level: { type: 'string' }, set: { type: 'object' } }, required: ['set'] },
    run: async ({ level, set }) => {
      mcpNeedRom();
      const dir = level ?? (selectedLevel ? selected : null), l = dir && modLevelOf(dir);
      if (!l) throw new Error(`${dir ?? 'No level open'}: not a mod level`);
      const fixed = Object.keys(set).filter((k) => ['id', 'folder', 'template'].includes(k));
      if (fixed.length) throw new Error(`${fixed.join(', ')} can't change`);
      await editModLevels((levels) => {
        const e = levels.find((x) => x.folder === l.folder);
        for (const [k, v] of Object.entries(set)) v === null ? delete e[k] : (e[k] = v);
      }, `${l.title}: ${Object.keys(set).join(', ')}`);
      renderList();
      return { level: dir, settings: modLevelOf(dir) };
    },
  },
  {
    name: 'rs64_rules_get',
    edit: false,
    description: 'A level\'s rules ("when X do Y", rules.json), plus the whens and actions a rule can use (with their parameters; mods\' own included). Defaults to the open level.',
    inputSchema: { type: 'object', properties: { level: { type: 'string' } } },
    run: async ({ level }) => {
      mcpNeedRom();
      const dir = mcpRulesLevel(level);
      return { level: dir, rules: (await readRules(dir)).rules, whens: RULE_WHENS, actions: RULE_ACTIONS,
        modActions: ruleExt.filter((e) => e.kind === 'action'), modEvents: ruleExt.filter((e) => e.kind === 'event') };
    },
  },
  {
    name: 'rs64_rules_set',
    edit: true,
    description: 'Replaces a level\'s rules with the given list (the shape rs64_rules_get returns: { when, object?, who?, actions: [{ do, ...params }] }). Whens and actions must be ones rs64_rules_get lists. Defaults to the open level.',
    inputSchema: { type: 'object', properties: { level: { type: 'string' }, rules: { type: 'array', items: { type: 'object' } } }, required: ['rules'] },
    run: async ({ level, rules }) => {
      mcpNeedRom();
      const dir = mcpRulesLevel(level);
      const whens = new Set([...RULE_WHENS.map((w) => w.when), ...ruleExt.filter((e) => e.kind === 'event').map((e) => e.name)]);
      const acts = new Set([...RULE_ACTIONS.map((a) => a.do), ...ruleExt.filter((e) => e.kind === 'action').map((e) => e.name)]);
      rules.forEach((r, i) => {
        if (!whens.has(r.when)) throw new Error(`Rule ${i}: unknown when "${r.when}"`);
        for (const a of r.actions ?? []) if (!acts.has(a.do)) throw new Error(`Rule ${i}: unknown action "${a.do}"`);
      });
      await writeRules(dir, { rules }, `Rules: ${dir.split('/').pop()}`);
      return { level: dir, rules: (await readRules(dir)).rules };
    },
  },
  {
    name: 'rs64_export_plan',
    edit: false,
    description: 'What Export mod would make, without saving anything: each mod (name and the changed files or menu buttons in it, grouped by kind or the user\'s groups, linked changes kept together), and level rules that name objects that are gone.',
    inputSchema: { type: 'object', properties: {} },
    run: async () => {
      mcpNeedRom();
      const items = await exportItems(), doc = await readJson(EXPORT_GROUPS, () => ({ groups: [], assign: {}, kindNames: {} }));
      const base = (projectHandle?.name ?? 'my-rs64-mod').replace(/\.rs64proj$/, '');
      const kindNames = Object.fromEntries(MOD_KINDS.map((k) => [k, doc.kindNames?.[k] ?? `${base} - ${KIND_LABEL[k]}`]));
      return { mods: exportPlan(items, doc, await exportLinks(items), kindNames), brokenRules: await brokenRuleLines() };
    },
  },
  {
    name: 'rs64_search',
    edit: false,
    description: 'Finds text across the game: voice lines (subtitles and descriptions), level object and marker names (every level), cutscene actor names, menu labels (the game\'s and the project\'s), and model ids. Case-insensitive; in limits where to look.',
    inputSchema: { type: 'object', properties: { query: { type: 'string', minLength: 1 }, in: { type: 'array', items: { type: 'string', enum: ['voice', 'objects', 'cutscenes', 'menus', 'models'] } },
      limit: { type: 'integer', minimum: 1, maximum: 500 } }, required: ['query'] },
    run: async ({ query, in: where = ['voice', 'objects', 'cutscenes', 'menus', 'models'], limit = 50 }) => {
      mcpNeedRom();
      const q = query.toLowerCase(), has = (s) => String(s ?? '').toLowerCase().includes(q), out = {};
      if (where.includes('voice')) out.voice = (audioBank?.voice ?? []).filter((r) => has(r.text) || has(r.label)).slice(0, limit).map((r) => ({ id: r.key, label: r.label, text: r.text.trim(), voiceIds: r.voiceIds ?? [] }));
      if (where.includes('objects')) {
        out.objects = [];
        for (const l of mcpLevels()) {
          for (const it of parseDAT(await rom.read(l.id + '/dat')).items) if (has(it.name) || has(it.model)) out.objects.push({ level: l.id, ...mcpItem(it) });
          if (out.objects.length >= limit) break;
        }
        out.objects = out.objects.slice(0, limit);
      }
      if (where.includes('cutscenes')) {
        out.cutscenes = [];
        for (const p of [...rom.assets.keys()].filter((k) => /^data\/cuts\/id/.test(k)).sort()) {
          parseCutscene(await rom.read(p)).actors.forEach((a, i) => has(a.name) && out.cutscenes.push({ path: p, actor: i, name: a.name }));
          if (out.cutscenes.length >= limit) break;
        }
        out.cutscenes = out.cutscenes.slice(0, limit);
      }
      if (where.includes('menus')) {
        const doc = menuWithDefaults((await readMenu()).doc);
        out.menus = [...Object.entries(NATIVE_MENUS).flatMap(([m, n]) => n.entries.filter((e) => has(menuText(e.label)) || has(e.alias)).map((e) => ({ menu: m, game: true, alias: e.alias ?? null, label: menuText(e.label) }))),
          ...doc.buttons.filter((b) => has(b.id) || has(menuText(b.label ?? b.label_off ?? ''))).map((b) => ({ menu: b.menu ?? 'main_menu', game: false, id: b.id, label: menuText(b.label ?? b.label_off ?? '') }))].slice(0, limit);
      }
      if (where.includes('models')) out.models = [...rom.assets.values()].filter((a) => a.name.endsWith('_HOB') && a.size > 8 && has(a.path)).slice(0, limit).map((a) => a.path.replace(/_HOB$/, ''));
      return out;
    },
  },
  {
    name: 'rs64_view',
    edit: false,
    description: 'Gets or sets the explorer\'s view camera (then rs64_view_capture shows it). position / target are level coordinates whenever a level is shown, a cutscene over it included (as rs64_level_objects gives them; Y points down), else the scene\'s. frame: an object (type + index) to fit in view, or "all" for the whole level or model. cutsceneCamera: look through the cutscene\'s own camera (true) or fly freely (false). With no arguments it only reports the view.',
    inputSchema: { type: 'object', properties: { position: vec3, target: vec3, frame: { oneOf: [{ type: 'string', enum: ['all'] }, { type: 'object', properties: { type: { type: 'integer' }, index: { type: 'integer' } }, required: ['type', 'index'] }] },
      cutsceneCamera: { type: 'boolean' } } },
    run: ({ position, target, frame: fit, cutsceneCamera }) => {
      mcpNeedRom();
      const space = root?.getObjectByName('markers') ?? null;
      const toWorld = (v) => (space ? space.localToWorld(new THREE.Vector3(...v)) : new THREE.Vector3(...v));
      const toSpace = (v) => mcpRound((space ? space.worldToLocal(v.clone()) : v).toArray());
      if (cutsceneCamera != null && cut?.file && $('cutCam').checked !== cutsceneCamera) $('cutCam').click();
      if (fit === 'all') {
        if (space) frameLevel(root, space);
        else if (root) frame(root);
      } else if (fit) {
        mcpNeedLevel();
        frame(mcpObject(fit.type, fit.index)[0]);
      }
      if (position) camera.position.copy(toWorld(position));
      if (target) controls.target.copy(toWorld(target));
      controls.update();
      return { position: toSpace(camera.position), target: toSpace(controls.target), space: space ? 'level' : 'scene', cutsceneCamera: !!(cut?.file && $('cutCam').checked) };
    },
  },
  {
    name: 'rs64_cutscene_play',
    edit: false,
    description: 'Plays, pauses or seeks the open cutscene (seek: seconds; it stays paused or playing as it was). Returns where it is.',
    inputSchema: { type: 'object', properties: { action: { type: 'string', enum: ['play', 'pause', 'seek'] }, seconds: { type: 'number', minimum: 0 } }, required: ['action'] },
    run: ({ action, seconds = 0 }) => {
      mcpNeedRom();
      if (!cut) throw new Error('No cutscene open: rs64_open a cutscene first');
      if (action === 'seek') {
        cut.t = THREE.MathUtils.clamp(seconds / cut.data.length, 0, 1);
        $('cutTime').value = cut.t;
        if (cut.playing) startCutAudio(cut.t * cut.data.length);
        else stopCutAudio();
      } else if ((action === 'play') !== cut.playing) $('cutPlay').click();
      return { seconds: +(cut.t * cut.data.length).toFixed(2), length: cut.data.length, playing: cut.playing };
    },
  },
  {
    name: 'rs64_cutscene_edit',
    edit: true,
    description: 'Changes the open cutscene\'s structure. op: "deleteActor" (actor; its child actors and the events naming them go too), "setModel" (actor, model: a model id or actor name, keeping its _N suffix), "addCamera", "deleteCamera" (camera; not the last one), "setLength" (seconds; keys and snapshots keep their times), "blank" (removes every actor, camera move and audio event).',
    inputSchema: { type: 'object', properties: { op: { type: 'string', enum: ['deleteActor', 'setModel', 'addCamera', 'deleteCamera', 'setLength', 'blank'] }, actor: { type: 'integer' },
      camera: { type: 'integer' }, model: { type: 'string' }, seconds: { type: 'number', exclusiveMinimum: 0 } }, required: ['op'] },
    run: async ({ op, actor, camera, model, seconds }) => {
      mcpNeedCut();
      const f = cut.file, ceremony = lastCutPath?.endsWith('id3_special');
      const done = async (ok, what) => {
        if (ok === false) throw new Error($('cutStatus').textContent || `${what} failed`);
        const c = WEBMCP_TOOLS.find((x) => x.name === 'rs64_cutscene').run({});
        return { ...(await c), done: what };
      };
      if (op === 'deleteActor') {
        if (!f.actors[actor]) throw new Error(`No actor ${actor}`);
        if (ceremony && actor <= 0x1b) throw new Error('The medal ceremony finds the medal by its actor number (27): actors up to it cannot be removed');
        const gone = new Set([actor]);
        for (let grew = true; grew;) {
          grew = false;
          f.actors.forEach((a, k) => !gone.has(k) && gone.has(a.parent) && (grew = gone.add(k)));
        }
        setCutTrack(null);
        return done(await cutAuthEdit('Remove actor', (doc) => authoringDropActors(doc, gone), (g) => cutRemoveActor(g, actor)), `removed ${gone.size} actor(s)`);
      }
      if (op === 'setModel') {
        mcpTrack(actor);
        await cutScanPathModels();
        const name = cutModelNames().includes(model) ? model : model && (await cutActorNameFor(model));
        if (!name) throw new Error(`Cutscenes can't load ${model}`);
        const suffix = f.actors[actor].name.match(/_\d+$/)?.[0] ?? '';
        return done(await editCutFile('Change model', (g) => cutSetActorName(g, actor, name + suffix)), `actor ${actor} is ${name + suffix}`);
      }
      if (op === 'addCamera') {
        let k = -1;
        const ok = await editCutFile('Add camera', (g) => (k = cutAddCamera(g)));
        return done(ok, `camera ${k} added`);
      }
      if (op === 'deleteCamera') {
        mcpTrack(undefined, camera);
        if (f.cameras.filter(Boolean).length <= 1) throw new Error('The last camera cannot be removed');
        setCutTrack(null);
        return done(await cutAuthEdit('Remove camera', (doc) => delete doc.tracks['cam:' + camera], (g) => cutRemoveCamera(g, camera)), `camera ${camera} removed`);
      }
      if (op === 'setLength') {
        if (!(seconds > 0)) throw new Error('Give seconds');
        const old = f.length;
        return done(await cutAuthEdit('Length', (doc, g) => {
          for (const tr of Object.values(doc.tracks)) {
            if (tr.snaps) tr.snaps = cutRetimeSnaps(tr.snaps, old, seconds);
            if (tr.modes) tr.modes = tr.modes.map((m) => ({ ...m, t0: (m.t0 * old) / seconds, t1: Math.min(1, (m.t1 * old) / seconds) })).filter((m) => m.t0 < 1);
            if (tr.shots) tr.shots = cutRetimeSnaps(tr.shots, old, seconds);
          }
          cutSyncModeSpans(doc, g);
        }, (g) => cutSetLength(g, seconds)), `length ${seconds} s`);
      }
      if (ceremony) throw new Error('The medal ceremony needs its medal actor (27): it cannot start from blank');
      setCutTrack(null);
      return done(await editCutFile('Start from blank', (g) => cutBlank(g, g.length), (doc) => (doc.tracks = {})), 'blank');
    },
  },
  {
    name: 'rs64_level_rename_object',
    edit: true,
    description: 'Names an object in the open level (rules, camera shots and destroy lists name objects). Refused when the mission\'s script looks the old name up, or the new name is taken. The level\'s rules that named it follow the new name.',
    inputSchema: { type: 'object', properties: { type: { type: 'integer' }, index: { type: 'integer' }, name: { type: 'string', pattern: '^[!-~]{1,47}$' } }, required: ['type', 'index', 'name'] },
    run: async ({ type, index, name }) => {
      mcpNeedLevel();
      const it = mcpObject(type, index)[1], dir = selected, dv = await rom.read(dir + '/dat'), old = it.name;
      if (!/^[!-~]{1,47}$/.test(name)) throw new Error('A name is 1 to 47 printable characters with no spaces');
      if (old === name) return { object: mcpItem(it) };
      if (old && nameUses(romBytes.subarray(0, BLOB_START), old, 0, 0)) throw new Error(`The mission's script looks "${old}" up by name, so it can't be renamed`);
      if (parseDAT(dv).items.some((x) => x.name === name)) throw new Error(`"${name}" is already used in this level`);
      let moved = 0;
      await historyGroup(`Name ${name}`, async () => {
        await editAsset(dir + '/dat', (b) => datNameItem(b, it, name), `Name ${name}`);
        if (!old) return;
        const doc = await readRules(dir);
        moved = rulesRename(doc, old, name, ruleExt);
        if (moved) await writeRules(dir, doc, `Rules: ${old} -> ${name}`);
      });
      await reselect(type, index);
      const lv = dir.split('/').pop(), shots = old ? await virtualText(SHOT_LINKS) : '';
      return { object: mcpItem(mcpObject(type, index)[1]), rulesMoved: moved,
        note: old && (shotLinkGet(shots, lv, old) || shotDestroyGet(shots, lv, old).length) ? `camera_shots.txt still names ${old}: set its links again with rs64_level_set_link` : undefined };
    },
  },
  {
    name: 'rs64_level_links',
    edit: false,
    description: 'What an object in the open level is linked to, as its Events / Objects panel shows: for a trigger area (type 7 sub 0x18/0x19) its camera shot, destroy list and the wave it starts; for an enemy its wave and path; for a wingman start its path; for a music region its song; for a path whether NPCs attack the player at its end. Also lists the level\'s waves, cameras, paths and songs to choose from.',
    inputSchema: { type: 'object', properties: { type: { type: 'integer' }, index: { type: 'integer' } }, required: ['type', 'index'] },
    run: async ({ type, index }) => {
      mcpNeedLevel();
      const it = mcpObject(type, index)[1], dir = selected, lv = dir.split('/').pop(), dv = await rom.read(dir + '/dat'), items = parseDAT(dv).items;
      const waves = await readWaves(dir), shots = await virtualText(SHOT_LINKS), out = { object: mcpItem(it), links: {} }, L = out.links;
      const waveName = (flag) => (flag ? Object.entries(waves).find(([, f]) => f === flag)?.[0] ?? `stock flag ${flag}` : null);
      if (it.type === 7 && (it.sub === 0x18 || it.sub === 0x19) && it.name) {
        const s = shotLinkGet(shots, lv, it.name);
        Object.assign(L, { cameraShot: s ? { camera: s.camera, personal: !!s.personal } : null, destroy: shotDestroyGet(shots, lv, it.name), startWave: waveName(it.onEnter?.flag) });
      }
      if (it.type === 0 && WAVE_FIELDS[it.sub]) Object.assign(L, { waitWave: waveName(waveGet(dv, it).flag), path: npcPathGet(dv, it) || null });
      if (it.type === 0 && it.sub === 0x27) L.path = npcPathGet(dv, it) || null;
      if (it.type === 6 && it.sub === 0x10) L.song = parseMusicCues(romBytes).cues.find((c) => c.cue === it.songs?.[0])?.name ?? it.songs?.[0] ?? null;
      if (it.type === 3 && it.sub === 3 && it.points?.length >= 4) L.attackAtEnd = splineAttackEnd(u8(dv), it);
      out.choices = { waves: Object.keys(waves), cameras: items.filter((c) => c.type === 0 && c.sub === 0x26 && c.name).map((c) => c.name), paths: items.filter((x) => x.type === 3 && x.name).map((x) => x.name),
        songs: parseMusicCues(romBytes).cues.map((c) => ({ cue: c.cue, name: c.name })) };
      return out;
    },
  },
  {
    name: 'rs64_level_set_link',
    edit: true,
    description: 'Sets one link of an object in the open level (see rs64_level_links). link: "cameraShot" (value: a camera name or null; personal: co-op only the trigger\'s player sees it), "destroy" (value: object names), "startWave" / "waitWave" (value: a wave name, new names make a wave; null for none; force replaces a trigger\'s stock flag), "path" (value: a path name or null; copyFrom: { level, path } copies a stock flight path), "song" (value: a song name or cue number), "attackAtEnd" (value: boolean; false needs force, as a path leading nowhere crashes the game).',
    inputSchema: { type: 'object', properties: { type: { type: 'integer' }, index: { type: 'integer' }, link: { type: 'string', enum: ['cameraShot', 'destroy', 'startWave', 'waitWave', 'path', 'song', 'attackAtEnd'] },
      value: {}, personal: { type: 'boolean' }, force: { type: 'boolean' }, copyFrom: { type: 'object', properties: { level: { type: 'string' }, path: { type: 'string' } } } }, required: ['type', 'index', 'link'] },
    run: async ({ type, index, link, value = null, personal = false, force = false, copyFrom }) => {
      mcpNeedLevel();
      const it = mcpObject(type, index)[1], dir = selected, lv = dir.split('/').pop(), dv = await rom.read(dir + '/dat'), items = parseDAT(dv).items;
      const area = it.type === 7 && (it.sub === 0x18 || it.sub === 0x19) && it.name, enemy = it.type === 0 && WAVE_FIELDS[it.sub], wing = it.type === 0 && it.sub === 0x27;
      const need = (ok, what) => {
        if (!ok) throw new Error(`This object has no ${what} link: see rs64_level_links`);
      };
      const waveFlag = async (name) => {
        if (name == null) return 0;
        const waves = await readWaves(dir);
        if (waves[name]) return waves[name];
        const flag = waveFlagFor(waves, name, levelUsedFlags(dv));
        if (!flag) throw new Error('No free mission flag left in this level');
        await addWave(dir, waves, name, flag, `Wave ${name}`);
        return flag;
      };
      if (link === 'cameraShot') {
        need(area, 'camera shot');
        if (value != null && !items.some((c) => c.type === 0 && c.sub === 0x26 && c.name === value)) throw new Error(`No camera ${value} in this level`);
        await editText(SHOT_LINKS, shotLinkSet(await virtualText(SHOT_LINKS), lv, it.name, value, personal), value ? `Camera shot: ${it.name} -> ${value}` : `No camera shot: ${it.name}`);
      } else if (link === 'destroy') {
        need(area, 'destroy');
        const names = [value ?? []].flat();
        await editText(SHOT_LINKS, shotDestroySet(await virtualText(SHOT_LINKS), lv, it.name, names), `Destroy: ${it.name}`);
      } else if (link === 'startWave') {
        need(area, 'wave');
        if (triggerWave(it.onEnter, await readWaves(dir)) === 'stock' && !force) throw new Error(`${it.name} already sets mission flag ${it.onEnter.flag}, which the level's script may need: pass force to replace it`);
        await historyGroup(`Start wave: ${it.name}`, async () => {
          const flag = await waveFlag(value), base = it.sub === 0x18 ? 0x38 : 0x48, old = it.onEnter.flag;
          await editAsset(dir + '/dat', (bytes) => {
            const d = dvOf(bytes), o = it.offset + base;
            d.setUint32(o + 8, flag);
            d.setUint8(o + 0xF, flag ? 1 : 0);
            if (old && d.getUint32(o + 0x18) === old) {
              d.setUint32(o + 0x18, 0);
              d.setUint8(o + 0x1F, 0);
            }
          });
        });
        await reselect(it.type, it.index);
      } else if (link === 'waitWave') {
        need(enemy, 'wave');
        await historyGroup(`Wave: ${it.name}`, async () => {
          const flag = await waveFlag(value);
          await editItem(it, (bytes) => waveSet(bytes, it, flag ? { flag, counter: 0, atLeast: 0, gridSpawn: 0 } : { flag: 0 }), undefined, dir);
        });
      } else if (link === 'path') {
        need(enemy || wing, 'path');
        if (copyFrom) {
          const src = (await getPathCatalog()).find((c) => c.dir === copyFrom.level && c.name === copyFrom.path);
          if (!src) throw new Error(`No stock flight path ${copyFrom.path} in ${copyFrom.level}`);
          const dropped = await copyPathFor(it, src, true);
          return { ...(await WEBMCP_TOOLS.find((x) => x.name === 'rs64_level_links').run({ type, index })), droppedEvents: dropped };
        }
        if (value != null && !items.some((x) => x.type === 3 && x.name === value)) throw new Error(`No path ${value} in this level`);
        await editItem(it, (bytes) => npcSetPath(bytes, it, value ?? (enemy ? '-' : '')), `Path: ${it.name} -> ${value ?? 'none'}`, dir);
      } else if (link === 'song') {
        need(it.type === 6 && it.sub === 0x10, 'song');
        const cue = parseMusicCues(romBytes).cues.find((c) => c.cue === value || c.name === value);
        if (!cue) throw new Error(`No song ${value}: see rs64_level_links choices.songs`);
        await editItem(it, (b) => musicZoneSet(b, it, cue.cue), `Song: ${it.name || 'music region'} -> ${cue.name}`);
      } else {
        need(it.type === 3 && it.sub === 3 && it.points?.length >= 4, 'attack-at-end');
        if (!value && !force) throw new Error('A craft that reaches the end of a path leading nowhere crashes the game: pass force if this path continues into another one');
        const tpl = await getAttackTemplate();
        await editItem(it, (bytes) => splineSetAttackEnd(bytes, it, !!value, tpl), `${value ? 'Attack' : 'No attack'} at the end: ${it.name || 'path'}`);
      }
      return WEBMCP_TOOLS.find((x) => x.name === 'rs64_level_links').run({ type, index });
    },
  },
  {
    name: 'rs64_level_add_enemy',
    edit: true,
    description: 'Adds an enemy (or other NPC unit) of any kind to the open level at pos: sub is its unit subtype (kinds lists them; one this level has none of is copied from another level, its wave and path reset). Then give it a wave and path with rs64_level_set_link.',
    inputSchema: { type: 'object', properties: { sub: { type: 'integer' }, pos: vec3, kinds: { type: 'boolean', description: 'true: only list the kinds' } } },
    run: async ({ sub, pos, kinds }) => {
      mcpNeedLevel();
      const all = ENEMY_GROUPS.flatMap(([group, subs]) => subs.filter((s) => WAVE_FIELDS[s]).map((s) => ({ sub: s, name: SUBTYPES[s], group })));
      if (kinds || sub == null) return { kinds: all };
      if (!all.some((k) => k.sub === sub)) throw new Error(`Not an enemy kind: ${sub}; pass kinds: true for the list`);
      if (!pos) throw new Error('Give pos');
      await addEnemy(sub, pos);
      return { added: sel?.it ? mcpItem(sel.it) : null };
    },
  },
  {
    name: 'rs64_menu_title',
    edit: true,
    description: 'Overrides a game menu\'s title in the project\'s menu mod: label (text, or { en, de, fr, key } per language), x / y offsets; reset: true goes back to the game\'s title. A mod page\'s title is its opening button\'s "title" field (rs64_menu_set_button).',
    inputSchema: { type: 'object', properties: { menu: { type: 'string' }, label: {}, x: { type: 'number' }, y: { type: 'number' }, reset: { type: 'boolean' } }, required: ['menu'] },
    run: async ({ menu, label, x, y, reset }) => {
      mcpNeedRom();
      if (!NATIVE_MENUS[menu]) throw new Error(`${menu} is not one of the game's menus`);
      mcpEditMenu();
      await editMenuTitle(menu, (t) => {
        if (reset) return Object.keys(t).forEach((k) => delete t[k]);
        if (label !== undefined) label === null ? delete t.label : (t.label = label);
        if (x !== undefined) x === null ? delete t.x : (t.x = x);
        if (y !== undefined) y === null ? delete t.y : (t.y = y);
      }, `${menu} title`);
      return { menu, title: (await readMenu()).doc.titles?.[menu] ?? null };
    },
  },
  {
    name: 'rs64_menu_text',
    edit: true,
    description: 'Reads or sets the project\'s menu text keys (mod/menu_text.json: per language code, key -> text) that labels with a "key" use, so other languages can translate them. With code, key and text it sets one (text null removes it); with nothing it lists them all.',
    inputSchema: { type: 'object', properties: { code: { type: 'string', pattern: '^[a-z]{2,3}$' }, key: { type: 'string' }, text: { type: ['string', 'null'] } } },
    run: async ({ code, key, text }) => {
      mcpNeedRom();
      const cur = await readJson('mod/menu_text.json', () => ({}));
      if (!code || !key || text === undefined) return { text: cur };
      const next = { ...cur, [code]: { ...(cur[code] ?? {}) } };
      if (text === null) delete next[code][key];
      else next[code][key] = text;
      if (!Object.keys(next[code]).length) delete next[code];
      await editText('mod/menu_text.json', JSON.stringify(next, null, 2), `Menu text ${code}: ${key}`);
      if (menuSel) showMenu(menuSel);
      return { text: next };
    },
  },
  {
    name: 'rs64_crawl',
    edit: true,
    description: 'Reads or sets the opening crawl of a level (a stock chapter level 0, 5, 10, 15, or a mod level; defaults to the open level): chapter, title and body text. The first change makes the mod\'s own crawl (replacing that chapter\'s, or the one the mod level names). Characters the crawl font lacks are listed in fontGaps.',
    inputSchema: { type: 'object', properties: { level: { type: 'string' }, chapter: { type: 'string' }, title: { type: 'string' }, body: { type: 'string' } } },
    run: async ({ level, chapter, title, body }) => {
      mcpNeedRom();
      const dir = mcpRulesLevel(level), ctx = await levelCrawl(dir);
      if (!ctx) throw new Error(`${dir} has no opening crawl (only chapter levels 0, 5, 10, 15 and mod levels do)`);
      const change = Object.fromEntries(Object.entries({ chapter, title, body }).filter(([, v]) => v !== undefined));
      if (Object.keys(change).length) await saveCrawl(ctx, 'text', change);
      const now = Object.keys(change).length ? await levelCrawl(dir) : ctx;
      return { level: dir, name: now.name, ownFile: now.exists, label: now.label, chapter: now.c.chapter, title: now.c.title, body: now.c.body,
        fontGaps: crawlFontGaps(`${now.c.chapter}${now.c.title}${now.c.body}`) };
    },
  },
  {
    name: 'rs64_terrain_get',
    edit: false,
    description: 'The open level\'s terrain: a height grid with a vertex every 0.5 level units (x and z as rs64_level_objects gives them; heights are level Y, which points down, so smaller is higher). points: [[x, z], ...] returns the ground height and tile texture there; region { x, z, radius, step } returns a grid of heights around a point (step in level units, default 2).',
    inputSchema: { type: 'object', properties: { points: { type: 'array', items: { type: 'array', items: { type: 'number' }, minItems: 2, maxItems: 2 } },
      region: { type: 'object', properties: { x: { type: 'number' }, z: { type: 'number' }, radius: { type: 'number' }, step: { type: 'number' } }, required: ['x', 'z', 'radius'] } } },
    run: async ({ points = [], region }) => {
      const s = await mcpSculptLoad(true), g = s.grid, scale = mcpTerrainScale(s);
      const tex = (x, z) => g.tex[Math.min(g.h - 1, Math.max(0, Math.floor((z * 2) / 4))) * g.w + Math.min(g.w - 1, Math.max(0, Math.floor((x * 2) / 4)))];
      const out = { level: selected, size: [(g.W - 1) / 2, (g.H - 1) / 2], spacing: 0.5, heightStep: scale, heightRange: [-128 * scale, 127 * scale],
        points: points.map(([x, z]) => ({ x, z, y: +groundAt(g.hts, g, scale, x, z).toFixed(3), texture: tex(x, z) })) };
      if (region) {
        const step = Math.max(0.5, region.step ?? 2), rows = [];
        for (let z = region.z - region.radius; z <= region.z + region.radius + 1e-6; z += step) {
          const row = [];
          for (let x = region.x - region.radius; x <= region.x + region.radius + 1e-6; x += step) row.push(+groundAt(g.hts, g, scale, x, z).toFixed(2));
          rows.push(row);
        }
        out.region = { x0: region.x - region.radius, z0: region.z - region.radius, step, rows };
      }
      return out;
    },
  },
  {
    name: 'rs64_terrain_brush',
    edit: true,
    description: 'Shapes the open level\'s terrain with one brush stroke at (x, z), radius in level units, falling off smoothly to the edge: "raise" / "lower" by amount level units at the centre; "flatten" toward height (a level Y; default the height at the centre) by strength 0-1 (1 = all the way at the centre); "smooth" by strength 0-1, passes times; "paint" sets texture (a tile texture id from rs64_terrain_get) on the tiles in the radius. Placed objects keep their height above the ground unless followObjects is false. One undo step; refused if the reshaped terrain no longer fits the level\'s tile budget.',
    inputSchema: { type: 'object', properties: { tool: { type: 'string', enum: ['raise', 'lower', 'flatten', 'smooth', 'paint'] }, x: { type: 'number' }, z: { type: 'number' }, radius: { type: 'number', exclusiveMinimum: 0 },
      amount: { type: 'number' }, height: { type: 'number' }, strength: { type: 'number', minimum: 0, maximum: 1 }, passes: { type: 'integer', minimum: 1, maximum: 50 }, texture: { type: 'integer' }, followObjects: { type: 'boolean' } },
      required: ['tool', 'x', 'z', 'radius'] },
    run: async ({ tool, x, z, radius, amount, height, strength = 1, passes = 1, texture, followObjects = true }) => {
      const s = await mcpSculptLoad(), g = s.grid, scale = mcpTerrainScale(s), gx = x * 2, gy = z * 2, r = radius * 2;
      const hts = g.hts.slice(), texs = g.tex.slice();
      if (tool === 'paint') {
        if (texture == null) throw new Error('Give texture (a tile texture id, as rs64_terrain_get reports one)');
        for (let ty = 0; ty < g.h; ty++) for (let tx = 0; tx < g.w; tx++) if (Math.hypot(tx * 4 + 2 - gx, ty * 4 + 2 - gy) <= Math.max(r, 2)) g.tex[ty * g.w + tx] = texture;
      } else {
        if ((tool === 'raise' || tool === 'lower') && !(amount > 0)) throw new Error('Give amount (level units, > 0)');
        const flat = (height ?? groundAt(g.hts, g, scale, x, z)) / scale;
        for (let p = 0; p < (tool === 'smooth' ? passes : 1); p++) {
          const src = g.hts.slice(), at = (ix, iy) => src[Math.min(g.H - 1, Math.max(0, iy)) * g.W + Math.min(g.W - 1, Math.max(0, ix))];
          for (let iy = Math.max(0, Math.floor(gy - r)); iy <= Math.min(g.H - 1, Math.ceil(gy + r)); iy++) {
            for (let ix = Math.max(0, Math.floor(gx - r)); ix <= Math.min(g.W - 1, Math.ceil(gx + r)); ix++) {
              const d = Math.hypot(ix - gx, iy - gy);
              if (d > r) continue;
              let w = 1 - d / r;
              w = w * w * (3 - 2 * w);
              const i = iy * g.W + ix;
              // Heights are stored game Y-down, so raising subtracts.
              if (tool === 'raise') g.hts[i] -= (amount / scale) * w;
              else if (tool === 'lower') g.hts[i] += (amount / scale) * w;
              else if (tool === 'flatten') g.hts[i] += (flat - g.hts[i]) * strength * w;
              else g.hts[i] += ((at(ix - 1, iy) + at(ix + 1, iy) + at(ix, iy - 1) + at(ix, iy + 1)) / 4 - g.hts[i]) * strength * w;
              g.hts[i] = Math.max(-128, Math.min(127, g.hts[i]));
            }
          }
        }
      }
      const bytes = rebuildTerrain();
      if (!bytes) {
        g.hts.set(hts);
        g.tex.set(texs);
        rebuildTerrain();
        throw new Error(`The reshaped terrain does not fit the level's tile budget (${$('sculptInfo').textContent}); nothing changed`);
      }
      const follow = $('optFollow').checked;
      $('optFollow').checked = followObjects;
      try {
        await commitTerrain(bytes);
      } finally {
        $('optFollow').checked = follow;
      }
      return { tool, at: { x, z, y: +groundAt(g.hts, g, scale, x, z).toFixed(3) }, tiles: $('sculptInfo').textContent };
    },
  },
  {
    name: 'rs64_cutscene_adopt_object',
    edit: true,
    description: 'Makes a level object (type + index from rs64_level_objects, shown in the open cutscene\'s level) into a cutscene actor posed where it stands, so its keys can animate it (rs64_cutscene_set_key with levelPos / rotation). The level object itself stays in the level: the game draws a level\'s buildings and fixed objects during its cutscenes (not its moving units), so adopting a fixed one shows it twice in the game unless the original is moved away. Returns the new actor.',
    inputSchema: { type: 'object', properties: { type: { type: 'integer' }, index: { type: 'integer' } }, required: ['type', 'index'] },
    run: async ({ type, index }) => {
      mcpNeedCut();
      const h = cutLevelHolders().find((o) => o.userData.placedItem.type === type && o.userData.placedItem.index === index);
      if (!h) throw new Error(`No level object ${type}/${index} with a model in this cutscene's level (turn on Level units, or pick another): see rs64_level_objects`);
      const src = h.userData.source, tableAt = cutTableNameFor(src.id);
      const name = tableAt != null ? cutStr(tableAt + 0x20, 16) : await cutActorNameFor(src.id);
      if (!name) throw new Error(`Cutscenes can't load ${src.id}`);
      const tableScale = tableAt != null ? new DataView(romBytes.buffer, tableAt + 0x54, 4).getFloat32(0) || 1 : 1;
      const pos = new THREE.Vector3(), q = new THREE.Quaternion(), sc = new THREE.Vector3();
      h.matrix.decompose(pos, q, sc);
      const w = THREE.MathUtils.clamp(q.w, -1, 1), sin = Math.sqrt(1 - w * w), axis = sin < 1e-6 ? [0, 1, 0] : [q.x / sin, q.y / sin, q.z / sin];
      const vals = [pos.x * 4, pos.y * 4, pos.z * 4, sc.x / HOB_TO_WORLD / 0.2222 / tableScale, ...axis, THREE.MathUtils.radToDeg(2 * Math.acos(w)), 0, 0, 0];
      let i = -1;
      const ok = await editCutFile(`Add actor ${name} from the level`, (f) => {
        i = cutAddActor(f, name);
        vals.forEach((v, ch) => cutSetKey(f, { actor: i, ch }, 0, v));
      });
      if (!ok) throw new Error($('cutStatus').textContent || 'The actor was not added');
      setCutTrack({ actor: i });
      return { actor: { index: i, name: cut.file.actors[i].name }, levelPos: mcpRound(pos.toArray()) };
    },
  },
];

// The current API is document.modelContext; navigator.modelContext is the older surface. registerTool returns a Promise in newer builds.
const webmcpContext = document.modelContext ?? navigator.modelContext;
if (webmcpContext?.registerTool) {
  for (const t of WEBMCP_TOOLS) {
    const tool = { name: t.name, description: t.description, inputSchema: t.inputSchema, annotations: { readOnlyHint: !t.edit },
      // A thrown error reaches the agent only as "the invocation failed", so its message is returned instead.
      execute: async (input) => {
        try {
          const r = await t.run(input ?? {});
          // A tool answering with an image (rs64_view_capture) hands over its MCP content as is.
          return r?.mcpContent ? { content: r.mcpContent } : { content: [{ type: 'text', text: JSON.stringify(r) }] };
        } catch (e) {
          return { content: [{ type: 'text', text: `Error: ${e.message}` }], isError: true };
        }
      } };
    Promise.resolve().then(() => webmcpContext.registerTool(tool)).catch((e) => console.warn(`WebMCP: ${t.name} not registered:`, e));
  }
}
