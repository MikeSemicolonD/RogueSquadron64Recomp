'use strict';
// ---- Level editing ----
// The Edit button switches the level view from viewing to editing: Terrain (sculpt and paint), Objects (placed type-0 items) and Events
// (paths, zones, triggers and the rest). A left click selects; the gizmo moves a proxy in game space (the world group's frame) and each
// finished drag or field change rewrites the item's floats in place in the level DAT. Q/W/E/R pick the tool as in Unity; with the right
// mouse button held those keys fly the camera instead.
let rulesPanelDir = null, editMode = null, lastEditMode = 'terrain', editTool = 'translate', sel = null, placing = null, rmbHeld = false, addTemplates = new Map();
const gizmo = new TransformControls(camera, canvas), editProxy = new THREE.Object3D(), _eq = new THREE.Quaternion();
scene.add(gizmo.getHelper());
gizmo.addEventListener('mouseDown', onTrackGizmoDown);
gizmo.addEventListener('mouseUp', onTrackGizmoUp);
gizmo.addEventListener('mouseUp', onCrawlGizmoUp);
gizmo.addEventListener('objectChange', onCrawlGizmoMove);
// The gizmo also draws an unconnected arrowhead / box on each axis's negative side; keep only the ones on a line.
const gizmoParts = gizmo.getHelper().children.find((c) => c.gizmo)?.gizmo;
for (const g of [gizmoParts?.translate, gizmoParts?.scale]) for (const h of [...(g?.children ?? [])]) {
  const a = { X: 'x', Y: 'y', Z: 'z' }[h.name];
  if (!a) continue;
  // Handle offsets are baked into each geometry (setupGizmo), so test where the geometry sits.
  h.geometry.computeBoundingBox();
  if (h.geometry.boundingBox.getCenter(new THREE.Vector3())[a] < -0.05) h.removeFromParent();
}
// Edit panel buttons are icons; each keeps its label as the title (and accessible name).
const EDIT_ICONS = {
  edit: 'M11.3 1.3l3.4 3.4-9.2 9.2H2.1v-3.4zM10 4.4l-6.4 6.4v1.6h1.6L11.6 6z',
  terrain: 'M1 14l4.5-8 3 4.5 2-3L15 14z',
  objects: 'M8 1l6 3.5v7L8 15l-6-3.5v-7zm0 1.7L3.6 5.2 8 7.7l4.4-2.5zM3 6.1v4.8l4.5 2.6V8.6zm10 0L8.5 8.6v4.9l4.5-2.6z',
  events: 'M3 1h1.5v14H3zM5.5 2h8l-2 3 2 3h-8z',
  view: 'M3 1l10 6.2-4.4 1.1 2.6 4.6-1.8 1-2.6-4.7L3 12.5z',
  move: 'M8 0l3 3H9v4h4V5l3 3-3 3V9H9v4h2l-3 3-3-3h2V9H3v2L0 8l3-3v2h4V3H5z',
  rotate: { stroke: 'M13.2 9.5A5.5 5.5 0 1 1 11.6 3.8M12.2 1v3.4H8.8' },
  scale: { stroke: 'M2 8.5h5.5V14H2zM7 9l6.5-6.5M9.5 2.5h4v4' },
  duplicate: 'M5 1h10v10H5zm2 2v6h6V3zM1 5h2v8h8v2H1z',
  delete: 'M5 1h6v2h4v2H1V3h4zM3 6h10l-1 9H4z',
  place: 'M8 1a5 5 0 0 1 5 5c0 3.5-5 9-5 9S3 9.5 3 6a5 5 0 0 1 5-5zm0 3a2 2 0 1 0 0 4 2 2 0 0 0 0-4z',
  download: 'M7 1h2v7.2l2.6-2.6 1.4 1.4L8 12 3 7l1.4-1.4L7 8.2zM2 13h12v2H2z',
  upload: 'M7 12h2V4.8l2.6 2.6L13 6 8 1 3 6l1.4 1.4L7 4.8zM2 13h12v2H2z',
  open: 'M1 3h5l1.5 1.5H15V6H3.2L1.6 13H1zm2.8 4H16l-2 7H2z',
  camAdd: 'M0.5 2.5h8v6h-8zm8 2L12 2.5v6L8.5 6.5zM11 9.5h1.6v2h2v1.6h-2v2H11v-2H9v-1.6h2z',
  save: 'M2 1h10l3 3v11H1V1zm2 1.5V6h7V2.5zM4 9v4.5h8V9z',
  undo: { stroke: 'M5.5 3.5L2.5 6.5l3 3M2.8 6.5h6.7a4 4 0 0 1 0 8H7' },
  redo: { stroke: 'M10.5 3.5l3 3-3 3M13.2 6.5H6.5a4 4 0 0 0 0 8H9' },
  play: 'M4 2l10 6-10 6z',
  // The mod package: a box with a play badge (play in game) or an arrow out of it (export).
  modPlay: { stroke: 'M6 1.5l4.5 2.5v5L6 11.5 1.5 9V4zM1.5 4L6 6.5 10.5 4M6 6.5v5', fill: 'M10 9l6 3.5-6 3.5z' },
  modExport: { stroke: 'M6 1.5l4.5 2.5v5L6 11.5 1.5 9V4zM1.5 4L6 6.5 10.5 4M6 6.5v5', fill: 'M12 8h1.6v3.6h1.9L12.8 15.5 10.1 11.6H12z' },
  blank: { stroke: 'M3 1.5h6.5l3.5 3.5v9.5H3zM9.5 1.5V5H13' },
  eye: { stroke: 'M1 8s2.5-4.5 7-4.5S15 8 15 8s-2.5 4.5-7 4.5S1 8 1 8zM8 6a2 2 0 1 0 0 4 2 2 0 0 0 0-4z' },
  close: { stroke: 'M3.5 3.5l9 9M12.5 3.5l-9 9' },
};
// An icon is a filled path, or { stroke } for line drawings (rotate, scale).
function setEditIcon(b, name) {
  const i = EDIT_ICONS[name];
  b.innerHTML = i.stroke ? `<svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true"><path fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" d="${i.stroke}"/>${i.fill ? `<path fill="currentColor" d="${i.fill}"/>` : ''}</svg>` : svgIcon(i);
  b.setAttribute('aria-label', b.title);
}
for (const b of $('editPanel').querySelectorAll('[data-icon]')) setEditIcon(b, b.dataset.icon);
for (const b of document.querySelectorAll('.histRow [data-icon], #cutEdit, #cutInspector [data-icon]')) setEditIcon(b, b.dataset.icon);
// The published site serves the save editor at save-editor/; in the repo it sits beside this folder.
if (/\/rs64-explorer\/(index\.html)?$/.test(location.pathname)) $('saveEditorLink').href = '../save-editor/rs64_save_editor.html';
$('btnMod').append(elem('span', { id: 'btnModLabel', textContent: 'Export mod' }));
$('btnMod').removeAttribute('aria-label');
$('btnOpen').onclick = openProject;
$('btnSave').onclick = saveProject;
$('btnUndo').onclick = undoEdit;
$('btnRedo').onclick = redoEdit;
// Global (world) axes by default: X red, Y green pointing up, Z blue. Local follows the object's own (game, Y-down) frame.
let editSpace = 'world';
function setEditSpace(space) {
  editSpace = space;
  gizmo.setSpace(space);
  const b = $('editSpace');
  b.title = space === 'world' ? 'Global axes: X red, Y green (up), Z blue. Click or press X for the object\'s local axes' : 'Local axes: the object\'s own frame. Click or press X for global axes';
  b.textContent = space === 'world' ? 'Global' : 'Local';
  b.setAttribute('aria-label', b.title);
  if (sel) attachGizmo();
}
setEditSpace('world');
$('editSpace').onclick = () => setEditSpace(editSpace === 'world' ? 'local' : 'world');
gizmo.addEventListener('dragging-changed', (e) => (controls.enabled = !e.value));
gizmo.addEventListener('objectChange', () => sel && proxyToItem());
// A press on the gizmo that moves nothing is not an edit.
const editState = () => sel && JSON.stringify([sel.it.pos, sel.it.rot, sel.it.scale, sel.it.size, sel.pathAt]);
let dragStart = null;
gizmo.addEventListener('mouseDown', () => (dragStart = editState()));
gizmo.addEventListener('mouseUp', () => sel && editState() !== dragStart && commitSel());
canvas.addEventListener('pointerdown', (e) => e.button === 2 && (rmbHeld = true));
addEventListener('pointerup', (e) => e.button === 2 && (rmbHeld = false));
// Which transforms mean something in the DAT: placed models and zones have a rotation and a scale/size; the other type-0 markers (starts,
// restarts, cameras, spawns) a facing but no scale that matters; splines, triggers and path events a position only (spline points are
// relative to it, so moving a spline moves the whole path; a path event slides along its spline).
const editTools = (s) => (s.o.userData.placedItem || s.it.type === 6 ? ['translate', 'rotate', 'scale'] : s.it.type === 0 ? ['translate', 'rotate'] : ['translate']);
const TOOL_KEYS = { KeyQ: 'view', KeyW: 'translate', KeyE: 'rotate', KeyR: 'scale' };
const editItemOf = (o) => o.userData.placedItem ?? (o.parent?.name === 'markers' ? o.userData.item : null);
// Each mode clicks and hovers only what it edits: Objects = placed objects plus object markers (starts, cameras), Events = the other
// markers. Terrain and other kinds never get in the way. A box marks the hovered item and the selection.
// Objects mode = placed models; Events mode = every legend marker (spawns, restarts and cameras included).
const editTargets = () => editMode === 'objects'
  ? root.children.filter((o) => o.userData.placedItem && o.visible)
  : visibleMarkers();
function editHit(e) {
  let o = root && pick(e, editTargets())?.object;
  while (o && !editItemOf(o)) o = o.parent;
  return o ?? null;
}
// Right-click in Objects / Events mode lists every editable item under the cursor (nearest first) instead of the inspect card, so nested or
// overlapping ones can be picked; hovering a row boxes it in the view.
function editPickList(e) {
  if (editMode !== 'objects' && editMode !== 'events') return false;
  pick(e, []);
  const found = [];
  for (const h of raycaster.intersectObjects(editTargets(), true)) {
    let o = h.object, shown = true;
    for (let p = o; p; p = p.parent) if (!p.visible) shown = false;
    while (o && !editItemOf(o)) o = o.parent;
    if (shown && o && !found.includes(o)) found.push(o);
  }
  $('tip').hidden = true;
  // One item needs no list: select it as a left click would. None just closes any open card.
  if (found.length < 2) {
    select(found[0] ?? null);
    return true;
  }
  const el = $('info'), list = elem('div');
  el.textContent = '';
  list.className = 'pickList';
  el.append(elem('b', { className: 'pickHead', textContent: `${found.length} ${editMode === 'objects' ? 'objects' : 'events'} under the cursor` }));
  for (const o of found) {
    const it = editItemOf(o), placed = !!o.userData.placedItem, row = elem('div');
    const color = placed ? TYPES[0].color : TYPES[markerKind(it)]?.color ?? 0xffffff;
    row.className = 'pickRow';
    row.tabIndex = 0;
    row.setAttribute('role', 'button');
    row.innerHTML = `<i style="background:#${color.toString(16).padStart(6, '0')}"></i><div>${esc(it.name || it.label || '(unnamed)')}</div><div class="kd">${esc(placed ? o.userData.source?.name ?? 'Object' : TYPES[markerKind(it)]?.label ?? 'Type ' + it.type)}</div>`;
    row.onmouseenter = row.onfocus = () => boxAround(hoverBox, o !== sel?.o && o);
    row.onmouseleave = row.onblur = () => (hoverBox.visible = false);
    row.onclick = () => select(o);
    row.onkeydown = (k) => k.key === 'Enter' && select(o);
    list.append(row);
  }
  el.append(list);
  const r = canvas.getBoundingClientRect();
  el.style.left = Math.min(e.clientX - r.left + 12, r.width - 390) + 'px';
  el.style.top = Math.min(e.clientY - r.top + 12, r.height - 220) + 'px';
  el.hidden = false;
  return true;
}
const hoverBox = new THREE.BoxHelper(new THREE.Object3D(), 0x8ab4ff), selBox = new THREE.BoxHelper(new THREE.Object3D(), 0xe8b84a);
hoverBox.visible = selBox.visible = false;
scene.add(hoverBox, selBox);
function boxAround(box, o) {
  box.visible = !!o;
  if (o) {
    o.updateMatrixWorld(true);
    box.setFromObject(o);
  }
}
// Hover in Objects / Events mode; returns false when the normal marker tooltip should run.
let editHovered = null;
function editHudRows(o) {
  const it = editItemOf(o), f = (v) => v.map((n) => +n.toFixed(2)).join(', '), placed = !!o.userData.placedItem;
  const rows = [[o === editHovered ? 'hover' : 'selected', it.name || it.label || '(unnamed)'], ['kind', placed ? 'Object' : TYPES[markerKind(it)]?.label ?? 'Type ' + it.type]];
  if (placed && o.userData.source) rows.push(['model', o.userData.source.name]);
  rows.push(['subtype', SUBTYPES[it.sub] ?? hex(it.sub)]);
  const pn = it.pathNodes?.[0];
  if (pn) rows.push(['along', `${pn.s.name || 'spline'} ${(((o === sel?.o && sel.pathAt) ?? pn.s.events[pn.k].at) * 100).toFixed(1)}%`]);
  else rows.push(['position', f(it.pos)]);
  if (it.rot && !pn) rows.push(['rotation', f(it.rot)]);
  if (it.scale && placed) rows.push(['scale', f(it.scale)]);
  if (it.size) rows.push(['size', f(it.size)]);
  if (it.radius > 0) rows.push(['radius', +it.radius.toFixed(2)]);
  return rows;
}
canvas.addEventListener('pointerleave', () => {
  if (!editHovered) return;
  editHovered = null;
  hud();
});
function editHover(e) {
  if (!editMode) return false;
  const o = (editMode === 'objects' || editMode === 'events') && !e.buttons && !gizmo.axis && !placing ? editHit(e) : null;
  if (o !== editHovered) {
    editHovered = o;
    hud();
  }
  boxAround(hoverBox, o !== sel?.o && o);
  canvas.style.cursor = o || placing ? 'pointer' : '';
  if (editMode !== 'events' || !o) $('tip').hidden = true;
  return editMode !== 'events' || !o;
}

function setEditMode(m) {
  if (m) lastEditMode = m;
  editMode = m;
  placing = null;
  $('editPanel').classList.toggle('open', !!m);
  $('editTab').classList.toggle('on', !!m);
  $('editTab').setAttribute('aria-pressed', !!m);
  for (const b of $('editBody').querySelectorAll('[data-mode]')) b.classList.toggle('on', b.dataset.mode === m);
  // Edit-panel controls and help rows name the modes they show in; "none" is viewing.
  for (const el of document.querySelectorAll('[data-modes]')) el.hidden = !el.dataset.modes.split(' ').includes(m ?? 'none');
  $('optSculpt').checked = m === 'terrain';
  loadSculpt();
  $('info').hidden = true;
  if (m !== 'events') $('rulesPanel').hidden = true;
  if (m === 'events') fillAddKinds();
  if (m === 'objects') fillAddEnemies();
  select(null);
}
$('editTab').onclick = () => setEditMode(editMode ? null : lastEditMode);
for (const b of $('editBody').querySelectorAll('[data-mode]')) b.onclick = () => setEditMode(b.dataset.mode);
for (const b of $('editBody').querySelectorAll('[data-tool]')) b.onclick = () => setTool(b.dataset.tool);
function setTool(t) {
  editTool = t;
  for (const b of $('editBody').querySelectorAll('[data-tool]')) b.classList.toggle('on', b.dataset.tool === t);
  attachGizmo();
}
function attachGizmo() {
  const tools = sel && !sel.fixed ? editTools(sel) : [];
  for (const b of $('editBody').querySelectorAll('[data-tool]')) b.disabled = !!sel && b.dataset.tool !== 'view' && !tools.includes(b.dataset.tool);
  if (tools.includes(editTool)) {
    gizmo.setMode(editTool);
    // A path event gets one arrow along its spline (the proxy's X is the tangent there); zones are footprints (height unused), so no vertical move.
    gizmo.setSpace(sel.pathNode ? 'local' : editSpace);
    gizmo.showX = true;
    gizmo.showY = !sel.pathNode && (sel.it.type !== 6 || editTool !== 'translate');
    gizmo.showZ = !sel.pathNode;
    gizmo.attach(editProxy);
  } else gizmo.detach();
  showCorners();
}
function select(o) {
  // A selection (or clicking empty space) closes the right-click card.
  $('info').hidden = true;
  editHovered = null;
  sel = null;
  gizmo.detach();
  editProxy.removeFromParent();
  if (o) {
    const it = editItemOf(o);
    // NPCs on a spline start at its first point whatever their own position says, and path events fire at a fraction along their spline,
    // so both move with the spline instead.
    sel = { o, it, fixed: o.userData.path ? `. It starts on the spline ${it.model}; move that in Events mode` : null, pathNode: it.pathNodes?.[0] };
    if (sel.pathNode) sel.pathAt = sel.pathNode.s.events[sel.pathNode.k].at;
    o.parent.add(editProxy);
    sel.base = { scale: it.scale?.slice(), size: it.size?.slice(), mq: o.quaternion.clone(), ms: o.scale.clone(), pos0: it.pos.slice() };
    if (it.type === 6) Object.assign(sel.base, { c0: new THREE.Vector3(...it.pos).add(zoneHalf(it)), rInv: placementRotation(it.rot).invert() });
    itemToProxy();
    sel.base.q = editProxy.quaternion.clone();
  }
  syncSelPanel();
  attachGizmo();
  boxAround(selBox, sel?.o);
  hoverBox.visible = false;
}
// A zone's gizmo sits at its footprint centre (pos + R * size / 2 over X/Z), so moving, rotating and scaling all pivot there.
const _m4b = new THREE.Matrix4(), _m4c = new THREE.Matrix4(), _xAxis = new THREE.Vector3(1, 0, 0);
const zoneHalf = (it) => new THREE.Vector3(it.size[0] / 2, 0, it.size[2] / 2).applyMatrix4(placementRotation(it.rot));
function itemToProxy() {
  const { o, it, base } = sel;
  editProxy.position.set(...(o.userData.path ? o.userData.path.points[0] : it.pos));
  if (it.type === 6) {
    editProxy.position.add(zoneHalf(it));
    editProxy.position.y = levelHeight(editProxy.position.x, editProxy.position.z);
  }
  editProxy.quaternion.setFromRotationMatrix(it.rot ? placementRotation(it.rot) : new THREE.Matrix4());
  if (sel.pathNode) editProxy.quaternion.setFromUnitVectors(_xAxis, sel.pathNode.curve.getTangentAt(sel.pathAt));
  const ratio = (now, was) => (now && was?.[0] ? now.map((v, a) => (was[a] ? v / was[a] : 1)) : [1, 1, 1]);
  editProxy.scale.set(...(it.size ? ratio(it.size, base.size) : ratio(it.scale, base.scale)));
}
// Path events fire at a fraction along their spline (arc length, as drawn): a drag slides the (first) node to the nearest point on it.
function slidePathEvent(at) {
  const { o, pathNode: { curve } } = sel;
  if (at == null) {
    let bd = Infinity;
    for (let i = 0; i <= 500; i++) {
      const d = curve.getPointAt(i / 500).distanceToSquared(editProxy.position);
      if (d < bd) [bd, at] = [d, i / 500];
    }
  }
  sel.pathAt = at;
  o.position.copy(curve.getPointAt(at));
  boxAround(selBox, o);
  syncSelPanel();
}
function proxyToItem() {
  if (sel.pathNode) return slidePathEvent();
  const { it, base } = sel, p = editProxy, d = p.position.toArray().map((v, a) => v - it.pos[a]);
  it.pos = p.position.toArray();
  if (it.rot && gizmo.mode === 'rotate') {
    const e = new THREE.Euler().setFromQuaternion(p.quaternion, 'ZYX');
    it.rot = [e.x, e.y, e.z].map((r) => THREE.MathUtils.radToDeg(r));
  }
  const k = p.scale.toArray();
  // An unscaled object (scale 0 = the game default, drawn at 0.25) keeps its 0 until it is actually scaled.
  if (base.scale && k.some((v) => v !== 1)) it.scale = (base.scale[0] ? base.scale : [0.25, 0.25, 0.25]).map((v, a) => v * k[a]);
  if (base.size) it.size = base.size.map((v, a) => v * k[a]);
  if (it.type === 6) {
    const c = p.position.clone().sub(zoneHalf(it));
    it.pos = [c.x, base.pos0[1], c.z];
  }
  if (it.points) for (const pt of it.points) for (let a = 0; a < 3; a++) pt[a] += d[a];
  showItemChange();
  syncSelPanel();
}
function showItemChange() {
  const { o, it, base } = sel;
  if (o.userData.compose) o.userData.compose();
  else if (it.type === 6) {
    // The drawn footprint (offsets from its old position) turned and stretched about the old centre, then moved to the new one;
    // the commit rebuilds it draped on the ground.
    const k = it.size.map((v, a) => (base.size[a] ? v / base.size[a] : 1));
    o.matrixAutoUpdate = false;
    o.matrix.makeTranslation(editProxy.position.x, base.c0.y, editProxy.position.z).multiply(placementRotation(it.rot)).multiply(_m4b.makeScale(k[0], 1, k[2]))
      .multiply(base.rInv).multiply(_m4c.makeTranslation(...new THREE.Vector3(...base.pos0).sub(base.c0)));
    o.matrixWorldNeedsUpdate = true;
  } else {
    o.position.set(...it.pos);
    o.quaternion.copy(editProxy.quaternion).multiply(_eq.copy(base.q).invert()).multiply(base.mq);
    o.scale.copy(base.ms).multiply(editProxy.scale);
  }
  if (it.type === 3) root.traverse((h) => h.userData.path === it && h.userData.compose());
  boxAround(selBox, o);
}
async function commitSel() {
  const { o, it } = sel;
  if (sel.pathNode) {
    // The node is { f32 fraction, u16, u16 } at spline + [+0x2C] + k * 8; the trigger's own position is not used by the path code.
    const { s, k, curve } = sel.pathNode, at = sel.pathAt;
    await editAsset(selected + '/dat', (bytes) => {
      const dv = dvOf(bytes);
      dv.setFloat32(s.offset + dv.getUint32(s.offset + 0x2C) + k * 8, at);
    });
    s.events[k].at = at;
    it.pos = it.nodePoints[0] = curve.getPointAt(at).toArray();
    return select(o);
  }
  await editAsset(selected + '/dat', (bytes) => {
    const dv = dvOf(bytes);
    const put = (off, v) => v.forEach((x, a) => dv.setFloat32(it.offset + off + a * 4, x));
    put(0x10, it.pos);
    if (it.rot) put(0x1C, it.rot);
    if (it.type === 0 && it.scale[0]) put(0x28, it.scale);
    if (it.size) put(0x28, it.size);
  });
  // Zones are draped on the terrain when built, so a changed zone is rebuilt rather than left transformed.
  if (it.type === 6) {
    const nm = makeMarker(it);
    nm.visible = o.visible;
    o.parent.add(nm);
    o.removeFromParent();
    select(nm);
  } else select(o);
}
// Mid-mission camera shot: the trigger's first firing in a mission plays one of the level's camera items (DAT 0/0x26) and its next
// cameras. The links live in the mod's camera_shots.txt (virtual asset mod/camera_shots.txt, exported raw at the mod root).
const SHOT_LINKS = 'mod/camera_shots.txt';
function shotLinkRow(it) {
  const dir = selected, lv = dir.split('/').pop(), row = elem('div', { className: 'shotRow' });
  const cams = (root?.getObjectByName('markers')?.children ?? []).map((m) => m.userData.item).filter((c) => c?.type === 0 && c.sub === 0x26 && c.name);
  const pick = selectOf([['', 'None'], ...cams.map((c) => [c.name, c.name])], null, { title: 'When this trigger first fires in a mission, the game cuts to this camera (and its next cameras) while the fight keeps running, then hands control back' });
  const preview = elem('button', { title: 'Preview the camera shot (opens it in the Scene panel)' });
  setEditIcon(preview, 'eye');
  const personal = elem('input', { type: 'checkbox', title: 'Personal: in online co-op only the player who set the trigger off sees the shot (use it when the camera chain ends at a restart point that moves the player). Off: both players see it' });
  const personalLabel = elem('label', { className: 'shotPersonal' }, personal, ' Personal');
  const text = () => virtualText(SHOT_LINKS);
  // A target flying a path exists only once the level spawns it; before that the game holds the camera's own direction (verified lv_3).
  const note = elem('small', { className: 'shotNote' });
  const warn = async () => {
    note.textContent = '';
    const cam = cams.find((c) => c.name === pick.value);
    if (!cam) {
      if (pick.value) note.textContent = `${pick.value} is not a camera in this level (deleted, or another level's): the game drops this shot. Pick a camera or None.`;
      return;
    }
    const dv = await rom.read(dir + '/dat'), { items } = parseDAT(dv), tgt = camRecord(cam, dv).target, t = items.find((i) => i.type === 0 && i.name === tgt);
    if (t?.model && items.some((s) => s.type === 3 && s.name === t.model)) note.textContent = `Aims at ${tgt}, which flies a path: if the level has not spawned it when the shot plays, the camera keeps its own direction instead.`;
  };
  text().then((t) => {
    const link = shotLinkGet(t, lv, it.name);
    if (link && !cams.some((c) => c.name === link.camera)) {
      pick.append(new Option(`${link.camera} (missing)`, link.camera));
      pick.disabled = false;
    }
    pick.value = link?.camera ?? '';
    personal.checked = !!link?.personal;
    preview.disabled = personal.disabled = !pick.value;
    warn();
  });
  const save = async (label) => {
    const t = await text();
    await editText(SHOT_LINKS, shotLinkSet(t, lv, it.name, pick.value || null, personal.checked), label);
    preview.disabled = personal.disabled = !pick.value;
    warn();
  };
  pick.onchange = () => save(pick.value ? `Camera shot: ${it.name} -> ${pick.value}` : `No camera shot: ${it.name}`);
  personal.onchange = () => save(`Camera shot ${personal.checked ? 'personal' : 'shared'}: ${it.name}`);
  preview.onclick = (e) => {
    e.preventDefault();
    const cam = cams.find((c) => c.name === pick.value);
    if (cam) showCutscene(`seq:${dir}:${cam.index}`);
  };
  row.append('Play camera shot ', pick, preview, personalLabel, note);
  if (!cams.length) Object.assign(pick, { disabled: true, title: 'This level has no camera items; add one first' });
  return row;
}
// ---- Mod levels (mod/levels.json): new levels that play as a stock template level, with copies of its files as their own ----
const MOD_LEVELS = 'mod/levels.json';
let modLevels = [];
// Aliases every mod level's folder and intro/extro onto its template's files (and drops those of removed levels).
function applyModLevelsText(text) {
  modLevels = modLevelsParse(text);
  const want = new Map(modLevels.flatMap((l) => modLevelAliases(l, [...rom.assets.keys()].filter((k) => !rom.aliases.has(k)))));
  for (const p of [...rom.aliases.keys()]) if (want.get(p) !== rom.aliases.get(p)) rom.unalias(p);
  for (const [p, from] of want) rom.alias(p, from);
}
const applyModLevels = async () => applyModLevelsText(await virtualText(MOD_LEVELS));
const modLevelOf = (dir) => modLevels.find((l) => dir === 'data/level/' + l.folder);
// The stock level a level directory plays as (its own for a stock level): per-level game data (textures, sky, fog) comes from it.
const stockDir = (dir) => {
  const l = dir && modLevelOf(dir);
  return l ? 'data/level/lv_' + l.template.toString(36) : dir;
};
async function editModLevels(change, label) {
  const levels = modLevelsParse(await virtualText(MOD_LEVELS));
  change(levels);
  await editText(MOD_LEVELS, modLevelsWrite(levels), label);
  await applyModLevels();
}
const CRAFT = [['xwing', 'X-wing'], ['ywing', 'Y-wing'], ['awing', 'A-wing'], ['vwing', 'V-wing'], ['speeder', 'Snowspeeder'], ['falcon', 'Millennium Falcon'], ['tie', 'TIE interceptor'], ['t16', 'T-16 Skyhopper'], ['naboo', 'Naboo starfighter']];
// What a template brings that the new level cannot change (yet).
const TEMPLATE_NOTES = { 0: 'opening crawl (chapter 1)', 5: 'opening crawl (chapter 2)', 10: 'opening crawl (chapter 3); no Y-wing or speeder', 15: 'opening crawl (chapter 4)', 16: 'no craft select (T-16), friendlies-saved medal', 3: 'speeder only', 6: 'speeder only', 11: 'speeder only', 18: 'speeder only', 8: 'X-wing and Y-wing only', 17: 'X-wing only' };
// blank: only a player start on flat ground, with this level's look (sky, textures, music); give it objectives in Level settings.
async function newModLevel(fromDir, blank = false) {
  const tmpl = parseInt(fromDir.split('/lv_')[1], 36), from = lvName(fromDir.split('/').pop());
  const ask = blank
    ? `New blank level with the look of ${from}: only a player start on flat ground. Give it objectives in Level settings.\n\nTitle:`
    : `New level from ${from}${TEMPLATE_NOTES[tmpl] ? ` (it keeps: ${TEMPLATE_NOTES[tmpl]})` : ''}.\nIt plays this level's mission script, so keep the objects it uses.\n\nTitle:`;
  const title = prompt(ask, 'New level')?.trim();
  if (!title) return;
  let made;
  // A copy starts from the template's medal thresholds, a blank level from simple ones.
  const medals = blank ? undefined : medalInfo(await rom.read('data/medal_info'), tmpl);
  await historyGroup(`New level: ${title}`, async () => {
    await editModLevels((levels) => levels.push((made = modLevelNew(levels, title, tmpl, medals))), `New level: ${title}`);
    if (!blank) return;
    const dir = 'data/level/' + made.folder;
    await editAsset(dir + '/dat', (b) => datBlank(b), `Blank level: ${title}`);
    await editAsset(dir + '/hmp', (b) => hmpFlat(b), `Flat ground: ${title}`);
    // Its intro and outro start blank (3 s on the template's opening camera), not the template's scenes; templates past 15 have none.
    for (const kind of ['intro', 'extro']) {
      const p = `data/cuts/${made.folder}_${kind}`;
      if (!rom.aliases.has(p)) continue;
      await editAsset(p, (b) => {
        const f = parseCutscene(dvOf(b));
        cutBlank(f, 3);
        return writeCutscene(f);
      }, `Blank ${kind}: ${title}`);
    }
  });
  renderList();
  await showLevel('data/level/' + made.folder);
}
// A voice-line picker: None, the level briefings, then every other line with audio (lines without a subtitle get the Audio tab's
// description, by speech index).
async function fillVoiceSelect(sel) {
  sel.append(new Option('None', ''));
  const briefing = elem('optgroup');
  briefing.label = 'Level briefings';
  for (let v = 0x279; v <= 0x28B; v++) briefing.append(new Option(`${lvName('lv_' + (v - 0x279).toString(36))}: ${voiceWords(v) ?? ''}`.slice(0, 90), String(v)));
  const others = elem('optgroup');
  others.label = 'Other voice lines';
  const voiceMap = parseVoiceTable(romBytes, await rom.read('data/voice_TXT')).voiceMap, cats = globalThis.RS64_VOICE_CATS ?? {};
  const unsubtitled = (v) => {
    const [cat, levels] = cats[voiceMap[v]] ?? ['', []];
    return levels.length ? `(${cat.split(': ').pop()}, ${levels.map(lvName).join(', ')})` : cat ? `(${cat})` : '(no subtitle)';
  };
  // Ids mapped to speech 0 (a one-frame silence) have no audio.
  for (let v = 0; v < 0x2F4; v++) if ((v < 0x279 || v > 0x28B) && voiceMap[v] && voiceLine(v)) others.append(new Option(`${v} ${voiceLine(v).trim() || unsubtitled(v)}`.slice(0, 90), String(v)));
  sel.append(briefing, others);
}
// Import / Play / Remove for a recording the mod ships at path: any audio the browser decodes (.wav, .mp3, .ogg), stored as an 8 kHz
// MORT stream the game streams. onChange(has) runs after each change and once at the start.
function recordingControls(path, what, onChange) {
  const note = elem('small', { className: 'shotNote' });
  const imp = elem('button', { textContent: 'Import recording…', title: `A .wav or .mp3 played instead of the ${what}'s voice line; converted to the game's 8 kHz voice codec` });
  const play = elem('button', { textContent: 'Play', title: 'How it will sound in the game' });
  const remove = elem('button', { textContent: 'Remove' });
  const recording = async () => {
    const dv = await rom.read(path).catch(() => null);
    return dv?.byteLength ? u8(dv) : null;
  };
  const show = async () => {
    const m = await recording();
    play.hidden = remove.hidden = note.hidden = !m;
    note.textContent = m ? `Recording: ${((new DataView(m.buffer, m.byteOffset).getUint16(4) * 160) / 8000).toFixed(1)} s, played instead of the voice line.` : '';
    onChange(!!m);
  };
  imp.onclick = (e) => {
    e.preventDefault();
    const pick = elem('input', { type: 'file', accept: 'audio/*' });
    pick.onchange = async () => {
      const f = pick.files[0];
      if (!f) return;
      try {
        audioCtx ??= new AudioContext();
        const buf = await audioCtx.decodeAudioData(await f.arrayBuffer());
        const off = new OfflineAudioContext(1, Math.max(1, Math.ceil(buf.duration * 8000)), 8000);
        const src = off.createBufferSource();
        src.buffer = buf;
        src.connect(off.destination);
        src.start();
        const pcm = Int16Array.from((await off.startRendering()).getChannelData(0), (v) => Math.round(Math.max(-1, Math.min(1, v)) * 32767));
        await editAsset(path, () => encodeMort(pcm), `Recording: ${what}`);
      } catch (err) {
        note.hidden = false;
        note.textContent = `Could not read ${f.name}: ${err.message}`;
        return;
      }
      await show();
    };
    pick.click();
  };
  play.onclick = async (e) => {
    e.preventDefault();
    const m = await recording();
    if (!m) return;
    const pcm = decodeMort(m, 0, m.length);
    audioCtx ??= new AudioContext();
    await audioCtx.resume();
    const b = audioCtx.createBuffer(1, pcm.length, 8000);
    b.getChannelData(0).set(Float32Array.from(pcm, (v) => v / 32768));
    const s = audioCtx.createBufferSource();
    s.buffer = b;
    s.connect(audioCtx.destination);
    s.start();
  };
  remove.onclick = async (e) => {
    e.preventDefault();
    await editAsset(path, () => new Uint8Array(0), `Remove recording: ${what}`);
    await show();
  };
  show();
  const row = elem('div', { className: 'recRow' });
  row.append(imp, play, remove);
  return { row, note };
}
const fld = (label, el, tip) => {
  const row = elem('label', { title: tip ?? '' });
  row.append(label, el);
  return row;
};
// Label centred above its field.
const stacked = (row) => (row.classList.add('stacked'), row);
// Level settings: title, briefing (text and voice), approved craft and medal thresholds, shown in SELECT LEVEL and the debrief.
async function modLevelDialog(dir) {
  const l = modLevelOf(dir);
  if (!l) return;
  const d = elem('dialog');
  d.className = 'panel levelDialog';
  const title = elem('input', { value: l.title, maxLength: 40, autofocus: true });
  const desc = elem('textarea', { value: l.description ?? '', rows: 4 });
  const fontNote = elem('small', { className: 'shotNote' });
  const checkFont = () => {
    const bad = [...new Set([...(title.value + desc.value).toUpperCase()].filter((c) => !/[A-Z0-9 \n!()_\-/\\,.:']/.test(c)))];
    fontNote.textContent = bad.length ? `The game's menu font cannot draw ${bad.join(' ')}; they are left out. Letters show in capitals.` : 'Letters show in capitals.';
  };
  title.oninput = desc.oninput = checkFont;
  checkFont();
  const voice = elem('select');
  await fillVoiceSelect(voice);
  voice.value = l.voice != null ? String(l.voice) : '';
  const useSub = elem('button', { textContent: 'Copy voiceline subtitles to Description', title: 'Stock descriptions are the exact words of their briefing line' });
  // Only offered when the chosen line has words to copy.
  const syncUseSub = () => (useSub.hidden = voice.disabled || !voice.value || !(voiceWords(+voice.value) ?? '').trim());
  voice.onchange = syncUseSub;
  syncUseSub();
  useSub.onclick = (e) => {
    e.preventDefault();
    if (voice.value) desc.value = (voiceWords(+voice.value) ?? '').trim();
    checkFont();
  };
  // Its own briefing recording, played instead of the voice line.
  const { row: recRow, note: recNote } = recordingControls(`mod/assets/level/${l.folder}/brief`, 'briefing', (has) => {
    voice.disabled = has;
    syncUseSub();
  });
  const crafts = CRAFT.map(([k, name]) => {
    const c = elem('input', { type: 'checkbox', checked: (l.craft ?? []).includes(k) });
    return [k, c, elem('label', null, c, ' ' + name)];
  });
  const craftBox = elem('div', { className: 'craftGrid', title: 'Craft approved for this level; none ticked keeps the template\'s' }, ...crafts.map(([, , lab]) => lab));
  const tiers = ['bronze', 'silver', 'gold'], cols = [['time', 'Time (min)', 0.5], ['kills', 'Kills', 1], ['accuracy', 'Accuracy %', 1], ['friendlies', 'Friendlies saved', 1]];
  const medals = elem('table', { className: 'medalGrid', title: 'A medal needs all of: time at most, the rest at least. Empty keeps the template\'s thresholds' });
  medals.innerHTML = `<tr><th></th>${cols.map(([, h]) => `<th>${h}</th>`).join('')}</tr>`;
  const cells = tiers.map((t) => {
    const ins = cols.map(([k, , step]) => [k, elem('input', { type: 'number', min: 0, step, value: l.medals?.[t]?.[k] ?? '' })]);
    medals.append(elem('tr', null, elem('th', { textContent: t[0].toUpperCase() + t.slice(1) }), ...ins.map(([, i]) => elem('td', null, i))));
    return [t, ins];
  });
  // Opening crawl: the template's (0, 5, 10 and 15 have one), none, or its own, which the Cutscene list's Opening crawl edits.
  // Starting song: any song of the in-mission cue table (the template's by default).
  const cueTable = parseMusicCues(romBytes), tmplSong = cueTable.cues.find((c) => c.song === cueTable.levelSongs[l.template]);
  const music = selectOf([['', `The template's (${tmplSong?.name ?? '?'})`], ...songChoices()], l.music ?? '', { title: 'Plays from the start of the mission; music regions placed in the level change it as the player flies into them' });
  const tmplCrawl = [0, 5, 10, 15].includes(l.template), ownCrawl = l.crawl && l.crawl !== false;
  const crawlMode = selectOf([...(tmplCrawl ? [['tmpl', "The template's"]] : []), ['none', 'None'], ...(ownCrawl ? [['own', 'Its own']] : [])],
    ownCrawl ? 'own' : l.crawl === false || !tmplCrawl ? 'none' : 'tmpl', { title: 'Edit the crawl from the Cutscene list (Opening crawl): the first edit gives the level its own' });
  const crawlRow = fld(elem('b', { textContent: 'Opening crawl ' }), crawlMode);
  // Objectives: the level's own success and failure (the template's mission script then does not run). Targets are wired into the DAT.
  const datPath = dir + '/dat', datView = await rom.read(datPath), datItems = parseDAT(datView).items;
  const itemName = (it) => it.name || `${it.label ?? 'item'} ${it.index}`;
  const units = datItems.map((it, i) => [it, i]).filter(([it]) => isUnit(it));
  const zones = datItems.map((it, i) => [it, i]).filter(([it]) => it.type === 7 && it.onEnter);
  const cams = datItems.filter((it) => it.type === 0 && it.sub === 0x26 && it.name).map((it) => it.name);
  const GOALS = [['destroy', 'Destroy units'], ['reach', 'Reach an area'], ['protect', 'Protect a unit'], ['survive', 'Survive for'], ['timelimit', 'Finish within']];
  const objRows = [], objBox = elem('div', { className: 'objectives' });
  const objNote = elem('small', { className: 'shotNote' });
  const addObjective = (o = { goal: 'destroy', text: '' }) => {
    const row = elem('div', { className: 'objRow' });
    const text = elem('input', { value: o.text ?? '', placeholder: 'Shown in the pause menu', maxLength: 40 });
    const goal = selectOf(GOALS, o.goal);
    const unitOpts = units.map(([it, i]) => [i, itemName(it)]);
    const pickUnits = selectOf(unitOpts, null, { multiple: true, size: 5, title: 'Ctrl-click to pick several' });
    const pickUnit = selectOf(unitOpts), pickZone = selectOf(zones.map(([it, i]) => [i, itemName(it)]));
    const minutes = elem('input', { type: 'number', min: 0.25, step: 0.25, value: o.seconds ? o.seconds / 60 : 5, title: 'Minutes' });
    const targets = o.counter || o.flag ? objectiveTargets(datView, o).map((it) => String(datItems.findIndex((x) => x.offset === it.offset))) : [];
    for (const opt of pickUnits.options) opt.selected = targets.includes(opt.value);
    if (targets[0]) pickUnit.value = pickZone.value = targets[0];
    const minutesRow = fld('Minutes ', minutes);
    const remove = elem('button', { title: 'Remove objective' });
    setEditIcon(remove, 'delete');
    const sync = () => {
      pickUnits.hidden = goal.value !== 'destroy';
      pickUnit.hidden = goal.value !== 'protect';
      pickZone.hidden = goal.value !== 'reach';
      minutesRow.hidden = goal.value !== 'survive' && goal.value !== 'timelimit';
    };
    // Callout when it completes (protect / time limit: when it fails), as the stock scripts make one: a voice line (or the mod's own
    // recording, kept under a name of its own so removing an objective leaves the others' files alone) and a cut to a level camera.
    const voice = elem('select', { title: 'Played (with its subtitle) when the objective completes, or fails for protect and time limit' });
    fillVoiceSelect(voice).then(() => (voice.value = o.voice != null ? String(o.voice) : ''));
    const camera = selectOf([['', 'None'], ...cams.map((c) => [c, c])], cams.includes(o.camera) ? o.camera : '', { title: 'The game cuts to this camera position when the objective completes, or fails for protect and time limit' });
    const recName = o.recording ?? `obj_${Math.random().toString(36).slice(2, 8)}`;
    let hasRec = false;
    const rec = recordingControls(`mod/assets/level/${l.folder}/${recName}`, 'objective', (has) => {
      hasRec = has;
      voice.disabled = has;
    });
    // The pause screen reads each line aloud with its own voice (none: it stays silent rather than reading the template's line).
    const pauseVoice = elem('select', { title: 'Read aloud when the pause screen comes to this objective' });
    fillVoiceSelect(pauseVoice).then(() => (pauseVoice.value = o.pauseVoice != null ? String(o.pauseVoice) : ''));
    const pauseName = o.pauseRecording ?? `pause_${Math.random().toString(36).slice(2, 8)}`;
    let hasPauseRec = false;
    const pauseRec = recordingControls(`mod/assets/level/${l.folder}/${pauseName}`, 'pause screen line', (has) => {
      hasPauseRec = has;
      pauseVoice.disabled = has;
    });
    const callout = elem('div', { className: 'objCallout' });
    callout.append(fld('When done ', voice), rec.row, rec.note, fld('Camera ', camera), fld('Pause screen ', pauseVoice), pauseRec.row, pauseRec.note);
    goal.onchange = sync;
    sync();
    remove.onclick = (e) => {
      e.preventDefault();
      objRows.splice(objRows.indexOf(entry), 1);
      row.remove();
      addBtn.hidden = false;
    };
    row.append(text, goal, remove, pickUnits, pickUnit, pickZone, minutesRow, callout);
    const entry = {
      value: () => ({
        text: text.value.trim(), goal: goal.value, seconds: +minutes.value * 60,
        targets: goal.value === 'destroy' ? [...pickUnits.selectedOptions].map((x) => +x.value) : goal.value === 'protect' ? [+pickUnit.value] : goal.value === 'reach' ? [+pickZone.value] : [],
        voice: !hasRec && voice.value ? +voice.value : undefined, camera: camera.value || undefined, recording: hasRec ? recName : undefined,
        pauseVoice: !hasPauseRec && pauseVoice.value ? +pauseVoice.value : undefined, pauseRecording: hasPauseRec ? pauseName : undefined,
      }),
    };
    objRows.push(entry);
    objBox.append(row);
    addBtn.hidden = objRows.length >= 4;
  };
  const addBtn = elem('button', { className: 'addObjective', textContent: 'Add objective', title: 'Up to 4; the mission is won when every destroy, reach and survive objective is done' });
  addBtn.onclick = (e) => {
    e.preventDefault();
    addObjective();
  };
  for (const o of l.objectives ?? []) addObjective(o);
  objNote.textContent = l.objectives?.length ? '' : "Without objectives the level keeps its template's mission script.";
  const save = elem('button', { title: 'Save' });
  const del = elem('button', { title: 'Removes the level from the mod (its edited files stay in the project history)' });
  const shut = elem('button', { className: 'dlgClose', title: 'Close without saving' });
  setEditIcon(save, 'save');
  setEditIcon(del, 'delete');
  setEditIcon(shut, 'close');
  save.append('Save');
  del.append('Delete level');
  save.removeAttribute('aria-label');
  del.removeAttribute('aria-label');
  const close = () => d.remove();
  shut.onclick = close;
  d.addEventListener('close', close);
  del.onclick = async () => {
    if (!confirm(`Delete ${l.title}?`)) return;
    close();
    await editModLevels((levels) => levels.splice(levels.findIndex((x) => x.folder === l.folder), 1), `Delete level: ${l.title}`);
    renderList();
    await showLevel(stockDir('data/level/lv_' + l.template.toString(36)));
  };
  save.onclick = async () => {
    const m = Object.fromEntries(cells.map(([t, ins]) => [t, Object.fromEntries(ins.filter(([, i]) => i.value !== '').map(([k, i]) => [k, +i.value]))]));
    const hasMedals = tiers.every((t) => Object.keys(m[t]).length);
    const objs = objRows.map((r) => r.value());
    const bad = objs.find((o) => !o.text || (['destroy', 'protect', 'reach'].includes(o.goal) && !o.targets.length));
    if (objs.length && (bad || !objs.some((o) => ['destroy', 'reach', 'survive'].includes(o.goal)))) {
      objNote.textContent = bad ? `"${bad.text || 'An objective'}" needs text and a target.` : 'At least one objective must be destroy, reach or survive, or the mission cannot be won.';
      return;
    }
    close();
    let objectives = [];
    await historyGroup(`Level settings: ${title.value.trim()}`, async () => {
      if (objs.length || l.objectives?.length) {
        await editAsset(datPath, (b) => {
          const r = objectivesApply(b, l.objectives, objs);
          objectives = r.objectives;
          b.set(r.bytes);
        }, `Objectives: ${l.title}`);
      }
      await editModLevels((levels) => {
        const x = levels.find((y) => y.folder === l.folder);
        x.title = title.value.trim() || x.title;
        x.description = desc.value.trim();
        if (voice.value) x.voice = +voice.value;
        else delete x.voice;
        const craft = crafts.filter(([, c]) => c.checked).map(([k]) => k);
        if (craft.length) x.craft = craft;
        else delete x.craft;
        if (hasMedals) x.medals = m;
        else delete x.medals;
        if (crawlMode.value === 'none' && tmplCrawl) x.crawl = false;
        else if (crawlMode.value !== 'own') delete x.crawl;
        if (music.value) x.music = +music.value;
        else delete x.music;
        if (objectives.length) x.objectives = objectives;
        else delete x.objectives;
      }, `Level settings: ${title.value.trim()}`);
    });
    renderList();
  };
  const tmplNote = TEMPLATE_NOTES[l.template], hr = () => elem('hr');
  d.append(shut, elem('b', { textContent: l.title }),
    ...(tmplNote ? [elem('small', { className: 'shotNote', textContent: 'From its template it keeps: ' + tmplNote })] : []),
    stacked(fld('Title', title)), fontNote, stacked(fld('Description', desc, 'The SELECT LEVEL briefing text')), fld('Briefing voice ', voice, 'Played when the level is highlighted in SELECT LEVEL'), useSub, recRow, recNote, hr(),
    elem('b', { textContent: 'Approved craft' }), craftBox, hr(),
    elem('b', { textContent: 'Objectives' }), objBox, addBtn, objNote, hr(),
    elem('b', { textContent: 'Medals' }), medals, hr(),
    crawlRow,
    hr(), fld(elem('b', { textContent: 'Starting song ' }), music),
    hr(), elem('div', { className: 'actions' }));
  d.lastChild.append(save, del);
  document.body.append(d);
  d.showModal();
}
// The marker legend stops above the scene panel, whatever height its buttons give it.
new ResizeObserver(() => document.documentElement.style.setProperty('--scene-h', $('scene').offsetHeight + 'px')).observe($('scene'));
function fillLevelButton() {
  const b = $('btnLevelSettings'), dir = selectedLevel && !String(selected).startsWith('cut:') ? selected : null;
  $('btnBlankLevel').hidden = true;
  b.hidden = !dir || !(modLevelOf(dir) || /\/lv_[0-9a-i]$/.test(dir));
  if (b.hidden) return;
  if (modLevelOf(dir)) b.textContent = 'Level settings…';
  else {
    setEditIcon(b, 'duplicate');
    b.append('New level from this one…');
    b.removeAttribute('aria-label');
  }
  b.onclick = () => (modLevelOf(dir) ? modLevelDialog(dir) : newModLevel(dir));
  const blank = $('btnBlankLevel');
  blank.hidden = !!modLevelOf(dir);
  if (!blank.firstChild?.tagName) {
    setEditIcon(blank, 'blank');
    blank.append('New blank level…');
    blank.removeAttribute('aria-label');
  }
  blank.onclick = () => newModLevel(dir, true);
}
// The selected item again after its level reloads (an edit can move or grow its record).
async function reselect(type, index) {
  await reloadLevel(selected);
  select(editObjects().findLast(([, x]) => x.type === type && x.index === index && !x.pathEvent)?.[0] ?? null);
}
// Edits the level's DAT, then reloads and reselects the item (its record may move or grow).
async function editItem(it, change, label, dir = selected) {
  await editAsset(dir + '/dat', change, label);
  await reselect(it.type, it.index);
}
// Every editable object in the scene with its level item, in scene order.
function editObjects() {
  const out = [];
  root.traverse((o) => {
    const x = editItemOf(o);
    if (x) out.push([o, x]);
  });
  return out;
}
// Objects a trigger can destroy (camera_shots.txt destroy lines): named units and props; a far one may not be loaded when it fires.
const DESTROY_SKIP = new Set([0x01, 0x02, 0x26, 0x27]), DESTROY_FAR = 40;
function destroyRow(it) {
  const dir = selected, lv = dir.split('/').pop(), row = elem('div', { className: 'shotRow' });
  const dist = (x) => Math.hypot(x.pos[0] - it.pos[0], x.pos[2] - it.pos[2]);
  Promise.all([rom.read(dir + '/dat'), virtualText(SHOT_LINKS)]).then(([dv, text]) => {
    const objs = parseDAT(dv).items.filter((x) => x.type === 0 && x.name && !DESTROY_SKIP.has(x.sub)).sort((a, b) => dist(a) - dist(b));
    const names = shotDestroyGet(text, lv, it.name);
    const save = (next, label) => virtualText(SHOT_LINKS).then((t) => editText(SHOT_LINKS, shotDestroySet(t, lv, it.name, next), label)).then(syncSelPanel);
    row.append('Destroy ');
    for (const n of names) {
      const o = objs.find((x) => x.name === n), chip = elem('span', { className: 'destroyChip' });
      const del = elem('button', { textContent: '×', title: `Stop destroying ${n}` });
      del.onclick = () => save(names.filter((x) => x !== n), `Keep ${n}: ${it.name}`);
      chip.append(n, del);
      chip.title = !o ? `${n} is not in this level: the game skips it` : dist(o) > DESTROY_FAR ? `${n} is ${dist(o).toFixed(0)} units from the trigger: if it is not loaded when the trigger fires it only loses its health (rubble, no explosion)` : `Destroyed with its own explosion when the trigger fires; the camera shot starts half a second later`;
      if (!o || dist(o) > DESTROY_FAR) chip.classList.add('warn');
      row.append(chip);
    }
    const choices = objs.filter((x) => !names.includes(x.name)).map((x) => [x.name, `${x.name} (${SUBTYPES[x.sub] ?? 'object'}, ${dist(x).toFixed(0)} away)`]);
    const add = selectOf([['', names.length ? 'Add…' : 'Nothing'], ...choices], null, { title: 'When this trigger first fires, destroy an object: it dies with its own explosion and counts for objectives like any kill' });
    add.onchange = () => add.value && save([...names, add.value], `Destroy ${add.value}: ${it.name}`);
    row.append(add);
  });
  return row;
}
// Waves: enemies wait for a mission flag that a trigger raises. Modders see wave names; the flags live in the sidecar
// explorer/<level dir>/waves.json ({ name: flag }), chosen by waveFlagFor.
const wavesPath = (dir) => `explorer/${dir}/waves.json`;
const readWaves = (dir) => readJson(wavesPath(dir), () => ({}));
const addWave = (dir, waves, name, flag, label) => writeJson(wavesPath(dir), { ...waves, [name]: flag }, label, 1);
function wavePicker(waves, flag, title) {
  const stock = flag && !Object.values(waves).includes(flag) ? [[flag, `Stock flag ${flag}`]] : [];
  return selectOf([[0, 'None'], ...Object.entries(waves).map(([n, f]) => [f, n]), ...stock, ['new', 'New wave…']], flag, { title });
}
// The picked wave's flag (0 = none); a new wave asks for its name and is stored. null when cancelled.
async function chooseWave(pick, dir, waves) {
  if (pick.value !== 'new') return +pick.value;
  const name = prompt('Wave name', `Wave ${Object.keys(waves).length + 1}`)?.trim();
  if (!name) return null;
  const flag = waveFlagFor(waves, name, levelUsedFlags(await rom.read(dir + '/dat')));
  if (!flag) {
    alert('No free mission flag left in this level');
    return null;
  }
  if (!waves[name]) await addWave(dir, waves, name, flag);
  return flag;
}
// Trigger side: entering the area raises the wave's flag (its enter block, +0x38 for 0x18 / +0x48 for 0x19, sets flag = 1).
function waveTriggerRow(it) {
  const dir = selected, row = elem('div', { className: 'shotRow' }), base = it.sub === 0x18 ? 0x38 : 0x48;
  readWaves(dir).then((waves) => {
    const stock = triggerWave(it.onEnter, waves) === 'stock';
    const pick = wavePicker(waves, it.onEnter.flag, 'Start a wave: entering this area brings in every enemy waiting for it');
    pick.onchange = () => historyGroup(`Start wave: ${it.name}`, async () => {
      if (stock && !confirm(`${it.name} already ${it.onEnter.value ? 'sets' : 'clears'} mission flag ${it.onEnter.flag}, which the level's script may need. Replace that with the wave?`)) return syncSelPanel();
      const flag = await chooseWave(pick, dir, waves);
      if (flag === null) return syncSelPanel();
      const old = it.onEnter.flag;
      await editAsset(dir + '/dat', (bytes) => {
        const dv = dvOf(bytes), o = it.offset + base;
        dv.setUint32(o + 8, flag);
        dv.setUint8(o + 0xF, flag ? 1 : 0);
        // An exit effect clearing the old flag would clear the wave's.
        if (old && dv.getUint32(o + 0x18) === old) {
          dv.setUint32(o + 0x18, 0);
          dv.setUint8(o + 0x1F, 0);
        }
      });
      await reselect(it.type, it.index);
    });
    row.append('Start wave ', pick);
  });
  return row;
}
// A stock fly-to-the-player event record (path event 0x4E, target "+"), the template the attack switch copies.
let attackTemplate = null;
async function getAttackTemplate() {
  for (const p of [...rom.assets.keys()].filter((p) => /^data\/level\/lv_.\/dat$/.test(p)).sort()) {
    if (attackTemplate) break;
    const dv = await rom.read(p), t = parseDAT(dv).items.find((x) => x.type === 7 && x.sub === 0x4E && !x.pathEvent?.target);
    if (t) attackTemplate = new Uint8Array(dv.buffer, dv.byteOffset + t.offset, dv.getUint32(t.offset + 8)).slice();
  }
  return attackTemplate;
}
function pathAttackRow(it) {
  const label = elem('label', { className: 'shotPersonal', title: 'At the end of this path, NPCs flying it turn on the player (a fly-to event with no target at the very end)' });
  const box = elem('input', { type: 'checkbox', disabled: true });
  label.append(box, ' At the end: attack the player');
  rom.read(selected + '/dat').then((dv) => {
    box.checked = splineAttackEnd(u8(dv), it);
    box.disabled = false;
  });
  box.onchange = async () => {
    if (!box.checked && !confirm('A craft that reaches the end of a path that leads nowhere crashes the game. Untick only if this path continues into another one or the craft is sent off it before the end. Remove the attack?')) {
      box.checked = true;
      return;
    }
    const tpl = await getAttackTemplate();
    await editItem(it, (bytes) => splineSetAttackEnd(bytes, it, box.checked, tpl), `${box.checked ? 'Attack' : 'No attack'} at the end: ${it.name || 'path'}`);
  };
  return label;
}
// Stock paths that waves can fly (named by an enemy of a WAVE_FIELDS kind), from every level, for "Copy from another level".
let pathCatalog = null;
async function getPathCatalog() {
  if (pathCatalog) return pathCatalog;
  pathCatalog = [];
  for (const p of [...rom.assets.keys()].filter((p) => /^data\/level\/lv_.\/dat$/.test(p)).sort()) {
    const dv = await rom.read(p), items = parseDAT(dv).items, kinds = new Map();
    for (const e of items.filter((x) => x.type === 0 && WAVE_FIELDS[x.sub])) {
      const path = waveGet(dv, e).path;
      if (!kinds.has(path)) kinds.set(path, new Set());
      kinds.get(path).add(SUBTYPES[e.sub]);
    }
    for (const s of items.filter((x) => x.type === 3 && x.sub === 3 && x.name && kinds.has(x.name) && x.points?.length >= 4)) pathCatalog.push({ dir: p.slice(0, -4), name: s.name, index: s.index, kinds: [...kinds.get(s.name)].join(', ') });
  }
  return pathCatalog;
}
// Copies a stock path next to the enemy (its first interior point, where the NPC starts, at the enemy) and puts the enemy on it.
async function copyPathFor(it, src) {
  await historyGroup(`Copy path ${src.name}: ${it.name}`, async () => {
    const fdv = await rom.read(src.dir + '/dat'), from = u8(fdv).slice();
    const attack = await getAttackTemplate();
    const s = parseDAT(new DataView(from.buffer)).items.find((x) => x.type === 3 && x.index === src.index);
    let dropped = 0;
    await editAsset(selected + '/dat', (bytes) => {
      const r = splineCopy(from, s, bytes, { name: uniqueName(bytes, src.name), at: it.pos, attack });
      dropped = r.dropped;
      const copy = parseDAT(new DataView(r.bytes.buffer)).items.filter((x) => x.type === 3).at(-1);
      const enemy = parseDAT(new DataView(r.bytes.buffer)).items.find((x) => x.type === 0 && x.index === it.index);
      return npcSetPath(r.bytes, enemy, copy.name);
    });
    if (dropped) alert(`${dropped} of ${src.name}'s path events named things in its own level (attack targets, holds) and were left out.`);
  });
  await reselect(it.type, it.index);
}
// Enemy side: waits for a wave (WAVE_FIELDS gate, spawned when its flag is raised) and flies a path once it appears.
function waveEnemyRow(it) {
  const dir = selected, row = elem('div', { className: 'shotRow' });
  Promise.all([readWaves(dir), rom.read(dir + '/dat')]).then(([waves, dv]) => {
    const w = waveGet(dv, it);
    const pick = wavePicker(waves, w.flag, 'Wait for a wave: it stays out of the level until a trigger starts the wave, then appears and flies its path');
    pick.onchange = () => historyGroup(`Wave: ${it.name}`, async () => {
      const flag = await chooseWave(pick, dir, waves);
      if (flag === null) return syncSelPanel();
      await editItem(it, (bytes) => waveSet(bytes, it, flag ? { flag, counter: 0, atLeast: 0, gridSpawn: 0 } : { flag: 0 }), undefined, dir);
    });
    row.append('Waits for wave ', pick, ' Path ', npcPathSelect(it, dv, '-', 'None (goes for the player)'));
  });
  return row;
}
// The path an enemy or wingman flies: the level's paths, or one copied from another level. none = the "no path" value and its label.
function npcPathSelect(it, dv, none, noneLabel) {
  const dir = selected, items = parseDAT(dv).items, splines = items.filter((x) => x.type === 3 && x.name), cur = npcPathGet(dv, it) || none;
  // A path event 0x4E with no target sends the craft at the player.
  const triggers = items.filter((x) => x.type === 7);
  const attacks = (s) => (s.events ?? []).some((e) => triggers.find((t) => t.index === e.trigger && t.sub === 0x4E && !t.pathEvent?.target));
  const other = [none, ...splines.map((s) => s.name)].includes(cur) ? [] : [[cur, `${cur} (${cur.startsWith('+') ? 'roam area' : 'missing'})`]];
  const pairs = [[none, noneLabel], ...splines.map((s) => [s.name, s.name + (attacks(s) ? ' (then attacks the player)' : '')]), ...other, ['__copy', 'Copy from another level…']];
  const path = selectOf(pairs, cur, { title: 'The path it flies from where it appears (a path starts it at the path\'s first point)' });
  path.onchange = async () => {
    if (path.value !== '__copy') return editItem(it, (bytes) => npcSetPath(bytes, it, path.value), `Path: ${it.name} -> ${path.value}`, dir);
    const cat = await getPathCatalog();
    const from = selectOf([['', 'Pick a path to copy…'], ...cat.map((c, k) => [k, `${c.dir.split('/').pop()} ${c.name} (${c.kinds})`])], null, { title: 'A stock flight path to copy into this level (level, path, what flies it); drag its points in Events mode afterwards' });
    from.onchange = () => (from.value ? copyPathFor(it, cat[+from.value]) : syncSelPanel());
    path.replaceWith(from);
  };
  return path;
}
// ---- Level rules (layout A): "When…" boxes on the selected area, path point or unit, and level-wide ones in Level settings ----
const rulesFolderOf = (dir) => modLevelOf(dir)?.folder ?? dir.split('/').pop();
async function readRules(dir) {
  return rulesParse(await virtualText(RULES_PATH(rulesFolderOf(dir))));
}
// Every level object name the level's rules refer to, present or not.
const ruleNames = async (dir) => new Set((await readRules(dir)).rules.flatMap((r) => ruleRefs(r, ruleExt).map((x) => x.name)));
async function writeRules(dir, doc, label) {
  await editText(RULES_PATH(rulesFolderOf(dir)), rulesWrite(doc), label);
}
// The object a rule attaches to needs a unique name; unnamed or shared ones get one, unless the stock script finds it by name.
// Rules already on a shared name stay on it: in game they fire for every object of that name, not just this one.
async function ensureRuleName(dir, it) {
  const dv = await rom.read(dir + '/dat'), items = parseDAT(dv).items;
  if (it.name && items.filter((x) => x.name === it.name).length === 1) return it.name;
  if (it.name && nameUses(romBytes.subarray(0, BLOB_START), it.name, 0, 0)) {
    alert(`"${it.name}" is shared with another object and the mission's script looks it up by name, so it cannot be renamed for a rule.`);
    return null;
  }
  const bytes = u8(dv), ruled = await ruleNames(dir);
  const name = uniqueName(bytes, it.name || (it.pathEvent ? 'rule_point' : it.type === 7 ? 'rule_area' : 'rule_unit'), ruled);
  await editAsset(dir + '/dat', (b) => datNameItem(b, it, name), `Name ${name}`);
  return name;
}
// A picker for one rule parameter, filled from the level; returns the element. set(value) stores it.
async function rulePicker(p, value, set, ctx) {
  const sel = (opts, cls) => {
    const s = selectOf([['', 'Pick…'], ...opts], value ?? '', { className: cls ?? '' }, '(missing)');
    s.onchange = () => set(s.value === '' ? undefined : /^(voice|song|enemyKind|objective|number|flag)$/.test(p.picker) ? +s.value : s.value);
    return s;
  };
  const items = ctx.items, named = (f) => items.filter((it) => it.name && f(it)).map((it) => [it.name, it.name]);
  if (p.picker === 'area') return sel(named((it) => it.type === 7 && it.onEnter && !it.pathEvent), 'obj');
  if (p.picker === 'path') return sel(named((it) => it.type === 3), 'obj');
  if (p.picker === 'pathPoint') return sel(named((it) => it.pathEvent), 'obj');
  if (p.picker === 'unit') return sel(named((it) => isUnit(it)), 'obj');
  if (p.picker === 'camera') return sel(named((it) => it.type === 0 && it.sub === 0x26), 'obj');
  if (p.picker === 'song') return sel(songChoices());
  if (p.picker === 'enemyKind') return sel(ENEMY_GROUPS.flatMap(([, subs]) => subs.filter((s) => WAVE_FIELDS[s]).map((s) => [s, SUBTYPES[s]])));
  if (p.picker === 'objective') return sel((modLevelOf(ctx.dir)?.objectives ?? []).map((o, i) => [i + 1, `${i + 1}: ${o.text}`]));
  if (p.picker === 'voice') {
    const s = elem('select');
    await fillVoiceSelect(s);
    s.value = value != null ? String(value) : '';
    s.onchange = () => set(s.value === '' ? undefined : +s.value);
    return s;
  }
  if (p.picker === 'recording') {
    const name = value || `rule_${Math.random().toString(36).slice(2, 8)}`;
    const rec = recordingControls(`mod/assets/level/${rulesFolderOf(ctx.dir)}/${name}`, 'rule', (has) => has && value !== name && set(name));
    return elem('div', null, rec.row, rec.note);
  }
  const kind = p.picker === 'text' ? 'text' : 'number';
  const inp = elem('input', { type: kind, value: value ?? '', placeholder: p.label ?? p.key, min: 0, step: p.picker === 'seconds' ? 0.5 : 1 });
  inp.onchange = () => set(inp.value === '' ? undefined : kind === 'text' ? inp.value : +inp.value);
  return inp;
}
const optGroup = (label, opts) => elem('optgroup', { label }, ...opts.map(([v, t]) => new Option(t, v)));
// The in-mission cue table's songs as [song id, name], one each.
const songChoices = () => [...new Map(parseMusicCues(romBytes).cues.map((c) => [c.song, c.name])).entries()];
// Actions and custom whens native mods offer (mods/<id>/rules_ext.json in the game folder); ask = pick the folder if none is granted.
let ruleExt = [];
async function loadRuleExt(ask) {
  let dir = ask ? await gameFolder().catch(() => null) : await idbKv('readonly', (s) => s.get('gameDir')).catch(() => null);
  if (!ask && dir && (await dir.queryPermission({ mode: 'readwrite' })) !== 'granted') dir = null;
  const mods = dir && (await dir.getDirectoryHandle('mods').catch(() => null));
  if (!mods) return false;
  const out = [];
  for await (const [id, h] of mods.entries()) {
    const f = h.kind === 'directory' && (await h.getFileHandle('rules_ext.json').then((x) => x.getFile(), () => null));
    if (f) out.push(...rulesExtParse(await f.text(), id));
  }
  ruleExt = out;
  rulesChanged();
  return true;
}
// Every shown rules view re-renders after any rules.json write, so none edits a stale copy of the file.
const liveRuleViews = new Set();
function rulesChanged() {
  for (const v of liveRuleViews) (v.box.isConnected ? v.render() : liveRuleViews.delete(v));
}
// owner: { kind: 'area' | 'path' | 'unit', it } on a selected object, or { kind: 'level' } in the Level events window.
function rulesSection(owner) {
  const dir = selected, box = elem('div', { className: 'rulesBox' });
  const render = async () => {
    const [doc, dv] = await Promise.all([readRules(dir), rom.read(dir + '/dat')]), items = parseDAT(dv).items, ctx = { dir, items };
    const own = !!modLevelOf(dir)?.objectives?.length, whens = RULE_WHENS.filter((w) => w.on === owner.kind && (own || !w.objectives));
    const broken = new Set(rulesBroken(doc, items, ruleExt).map((b) => `${b.rule}:${b.field}`));
    // Level events also list rules whose object was deleted, so they can be pointed at another one or removed.
    const orphan = (i) => owner.kind === 'level' && broken.has(`${i}:object`);
    const custom = owner.kind === 'level' ? ruleExt.filter((e) => e.kind === 'event') : [];
    const mine = doc.rules.map((r, i) => [r, i]).filter(([r, i]) => orphan(i) || (owner.kind === 'level' && r.when === 'custom') || (whens.some((w) => w.when === r.when) && (owner.kind === 'level' || r.object === owner.it.name)));
    const save = (label) => writeRules(dir, doc, label).then(rulesChanged);
    box.replaceChildren(elem('b', { className: 'rulesHead', textContent: 'When…' }));
    for (const [r, i] of mine) {
      const rule = elem('div', { className: 'rule' });
      const head = elem('div', { className: 'when' });
      const on = RULE_WHENS.find((x) => x.when === r.when)?.on;
      const whenPairs = (orphan(i) ? RULE_WHENS.filter((x) => x.on === on) : whens).map((x) => [x.when, x.label]);
      const fromMods = custom.length ? [optGroup('From mods', custom.map((e) => [`custom:${e.name}`, `${e.label} (${e.mod})`]))] : [];
      const w = selectOf([...whenPairs, ...fromMods], r.when === 'custom' ? `custom:${r.custom}` : r.when, null, () => `${r.custom} (needs a mod)`);
      w.onchange = () => {
        const c = w.value.startsWith('custom:');
        r.when = c ? 'custom' : w.value;
        if (c) r.custom = w.value.slice(7);
        else delete r.custom;
        save('Rule: when');
      };
      const every = elem('input', { type: 'checkbox', checked: !!r.every, title: 'Every time (otherwise only the first time)' });
      every.onchange = () => ((r.every = every.checked), save('Rule: every time'));
      const del = elem('button', { title: 'Remove rule' });
      setEditIcon(del, 'delete');
      del.onclick = () => (doc.rules.splice(i, 1), save('Remove rule'));
      head.append(w);
      if (orphan(i)) head.append(await rulePicker({ picker: on === 'path' ? 'pathPoint' : on }, r.object, (v) => ((r.object = v), save('Rule: object')), ctx));
      for (const p of RULE_WHENS.find((x) => x.when === r.when)?.params ?? []) head.append(await rulePicker(p, r[p.key], (v) => ((r[p.key] = v), save('Rule: ' + p.key)), ctx));
      const ev = elem('label');
      ev.append(every, ' every time');
      head.append(ev, del);
      rule.append(head);
      if (broken.has(`${i}:object`)) rule.append(elem('small', { className: 'warn', textContent: `"${r.object}" is gone: pick another or remove the rule.` }));
      for (const [k, a] of (r.actions ?? []).entries()) {
        const row = elem('div', { className: 'act' });
        const extActs = ruleExt.filter((e) => e.kind === 'action');
        const actPairs = RULE_ACTIONS.filter((x) => own || !x.objectives).map((x) => [x.do, x.label]);
        const kind = selectOf([...actPairs, ...(extActs.length ? [optGroup('From mods', extActs.map((e) => [e.name, `${e.label} (${e.mod})`]))] : [])], a.do, null, '(needs a mod)');
        kind.onchange = () => ((r.actions[k] = { do: kind.value }), save('Rule: action'));
        const rm = elem('button', { title: 'Remove action' });
        setEditIcon(rm, 'delete');
        rm.onclick = () => (r.actions.splice(k, 1), save('Remove action'));
        row.append(kind);
        for (const p of RULE_ACTIONS.find((x) => x.do === a.do)?.params ?? extActs.find((e) => e.name === a.do)?.params ?? []) {
          const el = await rulePicker(p, a[p.key], (v) => ((a[p.key] = v), save('Rule: ' + p.key)), ctx);
          if (broken.has(`${i}:actions.${k}.${p.key}`)) el.classList.add('bad');
          row.append(el);
        }
        if (a.do === 'spawn') row.append(spawnWaveButton(a, dir, () => save('Spawn wave')));
        row.append(rm);
        rule.append(row);
      }
      const addAct = elem('button', { className: 'addAct', textContent: '+ Add action' });
      addAct.onclick = () => ((r.actions ??= []).push({ do: 'voice' }), save('Add action'));
      rule.append(addAct);
      box.append(rule);
    }
    const add = elem('button', { className: 'addRule', textContent: '+ Add rule' });
    add.onclick = async () => {
      const r = { when: whens[0].when, actions: [] };
      if (owner.kind !== 'level') {
        const name = await ensureRuleName(dir, owner.it);
        if (!name) return;
        r.object = name;
        const d = await readRules(dir);
        d.rules.push(r);
        await writeRules(dir, d, 'Add rule');
        await reselect(owner.it.type, owner.it.index);
        return rulesChanged();
      }
      doc.rules.push(r);
      save('Add rule');
    };
    box.append(add);
  };
  liveRuleViews.add({ box, render });
  render();
  return box;
}
// Events mode with nothing selected: the level-wide rules, then every object rule of the level, one row per object.
function levelRulesPanel() {
  const dir = selected, box = elem('div', { className: 'levelRules' });
  const filter = elem('input', { type: 'search', placeholder: 'Filter by name, when or action' });
  const badOnly = elem('input', { type: 'checkbox' }), badLabel = elem('label');
  const list = elem('div', { className: 'ruleList' }), tools = elem('div', { className: 'when' });
  const whenLabel = (r) => RULE_WHENS.find((w) => w.when === r.when)?.label ?? r.when;
  const render = async () => {
    const [doc, dv] = await Promise.all([readRules(dir), rom.read(dir + '/dat')]), items = parseDAT(dv).items;
    const broken = new Set(rulesBroken(doc, items, ruleExt).filter((b) => b.field === 'object').map((b) => b.rule)), groups = new Map();
    doc.rules.forEach((r, i) => r.when !== 'custom' && RULE_WHENS.find((w) => w.when === r.when)?.on !== 'level' && (groups.get(r.object) ?? groups.set(r.object, []).get(r.object)).push([r, i]));
    const q = filter.value.trim().toLowerCase();
    list.replaceChildren();
    for (const [name, rules] of [...groups].sort(([a], [b]) => String(a).localeCompare(String(b)))) {
      const bad = rules.some(([, i]) => broken.has(i)), acts = rules.reduce((n, [r]) => n + (r.actions?.length ?? 0), 0);
      const text = rules.map(([r]) => [whenLabel(r), ...(r.actions ?? []).map((a) => RULE_ACTIONS.find((x) => x.do === a.do)?.label ?? a.do)].join(' ')).join(' ');
      if ((badOnly.checked && !bad) || (q && !`${name} ${text}`.toLowerCase().includes(q))) continue;
      const row = elem('button', { className: 'ruleRow' + (bad ? ' bad' : ''), title: bad ? 'Its object is gone: point it at another or remove it in Level events above' : 'Select it' });
      row.append(elem('b', { textContent: name }), ` ${rules.map(([r]) => whenLabel(r)).join('; ')} (${acts} action${acts === 1 ? '' : 's'})`);
      row.onclick = () => bad || selectNamed(name);
      list.append(row);
    }
    if (!list.children.length) list.append(elem('small', { className: 'shotNote', textContent: groups.size ? 'No rules match.' : 'No rules on objects yet: select an area, path point or unit and add one.' }));
  };
  filter.oninput = render;
  badOnly.onchange = render;
  badLabel.append(badOnly, ' broken only');
  tools.append(filter, badLabel);
  const fromMods = elem('button', { title: 'Offer the actions and events of the native mods in the game folder (each mod\'s rules_ext.json)' });
  setEditIcon(fromMods, 'upload');
  fromMods.append('Mod actions…');
  fromMods.hidden = !window.showDirectoryPicker;
  fromMods.onclick = () => loadRuleExt(true).then((ok) => ok || alert('No mods folder found in the game folder'));
  box.append(elem('b', { textContent: 'Level-wide' }), rulesSection({ kind: 'level' }), fromMods, elem('b', { textContent: 'Rules on objects' }), tools, list);
  liveRuleViews.add({ box: list, render });
  render();
  return box;
}
function openRulesPanel() {
  rulesPanelDir = selected;
  $('rulesBody').replaceChildren(levelRulesPanel());
  $('rulesPanel').hidden = false;
  if (window.showDirectoryPicker) loadRuleExt(false);
}
setEditIcon($('btnRules'), 'events');
$('btnRules').append('Level events…');
$('btnRules').removeAttribute('aria-label');
$('btnRules').onclick = () => ($('rulesPanel').hidden || rulesPanelDir !== selected ? openRulesPanel() : ($('rulesPanel').hidden = true));
setEditIcon($('rulesClose'), 'close');
$('rulesClose').onclick = () => ($('rulesPanel').hidden = true);
// ---- Opening crawls (mod/assets/crawl/<name>/crawl.json): the crawl of the level being edited. A stock chapter level's (0, 5, 10,
// 15) is the mod's replacement for that chapter; a mod level's is the crawl its levels.json names. Until the first edit the game's (or
// the template's) crawl shows, and that edit makes the file. Its backdrop and fly-by craft are the chapter's unless it has its own image.
const CRAWL_FILE = /^mod\/assets\/crawl\/([^/]+)\/crawl\.json$/;
const crawlBackdrop = (name) => `mod/assets/crawl/${name}/backdrop`, crawlDat = (name) => `mod/assets/crawl/${name}/dat`;
const CHAPTER_OF = { 0: 0, 5: 1, 10: 2, 15: 3 }, ROMAN = ['I', 'II', 'III', 'IV'];
let crawlFontData = null, crawlKeySel = null;
async function modCrawls() {
  const out = [];
  for (const p of edits.keys()) {
    const m = CRAWL_FILE.exec(p), text = m && (await virtualText(p));
    if (text) out.push({ name: m[1], ...crawlParse(text) });
  }
  return out.sort((a, b) => a.name.localeCompare(b.name));
}
const hasBytes = (path) => rom.read(path).then((d) => d.byteLength > 0, () => false);
// The crawl a level opens, or null (a stock level without a chapter, a mod level set to none): { dir, name (its file, or the one the
// first edit makes; manifest names are 15 characters), exists, c, base (the stock planet: backdrop and craft), replaces, level, label }.
async function levelCrawl(dir) {
  const ml = modLevelOf(dir), n = ml ? ml.template : parseInt(dir.split('/lv_')[1], 36), chapter = CHAPTER_OF[n], crawls = await modCrawls();
  const stock = chapter != null ? stockCrawls(frontText)[chapter] : null, blank = { chapter: '', title: '', body: '', planet: chapter ?? 0, base: chapter ?? 0, replaces: -1, camera: [] };
  if (!ml) {
    if (chapter == null) return null;
    const mine = crawls.filter((c) => c.replaces === chapter).at(-1);
    return { dir, name: mine?.name ?? `chapter_${chapter + 1}`, exists: !!mine, c: mine ?? { ...blank, ...stock }, base: chapter, replaces: chapter, level: null, label: `Chapter ${ROMAN[chapter]}` };
  }
  if (ml.crawl === false) return null;
  const named = typeof ml.crawl === 'string' ? crawls.find((c) => c.name === ml.crawl) : null, inline = ml.crawl && typeof ml.crawl === 'object' ? ml.crawl : null;
  const c = named ?? (inline ? { ...blank, ...inline, planet: inline.planet ?? 0 } : stock ? { ...blank, ...stock } : blank);
  const taken = new Set(crawls.map((x) => x.name)), stem = ml.folder.toLowerCase().replace(/[^a-z0-9_-]+/g, '_').slice(0, 15);
  let name = named?.name ?? stem;
  for (let i = 2; !named && taken.has(name); i++) name = stem.slice(0, 15 - String(i).length - 1) + '_' + i;
  return { dir, name, exists: !!named, c, base: c.planet === 'own' ? c.base : c.planet, replaces: -1, level: ml, label: ml.title };
}
// One undo step: more() writes the crawl's own files first, then crawl.json (made on the first edit; planet "own" while it has its own
// backdrop) and, for a mod level, its levels.json entry naming it.
async function saveCrawl(ctx, label, change = {}, more = null) {
  await historyGroup(`Crawl ${ctx.label}: ${label}`, async () => {
    if (more) await more();
    const own = await hasBytes(crawlBackdrop(ctx.name));
    await editText(CRAWL_PATH(ctx.name), crawlWrite({ ...ctx.c, ...change, planet: own ? 'own' : ctx.base, base: ctx.base, replaces: ctx.replaces }), `Crawl ${ctx.label}: ${label}`);
    if (ctx.level && ctx.level.crawl !== ctx.name) {
      const levels = modLevelsParse(await virtualText(MOD_LEVELS)), x = levels.find((l) => l.folder === ctx.level.folder);
      if (x) {
        x.crawl = ctx.name;
        await editText(MOD_LEVELS, modLevelsWrite(levels), `Crawl ${ctx.label}: ${label}`);
        await applyModLevels();
      }
    }
  });
  crawlsChanged();
}
// A backdrop from any image the browser opens, scaled to cover 640 x 128; encodeJpeg writes it the way the game's JFIF decoder reads.
async function importCrawlBackdrop(ctx) {
  const pick = elem('input', { type: 'file', accept: 'image/*' });
  const f = await new Promise((res) => {
    pick.onchange = () => res(pick.files[0]);
    pick.click();
  });
  if (!f) return;
  const img = await createImageBitmap(f), cv = new OffscreenCanvas(640, 128), cx = cv.getContext('2d'), k = Math.max(640 / img.width, 128 / img.height);
  cx.drawImage(img, (640 - img.width * k) / 2, (128 - img.height * k) / 2, img.width * k, img.height * k);
  const jpeg = encodeJpeg(cx.getImageData(0, 0, 640, 128).data, 640, 128);
  await saveCrawl(ctx, 'backdrop', {}, () => editAsset(crawlBackdrop(ctx.name), () => jpeg, `Crawl ${ctx.label}: backdrop`));
}
// The right-click card for the backdrop: it is a texture like any other.
function crawlBackdropCard() {
  const ctx = cut?.crawl?.ctx;
  if (!ctx) return null;
  const own = cut.crawl.ownBackdrop, path = own ? crawlBackdrop(ctx.name) : `data/backdrop/planet_${ctx.base}`;
  const download = async () => saveBlob(new Blob([u8(await rom.read(path))], { type: 'image/jpeg' }), `${ctx.name}_backdrop.jpg`);
  return { title: 'Backdrop', rows: [['Image', `640x128 JPEG, ${own ? 'this crawl\'s own' : `${CRAWL_PLANETS[ctx.base]} (backdrop/planet_${ctx.base})`}`], ['Shown', 'behind the 3D from the pan down, rising with the camera']],
    actions: [['Download image', 'The backdrop as a JPEG', download], ['Replace texture from image…', 'Scaled to cover 640 x 128; Export mod saves it', () => importCrawlBackdrop(ctx)],
      ...(own ? [['Use the game\'s backdrop', `Back to ${CRAWL_PLANETS[ctx.base]}`, () => saveCrawl(ctx, 'game backdrop', {}, () => editAsset(crawlBackdrop(ctx.name), () => new Uint8Array(0), `Crawl ${ctx.label}: game backdrop`))]] : [])] };
}
async function crawlFontOf() {
  if (!crawlFontData) {
    const f = crawlFont(romBytes, await rom.read('data/fonts/starwars50')), cv = new OffscreenCanvas(f.cell, f.cell * f.glyphs.size);
    const img = new ImageData(f.cell, f.cell * f.glyphs.size);
    // I4, two pixels a byte (high nibble first), as alpha on the crawl's colour (235, 205, 105; 0x800CD4F8).
    for (let i = 0; i < img.width * img.height; i++) {
      const v = ((f.pixels[i >> 1] ?? 0) >> (i & 1 ? 0 : 4)) & 15;
      img.data.set([235, 205, 105, v * 17], i * 4);
    }
    cv.getContext('2d').putImageData(img, 0, 0);
    crawlFontData = { ...f, sheet: cv };
  }
  return crawlFontData;
}
// After any crawl write (and undo): the open crawl is rebuilt where the playhead was.
function crawlsChanged() {
  if (cut?.crawl) showCrawl(cut.crawl.which, { t: cut.t, playing: cut.playing });
}
// The crawl window: its text (its camera and routes are edited with the cutscene pencil, in the cutscene inspector and the view).
function openCrawlPanel(ctx) {
  const c = ctx.c, box = elem('div', { className: 'crawlEdit' }), save = (label, change) => saveCrawl(ctx, label, change);
  const chapter = elem('input', { value: c.chapter, placeholder: 'Chapter V' }), title = elem('input', { value: c.title, placeholder: 'THE ICE MOON' });
  const body = elem('textarea', { value: c.body, rows: 6 }), note = elem('small', { className: 'shotNote' });
  const gaps = () => {
    const bad = crawlFontGaps(chapter.value + title.value + body.value);
    note.textContent = bad.length ? `The crawl font cannot draw ${bad.join(' ')}; they are left out.` : '';
  };
  chapter.oninput = title.oninput = body.oninput = gaps;
  chapter.onchange = () => save('chapter', { chapter: chapter.value.trim() });
  title.onchange = () => save('title', { title: title.value.trim() });
  body.onchange = () => save('text', { body: body.value.trim() });
  gaps();
  const status = ctx.exists ? `${ctx.label}: this mod's crawl (${ctx.name})` : `${ctx.label}: the game's crawl; an edit makes this mod's own`;
  const textRow = stacked(fld('Text', body, 'Wrapped to the crawl width; an empty line starts a paragraph'));
  textRow.classList.add('crawlText');
  box.append(elem('small', { className: 'shotNote', textContent: status }), stacked(fld('Chapter', chapter, 'Small line above the title; may be empty')), stacked(fld('Title', title)), textRow, note);
  $('crawlBody').replaceChildren(box);
  $('crawlPanel').hidden = false;
  placeCrawlPanel();
}
// Beside the cutscene timeline when there is room, else below it (as the cutscene inspector) and left of the inspector while that is
// open, unless dragged by its title.
let crawlPos = stored('rs64.crawlPos', null);
function placeCrawlPanel() {
  const p = $('crawlPanel');
  if (p.hidden) return;
  const main = p.parentElement.getBoundingClientRect(), tl = $('cutPanel').getBoundingClientRect(), beside = $('cutPanel').hidden || tl.right + 10 + p.offsetWidth <= main.right - 10;
  const ins = $('cutInspector').hidden ? null : $('cutInspector').getBoundingClientRect();
  const left = crawlPos ? THREE.MathUtils.clamp(crawlPos.x, 0, Math.max(0, main.width - p.offsetWidth)) : ins ? Math.max(0, ins.left - main.left - p.offsetWidth - 10) : null;
  const top = crawlPos ? THREE.MathUtils.clamp(crawlPos.y, 0, Math.max(0, main.height - 80)) : beside ? 10 : tl.bottom - main.top + 8;
  Object.assign(p.style, { top: `${top}px`, left: left == null ? '' : `${left}px`, right: left == null ? '' : 'auto', maxHeight: `${main.height - top - 10}px` });
}
new ResizeObserver(placeCrawlPanel).observe($('cutPanel'));
// The window keeps the size it was dragged to (resizing it writes its inline size).
const crawlSize = stored('rs64.crawlSize', null);
if (crawlSize) Object.assign($('crawlPanel').style, { width: `${crawlSize.w}px`, height: `${crawlSize.h}px` });
new ResizeObserver(() => {
  const p = $('crawlPanel');
  if (!p.hidden && p.style.height) store('rs64.crawlSize', { w: p.offsetWidth, h: p.offsetHeight });
}).observe($('crawlPanel'));
{
  const head = $('crawlPanel').querySelector('.rulesPanelHead');
  head.title = 'Drag to move the window; double-click to put it back';
  let drag = null;
  head.addEventListener('pointerdown', (e) => {
    if (e.target.closest('button')) return;
    const r = $('crawlPanel').getBoundingClientRect();
    drag = { dx: e.clientX - r.left, dy: e.clientY - r.top };
    head.setPointerCapture(e.pointerId);
  });
  head.addEventListener('pointermove', (e) => {
    if (!drag) return;
    const main = $('crawlPanel').parentElement.getBoundingClientRect();
    crawlPos = { x: e.clientX - main.left - drag.dx, y: e.clientY - main.top - drag.dy };
    placeCrawlPanel();
  });
  head.addEventListener('pointerup', () => {
    drag = null;
    store('rs64.crawlPos', crawlPos);
  });
  head.addEventListener('dblclick', () => {
    crawlPos = null;
    store('rs64.crawlPos', null);
    placeCrawlPanel();
  });
}
setEditIcon($('crawlClose'), 'close');
$('crawlClose').onclick = () => ($('crawlPanel').hidden = true);
// Selects the level object of that name (a unit in Objects mode) and moves the view onto it.
function selectNamed(name) {
  const find = () => editObjects().find(([, x]) => x.name === name)?.[0];
  let o = find();
  if (o && isUnit(editItemOf(o)) && editMode !== 'objects') {
    setEditMode('objects');
    o = find();
  }
  if (!o) return;
  const d = o.getWorldPosition(new THREE.Vector3()).sub(controls.target);
  controls.target.add(d);
  camera.position.add(d);
  select(o);
}
// A spawn action's wave: count copies of the kind on the path, waiting for a free flag the action raises.
function spawnWaveButton(a, dir, done) {
  const b = elem('button', { textContent: a.flag ? `Wave ready (flag ${a.flag})` : 'Create wave', title: 'Adds the enemies to the level, waiting off-stage until this action brings them in' });
  b.disabled = !!a.flag || !a.kind || !a.count || !a.path;
  b.onclick = async () => {
    const t = await enemyTemplate(a.kind), items = parseDAT(await rom.read(dir + '/dat')).items, path = items.find((x) => x.type === 3 && x.name === a.path);
    if (!t || !path) return;
    const waves = await readWaves(dir), name = `rule_${a.path}_${Object.keys(waves).length + 1}`;
    const flag = waveFlagFor(waves, name, levelUsedFlags(await rom.read(dir + '/dat')));
    if (!flag) return alert('No free mission flag left in this level');
    const tdv = await rom.read(t.dir + '/dat'), rec = new Uint8Array(tdv.buffer, tdv.byteOffset + t.item.offset, tdv.getUint32(t.item.offset + 8)).slice();
    await historyGroup(`Spawn wave ${name}`, async () => {
      await addWave(dir, waves, name, flag);
      for (let n = 0; n < Math.min(a.count, 8); n++) {
        await editAsset(dir + '/dat', (bytes) => {
          const out = datAddEnemy(bytes, rec, [path.pos[0] + n * 3, path.pos[1], path.pos[2]]);
          const enemy = parseDAT(new DataView(out.buffer)).items.filter((x) => x.type === 0).at(-1);
          return waveSet(out, enemy, { flag, path: a.path });
        }, `Spawn wave ${name}`);
      }
      a.flag = flag;
      await done();
    });
    await reloadLevel(dir);
  };
  return b;
}
// The song a music region plays when the player flies into it (a cue of the in-mission cue table).
function musicZoneRow(it) {
  const sel = selectOf(parseMusicCues(romBytes).cues.map((c) => [c.cue, c.name]), it.songs?.[0] ?? '', { title: 'Plays when the player enters this region' });
  sel.onchange = () => editItem(it, (b) => musicZoneSet(b, it, +sel.value), `Song: ${it.name || 'music region'} -> ${sel.selectedOptions[0].text}`);
  return elem('div', { className: 'shotRow' }, 'Song on entry ', sel);
}
// A wingman spawn's path (without one it stays where it is placed).
function wingmanPathRow(it) {
  const row = elem('div', { className: 'shotRow' });
  rom.read(selected + '/dat').then((dv) => row.append('Path ', npcPathSelect(it, dv, '', 'None (stays put)')));
  return row;
}
// Enemies from any level: the level's own record of that kind, else the first in another level (its gate and path reset, as they
// name that level's flags and splines).
async function enemyTemplate(sub) {
  const own = parseDAT(await rom.read(selected + '/dat')).items.find((x) => x.type === 0 && x.sub === sub);
  if (own) return { dir: selected, item: own };
  for (const p of [...rom.assets.keys()].filter((p) => /^data\/level\/lv_.\/dat$/.test(p)).sort()) {
    const item = parseDAT(await rom.read(p)).items.find((x) => x.type === 0 && x.sub === sub);
    if (item) return { dir: p.slice(0, -4), item };
  }
  return null;
}
// The Add palette: what the current edit mode can place, by category, filtered by the search box. Picking one arms placement.
const ENEMY_GROUPS = [
  ['Imperial fighters', [0x0C, 0x12, 0x13, 0x29]],
  ['Imperial ground units', [0x1F, 0x0F, 0x47, 0x25, 0x3B]],
  ['Imperial transports', [0x24, 0x2C, 0x3E, 0x30, 0x31, 0x2F]],
  ['Rebels and civilians', [0x2E, 0x2B, 0x3C, 0x3D, 0x35, 0x34, 0x2D]],
  ['Other', [0x21, 0x48]],
];
let addEntries = [];
function renderAddList() {
  const q = $('addSearch').value.trim().toLowerCase(), el = $('addList');
  el.innerHTML = '';
  let group = null;
  for (const e of addEntries) {
    if (q && !`${e.label} ${e.group}`.toLowerCase().includes(q)) continue;
    if (e.group !== group) {
      group = e.group;
      el.append(elem('b', { textContent: group }));
    }
    const row = elem('button', { className: 'addRow' + (placing === e.key ? ' active' : ''), title: e.tip ?? '' });
    row.innerHTML = `${e.icon ?? ''}<span>${esc(e.label)}</span>`;
    row.onclick = () => {
      placing = placing === e.key ? null : e.key;
      select(null);
      renderAddList();
    };
    el.append(row);
  }
  if (!el.childElementCount) el.append(elem('small', { className: 'shotNote', textContent: q ? 'Nothing matches.' : 'Nothing to add here.' }));
  // While placing, the list is covered so the next step is clear: click the ground, or cancel.
  $('addOverlay').hidden = !placing;
  if (!placing) dropGhost();
  $('addOverlayText').textContent = `Click the ground to place: ${addEntries.find((e) => e.key === placing)?.label ?? 'it'}`;
}
// While placing, a see-through marker of the kind follows the cursor over the ground, where a click would put it.
let ghost = null;
function dropGhost() {
  ghost?.removeFromParent();
  ghost = null;
}
const placingTemplate = () => (placing && !placing.startsWith('enemy:') ? addTemplates.get(placing)?.it ?? addTemplates.get(placing) : null);
function placeGhost(e, zone) {
  const markers = root?.getObjectByName('markers');
  if (!placing || !markers || editMode === 'terrain') return dropGhost();
  if (zone || ghost?.userData.kind !== placing || ghost.parent !== markers) {
    dropGhost();
    const t = placingTemplate();
    const it = !t ? { type: 0, sub: +placing.slice(6), name: '', pos: [0, 0, 0], rot: [0, 0, 0], scale: [1, 1, 1] } : { ...t, pos: zone?.pos ?? [0, 0, 0], ...(zone && { size: zone.size, rot: zone.rot }) };
    ghost = makeMarker(it);
    ghost.traverse((o) => {
      o.raycast = () => {};
      if (!o.material) return;
      o.material = o.material.clone();
      Object.assign(o.material, { transparent: true, opacity: 0.55, depthWrite: false });
    });
    ghost.userData = { kind: placing };
    markers.add(ghost);
    if (zone) return;
  }
  const hit = cursorGround(e);
  ghost.visible = !!hit;
  if (hit) ghost.position.copy(hit);
}
// The ground point under the cursor in game space (the markers' frame), or null.
function cursorGround(e) {
  const markers = root?.getObjectByName('markers'), terrain = root?.getObjectByName('terrain'), hit = markers && pick(e, terrain ? [terrain] : [root]);
  return hit ? markers.worldToLocal(hit.point.clone()) : null;
}
canvas.addEventListener('pointermove', (e) => zoneDrag || placeGhost(e));
// Zones are dragged out: press on one corner, release on the opposite one (a click keeps the copied zone's size).
let zoneDrag = null;
const zoneFrom = (a, b, t) => ({ pos: [Math.min(a.x, b.x), a.y, Math.min(a.z, b.z)], size: [Math.abs(b.x - a.x), t.size[1], Math.abs(b.z - a.z)], rot: [0, 0, 0] });
canvas.addEventListener('pointerdown', (e) => {
  const t = placingTemplate();
  if (e.button !== 0 || t?.type !== 6 || !(editMode === 'events')) return;
  const a = cursorGround(e);
  if (!a) return;
  zoneDrag = { a, t };
  controls.enabled = false;
  canvas.setPointerCapture(e.pointerId);
  e.stopImmediatePropagation();
}, true);
canvas.addEventListener('pointermove', (e) => {
  if (!zoneDrag) return;
  const b = cursorGround(e);
  if (b) placeGhost(e, zoneFrom(zoneDrag.a, b, zoneDrag.t));
}, true);
canvas.addEventListener('pointerup', (e) => {
  if (!zoneDrag) return;
  e.stopImmediatePropagation();
  const { a, t } = zoneDrag, b = cursorGround(e) ?? a, z = zoneFrom(a, b, t), src = addTemplates.get(placing);
  zoneDrag = null;
  controls.enabled = true;
  placing = null;
  renderAddList();
  // A press that barely moved places the zone as copied, at the press point.
  if (z.size[0] < 0.5 && z.size[2] < 0.5) addItem(src, [a.x, a.y, a.z]);
  else addItem(src, z.pos, z);
}, true);
$('addSearch').oninput = renderAddList;
$('addCancel').onclick = () => {
  placing = null;
  renderAddList();
};
function fillAddEnemies() {
  const have = new Set();
  root?.traverse((o) => editItemOf(o)?.type === 0 && have.add(editItemOf(o).sub));
  addEntries = [];
  for (const [group, subs] of ENEMY_GROUPS) {
    for (const s of subs.filter((x) => WAVE_FIELDS[x] && (have.has(x) || WAVE_FOREIGN.has(x))).sort((a, b) => SUBTYPES[a].localeCompare(SUBTYPES[b]))) {
      addEntries.push({ key: 'enemy:' + s, group, label: SUBTYPES[s], foreign: !have.has(s), icon: icon(TYPES[0]) });
    }
  }
  renderAddList();
}
async function addEnemy(sub, pos) {
  const t = await enemyTemplate(sub);
  if (!t) return;
  const dv = await rom.read(t.dir + '/dat'), rec = new Uint8Array(dv.buffer, dv.byteOffset + t.item.offset, dv.getUint32(t.item.offset + 8)).slice();
  await editAsset(selected + '/dat', (bytes) => datAddEnemy(bytes, rec, pos), `Add ${SUBTYPES[sub]}`);
  const count = parseDAT(await rom.read(selected + '/dat')).items.filter((x) => x.type === 0).length;
  await reselect(0, count - 1);
}
function syncSelPanel() {
  const it = sel?.it;
  $('selInfo').textContent = !sel
    ? `Click ${editMode === 'objects' ? 'an object' : 'an event marker'} to select it`
    : `${it.name || '(unnamed)'}: ${it.type === 0 && it.model ? it.model : TYPES[markerKind(it)]?.label ?? 'item'}` + (sel.fixed ?? '');
  const f = $('selFields');
  f.textContent = '';
  if (sel?.pathNode) {
    const row = elem('label');
    row.textContent = `Along ${sel.pathNode.s.name || 'spline'} %`;
    const inp = elem('input', { type: 'number', step: 'any', min: 0, max: 100, value: +(sel.pathAt * 100).toFixed(2) });
    inp.onchange = () => {
      const n = parseFloat(inp.value);
      if (!Number.isFinite(n)) return;
      slidePathEvent(THREE.MathUtils.clamp(n / 100, 0, 1));
      commitSel();
    };
    row.append(inp);
    f.append(row);
  } else if (sel && !sel.fixed) for (const [label, , key] of PLACE_FIELDS) {
    // Only placed models use their scale; the other type-0 markers ignore it.
    if (!Array.isArray(it[key]) || (key === 'scale' && !sel.o.userData.placedItem)) continue;
    const row = elem('label');
    row.textContent = label;
    it[key].forEach((v, a) => {
      const inp = elem('input', { type: 'number', step: 'any', value: +v.toFixed(3), title: 'XYZ'[a] });
      inp.onchange = () => {
        const n = parseFloat(inp.value);
        if (!Number.isFinite(n)) return;
        const d = key === 'pos' ? n - it.pos[a] : 0;
        it[key] = it[key].slice();
        it[key][a] = n;
        if (it.points && d) for (const pt of it.points) pt[a] += d;
        itemToProxy();
        showItemChange();
        commitSel();
      };
      row.append(inp);
    });
    f.append(row);
  }
  if (sel && editMode === 'events' && it.type === 7 && (it.sub === 0x18 || it.sub === 0x19) && it.name) f.append(shotLinkRow(it), destroyRow(it), waveTriggerRow(it));
  if (sel && editMode === 'objects' && it.type === 0 && WAVE_FIELDS[it.sub]) f.append(waveEnemyRow(it));
  if (sel && editMode === 'events' && it.type === 0 && it.sub === 0x27) f.append(wingmanPathRow(it));
  if (sel && editMode === 'events' && it.type === 7 && it.pathEvent) f.append(rulesSection({ kind: 'path', it }));
  else if (sel && editMode === 'events' && it.type === 7 && it.onEnter) f.append(rulesSection({ kind: 'area', it }));
  if (sel && editMode === 'objects' && isUnit(it)) f.append(rulesSection({ kind: 'unit', it }));
  if (sel && editMode === 'events' && it.type === 6 && it.sub === 0x10) f.append(musicZoneRow(it));
  if (sel && editMode === 'events' && it.type === 3 && it.sub === 3 && it.points?.length >= 4 && !sel.pathNode) f.append(pathAttackRow(it));
  $('btnDup').disabled = !sel || !!sel.fixed || !!it.pathEvent;
  $('btnDel').disabled = !sel || !canDelete(it);
  if (rulesPanelDir !== selected) $('rulesPanel').hidden = true;
  hud();
}
// Copies an item's record with a new position and appends it as the last of its kind, so no existing index moves.
// src is an item of this level, or { it, dv } from another level's DAT (copied inert).
// zone: { size, rot } written over a zone's footprint (+0x28) and rotation (+0x1C) when it was dragged out.
async function addItem(src, pos, zone) {
  const it = src.it ?? src, dir = selected, dv = src.dv ?? (await rom.read(dir + '/dat'));
  let rec = new Uint8Array(dv.buffer, dv.byteOffset + it.offset, dv.getUint32(it.offset + 8)).slice();
  if (src.dv) rec = recordInert(rec, it);
  pos.forEach((v, a) => new DataView(rec.buffer).setFloat32(0x10 + a * 4, v));
  if (zone) zone.size.concat(zone.rot).forEach((v, a) => new DataView(rec.buffer).setFloat32((a < 3 ? 0x28 : 0x1C - 12) + a * 4, v));
  // A copy keeps its source's name; one that rules name (its source deleted or not) gets its own, so it never takes over their rules.
  const ruled = await ruleNames(dir);
  await historyGroup(`Add ${it.name || TYPES[markerKind(it)]?.label || 'item'}`, async () => {
    await editAsset(dir + '/dat', (bytes) => datAddItem(bytes, it.type, rec));
    if (!it.name || !ruled.has(it.name)) return;
    await editAsset(dir + '/dat', (bytes) => {
      const added = parseDAT(dvOf(bytes)).items.filter((x) => x.type === it.type).reduce((a, b) => (a && a.index >= b.index ? a : b), null);
      return datNameItem(bytes, added, uniqueName(bytes, it.name, ruled));
    });
  });
  await reloadLevel(dir);
  const last = editObjects().filter(([, x]) => x.type === it.type).reduce((a, b) => (a && a[1].index >= b[1].index ? a : b), null);
  if (last) select(last[0]);
}
$('btnDup').onclick = () => sel && !sel.fixed && !sel.it.pathEvent && addItem(sel.it, [sel.it.pos[0] + 4, sel.it.pos[1], sel.it.pos[2]]);
$('btnDel').onclick = () => sel && canDelete(sel.it) && deleteItem(sel.it);
// Event kinds any level can add, copied from another level when this one has none (a blank level starts with only its player start):
// the template's stock DAT first, then the stock levels in order.
const FOREIGN_KINDS = ['player', 'wing', 'cam', 'zhi', 'zlo', 'zmus', 'zunit', 'zmtrig', 'tvoice', 'tevent', 'tspawn', 'troam', 'twaypoint'];
async function foreignTemplates(dir) {
  const tmpl = modLevelOf(dir)?.template, first = tmpl != null ? `data/level/lv_${tmpl.toString(36)}/dat` : null, out = new Map();
  const dats = [...rom.assets.keys()].filter((k) => /^data\/level\/lv_[0-9a-i]\/dat$/.test(k)).sort((a, b) => (b === first) - (a === first) || a.localeCompare(b));
  for (const p of dats) {
    const dv = await rom.read(p);
    for (const it of parseDAT(dv).items) {
      const k = String(markerKind(it));
      if (FOREIGN_KINDS.includes(k) && !it.pathEvent && !out.has(k)) out.set(k, { it, dv });
    }
  }
  return out;
}
async function fillAddKinds() {
  addTemplates = new Map();
  for (const m of root?.getObjectByName('markers')?.children ?? []) {
    const it = m.userData.item, k = String(markerKind(it));
    if (!it.pathEvent && !addTemplates.has(k)) addTemplates.set(k, it);
  }
  const local = new Set(addTemplates.keys()), dir = selected;
  for (const [k, t] of await foreignTemplates(dir)) if (!addTemplates.has(k)) addTemplates.set(k, t);
  if (dir !== selected || editMode !== 'events') return;
  addEntries = [];
  for (const [group, kinds] of LEGEND_GROUPS) {
    for (const k of kinds ?? [...addTemplates.keys()].filter((x) => x.startsWith('path:') || x === '3')) if (addTemplates.has(k)) addEntries.push({ key: k, group, label: TYPES[k]?.label ?? k, tip: TYPES[k]?.tip, foreign: !local.has(k), icon: TYPES[k] ? icon(TYPES[k]) : '' });
  }
  renderAddList();
}
// Left clicks in Objects / Events mode select (or place) instead of opening the info card.
function editClick(e) {
  if (editMode !== 'objects' && editMode !== 'events') return false;
  if (gizmo.axis) return true;
  if (placing) {
    const terrain = root.getObjectByName('terrain'), hit = pick(e, terrain ? [terrain] : [root]);
    if (!hit) return true;
    const kind = placing, at = root.worldToLocal(hit.point.clone()).toArray();
    placing = null;
    renderAddList();
    if (kind.startsWith('enemy:')) return addEnemy(+kind.slice(6), at), true;
    addItem(sel && String(markerKind(sel.it)) === kind ? sel.it : addTemplates.get(kind), at);
    return true;
  }
  select(editHit(e));
  return true;
}
const EDIT_MODES = ['terrain', 'objects', 'events'];
function editKey(e) {
  // Tab / Shift+Tab cycle the edit modes while editing.
  if (editMode && e.code === 'Tab' && !e.ctrlKey && !e.altKey && !e.metaKey) {
    e.preventDefault();
    setEditMode(EDIT_MODES[(EDIT_MODES.indexOf(editMode) + (e.shiftKey ? 2 : 1)) % 3]);
    return true;
  }
  if (editMode !== 'objects' && editMode !== 'events') return false;
  if (TOOL_KEYS[e.code] && !rmbHeld && !e.ctrlKey && !e.metaKey && !e.altKey) setTool(TOOL_KEYS[e.code]);
  else if (e.code === 'KeyX' && !e.ctrlKey && !e.metaKey) setEditSpace(editSpace === 'world' ? 'local' : 'world');
  else if (DELETE_KEYS.has(e.key) && sel && canDelete(sel.it)) deleteItem(sel.it);
  else if (e.code === 'KeyD' && (e.ctrlKey || e.metaKey) && sel && !sel.fixed && !sel.it.pathEvent) {
    e.preventDefault();
    addItem(sel.it, [sel.it.pos[0] + 4, sel.it.pos[1], sel.it.pos[2]]);
  } else if (e.code === 'Escape') {
    placing = null;
    renderAddList();
    select(null);
  } else return false;
  return true;
}
// Zones in Scale mode also get corner handles: dragging one moves that corner over the ground while the opposite corner stays put. A zone
// spans pos .. pos + R * size (X/Z), as makeMarker draws it.
const cornerMat = new THREE.MeshBasicMaterial({ color: 0xe8b84a, depthTest: false, transparent: true });
const cornerGeo = new THREE.SphereGeometry(1, 12, 8), _cw = new THREE.Vector3();
// Constant on-screen size: about 1.2% of the distance to the camera.
const sizeCorners = () => corners.forEach((h) => h.scale.setScalar(camera.position.distanceTo(h.getWorldPosition(_cw)) * 0.012));
let corners = [], cornerDrag = null;
function cornerPoint(it, cu, cv) {
  const p = new THREE.Vector3(cu * it.size[0], 0, cv * it.size[2]).applyMatrix4(placementRotation(it.rot)).add(new THREE.Vector3(...it.pos));
  p.y = levelHeight(p.x, p.z) - 0.2;
  return p;
}
// A selected path gets a handle per control point: drag moves it over the ground, Shift+drag changes its height.
const pathPointMat = new THREE.MeshBasicMaterial({ color: 0x4ac0e8, depthTest: false, transparent: true });
function showCorners() {
  for (const c of corners) c.removeFromParent();
  corners = [];
  if (sel && editMode === 'events' && sel.it.type === 3 && sel.it.sub === 3 && sel.it.points?.length >= 4 && !sel.pathNode) {
    sel.it.points.forEach((p, k) => {
      const h = new THREE.Mesh(cornerGeo, pathPointMat);
      h.position.set(...p);
      h.renderOrder = 10;
      h.userData.pathPoint = k;
      sel.o.parent.add(h);
      corners.push(h);
    });
    return;
  }
  if (!sel || sel.it.type !== 6 || editTool !== 'scale') return;
  for (const [cu, cv] of [[0, 0], [1, 0], [1, 1], [0, 1]]) {
    const h = new THREE.Mesh(cornerGeo, cornerMat);
    h.position.copy(cornerPoint(sel.it, cu, cv));
    h.renderOrder = 10;
    h.userData.corner = [cu, cv];
    sel.o.parent.add(h);
    corners.push(h);
  }
}
// Capture-phase listeners run before the orbit controls and the gizmo, so a corner drag neither orbits nor reselects.
canvas.addEventListener('pointerdown', (e) => {
  if (e.button !== 0 || !corners.length) return;
  const h = pick(e, corners);
  if (!h) return;
  if (h.object.userData.pathPoint !== undefined) {
    cornerDrag = { point: h.object.userData.pathPoint, handle: h.object, vertical: e.shiftKey };
    controls.enabled = false;
    canvas.setPointerCapture(e.pointerId);
    e.stopImmediatePropagation();
    return;
  }
  const it = sel.it, rot = placementRotation(it.rot), [cu, cv] = h.object.userData.corner;
  const A = new THREE.Vector3((1 - cu) * it.size[0], 0, (1 - cv) * it.size[2]).applyMatrix4(rot).add(new THREE.Vector3(...it.pos));
  cornerDrag = { cu, cv, A, rot, inv: rot.clone().invert(), y: h.object.position.y, pos0: new THREE.Vector3(...it.pos), size0: it.size.slice() };
  controls.enabled = false;
  canvas.setPointerCapture(e.pointerId);
  e.stopImmediatePropagation();
}, true);
canvas.addEventListener('pointermove', (e) => {
  if (!cornerDrag) return;
  e.stopImmediatePropagation();
  const r = canvas.getBoundingClientRect(), c = cornerDrag, it = sel.it, o = sel.o;
  pointer.set(((e.clientX - r.left) / r.width) * 2 - 1, -((e.clientY - r.top) / r.height) * 2 + 1);
  raycaster.setFromCamera(pointer, camera);
  if (c.point !== undefined) {
    // Over the ground: a level plane through the point; height: an upright plane through it facing the camera.
    const at = root.localToWorld(c.handle.position.clone()), up = root.localToWorld(new THREE.Vector3(0, 1, 0)).sub(root.localToWorld(new THREE.Vector3()));
    const normal = c.vertical ? camera.getWorldDirection(new THREE.Vector3()).projectOnPlane(up).normalize() : up.normalize();
    const w = raycaster.ray.intersectPlane(new THREE.Plane().setFromNormalAndCoplanarPoint(normal, at), new THREE.Vector3());
    if (!w) return;
    const l = root.worldToLocal(w), p = c.handle.position;
    if (c.vertical) p.y = l.y;
    else p.set(l.x, p.y, l.z);
    it.points[c.point] = p.toArray();
    return;
  }
  const plane = new THREE.Plane().setFromNormalAndCoplanarPoint(new THREE.Vector3(0, 1, 0), root.localToWorld(new THREE.Vector3(0, c.y, 0)));
  const w = raycaster.ray.intersectPlane(plane, new THREE.Vector3());
  if (!w) return;
  const d = root.worldToLocal(w).sub(c.A).applyMatrix4(c.inv);
  it.size = [c.cu ? d.x : -d.x, it.size[1], c.cv ? d.z : -d.z];
  const p = new THREE.Vector3(c.cu ? 0 : d.x, 0, c.cv ? 0 : d.z).applyMatrix4(c.rot).add(c.A);
  it.pos = [p.x, c.pos0.y, p.z];
  // Preview: the drawn footprint stretched along the zone's own axes; the commit rebuilds it draped on the ground.
  const k = [it.size[0] / (c.size0[0] || 1), 1, it.size[2] / (c.size0[2] || 1)];
  o.matrixAutoUpdate = false;
  o.matrix.makeTranslation(...it.pos).multiply(c.rot).multiply(new THREE.Matrix4().makeScale(...k)).multiply(c.inv);
  o.matrixWorldNeedsUpdate = true;
  corners.forEach((h) => h.position.copy(cornerPoint(it, ...h.userData.corner)));
  boxAround(selBox, o);
  syncSelPanel();
}, true);
canvas.addEventListener('pointerup', (e) => {
  if (!cornerDrag) return;
  e.stopImmediatePropagation();
  const c = cornerDrag;
  cornerDrag = null;
  controls.enabled = true;
  if (c.point === undefined) return commitSel();
  const it = sel.it;
  editItem(it, (bytes) => splineSetPoints(bytes, it, it.points), `Move point ${c.point + 1}: ${it.name || 'path'}`);
}, true);
setTool('translate');
setEditMode(null);
// Rules naming objects their level no longer has, as "level: name" lines (for the export warning).
async function brokenRuleLines() {
  const out = [];
  for (const p of modDirty().filter((p) => /^mod\/assets\/level\/[^/]+\/rules\.json$/.test(p))) {
    const folder = p.split('/')[3], dir = 'data/level/' + folder;
    const items = parseDAT(await rom.read(dir + '/dat')).items;
    for (const b of rulesBroken(rulesParse(await virtualText(p)), items, ruleExt)) out.push(`${folder}: ${b.name}`);
  }
  return out;
}
// The export items: every changed path, with the menu file split into its entries (menuItemKeys).
async function exportItems() {
  const paths = modDirty();
  if (!paths.includes(MENU_FILE)) return paths;
  const doc = menuParse(await virtualText(MENU_FILE)).doc;
  return doc ? [...paths.filter((p) => p !== MENU_FILE), ...menuItemKeys(doc)] : paths;
}
// The mod's files, named relative to its folder: mod.json and one assets/<path>.bps per edited asset. keys (exportItems) picks what goes in;
// a mod level's .from copies go with levels.json (else with the first Levels change).
async function modFiles(id, name, keys = null) {
  const all = await exportItems();
  keys ??= all;
  const paths = keys.filter((k) => !/^(menu|page):/.test(k)), menuKeys = new Set(keys.filter((k) => /^(menu|page):/.test(k)));
  const firstLevels = all.find((k) => k === 'mod/levels.json') ?? all.find((k) => modChangeKind(k) === 'levels');
  const manifest = {
    game_id: 'rs64', id, display_name: name, description: 'Made with the RS64 Explorer: ' + keys.map((p) => p.replace(/^(data|mod)\//, '')).join(', '),
    version: '1.0.0', authors: ['RS64 Explorer'], minimum_recomp_version: '0.0.0',
  };
  const files = [{ name: 'mod.json', data: new TextEncoder().encode(JSON.stringify(manifest, null, 2) + '\n') }];
  for (const path of paths) {
    const now = await rom.read(path);
    if (path.startsWith('mod/')) {
      files.push({ name: path.slice(4), data: u8(now).slice() });
      continue;
    }
    files.push({ name: `assets/${path.replace(/^data\//, '')}.bps`, data: encodeBPS(edits.get(path), u8(now)) });
  }
  if (menuKeys.size) {
    const doc = menuParse(await virtualText(MENU_FILE)).doc, ids = new Set(doc.buttons.filter((b) => menuKeys.has(menuItemKey(doc, b))).map((b) => b.id));
    files.push({ name: MENU_FILE.slice(4), data: new TextEncoder().encode(menuWrite(menuWriteSubset(doc, ids))) });
  }
  // A mod level's files: copies of its template's (.from), patched by the .bps above when edited.
  if (!firstLevels || keys.includes(firstLevels)) for (const [path, from] of rom.aliases) files.push({ name: `assets/${path.replace(/^data\//, '')}.from`, data: new TextEncoder().encode(from.replace(/^data\//, '') + '\n') });
  return [...files, ...(await cutDerivedModelFiles(paths))];
}
const MENU_FILE = 'mod/roguesq_menu.json';
const KIND_LABEL = { levels: 'Levels', models: 'Models', audio: 'Audio', ui: 'UI', fonts: 'Fonts', other: 'Other' };
// (user, used) pairs between export items: a cutscene or level that shows a model the project changes, and menu entries that need
// each other (menuLinks). Found the way the scenes load their models: a cutscene actor as showCutscene, a level item as the level view.
async function exportLinks(items) {
  const set = new Set(items), out = [];
  const modelFiles = (id) => ['_HOB', '_HMT'].map((ext) => id + ext).filter((p) => set.has(p));
  const changedModels = new Set(items.filter((p) => modChangeKind(p) === 'models').map((p) => p.replace(/_(HOB|HMT)$/, '')));
  if (changedModels.size) {
    await cutScanPathModels();
    const index = await buildObjectIndex();
    for (const p of items.filter((x) => x.startsWith('data/cuts/'))) {
      for (const a of parseCutscene(await rom.read(p)).actors) {
        const base = cutBase(a.name), tabled = cutAsset(base).path && 'data/' + cutAsset(base).path;
        const id = cutPathModelId(a.name) ?? (tabled && rom.assets.has(tabled + '_HOB') ? tabled : index.get(base));
        for (const m of id && changedModels.has(id) ? modelFiles(id) : []) out.push([p, m]);
      }
    }
    for (const dir of new Set(items.map((p) => p.match(/^data\/level\/[^/]+/)?.[0]).filter(Boolean))) {
      const dv = await rom.read(dir + '/dat').catch(() => null), mine = items.filter((p) => p.startsWith(dir + '/'));
      for (const it of dv ? parseDAT(dv).items : []) {
        const slot = it.type === 0 && it.sub === 0x06 && it.name.replace(/[*#]\d+$/, ''), sub = it.type === 0 && SUBTYPE_MODEL[it.sub]?.split('#')[0];
        const ids = [it.model && index.get(it.model), sub && 'data/' + sub, sub && `data/${sub}_hi`, slot && 'data/pl_crafts/' + slot, slot && index.get(slot)];
        for (const id of ids.filter((x) => x && changedModels.has(x))) for (const m of modelFiles(id)) for (const p of mine) out.push([p, m]);
      }
    }
  }
  if (items.some((p) => /^(menu|page):/.test(p))) out.push(...menuLinks(menuParse(await virtualText(MENU_FILE)).doc));
  return [...new Map(out.map((l) => [l.join('\n'), l])).values()];
}
$('btnMod').onclick = async () => {
  const broken = await brokenRuleLines();
  if (broken.length && !confirm(`Some level rules name objects that are gone and will be skipped in the game:\n${broken.join('\n')}\n\nExport anyway?`)) return;
  const items = await exportItems(), doc = await readJson(EXPORT_GROUPS, () => ({ groups: [], assign: {}, kindNames: {} }));
  const base = (projectHandle?.name ?? 'my-rs64-mod').replace(/\.rs64proj$/, '');
  const kindNames = Object.fromEntries(MOD_KINDS.map((k) => [k, doc.kindNames?.[k] ?? `${base} - ${KIND_LABEL[k]}`]));
  const plan = exportPlan(items, doc, await exportLinks(items), kindNames);
  if (plan.length <= 1) {
    const name = prompt('Mod folder name (letters, digits and dashes)', plan[0]?.name ?? base);
    if (name) await saveMods([{ name, keys: items }]);
    return;
  }
  exportDialog(items, doc, base);
};
const EXPORT_GROUPS = 'explorer/export-groups.json';
const modFolderId = (name) => name.toLowerCase().replace(/[^a-z0-9-]+/g, '-').replace(/-+/g, '-').replace(/^-|-$/g, '') || 'my-rs64-mod';
// One zip holding a folder per mod.
async function saveMods(mods) {
  const files = [];
  for (const m of mods) {
    const id = modFolderId(m.name);
    for (const f of await modFiles(id, m.name, m.keys)) files.push({ name: `${id}/${f.name}`, data: f.data });
  }
  saveBlob(new Blob([zipStore(files)], { type: 'application/zip' }), `${mods.length === 1 ? modFolderId(mods[0].name) : modFolderId(mods[0].name.replace(/ - [^-]*$/, ''))}.zip`);
}
// The split export: a row per mod (exportPlan) with its name, change count and what it merged; each change can move to another group,
// "+ Group" adds one. Groups, assignments and renamed kind groups are kept in the project (EXPORT_GROUPS).
async function exportDialog(items, doc, base) {
  const dlg = $('exportDialog') ?? document.body.appendChild(elem('dialog', { id: 'exportDialog' }));
  const links = await exportLinks(items), skip = new Set();
  doc.groups ??= [];
  doc.assign ??= {};
  let allInOne = false;
  const kindNames = () => Object.fromEntries(MOD_KINDS.map((k) => [k, doc.kindNames?.[k] ?? `${base} - ${KIND_LABEL[k]}`]));
  const keyLabel = (k) => k.replace(/^menu:/, 'Menu button ').replace(/^page:/, 'Menu page ').replace(/^(data|mod)\//, '');
  const render = () => {
    const plan = exportPlan(items, doc, links, kindNames()), names = kindNames();
    const groupNames = [...new Set([...MOD_KINDS.map((k) => names[k]), ...doc.groups.map((g) => g.name)])];
    const rows = plan.map((e) => {
      const tick = elem('input', { type: 'checkbox', checked: !skip.has(e.name), title: 'Export this mod' });
      tick.onchange = () => (tick.checked ? skip.delete(e.name) : skip.add(e.name));
      const name = elem('input', { value: e.name, title: 'The mod\'s name (its folder is made from it)' });
      name.onchange = () => {
        const kind = MOD_KINDS.find((k) => names[k] === e.name), g = doc.groups.find((x) => x.name === e.name);
        if (!name.value.trim() || groupNames.includes(name.value)) return render();
        if (kind) doc.kindNames = { ...doc.kindNames, [kind]: name.value };
        if (g) {
          g.name = name.value;
          for (const k of Object.keys(doc.assign)) if (doc.assign[k] === e.name) doc.assign[k] = name.value;
        }
        render();
      };
      const list = elem('div', { className: 'exportKeys' }, ...e.keys.map((k) => {
        const pick = elem('select', { title: 'Which mod this change goes in' }, ...groupNames.map((n) => new Option(n, n)));
        pick.value = groupNames.includes(doc.assign[k]) ? doc.assign[k] : names[modChangeKind(k)];
        pick.onchange = () => {
          doc.assign[k] = pick.value;
          render();
        };
        return elem('div', null, elem('span', { textContent: keyLabel(k) }), pick);
      }));
      const more = elem('details', null, elem('summary', { textContent: `${e.keys.length} change${e.keys.length > 1 ? 's' : ''}` }), list);
      const note = e.merged.length ? elem('small', { className: 'shotNote', textContent: `${e.name} uses changes from ${e.merged.join(', ')}: they export as one mod.` }) : '';
      const del = doc.groups.some((g) => g.name === e.name) && elem('button', { title: 'Remove this group (its changes go back to their kind)' });
      if (del) {
        setEditIcon(del, 'delete');
        del.onclick = () => {
          doc.groups = doc.groups.filter((g) => g.name !== e.name);
          render();
        };
      }
      return elem('div', { className: 'exportRow' }, elem('div', { className: 'exportHead' }, tick, name, del || ''), note, more);
    });
    const addGroup = elem('button', { textContent: '+ Group', title: 'Add a mod to move changes into' });
    addGroup.onclick = () => {
      const n = prompt('Group name');
      if (!n || groupNames.includes(n)) return;
      doc.groups.push({ name: n });
      render();
    };
    const one = elem('input', { type: 'checkbox', checked: allInOne });
    one.onchange = () => {
      allInOne = one.checked;
      render();
    };
    const go = elem('button', { textContent: 'Export', title: 'Download the mods as one zip' }), cancel = elem('button', { textContent: 'Cancel' });
    cancel.onclick = () => dlg.close();
    go.onclick = async () => {
      const picked = plan.filter((e) => !skip.has(e.name));
      if (!picked.length) return;
      await editText(EXPORT_GROUPS, JSON.stringify(doc, null, 1), 'Export groups');
      dlg.close();
      await saveMods(allInOne ? [{ name: base, keys: picked.flatMap((e) => e.keys) }] : picked);
    };
    dlg.replaceChildren(elem('h3', { textContent: 'Export mods' }),
      elem('small', { className: 'shotNote', textContent: 'Each kind of change exports as its own mod, so players can turn them on separately. Changes that need each other stay in one mod.' }),
      ...rows, elem('div', { className: 'exportFoot' }, addGroup, elem('label', null, one, ` All in one mod (${base})`), elem('span', { className: 'spacer' }), cancel, go));
  };
  render();
  if (!dlg.open) dlg.showModal();
}
// The game folder's handle is kept in IndexedDB (handles cannot go in localStorage), so the picker shows once.
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
const GAME_EXE = 'RogueSquadron64Recomp.exe', PREVIEW_MOD = 'rs64-explorer-preview';
async function gameFolder() {
  let dir = await idbKv('readonly', (s) => s.get('gameDir')).catch(() => null);
  if (dir && (await dir.queryPermission({ mode: 'readwrite' })) !== 'granted' && (await dir.requestPermission({ mode: 'readwrite' })) !== 'granted') dir = null;
  if (dir) return dir;
  dir = await showDirectoryPicker({ id: 'rs64-game', mode: 'readwrite' });
  const hasExe = await dir.getFileHandle(GAME_EXE).then(() => true, () => false);
  const hasMods = await dir.getDirectoryHandle('mods').then(() => true, () => false);
  if (!hasExe && !hasMods) throw new Error(`Pick the folder that holds ${GAME_EXE} (for a build, build/Debug or build/Release)`);
  await idbKv('readwrite', (s) => s.put(dir, 'gameDir')).catch(() => {});
  return dir;
}
// The game's --boot-target for what is open: a cutscene file plays on its own, a level (or one of its sequences) starts its mission.
// A mod level boots as its template with ROGUESQ_MOD_LEVEL naming it.
function bootTarget() {
  const mc = modLevels.find((l) => selected === `cut:data/cuts/${l.folder}_intro` || selected === `cut:data/cuts/${l.folder}_extro`);
  if (mc) return `cutscene:${mc.template},${selected.endsWith('_intro') ? 0 : 1} --set ROGUESQ_MOD_LEVEL=${mc.id}`;
  const c = selected?.match(/^cut:data\/cuts\/id(\d+)_(intro|extro|special)$/);
  if (c) return `cutscene:${c[1]},${['intro', 'extro', 'special'].indexOf(c[2])}`;
  const ml = selectedLevel && modLevelOf(selected);
  if (ml) return `level:${ml.template} --set ROGUESQ_MOD_LEVEL=${ml.id}`;
  const lv = selectedLevel && selected.match(/\/lv_([0-9a-z])(:\d+)?$/);
  return lv ? `level:${parseInt(lv[1], 36)}` : '';
}
$('btnPlay').onclick = async () => {
  const st = $('playStatus');
  st.hidden = false;
  if (!window.showDirectoryPicker) {
    st.textContent = 'Writing into the game folder needs Chrome or Edge; use Export mod instead';
    return;
  }
  try {
    const dir = await gameFolder(), mods = await dir.getDirectoryHandle('mods', { create: true });
    // Rewritten from scratch so an edit that was undone leaves no patch behind.
    await mods.removeEntry(PREVIEW_MOD, { recursive: true }).catch(() => {});
    if (modDirty().length) {
      const root = await mods.getDirectoryHandle(PREVIEW_MOD, { create: true });
      for (const f of await modFiles(PREVIEW_MOD, 'RS64 Explorer preview')) {
        let d = root;
        const parts = f.name.split('/');
        for (const p of parts.slice(0, -1)) d = await d.getDirectoryHandle(p, { create: true });
        const w = await (await d.getFileHandle(parts.at(-1), { create: true })).createWritable();
        await w.write(f.data);
        await w.close();
      }
    }
    const target = bootTarget(), cmd = `.\\${GAME_EXE}` + (target ? ` --boot-target=${target}` : '');
    await navigator.clipboard.writeText(cmd).catch(() => {});
    const n = modDirty().length;
    st.innerHTML = `${n ? `${n} edited asset${n > 1 ? 's' : ''} written to` : 'No edits; removed'} ${esc(dir.name)}\\mods\\${PREVIEW_MOD}.<br>To play it, run this in ${esc(dir.name)} (copied):<br><code>${esc(cmd)}</code>`;
  } catch (e) {
    st.textContent = e.name === 'AbortError' ? '' : e.message;
    st.hidden = e.name === 'AbortError';
  }
};
