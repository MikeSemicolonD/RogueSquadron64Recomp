'use strict';

// ---- Level editing ----
// The Edit button switches the level view from viewing to editing: Terrain (sculpt and paint), Objects (placed type-0 items) and Events
// (paths, zones, triggers and the rest). A left click selects; the gizmo moves a proxy in game space (the world group's frame) and each
// finished drag or field change rewrites the item's floats in place in the level DAT. Q/W/E/R pick the tool as in Unity; with the right
// mouse button held those keys fly the camera instead.
let rulesPanelDir = null;
let lookPanelDir = null;
let editMode = null;
let lastEditMode = 'terrain';
let editTool = 'translate';
let sel = null;
let placing = null;
let placeBusy = false;
let keepPlacing = false;
let objDir = null;
let objRows = [];
let objItems = [];
let objPick = null;
let levelPanelDir = null;
let levelPanelKey = '';
let levelSaving = Promise.resolve();
let rulePathPick = null;
let lastSpawn = { kind: 0x0C, count: 4 };
let objSaving = Promise.resolve();
const objBoxes = [];
let rmbHeld = false;
let addTemplates = new Map();
const gizmo = new TransformControls(camera, canvas);
const editProxy = new THREE.Object3D();
const _eq = new THREE.Quaternion();
scene.add(gizmo.getHelper());
gizmo.addEventListener('mouseDown', onTrackGizmoDown);
gizmo.addEventListener('mouseUp', onTrackGizmoUp);
gizmo.addEventListener('mouseUp', onCrawlGizmoUp);
gizmo.addEventListener('objectChange', onCrawlGizmoMove);

// The gizmo also draws an unconnected arrowhead / box on each axis's negative side; keep only the ones on a line.
const gizmoParts = gizmo.getHelper().children.find((c) => c.gizmo)?.gizmo;

for (const g of [gizmoParts?.translate, gizmoParts?.scale])
  for (const h of [...(g?.children ?? [])])
  {
    const a = { X: 'x', Y: 'y', Z: 'z' }[h.name];

    if (!a)
      continue;

    // Handle offsets are baked into each geometry (setupGizmo), so test where the geometry sits.
    h.geometry.computeBoundingBox();

    if (h.geometry.boundingBox.getCenter(new THREE.Vector3())[a] < -0.05)
      h.removeFromParent();
  }

// Edit panel buttons are icons (icons/<name>.svg); each keeps its label as the title (and accessible name).
function setEditIcon(b, name) {
  b.innerHTML = iconImg(name);
  b.setAttribute('aria-label', b.title);
}

// A panel button drawn as an icon, its label as the tooltip and accessible name.
function iconButton(icon, title, onclick) {
  const b = elem('button', { title, onclick });

  setEditIcon(b, icon);

  return b;
}

for (const b of $('editPanel').querySelectorAll('[data-icon]'))
  setEditIcon(b, b.dataset.icon);

for (const b of document.querySelectorAll('.histRow [data-icon], #cutEdit, #cutPickRow [data-icon], #cutInspector [data-icon], #viewNav [data-icon]'))
  setEditIcon(b, b.dataset.icon);

// The side panel's and the brush's sliders get step arrows that move them one notch, the same as dragging. The label names its slider, or a click on its text would press the first arrow.
for (const input of document.querySelectorAll('#opts input[type=range], #sculptRadius, #sculptStrength, #optFogDist, #optSpread'))
{
  const [down, up] = input.dataset.steps?.split('|') ?? ['Decrease', 'Increase'];

  const arrow = (icon, title, step) => {

    const b = iconButton(icon, title, () => {

      const before = input.value;

      step();

      if (input.value !== before)
        input.dispatchEvent(new Event('input'));
    });

    b.type = 'button';
    b.className = 'step';

    return b;
  };

  input.closest('label').htmlFor ||= input.id;
  input.before(arrow('chevronLeft', down, () => input.stepDown(+input.dataset.notch || 1)));
  input.after(arrow('chevronRight', up, () => input.stepUp(+input.dataset.notch || 1)));
}

// The published site serves the save editor at save-editor/; in the repo it sits beside this folder.
if (/\/rs64-explorer\/(index\.html)?$/.test(location.pathname))
  $('saveEditorLink').href = '../save-editor/rs64_save_editor.html';

$('btnMod').append(elem('span', { id: 'btnModLabel', textContent: 'Export mod' }));
$('btnMod').removeAttribute('aria-label');
$('btnOpen').onclick = openProject;
$('btnSave').onclick = saveProject;
$('btnUndo').onclick = undoEdit;
$('btnRedo').onclick = redoEdit;

// Global (world) axes by default: X red, Y green pointing up, Z blue. Local follows the object's own (game, Y-down) frame.
let editSpace = 'world';

function setEditSpace(space)
{
  editSpace = space;
  gizmo.setSpace(space);

  const b = $('editSpace');

  b.title = space === 'world' ? 'Global axes: X red, Y green (up), Z blue. Click or press X for the object\'s local axes' : 'Local axes: the object\'s own frame. Click or press X for global axes';
  b.textContent = space === 'world' ? 'Global' : 'Local';
  b.setAttribute('aria-label', b.title);

  if (sel)
    attachGizmo();
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
const editTools = (s) => (isMusicZone(s.it) ? ['translate', 'scale'] : (s.o.userData.placedItem || s.it.type === 6 ? ['translate', 'rotate', 'scale'] : (s.it.type === 0 ? ['translate', 'rotate'] : ['translate'])));
const TOOL_KEYS = { KeyQ: 'view', KeyW: 'translate', KeyE: 'rotate', KeyR: 'scale' };
const editItemOf = (o) => o.userData.placedItem ?? (o.parent?.name === 'markers' ? o.userData.item : null);

// Each mode clicks and hovers only what it edits: Objects = placed objects plus object markers (starts, cameras), Events = the other markers. Terrain and other kinds never get in the way.
// A box marks the hovered item and the selection. Objects mode = placed models; Events mode = every legend marker (spawns, restarts and cameras included).
const editTargets = () => editMode === 'objects'
  ? root.children.filter((o) => o.userData.placedItem && o.visible)
  : visibleMarkers();

function editHit(e) {
  let o = root && pick(e, editTargets())?.object;

  while (o && !editItemOf(o))
    o = o.parent;

  return o ? musicOuter(o) : null;
}

// Right-click in Objects / Events mode lists every editable item under the cursor (nearest first) instead of the inspect card, so nested or
// overlapping ones can be picked; hovering a row boxes it in the view.
function editPickList(e)
{
  if (editMode !== 'objects' && editMode !== 'events')
    return false;

  pick(e, []);

  const found = [];

  for (const h of raycaster.intersectObjects(editTargets(), true)) {
    let o = h.object;
    let shown = true;

    for (let p = o; p; p = p.parent)
      if (!p.visible)
        shown = false;

    while (o && !editItemOf(o))
      o = o.parent;

    const one = o && musicOuter(o);

    if (shown && one && !found.includes(one))
      found.push(one);
  }

  // A path event lists before its path, and a low detail region after the high one the game would use there.
  const rank = (o) => isLowLod(editItemOf(o)) - !!editItemOf(o).pathEvent;

  found.sort((a, b) => rank(a) - rank(b));

  $('tip').hidden = true;

  // One item needs no list: select it as a left click would. None just closes any open card.
  if (found.length < 2) {
    select(found[0] ?? null);

    return true;
  }

  const el = $('info');
  const list = elem('div');

  el.textContent = '';
  list.className = 'pickList';
  el.append(elem('b', { className: 'pickHead', textContent: `${found.length} ${editMode === 'objects' ? 'objects' : 'events'} under the cursor` }));

  for (const o of found)
  {
    const it = editItemOf(o);
    const placed = !!o.userData.placedItem;
    const row = elem('div');
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

const hoverBox = new THREE.BoxHelper(new THREE.Object3D(), 0x8ab4ff);
const selBox = new THREE.BoxHelper(new THREE.Object3D(), 0xe8b84a);
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

function editHudRows(o)
{
  const it = editItemOf(o);
  const f = (v) => v.map((n) => +n.toFixed(2)).join(', ');
  const placed = !!o.userData.placedItem;

  const rows = [
    [o === editHovered ? 'hover' : 'selected', it.name || it.label || '(unnamed)'],
    ['kind', placed ? 'Object' : TYPES[markerKind(it)]?.label ?? 'Type ' + it.type]
  ];

  if (placed && o.userData.source)
    rows.push(['model', o.userData.source.name]);

  rows.push(['subtype', SUBTYPES[it.sub] ?? hex(it.sub)]);

  const pn = it.pathNodes?.[0];

  if (pn)
    rows.push(['along', `${pn.s.name || 'spline'} ${(((o === sel?.o && sel.pathAt) ?? pn.s.events[pn.k].at) * 100).toFixed(1)}%`]);
  else
    rows.push(['position', f(it.pos)]);

  if (it.rot && !pn)
    rows.push(['rotation', f(it.rot)]);

  if (it.scale && placed)
    rows.push(['scale', f(it.scale)]);

  if (it.size)
    rows.push(['size', f(it.size)]);

  if (it.radius > 0)
    rows.push(['radius', +it.radius.toFixed(2)]);

  return rows;
}

canvas.addEventListener('pointerleave', () => {
  if (!editHovered)
    return;

  editHovered = null;
  hud();
});

function editHover(e)
{
  if (!editMode)
    return false;

  const o = (editMode === 'objects' || editMode === 'events') && !e.buttons && !gizmo.axis && !placing ? editHit(e) : null;

  if (o !== editHovered) {
    editHovered = o;
    hud();
  }

  boxAround(hoverBox, o !== sel?.o && o);
  canvas.style.cursor = o || placing ? 'pointer' : '';

  if (editMode !== 'events' || !o)
    $('tip').hidden = true;

  return editMode !== 'events' || !o;
}

function setEditMode(m)
{
  if (m)
    lastEditMode = m;

  editMode = m;
  placing = null;
  objSetPick(null);
  stopRulePathPick();

  if (m !== 'objects' && m !== 'events')
    closeObjectives();

  $('editPanel').classList.toggle('open', !!m);
  $('editTab').classList.toggle('on', !!m);
  $('editTab').setAttribute('aria-pressed', !!m);

  for (const b of $('editBody').querySelectorAll('[data-mode]'))
    b.classList.toggle('on', b.dataset.mode === m);

  // Edit-panel controls and help rows name the modes they show in; "none" is viewing.
  for (const el of document.querySelectorAll('[data-modes]'))
    el.hidden = !el.dataset.modes.split(' ').includes(m ?? 'none');

  $('optSculpt').checked = m === 'terrain';
  loadSculpt();
  $('info').hidden = true;
  hud();

  if (m !== 'events')
    $('rulesPanel').hidden = true;

  if (m === 'events')
    fillAddKinds();

  if (m === 'objects')
    fillAddEnemies();

  select(null);
}

$('editTab').onclick = () => setEditMode(editMode ? null : lastEditMode);

for (const b of $('editBody').querySelectorAll('[data-mode]'))
  b.onclick = () => setEditMode(b.dataset.mode);

for (const b of $('editBody').querySelectorAll('[data-tool]'))
  b.onclick = () => setTool(b.dataset.tool);

function setTool(t) {
  editTool = t;

  for (const b of $('editBody').querySelectorAll('[data-tool]'))
    b.classList.toggle('on', b.dataset.tool === t);

  attachGizmo();
}

function attachGizmo()
{
  const tools = sel && !sel.fixed ? editTools(sel) : [];

  for (const b of $('editBody').querySelectorAll('[data-tool]'))
    b.disabled = !!sel && b.dataset.tool !== 'view' && !tools.includes(b.dataset.tool);

  // No handles while placing, or a click near the last placed thing would grab them.
  if (tools.includes(editTool) && !placing && !rulePathPick)
  {
    gizmo.setMode(editTool);

    // A path event gets one arrow along its spline (the proxy's X is the tangent there); zones are footprints (height unused), so no vertical move.
    gizmo.setSpace(sel.pathNode ? 'local' : editSpace);
    gizmo.showX = true;
    gizmo.showY = !sel.pathNode && (sel.it.type !== 6 || editTool !== 'translate');
    gizmo.showZ = !sel.pathNode;
    gizmo.attach(editProxy);
  }
  else
    gizmo.detach();

  showCorners();
}

function select(marker)
{
  const o = marker && musicOuter(marker);

  // A selection (or clicking empty space) closes the right-click card.
  $('info').hidden = true;
  editHovered = null;
  sel = null;
  gizmo.detach();
  editProxy.removeFromParent();

  if (o)
  {
    const it = editItemOf(o);

    // NPCs on a spline start on it whatever their own position says, and path events fire at a fraction along their spline,
    // so both move with the spline instead.
    sel = { o, it, fixed: o.userData.path ? `. It starts on the spline ${it.model}; move that in Events mode` : null, pathNode: it.pathNodes?.[0] };

    if (sel.pathNode)
      sel.pathAt = sel.pathNode.s.events[sel.pathNode.k].at;

    o.parent.add(editProxy);
    sel.base = { scale: it.scale?.slice(), size: it.size?.slice(), mq: o.quaternion.clone(), ms: o.scale.clone(), pos0: it.pos.slice() };

    if (musicPartner(it))
      sel.base.gaps = musicGaps(it.musicRegion ?? it, it.musicTrigger ?? it);

    itemToProxy();
    sel.base.q = editProxy.quaternion.clone();
  }

  syncSelPanel();
  attachGizmo();
  boxAround(selBox, sel?.o);
  hoverBox.visible = false;
}

// A zone's gizmo sits at its footprint centre (pos + R * size / 2 over X/Z), so moving, rotating and scaling all pivot there.
const _xAxis = new THREE.Vector3(1, 0, 0);
const zoneHalf = (it) => new THREE.Vector3(it.size[0] / 2, 0, it.size[2] / 2).applyMatrix4(placementRotation(it.rot));

function itemToProxy()
{
  const { o, it, base } = sel;

  editProxy.position.set(...(o.userData.path ? pathStart(o.userData.path) : it.pos));

  if (it.type === 6)
  {
    editProxy.position.add(zoneHalf(it));
    editProxy.position.y = levelHeight(editProxy.position.x, editProxy.position.z);
  }

  editProxy.quaternion.setFromRotationMatrix(it.rot ? placementRotation(it.rot) : new THREE.Matrix4());

  if (sel.pathNode)
    editProxy.quaternion.setFromUnitVectors(_xAxis, sel.pathNode.curve.getTangentAt(sel.pathAt));

  const ratio = (now, was) => (now && was?.[0] ? now.map((v, a) => (was[a] ? v / was[a] : 1)) : [1, 1, 1]);

  editProxy.scale.set(...(it.size ? ratio(it.size, base.size) : ratio(it.scale, base.scale)));
}

// Path events fire at a fraction along their spline (arc length, as drawn): a drag slides the (first) node to the nearest point on it.
function slidePathEvent(at)
{
  const { o, pathNode: { curve } } = sel;
  const dragged = at == null;
  const before = sel.pathAt;

  if (dragged) {
    let bd = Infinity;

    for (let i = 0; i <= 500; i++)
    {
      const d = curve.getPointAt(i / 500).distanceToSquared(editProxy.position);

      if (d < bd)
        [bd, at] = [d, i / 500];
    }
  }

  sel.pathAt = at;

  const tangent = curve.getTangentAt(at);

  o.position.copy(curve.getPointAt(at));
  o.geometry.dispose();
  o.geometry = beadGeometry(tangent);

  // The order's other beads stay put and its links keep their far ends.
  for (const c of o.children)
  {
    if (c.userData.to)
      aimLink(c, o.position);
    else if (c.userData.at)
      c.position.fromArray(c.userData.at).sub(o.position);
  }

  if (dragged && at !== before)
    moveHandlesTo(o.position, tangent);

  boxAround(selBox, o);
  syncSelPanel();
}

// Puts the move handles back on the bead mid-drag, facing along the path, and carries on the drag from there.
function moveHandlesTo(point, tangent)
{
  editProxy.position.copy(point);
  editProxy.quaternion.setFromUnitVectors(_xAxis, tangent);
  editProxy.updateWorldMatrix(true, false);

  const world = editProxy.getWorldPosition(new THREE.Vector3());

  gizmo.pointStart.copy(gizmo.pointEnd).add(gizmo.worldPositionStart).sub(world);
  gizmo.worldPositionStart.copy(world);
  editProxy.getWorldQuaternion(gizmo.worldQuaternionStart);
  gizmo._positionStart.copy(point);
  gizmo._quaternionStart.copy(editProxy.quaternion);
}

// A roam area's ball or ring depends on where it sits against the ground.
function redrawRoam(o, it)
{
  if (markerKind(it) !== 'troam')
    return;

  const isArea = (c) => c.userData.triggerArea;
  const old = o.children.find(isArea);
  const area = makeMarker(it).children.find(isArea);

  if (!old || !area)
    return;

  old.geometry.dispose();
  old.removeFromParent();
  o.add(area);
}

function proxyToItem()
{
  if (sel.pathNode)
    return slidePathEvent();

  const { it, base } = sel;
  const p = editProxy;
  const d = p.position.toArray().map((v, a) => v - it.pos[a]);

  it.pos = p.position.toArray();
  redrawRoam(sel.o, it);

  if (it.rot && gizmo.mode === 'rotate')
  {
    const e = new THREE.Euler().setFromQuaternion(p.quaternion, 'ZYX');

    it.rot = [e.x, e.y, e.z].map((r) => THREE.MathUtils.radToDeg(r));
  }

  const k = p.scale.toArray();

  // An unscaled object (scale 0 = the game default, drawn at 0.25) keeps its 0 until it is actually scaled.
  if (base.scale && k.some((v) => v !== 1))
    it.scale = (base.scale[0] ? base.scale : [0.25, 0.25, 0.25]).map((v, a) => v * k[a]);

  if (base.size)
    it.size = base.size.map((v, a) => v * k[a]);

  if (it.type === 6) {
    const c = p.position.clone().sub(zoneHalf(it));

    it.pos = [c.x, base.pos0[1], c.z];
  }

  if (it.points)
    for (const pt of it.points)
      for (let a = 0; a < 3; a++)
        pt[a] += d[a];

  showItemChange();
  syncSelPanel();
}

function showItemChange()
{
  const { o, it, base } = sel;

  if (o.userData.compose)
    o.userData.compose();
  else if (it.type === 6)
    drapeZone(o, it);
  else
  {
    o.position.set(...it.pos);
    o.quaternion.copy(editProxy.quaternion).multiply(_eq.copy(base.q).invert()).multiply(base.mq);
    o.scale.copy(base.ms).multiply(editProxy.scale);
  }

  if (it.type === 3)
    root.traverse((h) => h.userData.path === it && h.userData.compose());

  const partner = musicPartner(it);
  const other = partner && markerOf(o, partner);

  // A resized region leaves its inner box where it is.
  if (other && it.size.every((v, a) => v === base.size[a]))
    drapeZone(other, { ...partner, pos: [partner.pos[0] + it.pos[0] - base.pos0[0], partner.pos[1], partner.pos[2] + it.pos[2] - base.pos0[2]] });

  boxAround(selBox, o);
}

// Redraws a zone's marker over the ground as item says it is now (it may be a preview of an item not yet changed).
function drapeZone(o, item)
{
  const fresh = makeMarker(item);

  o.geometry.dispose();
  o.geometry = fresh.geometry;
  o.children[0].geometry.dispose();
  o.children[0].geometry = fresh.children[0].geometry;
  o.position.copy(fresh.position);
}

// A music region and its trigger area act as one item: the game reads only their position and size, so neither rotates.
const isMusicZone = (it) => it.type === 6 && (it.sub === 0x10 || it.sub === 0x39);
const musicPartner = (it) => (it.type === 6 ? it.musicRegion ?? it.musicTrigger ?? null : null);
const markerOf = (o, item) => o.parent.children.find((m) => m.userData.item === item);

// The two boxes select as one object, the outer box standing for both.
function musicOuter(o)
{
  const region = editItemOf(o)?.musicRegion;

  return (region && markerOf(o, region)) || o;
}

function mapSize()
{
  const h = root?.getObjectByName('terrain')?.userData.tiles?.hmp;

  return h ? [h.w * 2, h.h * 2] : [Infinity, Infinity];
}

function musicFollow(it, partner, base)
{
  if (it.musicRegion)
    return void Object.assign(partner, musicRegionBox(partner, it, base.gaps));

  // A moved region carries its trigger area; a resized one leaves it there and can't shrink past it, as the part outside would do nothing.
  if (it.size.every((v, a) => v === base.size[a]))
    partner.pos = [partner.pos[0] + it.pos[0] - base.pos0[0], partner.pos[1], partner.pos[2] + it.pos[2] - base.pos0[2]];
  else
    Object.assign(it, musicRegionBox(it, partner, musicGaps(it, partner).map((g) => Math.max(g, 0))));
}

// target is what was edited: the selection, or a music region's inner box dragged by its own corners.
async function commitSel(target = sel)
{
  const { o, it } = target;
  const partner = musicPartner(it);

  if (partner)
    musicFollow(it, partner, target.base);

  redrawRoam(o, it);

  if (sel.pathNode)
  {
    // The node is { f32 fraction, u16, u16 } at spline + [+0x2C] + k * 8; the trigger's own position is not used by the path code.
    const { s, k, curve } = sel.pathNode;
    const at = sel.pathAt;

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

    if (it.rot)
      put(0x1C, it.rot);

    if (it.type === 0 && it.scale[0])
      put(0x28, it.scale);

    if (it.size)
      put(0x28, it.size);

    if (partner)
      [[0x10, partner.pos], [0x28, partner.size]].forEach(([off, v]) => v.forEach((x, a) => dv.setFloat32(partner.offset + off + a * 4, x)));
  });

  // Zones are draped on the terrain when built, so a changed zone is rebuilt rather than left transformed.
  if (it.type === 6) {
    const other = partner && markerOf(o, partner);

    if (other)
    {
      const rebuilt = makeMarker(partner);

      rebuilt.visible = other.visible;
      other.parent.add(rebuilt);
      other.removeFromParent();
    }

    const nm = makeMarker(it);

    nm.visible = o.visible;
    o.parent.add(nm);
    o.removeFromParent();
    select(nm);
  } else
    select(o);
}

// Mid-mission camera shot: the trigger's first firing in a mission plays one of the level's camera items (DAT 0/0x26) and its next
// cameras. The links live in the mod's camera_shots.txt (virtual asset mod/camera_shots.txt, exported raw at the mod root).
const SHOT_LINKS = 'mod/camera_shots.txt';

function shotLinkRow(it)
{
  const dir = selected;
  const lv = dir.split('/').pop();
  const row = elem('div', { className: 'shotRow' });
  const cams = (root?.getObjectByName('markers')?.children ?? []).map((m) => m.userData.item).filter((c) => c?.type === 0 && c.sub === 0x26 && c.name);

  const pick = selectOf([
    ['', 'None'],
    ...cams.map((c) => [c.name, c.name])
  ], null, { title: 'When this trigger first fires in a mission, the game cuts to this camera (and its next cameras) while the fight keeps running, then hands control back' });

  const preview = elem('button', { title: 'Preview the camera shot (opens it in the Scene panel)' });

  setEditIcon(preview, 'eye');

  const personal = elem('input', {
    type: 'checkbox',
    title: 'In online co-op, only the player who set the trigger off sees the shot'
  });

  const personalLabel = elem('label', { className: 'shotPersonal' }, personal, ' Personal');
  const text = () => virtualText(SHOT_LINKS);

  // A target flying a path exists only once the level spawns it; before that the game holds the camera's own direction (verified lv_3).
  const note = elem('small', { className: 'shotNote' });

  const warn = async () => {
    note.textContent = '';

    const cam = cams.find((c) => c.name === pick.value);

    if (!cam)
    {
      if (pick.value)
        note.textContent = `${pick.value} is not a camera in this level (deleted, or another level's): the game drops this shot. Pick a camera or None.`;

      return;
    }

    const dv = await rom.read(dir + '/dat');
    const { items } = parseDAT(dv);
    const tgt = camRecord(cam, dv).target;
    const t = items.find((i) => i.type === 0 && i.name === tgt);

    if (t?.model && items.some((s) => s.type === 3 && s.name === t.model))
      note.textContent = `Aims at ${tgt}, which flies a path: if the level has not spawned it when the shot plays, the camera keeps its own direction instead.`;
  };

  text().then((t) => {
    const link = shotLinkGet(t, lv, it.name);

    if (link && !cams.some((c) => c.name === link.camera))
    {
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

    if (cam)
      showCutscene(`seq:${dir}:${cam.index}`);
  };

  row.append('Play camera shot ', pick, preview, personalLabel, note);

  if (!cams.length)
    Object.assign(pick, { disabled: true, title: 'Add a camera to this level first' });

  return row;
}

// ---- Mod levels (mod/levels.json): new levels that play as a stock template level, with copies of its files as their own ----
const MOD_LEVELS = 'mod/levels.json';
let modLevels = [];

// Aliases every mod level's folder and intro/extro onto its template's files (and drops those of removed levels).
function applyModLevelsText(text)
{
  modLevels = modLevelsParse(text);

  const want = new Map(modLevels.flatMap((l) => modLevelAliases(l, [...rom.assets.keys()].filter((k) => !rom.aliases.has(k)))));

  for (const p of [...rom.aliases.keys()])
    if (want.get(p) !== rom.aliases.get(p))
      rom.unalias(p);

  for (const [p, from] of want)
    rom.alias(p, from);
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

const CRAFT = [
  ['xwing', 'X-wing'],
  ['ywing', 'Y-wing'],
  ['awing', 'A-wing'],
  ['vwing', 'V-wing'],
  ['speeder', 'Snowspeeder'],
  ['falcon', 'Millennium Falcon'],
  ['tie', 'TIE interceptor'],
  ['t16', 'T-16 Skyhopper'],
  ['naboo', 'Naboo starfighter']
];

// What a template brings that the new level cannot change (yet).
const TEMPLATE_NOTES = {
  0: 'opening crawl (chapter 1)',
  5: 'opening crawl (chapter 2)',
  10: 'opening crawl (chapter 3); no Y-wing or speeder',
  15: 'opening crawl (chapter 4)',
  16: 'no craft select (T-16), friendlies-saved medal',
  3: 'speeder only',
  6: 'speeder only',
  11: 'speeder only',
  18: 'speeder only',
  8: 'X-wing and Y-wing only',
  17: 'X-wing only'
};

// blank: only a player start on flat ground, with this level's look (sky, textures, music); give it objectives in the Objectives window.
async function newModLevel(fromDir, blank = false, named = null)
{
  const tmpl = parseInt(fromDir.split('/lv_')[1], 36);
  let title = named?.trim() ?? 'New level';

  for (let n = 2; !named && modLevels.some((l) => l.title === title); n++)
    title = `New level ${n}`;

  if (!title)
    return;

  let made;

  // A copy starts from the template's medal thresholds, a blank level from simple ones.
  const medals = blank ? undefined : medalInfo(await rom.read('data/medal_info'), tmpl);

  await historyGroup(`New level: ${title}`, async () => {
    await editModLevels((levels) => levels.push((made = modLevelNew(levels, title, tmpl, medals))), `New level: ${title}`);

    if (!blank)
      return;

    const dir = 'data/level/' + made.folder;

    await editAsset(dir + '/dat', (b) => datBlank(b), `Blank level: ${title}`);

    const water = datWaterGet(await rom.read(dir + '/dat'));

    await editAsset(dir + '/hmp', (b) => hmpFlat(b, water?.on ? water.level : null), `Flat ground: ${title}`);

    // Its intro and outro start blank (3 s on the template's opening camera), not the template's scenes; templates past 15 have none.
    for (const kind of ['intro', 'extro']) {
      const p = `data/cuts/${made.folder}_${kind}`;

      if (!rom.aliases.has(p))
        continue;

      await editAsset(p, (b) => {
        const f = parseCutscene(dvOf(b));

        cutBlank(f, 3);

        return writeCutscene(f);
      }, `Blank ${kind}: ${title}`);
    }
  });

  renderList();
  await showLevel('data/level/' + made.folder);

  // WebMCP names its levels and manages the view itself.
  if (!named)
    setEditMode('objects');

  return made;
}

// A voice-line picker: None, the level briefings, then every other line with audio (lines without a subtitle get the Audio tab's
// description, by speech index).
async function fillVoiceSelect(sel)
{
  sel.append(new Option('None', ''));

  const briefing = elem('optgroup');

  briefing.label = 'Level briefings';

  for (let v = 0x279; v <= 0x28B; v++)
    briefing.append(new Option(`${lvName('lv_' + (v - 0x279).toString(36))}: ${voiceWords(v) ?? ''}`.slice(0, 90), String(v)));

  const others = elem('optgroup');

  others.label = 'Other voice lines';

  const voiceMap = parseVoiceTable(romBytes, await rom.read('data/voice_TXT')).voiceMap;
  const cats = globalThis.RS64_VOICE_CATS ?? {};

  const unsubtitled = (v) => {
    const [cat, levels] = cats[voiceMap[v]] ?? ['', []];

    return levels.length ? `(${cat.split(': ').pop()}, ${levels.map(lvName).join(', ')})` : (cat ? `(${cat})` : '(no subtitle)');
  };

  // Ids mapped to speech 0 (a one-frame silence) have no audio.
  for (let v = 0; v < 0x2F4; v++)
    if ((v < 0x279 || v > 0x28B) && voiceMap[v] && voiceLine(v))
      others.append(new Option(`${v} ${voiceLine(v).trim() || unsubtitled(v)}`.slice(0, 90), String(v)));

  sel.append(briefing, others);
}

// Import / Play / Remove for a recording the mod ships at path: any audio the browser decodes (.wav, .mp3, .ogg), stored as an 8 kHz
// MORT stream the game streams. onChange(has) runs after each change and once at the start.
function recordingControls(path, what, onChange)
{
  const note = elem('small', { className: 'shotNote' });

  const imp = elem('button', {
    textContent: 'Import recording…',
    title: `A .wav or .mp3 played instead of the ${what}'s voice line`
  });

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

      if (!f)
        return;

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

    if (!m)
      return;

    const pcm = decodeMort(m, 0, m.length);

    audioCtx ??= new AudioContext();
    await audioCtx.resume();

    const b = audioCtx.createBuffer(1, pcm.length, 8000);

    b.getChannelData(0).set(Float32Array.from(pcm, (v) => v / 32768));

    const s = audioCtx.createBufferSource();

    s.buffer = b;
    s.connect(audioOut());
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
// A level's own SELECT LEVEL picture: 320 x 200 in the game's JPEG layout, and a 160 x 100 mask.
let levelImageShow = null;

// The picture is read whole into the menu's memory, so it stays near the stock files' size (4.5 to 9 KB).
function levelImageJpeg(cv)
{
  const rgba = cv.getContext('2d').getImageData(0, 0, 320, 200).data;
  let jpeg = null;

  for (const q of [90, 75, 60, 45, 30])
  {
    jpeg = encodeJpeg(rgba, 320, 200, q);

    if (jpeg.length <= 10240)
      break;
  }

  return jpeg;
}

// The hologram's outline. A stock mask is cut to the shape of its own picture, so a custom one gets plain soft edges.
const levelMaskAt = (x, y) => THREE.MathUtils.smoothstep(Math.min(x, 159 - x, y, 99 - y), 3, 20);

function levelImageMask()
{
  const rgba = new Uint8ClampedArray(160 * 100 * 4);

  for (let y = 0; y < 100; y++)
    for (let x = 0; x < 160; x++)
    {
      rgba.fill(255 * levelMaskAt(x, y), (y * 160 + x) * 4, (y * 160 + x) * 4 + 3);
      rgba[(y * 160 + x) * 4 + 3] = 255;
    }

  return encodeJpeg(rgba, 160, 100, 75);
}

// Framing a level picture: the panels step aside and a frame shows the crop and the hologram's soft edge. Editing is off meanwhile, so a click only takes the shot.
let levelCompose = null;

function composeFit()
{
  const r = canvas.getBoundingClientRect();
  const p = $('composeFrame').parentElement.getBoundingClientRect();
  const s = Math.min(r.width / 320, r.height / 200);

  Object.assign($('composeFrame').style, {
    left: `${r.left - p.left + (r.width - 320 * s) / 2}px`,
    top: `${r.top - p.top + (r.height - 200 * s) / 2}px`,
    width: `${320 * s}px`,
    height: `${200 * s}px`
  });
}

function composeLevelImage(take)
{
  const mode = editMode;
  const back = sel?.o;
  const markers = root?.getObjectByName('markers');
  const shown = markers?.visible;
  const fade = new ImageData(160, 100);
  const fit = new ResizeObserver(composeFit);

  for (let y = 0; y < 100; y++)
    for (let x = 0; x < 160; x++)
      fade.data[(y * 160 + x) * 4 + 3] = 255 * (1 - levelMaskAt(x, y));

  $('composeFade').getContext('2d').putImageData(fade, 0, 0);
  select(null);
  setEditMode(null);

  if (markers)
    markers.visible = false;

  const end = () => {
    levelCompose = null;
    fit.disconnect();
    document.removeEventListener('keydown', key, true);
    document.body.classList.remove('composing');
    $('composeFrame').hidden = $('composeHint').hidden = true;

    if (markers)
      markers.visible = shown;

    setEditMode(mode);

    if (back)
      select(back);
  };

  const shoot = () => {
    const cv = viewAsLevelImage();

    end();
    take(cv);
  };

  // The flying keys still reach the view; undo and mode changes wait.
  const key = (e) => {
    if (e.code === 'Enter' || e.code === 'NumpadEnter')
      shoot();
    else if (e.code === 'Escape')
      end();
    else if (!e.ctrlKey && !e.metaKey && e.code !== 'Tab')
      return;

    e.preventDefault();
    e.stopPropagation();
  };

  levelCompose = { shoot };
  document.addEventListener('keydown', key, true);
  document.body.classList.add('composing');
  document.activeElement?.blur();
  $('composeFrame').hidden = $('composeHint').hidden = false;
  fit.observe(canvas);
  composeFit();
}

function levelImageCover(src, w, h)
{
  const cv = new OffscreenCanvas(320, 200);
  const k = Math.max(320 / w, 200 / h);

  cv.getContext('2d').drawImage(src, (320 - w * k) / 2, (200 - h * k) / 2, w * k, h * k);

  return cv;
}

// The view without markers, handles or selection boxes. WebGPU keeps the canvas image only until the frame is handed off, so it is drawn and copied in one go.
function viewAsLevelImage()
{
  const hide = [root?.getObjectByName('markers'), gizmo.getHelper(), selBox, hoverBox].filter((o) => o?.visible);

  hide.forEach((o) => (o.visible = false));
  renderer.render(scene, camera);

  const cv = levelImageCover(canvas, canvas.width, canvas.height);

  hide.forEach((o) => (o.visible = true));

  return cv;
}

function levelImageRow(l)
{
  const path = `mod/assets/level/${l.folder}/preview`;
  const stock = `data/backdrop/level${l.template}`;
  const img = elem('img', { className: 'levelImage', alt: 'The picture SELECT LEVEL shows' });

  const reset = iconButton('close', 'Back to the picture of the level it was copied from', async () => {
    await historyGroup(`${l.title}: copied picture`, async () => {
      await editAsset(path, () => new Uint8Array(0), 'Picture');
      await editAsset(path + 'alpha', () => new Uint8Array(0), 'Picture');
    });

    levelImageShow();
  });

  levelImageShow = async () => {
    const own = await hasBytes(path);
    const old = img.src;

    img.src = URL.createObjectURL(new Blob([u8(await rom.read(own ? path : stock))], { type: 'image/jpeg' }));
    URL.revokeObjectURL(old);
    reset.hidden = !own;
  };

  const save = async (cv, label) => {
    const jpeg = levelImageJpeg(cv);

    await historyGroup(`${l.title}: ${label}`, async () => {
      await editAsset(path, () => jpeg, 'Picture');
      await editAsset(path + 'alpha', levelImageMask, 'Picture');
    });

    levelImageShow();
  };

  const view = iconButton('camAdd', 'Frame the picture in the 3D view', () => composeLevelImage((cv) => save(cv, 'picture from the view')));

  const file = iconButton('open', 'Choose an image: it is cropped to fill the picture', async () => {
    const f = await pickImage();

    if (!f)
      return;

    const bmp = await createImageBitmap(f);

    await save(levelImageCover(bmp, bmp.width, bmp.height), 'picture from a file');
  });

  levelImageShow();

  return elem('div', { className: 'levelImageRow' }, img, view, file, reset);
}

async function modLevelDialog(dir)
{
  const l = modLevelOf(dir);

  if (!l)
    return void ($('levelPanel').hidden = true);

  const d = elem('div', { className: 'levelDialog' });
  const title = elem('input', { value: l.title, maxLength: 40 });
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

  const useSub = elem('button', {
    textContent: 'Copy voiceline subtitles to Description',
    title: 'Stock descriptions are the exact words of their briefing line'
  });

  // Only offered when the chosen line has words to copy.
  const syncUseSub = () => (useSub.hidden = voice.disabled || !voice.value || !(voiceWords(+voice.value) ?? '').trim());

  voice.onchange = syncUseSub;
  syncUseSub();

  useSub.onclick = (e) => {
    e.preventDefault();

    if (voice.value)
      desc.value = (voiceWords(+voice.value) ?? '').trim();

    checkFont();
    d.dispatchEvent(new Event('change'));
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

  const craftBox = elem('div', {
    className: 'craftGrid',
    title: 'Craft allowed on this level'
  }, ...crafts.map(([, , lab]) => lab));
  const tiers = ['bronze', 'silver', 'gold'];
  const cols = [['time', 'Time (min)', 0.5], ['kills', 'Kills', 1], ['accuracy', 'Accuracy %', 1], ['friendlies', 'Friendlies saved', 1]];

  const medals = elem('table', {
    className: 'medalGrid',
    title: 'A medal needs all of: time at most, the rest at least. Empty keeps the template\'s thresholds'
  });

  medals.innerHTML = `<tr><th></th>${cols.map(([, h]) => `<th>${h}</th>`).join('')}</tr>`;

  const cells = tiers.map((t) => {
    const ins = cols.map(([k, , step]) => [k, elem('input', { type: 'number', min: 0, step, value: l.medals?.[t]?.[k] ?? '' })]);

    medals.append(elem('tr', null, elem('th', { textContent: t[0].toUpperCase() + t.slice(1) }), ...ins.map(([, i]) => elem('td', null, i))));

    return [t, ins];
  });

  // Opening crawl: the template's (0, 5, 10 and 15 have one), none, or its own, which the Cutscene list's Opening crawl edits.
  // Starting song: any song of the in-mission cue table (the template's by default).
  const cueTable = parseMusicCues(romBytes);
  const tmplSong = cueTable.cues.find((c) => c.song === cueTable.levelSongs[l.template]);

  const music = selectOf([
    ['', `The template's (${tmplSong?.name ?? '?'})`],
    ...songChoices()
  ], l.music ?? '', { title: 'Plays from the start of the mission' });

  const tmplCrawl = [0, 5, 10, 15].includes(l.template);
  const ownCrawl = l.crawl && l.crawl !== false;

  const crawlMode = selectOf([...(tmplCrawl ? [['tmpl', "The template's"]] : []), ['none', 'None'], ...(ownCrawl ? [['own', 'Its own']] : [])],
    ownCrawl ? 'own' : (l.crawl === false || !tmplCrawl ? 'none' : 'tmpl'), { title: 'Edit the crawl from the Cutscene list (Opening crawl): the first edit gives the level its own' });

  const crawlRow = fld(elem('b', { textContent: 'Opening crawl ' }), crawlMode);

  const del = elem('button', { title: 'Removes the level from the mod (its edited files stay in the project history)' });

  setEditIcon(del, 'delete');
  del.append('Delete level');
  del.removeAttribute('aria-label');

  const close = () => ($('levelPanel').hidden = true);

  del.onclick = async () => {
    if (!confirm(`Delete ${l.title}?`))
      return;

    close();
    await editModLevels((levels) => levels.splice(levels.findIndex((x) => x.folder === l.folder), 1), `Delete level: ${l.title}`);
    renderList();
    await showLevel(stockDir('data/level/lv_' + l.template.toString(36)));
  };

  const saveSettings = async () => {
    const m =Object.fromEntries(cells.map(([t, ins]) => [t, Object.fromEntries(ins.filter(([, i]) => i.value !== '').map(([k, i]) => [k, +i.value]))]));
    const hasMedals = tiers.every((t) => Object.keys(m[t]).length);
    const renamed = (title.value.trim() || l.title) !== modLevelOf(dir)?.title;

    await editModLevels((levels) => {
      const x = levels.find((y) => y.folder === l.folder);

      x.title = title.value.trim() || x.title;
      x.description = desc.value.trim();

      if (voice.value)
        x.voice = +voice.value;
      else
        delete x.voice;

      const craft = crafts.filter(([, c]) => c.checked).map(([k]) => k);

      if (craft.length)
        x.craft = craft;
      else
        delete x.craft;

      if (hasMedals)
        x.medals = m;
      else
        delete x.medals;

      if (crawlMode.value === 'none' && tmplCrawl)
        x.crawl = false;
      else if (crawlMode.value !== 'own')
        delete x.crawl;

      if (music.value)
        x.music = +music.value;
      else
        delete x.music;
    }, `Level settings: ${title.value.trim()}`);

    levelPanelKey = levelSettingsKey(modLevelOf(dir));

    if (renamed)
      renderList();
  };

  // Every field saves when it changes, one save at a time.
  d.addEventListener('change', () => (levelSaving = levelSaving.then(saveSettings)));

  const tmplNote = TEMPLATE_NOTES[l.template];
  const hr = () => elem('hr');

  d.append(...(tmplNote ? [elem('small', { className: 'shotNote', textContent: 'From its template it keeps: ' + tmplNote })] : []),
    stacked(fld('Title', title)), fontNote, stacked(fld('Description', desc, 'The SELECT LEVEL briefing text')), fld('Briefing voice ', voice, 'Played when the level is highlighted in SELECT LEVEL'), useSub, recRow, recNote, hr(),
    elem('b', { textContent: 'Picture' }), levelImageRow(l), hr(),
    elem('b', { textContent: 'Approved craft' }), craftBox, hr(),
    elem('b', { textContent: 'Medals' }), medals, hr(),
    crawlRow,
    hr(), fld(elem('b', { textContent: 'Starting song ' }), music),
    hr(), elem('div', { className: 'actions' }));

  d.lastChild.append(del);
  closeObjectives();
  levelPanelDir = dir;
  levelPanelKey = levelSettingsKey(l);
  $('goalPanel').hidden = true;
  $('levelBody').replaceChildren(d);
  $('levelPanel').hidden = false;
}

const levelSettingsKey = (l) => JSON.stringify([l?.title, l?.description, l?.voice, l?.craft, l?.medals, l?.crawl, l?.music]);

// An undo or another tool changed the settings: rebuild, so a field can't save a stale value back.
function syncLevelSettings()
{
  if (!$('levelPanel').hidden)
    levelImageShow?.();

  if (!$('levelPanel').hidden && levelPanelDir === selected && levelSettingsKey(modLevelOf(selected)) !== levelPanelKey)
    modLevelDialog(selected);
}

setEditIcon($('levelClose'), 'close');
$('levelClose').onclick = () => ($('levelPanel').hidden = true);

// ---- Objectives of a mod level: a window beside the level that saves as things change; units are picked by clicking them in the view ----

const OBJ_GOALS = [
  ['destroy', 'Destroy units', 'Destroy the enemies'],
  ['reach', 'Reach an area', 'Reach the area'],
  ['protect', 'Protect a unit', 'Protect the unit'],
  ['survive', 'Survive for', 'Survive'],
  ['timelimit', 'Finish within', 'Finish in time']
];

const objDefaultText = (goal) => OBJ_GOALS.find(([k]) => k === goal)[2];
const objPicksUnits = (o) => o.goal === 'destroy' || o.goal === 'protect';
const objTargeted = (o) => objPicksUnits(o) || o.goal === 'reach';
const objWins = (o) => ['destroy', 'reach', 'survive'].includes(o.goal);
const objDone = (o) => o.text && (!objTargeted(o) || o.targets.length);
const objItemName = (it) => it.name || `${it.label ?? 'item'} ${it.index}`;

const objKeys = (rows) => JSON.stringify(rows.map((o) => [
  o.text,
  o.goal,
  objTargeted(o) ? [...o.targets].sort((a, b) => a - b) : o.seconds,
  o.voice,
  o.camera,
  o.recording,
  o.pauseVoice,
  o.pauseRecording
]));

// The level's saved objectives with their targets as DAT item indices. The kill count is left out so it is recounted from the targets.
async function readObjectives(dir)
{
  const dv = await rom.read(dir + '/dat');

  objItems = parseDAT(dv).items;

  return (modLevelOf(dir)?.objectives ?? []).map((o) => ({
    ...o,
    count: undefined,
    targets: objectiveTargets(dv, o).map((it) => objItems.findIndex((x) => x.offset === it.offset))
  }));
}

function syncObjNote()
{
  const done = objRows.filter(objDone);
  const bad = objRows.find((o) => !objDone(o) && o !== objPick?.row);
  let note = '';

  if (objPick?.row.goal === 'reach')
    note = 'Click a trigger area in the view, or make one with the add button. Esc cancels.';
  else if (objPick)
    note = 'Click units in the view to add or remove them. Esc when done.';
  else if (bad)
    note = `"${bad.text || 'An objective'}" isn't saved until it has ${bad.text ? 'a target' : 'text'}.`;
  else if (done.length && !done.some(objWins))
    note = 'Add a destroy, reach or survive objective too, or the mission cannot be won.';
  else if (!done.length)
    note = "Without objectives the level keeps its template's mission script.";

  $('objNote').textContent = note;
}

async function writeObjectives(dir)
{
  const l = modLevelOf(dir);
  const ready = objRows.filter(objDone);

  if (!l || dir !== objDir || (ready.length && !ready.some(objWins)))
    return;

  if (objKeys(await readObjectives(dir)) === objKeys(ready))
    return;

  let objectives = [];

  await historyGroup(`Objectives: ${l.title}`, async () => {
    await editAsset(dir + '/dat', (b) => {
      const r = objectivesApply(b, l.objectives, ready);

      objectives = r.objectives;
      b.set(r.bytes);
    });

    await editModLevels((levels) => {
      const x = levels.find((y) => y.folder === l.folder);

      if (objectives.length)
        x.objectives = objectives;
      else
        delete x.objectives;
    }, `Objectives: ${l.title}`);
  });

  rulesChanged();
}

// Saves run one at a time, each from the rows as they are by then.
function saveObjectives()
{
  const dir = objDir;

  syncObjNote();
  objSaving = objSaving.then(() => writeObjectives(dir)).catch((e) => ($('objNote').textContent = e.message));
}

// Gold boxes around the units the row being picked already has.
function showObjPicks()
{
  const picks = objPick ? editObjects().filter(([, x]) => objPick.row.targets.includes(objItems.findIndex((y) => y.offset === x.offset))) : [];

  picks.forEach(([o], k) => {
    if (!objBoxes[k])
    {
      objBoxes[k] = new THREE.BoxHelper(new THREE.Object3D(), 0xe8b84a);
      scene.add(objBoxes[k]);
    }

    boxAround(objBoxes[k], o);
  });

  for (const b of objBoxes.slice(picks.length))
    b.visible = false;
}

function objSetPick(p)
{
  const old = objPick;

  objPick = p;
  old?.draw();

  if (p) {
    stopPlacing();
    select(null);
    p.draw();
  }

  showObjPicks();
  syncObjNote();
}

function objToggleUnit(hit)
{
  const it = hit && editItemOf(hit);

  if (!it || !isUnit(it))
    return;

  const { row, draw } = objPick;
  const i = objItems.findIndex((x) => x.offset === it.offset);

  if (row.goal === 'protect')
    row.targets = row.targets[0] === i ? [] : [i];
  else
    row.targets = row.targets.includes(i) ? row.targets.filter((x) => x !== i) : [...row.targets, i];

  draw();
  showObjPicks();
  saveObjectives();
}

function objPickArea(e)
{
  let hit = pick(e, visibleMarkers())?.object;

  while (hit && !hit.userData.item)
    hit = hit.parent;

  const it = hit?.userData.item;

  if (it?.type !== 7 || !it.onEnter || it.pathEvent)
    return;

  const p = objPick;

  p.row.targets = [objItems.findIndex((x) => x.offset === it.offset)];
  objSetPick(null);
  p.refresh();
  saveObjectives();
}

function objectiveRow(o, pickNow = false)
{
  const l = modLevelOf(objDir);
  const row = elem('div', { className: 'objRow' });
  const text = elem('input', { value: o.text, placeholder: 'Shown in the pause menu', maxLength: 40 });
  const goal = selectOf(OBJ_GOALS, o.goal);
  const chips = elem('div', { className: 'objChips' });
  const pickZone = elem('select', { title: 'The area the player has to reach' });
  const camera = elem('select', { title: 'The game cuts to this camera position when the objective completes, or fails for protect and time limit' });
  const minutes = elem('input', { type: 'number', min: 0.25, step: 0.25, value: o.seconds ? o.seconds / 60 : 5, title: 'Minutes' });
  const minutesRow = fld('Minutes ', minutes);

  const fillZones = () => {
    const zones = objItems.map((it, i) => [i, objItemName(it), it]).filter(([, , it]) => it.type === 7 && it.onEnter);

    pickZone.replaceChildren(...selectOf([['', 'Pick an area…'], ...zones]).options);
    pickZone.value = o.goal === 'reach' ? String(o.targets[0] ?? '') : '';
  };

  const fillCameras = () => {
    const cams = objItems.filter((it) => it.type === 0 && it.sub === 0x26 && it.name).map((it) => [it.name, it.name]);

    camera.replaceChildren(...selectOf([['', 'None'], ...cams]).options);
    camera.value = cams.some(([c]) => c === o.camera) ? o.camera : '';
  };

  const pickBtn = iconButton('place', 'Pick in the view: click units to add or remove them', () => (objPick?.row === o ? objSetPick(null) : startPick()));
  const areaBtn = iconButton('place', 'Pick in the view: click the trigger area', () => (objPick?.row === o ? objSetPick(null) : startPick()));

  const draw = () => {
    const picked = objPicksUnits(o) ? o.targets.filter((i) => objItems[i]) : [];

    chips.replaceChildren(...picked.map((i) => {
      const chip = elem('button', { className: 'objChip', textContent: objItemName(objItems[i]), title: 'Remove from this objective' });

      chip.onclick = () => {
        o.targets = o.targets.filter((x) => x !== i);
        draw();
        showObjPicks();
        saveObjectives();
      };

      return chip;
    }));

    pickBtn.classList.toggle('on', objPick?.row === o);
    areaBtn.classList.toggle('on', objPick?.row === o);
  };

  const startPick = () => {
    if (objPicksUnits(o) && editMode !== 'objects')
      setEditMode('objects');

    objSetPick({ row: o, draw, refresh: () => row.refresh() });
  };

  // A Reach an area goal can make its own area where the view looks, set off by the player. It is the DAT's last item, so other picks keep their indices.
  const newArea = iconButton('add', 'New goal area: added where you are looking, set off by the player', async () => {
    objSetPick(null);
    await fillAddKinds();

    const src = addTemplates.get('tevent');
    const r = canvas.getBoundingClientRect();
    const at = cursorGround({ clientX: r.left + r.width / 2, clientY: r.top + r.height / 2 });

    if (!src || !at)
      return;

    await addItem(src, at.toArray());
    objItems = parseDAT(await rom.read(objDir + '/dat')).items;
    o.targets = [objItems.length - 1];
    fillZones();
    saveObjectives();
  });

  const unitBox = elem('div', { className: 'objTargets' }, pickBtn, chips);
  const zoneBox = elem('div', { className: 'objTargets' }, areaBtn, pickZone, newArea);

  const syncGoal = () => {
    unitBox.hidden = !objPicksUnits(o);
    zoneBox.hidden = o.goal !== 'reach';
    minutesRow.hidden = o.goal !== 'survive' && o.goal !== 'timelimit';
  };

  // A voice line, or the mod's own recording under a name of its own so removing an objective leaves the others' files alone.
  const speech = (voiceKey, recKey, prefix, what, tip, label) => {
    const voice = elem('select', { title: tip });
    const name = o[recKey] ?? `${prefix}_${Math.random().toString(36).slice(2, 8)}`;

    fillVoiceSelect(voice).then(() => (voice.value = o[voiceKey] != null ? String(o[voiceKey]) : ''));

    voice.onchange = () => {
      o[voiceKey] = voice.value ? +voice.value : undefined;
      saveObjectives();
    };

    const rec = recordingControls(`mod/assets/level/${l.folder}/${name}`, what, (has) => {
      voice.disabled = has;

      if (has === !!o[recKey])
        return;

      o[recKey] = has ? name : undefined;
      o[voiceKey] = has || !voice.value ? undefined : +voice.value;
      saveObjectives();
    });

    return [fld(label, voice), rec.row, rec.note];
  };

  const remove = iconButton('delete', 'Remove objective', () => {
    if (objPick?.row === o)
      objSetPick(null);

    objRows.splice(objRows.indexOf(o), 1);
    row.remove();
    $('objAdd').hidden = false;
    saveObjectives();
  });

  text.onchange = () => {
    o.text = text.value.trim();
    saveObjectives();
  };

  // The line follows the goal until the modder writes their own.
  goal.onchange = () => {
    if (text.value === objDefaultText(o.goal))
      text.value = o.text = objDefaultText(goal.value);

    o.goal = goal.value;
    o.targets = [];
    o.seconds = +minutes.value * 60;
    syncGoal();
    fillZones();
    draw();

    if (objTargeted(o))
      startPick();
    else
      objSetPick(null);

    saveObjectives();
  };

  pickZone.onchange = () => {
    o.targets = pickZone.value === '' ? [] : [+pickZone.value];
    saveObjectives();
  };

  minutes.onchange = () => {
    o.seconds = +minutes.value * 60;
    saveObjectives();
  };

  camera.onchange = () => {
    o.camera = camera.value || undefined;
    saveObjectives();
  };

  row.refresh = () => {
    fillZones();
    fillCameras();
    draw();
  };

  const callout = elem('details', { className: 'objCallout' }, elem('summary', { textContent: 'Voice and camera' }),
    ...speech('voice', 'recording', 'obj', 'objective', 'Played (with its subtitle) when the objective completes, or fails for protect and time limit', 'When done '),
    fld('Camera ', camera),
    ...speech('pauseVoice', 'pauseRecording', 'pause', 'pause screen line', 'Read aloud when the pause screen comes to this objective', 'Pause screen '));

  row.append(text, goal, remove, unitBox, zoneBox, minutesRow, callout);
  row.refresh();
  syncGoal();

  if (pickNow)
    startPick();

  return row;
}

// Follows the level after a reload or undo. Unfinished rows have no saved form, so they carry over without targets, whose indices may have moved.
async function syncObjectives(force = false)
{
  const dir = selected;

  if (!force && (objDir !== dir || $('objPanel').hidden))
    return;

  const saved = await readObjectives(dir);
  const done = objRows.filter(objDone);

  if (force || objKeys(saved) !== objKeys(done))
  {
    const unfinished = objRows.filter((o) => !objDone(o) || !done.some(objWins)).map((o) => ({ ...o, targets: [] }));

    objSetPick(null);
    objRows = [...saved, ...unfinished];
    $('objList').replaceChildren(...objRows.map((o) => objectiveRow(o)));
    $('objAdd').hidden = objRows.length >= 4;
    syncObjNote();

    return;
  }

  for (const row of $('objList').children)
    row.refresh();

  showObjPicks();
}

async function openObjectives(dir = selected)
{
  if (objDir !== dir)
    objRows = [];

  objDir = dir;
  $('levelPanel').hidden = true;
  $('goalPanel').hidden = true;
  $('objPanel').hidden = false;
  await syncObjectives(true);
}

function closeObjectives()
{
  $('objPanel').hidden = true;
  objSetPick(null);
}

$('objAdd').onclick = () => {
  const o = { goal: 'destroy', text: objDefaultText('destroy'), seconds: 300, targets: [] };

  objRows.push(o);
  $('objList').append(objectiveRow(o, true));
  $('objAdd').hidden = objRows.length >= 4;
};

setEditIcon($('btnObjectives'), 'goals');
$('btnObjectives').append('Objectives…');
$('btnObjectives').removeAttribute('aria-label');
$('btnObjectives').onclick = () => ($('objPanel').hidden || objDir !== selected ? openObjectives() : closeObjectives());
setEditIcon($('objClose'), 'close');
$('objClose').onclick = closeObjectives;

// The marker legend stops above the scene panel, whatever height its buttons give it.
new ResizeObserver(() => document.documentElement.style.setProperty('--scene-h', $('scene').offsetHeight + 'px')).observe($('scene'));

function fillLevelButton()
{
  const b = $('btnLevelSettings');
  const dir = selectedLevel && !String(selected).startsWith('cut:') ? selected : null;

  $('btnBlankLevel').hidden = true;
  b.hidden = !dir || !(modLevelOf(dir) || /\/lv_[0-9a-i]$/.test(dir));

  if (b.hidden)
    return;

  const keeps = TEMPLATE_NOTES[parseInt(dir.split('/lv_')[1], 36)];

  if (modLevelOf(dir)) {
    b.textContent = 'Level settings…';
    b.title = '';
  } else
  {
    setEditIcon(b, 'duplicate');
    b.append('New level from this one');
    b.removeAttribute('aria-label');
    b.title = `A full copy that plays this level's mission script, so keep the objects it uses${keeps ? `. It keeps: ${keeps}` : ''}`;
  }

  const levelOpen = () => !$('levelPanel').hidden && levelPanelDir === dir;

  b.onclick = () => (modLevelOf(dir) ? (levelOpen() ? ($('levelPanel').hidden = true) : modLevelDialog(dir)) : newModLevel(dir));

  const blank = $('btnBlankLevel');

  blank.hidden = !!modLevelOf(dir);

  if (!blank.firstChild?.tagName) {
    setEditIcon(blank, 'blank');
    blank.append('New blank level');
    blank.removeAttribute('aria-label');
  }

  blank.onclick = () => newModLevel(dir, true);
}

// The selected item again after its level reloads (an edit can move or grow its record).
async function reselect(type, index)
{
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

    if (x)
      out.push([o, x]);
  });

  return out;
}

// Objects a trigger can destroy (camera_shots.txt destroy lines): named units and props; a far one may not be loaded when it fires.
const DESTROY_SKIP = new Set([0x01, 0x02, 0x26, 0x27]);
const DESTROY_FAR = 40;

function destroyRow(it)
{
  const dir = selected;
  const lv = dir.split('/').pop();
  const row = elem('div', { className: 'shotRow' });
  const dist = (x) => Math.hypot(x.pos[0] - it.pos[0], x.pos[2] - it.pos[2]);

  Promise.all([rom.read(dir + '/dat'), virtualText(SHOT_LINKS)]).then(([dv, text]) => {
    const objs = parseDAT(dv).items.filter((x) => x.type === 0 && x.name && !DESTROY_SKIP.has(x.sub)).sort((a, b) => dist(a) - dist(b));
    const names = shotDestroyGet(text, lv, it.name);
    const save = (next, label) => virtualText(SHOT_LINKS).then((t) => editText(SHOT_LINKS, shotDestroySet(t, lv, it.name, next), label)).then(syncSelPanel);

    row.append('Destroy ');

    for (const n of names)
    {
      const o = objs.find((x) => x.name === n);
      const chip = elem('span', { className: 'destroyChip' });
      const del = elem('button', { textContent: '×', title: `Stop destroying ${n}` });

      del.onclick = () => save(names.filter((x) => x !== n), `Keep ${n}: ${it.name}`);
      chip.append(n, del);
      chip.title = !o ? `${n} is not in this level: the game skips it` : (dist(o) > DESTROY_FAR ? `${n} is ${dist(o).toFixed(0)} units from the trigger: if it is not loaded when the trigger fires it only loses its health (rubble, no explosion)` : `Destroyed with its own explosion when the trigger fires; the camera shot starts half a second later`);

      if (!o || dist(o) > DESTROY_FAR)
        chip.classList.add('warn');

      row.append(chip);
    }

    const choices = objs.filter((x) => !names.includes(x.name)).map((x) => [x.name, `${x.name} (${SUBTYPES[x.sub] ?? 'object'}, ${dist(x).toFixed(0)} away)`]);

    const add = selectOf([
      ['', names.length ? 'Add…' : 'Nothing'],
      ...choices
    ], null, { title: 'When this trigger first fires, destroy an object: it dies with its own explosion and counts for objectives like any kill' });

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

function wavePicker(waves, flag, title)
{
  const stock = flag && !Object.values(waves).includes(flag) ? [[flag, `Stock flag ${flag}`]] : [];

  return selectOf([[0, 'None'], ...Object.entries(waves).map(([n, f]) => [f, n]), ...stock, ['new', 'New wave…']], flag, { title });
}

// The picked wave's flag (0 = none); a new wave asks for its name and is stored. null when cancelled.
async function chooseWave(pick, dir, waves)
{
  if (pick.value !== 'new')
    return +pick.value;

  const name = prompt('Wave name', `Wave ${Object.keys(waves).length + 1}`)?.trim();

  if (!name)
    return null;

  const flag = waveFlagFor(waves, name, levelUsedFlags(await rom.read(dir + '/dat')));

  if (!flag) {
    alert('No free mission flag left in this level');

    return null;
  }

  if (!waves[name])
    await addWave(dir, waves, name, flag);

  return flag;
}

// Trigger side: entering the area raises the wave's flag (its enter block, +0x38 for 0x18 / +0x48 for 0x19, sets flag = 1).
function waveTriggerRow(it) {
  const dir = selected;
  const row = elem('div', { className: 'shotRow' });
  const base = it.sub === 0x18 ? 0x38 : 0x48;

  readWaves(dir).then((waves) => {
    const stock = triggerWave(it.onEnter, waves) === 'stock';
    const pick = wavePicker(waves, it.onEnter.flag, 'Start a wave: entering this area brings in every enemy waiting for it');

    pick.onchange = () => historyGroup(`Start wave: ${it.name}`, async () => {
      if (stock && !confirm(`${it.name} already ${it.onEnter.value ? 'sets' : 'clears'} mission flag ${it.onEnter.flag}, which the level's script may need. Replace that with the wave?`))
        return syncSelPanel();

      const flag = await chooseWave(pick, dir, waves);

      if (flag === null)
        return syncSelPanel();

      const old = it.onEnter.flag;

      await editAsset(dir + '/dat', (bytes) => {
        const dv = dvOf(bytes);
        const o = it.offset + base;

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

// A stock hold order with no flags that lasts 9999 seconds, the stop at the end of a path for UNITS_OWN_PATH.
let holdTemplate = null;

async function getHoldTemplate()
{
  for (const p of [...rom.assets.keys()].filter((p) => /^data\/level\/lv_.\/dat$/.test(p)).sort())
  {
    if (holdTemplate)
      break;

    const dv = await rom.read(p);
    const t = parseDAT(dv).items.find((x) => x.type === 7 && x.sub === 0x22 && x.pathEvent?.a === 9999 && !x.pathEvent.flag && !x.pathEvent.setFlag);

    if (t)
      holdTemplate = new Uint8Array(dv.buffer, dv.byteOffset + t.offset, dv.getUint32(t.offset + 8)).slice();
  }

  return holdTemplate;
}

// A stock fly-to-the-player event record (path event 0x4E, target "+"), the template the attack switch copies.
let attackTemplate = null;

async function getAttackTemplate()
{
  for (const p of [...rom.assets.keys()].filter((p) => /^data\/level\/lv_.\/dat$/.test(p)).sort())
  {
    if (attackTemplate)
      break;

    const dv = await rom.read(p);
    const t = parseDAT(dv).items.find((x) => x.type === 7 && x.sub === 0x4E && !x.pathEvent?.target);

    if (t)
      attackTemplate = new Uint8Array(dv.buffer, dv.byteOffset + t.offset, dv.getUint32(t.offset + 8)).slice();
  }

  return attackTemplate;
}

function pathAttackRow(it) {
  const label = elem('label', {
    className: 'shotPersonal',
    title: 'At the end of this path, NPCs flying it turn on the player (a fly-to event with no target at the very end)'
  });

  const box = elem('input', { type: 'checkbox', disabled: true });

  label.append(box, ' At the end: attack the player');

  rom.read(selected + '/dat').then((dv) => {
    box.checked = splineAttackEnd(u8(dv), it);
    box.disabled = false;
  });

  box.onchange = async () => {
    if (!box.checked && !confirm('A craft that reaches the end of a path that leads nowhere crashes the game. Untick only if this path continues into another one or the craft is sent off it before the end. Remove the attack?'))
    {
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

async function getPathCatalog()
{
  if (pathCatalog)
    return pathCatalog;

  pathCatalog = [];

  for (const p of [...rom.assets.keys()].filter((p) => /^data\/level\/lv_.\/dat$/.test(p)).sort())
  {
    const dv = await rom.read(p);
    const items = parseDAT(dv).items;
    const kinds = new Map();

    for (const e of items.filter((x) => x.type === 0 && WAVE_FIELDS[x.sub]))
    {
      const path = waveGet(dv, e).path;

      if (!kinds.has(path))
        kinds.set(path, new Set());

      kinds.get(path).add(SUBTYPES[e.sub]);
    }

    for (const s of items.filter((x) => isFlightPath(x) && x.name && kinds.has(x.name)))
      pathCatalog.push({ dir: p.slice(0, -4), name: s.name, index: s.index, kinds: [...kinds.get(s.name)].join(', ') });
  }

  return pathCatalog;
}

// Replaces a path's points (any count from 4; the craft starts at the second and stops at the second-last).
const setPathPoints = (it, points, label) => editItem(it, (bytes) => splineSetPoints(bytes, it, points), `${label}: ${it.name || 'path'}`);

// Adds a path built from stock path src (build = splineCopy or splineNew, starting it at the enemy) and puts the enemy on it.
async function addPathFor(it, src, build, name, label) {
  let dropped = 0;

  await historyGroup(label, async () => {
    const from = u8(await rom.read(src.dir + '/dat')).slice();
    const attack = UNITS_OWN_PATH.has(it.sub) ? await getHoldTemplate() : await getAttackTemplate();
    const s = parseDAT(new DataView(from.buffer)).items.find((x) => x.type === 3 && x.index === src.index);

    await editAsset(selected + '/dat', (bytes) => {
      const r = build(from, s, bytes, { name: uniqueName(bytes, name), at: it.pos, attack });
      const items = parseDAT(new DataView(r.bytes.buffer)).items;

      dropped = r.dropped;

      return npcSetPath(r.bytes, items.find((x) => x.type === 0 && x.index === it.index), items.filter((x) => x.type === 3).at(-1).name);
    });
  });

  await reselect(it.type, it.index);

  return dropped;
}

// Copies a stock path next to the enemy (its first interior point, where the NPC starts, at the enemy).
async function copyPathFor(it, src, quiet = false)
{
  const dropped = await addPathFor(it, src, splineCopy, src.name, `Copy path ${src.name}: ${it.name}`);

  if (dropped && !quiet)
    alert(`${dropped} of ${src.name}'s path events named things in its own level (attack targets, holds) and were left out.`);

  return dropped;
}

// A new straight path from the enemy, on the record of a stock path its kind flies.
async function newPathFor(it)
{
  const cat = await getPathCatalog();
  const src = cat.find((c) => c.kinds.split(', ').includes(SUBTYPES[it.sub])) ?? cat[0];

  return addPathFor(it, src, splineNew, `${it.name || 'Enemy'}_Path`, `New path: ${it.name}`);
}

// Enemy side: waits for a wave (WAVE_FIELDS gate, spawned when its flag is raised) and flies a path once it appears.
function waveEnemyRow(it)
{
  const dir = selected;
  const row = elem('div', { className: 'shotRow' });

  Promise.all([readWaves(dir), rom.read(dir + '/dat')]).then(([waves, dv]) => {
    const w = waveGet(dv, it);
    const pick = wavePicker(waves, w.flag, 'Wait for a wave: it stays out of the level until a trigger starts the wave, then appears and flies its path');

    pick.onchange = () => historyGroup(`Wave: ${it.name}`, async () => {
      const flag = await chooseWave(pick, dir, waves);

      if (flag === null)
        return syncSelPanel();

      await editItem(it, (bytes) => waveSet(bytes, it, flag ? { flag, counter: 0, atLeast: 0, gridSpawn: 0 } : { flag: 0 }), undefined, dir);
    });

    row.append('Waits for wave ', pick, ' Path ', npcPathSelect(it, dv, '-', 'None (goes for the player)'));
  });

  return row;
}

// The path an enemy or wingman flies: the level's paths, or one copied from another level. none = the "no path" value and its label.
function npcPathSelect(it, dv, none, noneLabel)
{
  const dir = selected;
  const items = parseDAT(dv).items;
  const splines = items.filter((x) => x.type === 3 && x.name);
  const cur = npcPathGet(dv, it) || none;

  // A path event 0x4E with no target sends the craft at the player.
  const triggers = items.filter((x) => x.type === 7);
  const attacks = (s) => (s.events ?? []).some((e) => triggers.find((t) => t.index === e.trigger && t.sub === 0x4E && !t.pathEvent?.target));
  const other = [none, ...splines.map((s) => s.name)].includes(cur) ? [] : [[cur, `${cur} (${cur.startsWith('+') ? 'roam area' : 'missing'})`]];

  const pairs = [
    ...(noneLabel ? [[none, noneLabel]] : []),
    ...splines.map((s) => [s.name, s.name + (attacks(s) ? ' (then attacks the player)' : '')]),
    ...other,
    ['__new', 'New path'],
    ['__copy', 'Copy from another level…']
  ];

  const path = selectOf(pairs, cur, { title: 'The path it flies from where it appears' });

  path.onchange = async () => {
    if (path.value === '__new')
      return newPathFor(it);

    if (path.value !== '__copy')
      return editItem(it, (bytes) => npcSetPath(bytes, it, path.value), `Path: ${it.name} -> ${path.value}`, dir);

    const cat = await getPathCatalog();

    const from = selectOf([
      ['', 'Pick a path to copy…'],
      ...cat.map((c, k) => [k, `${c.dir.split('/').pop()} ${c.name} (${c.kinds})`])
    ], null, { title: 'A stock flight path to copy into this level' });

    from.onchange = () => (from.value ? copyPathFor(it, cat[+from.value]) : syncSelPanel());
    path.replaceWith(from);
  };

  return path;
}

// ---- Level rules (layout A): "When…" boxes on the selected area, path point or unit, and level-wide ones in Level settings ----
const rulesFolderOf = (dir) => modLevelOf(dir)?.folder ?? dir.split('/').pop();

// rules.txt, or an older mod's rules.json until the first save converts it.
async function readRules(dir)
{
  const folder = rulesFolderOf(dir);
  const text = await virtualText(RULES_PATH(folder));

  return text ? rulesScriptParse(text) : rulesParse(await virtualText(RULES_JSON_PATH(folder)));
}

// Key order and the reader's line numbers aside, the same rules.
const rulesKey = (doc) => JSON.stringify(doc.rules.map(({ line, ...r }) => r), (k, v) => (v && typeof v === 'object' && !Array.isArray(v) ? Object.fromEntries(Object.keys(v).sort().map((key) => [key, v[key]])) : v));

const rulesStayJson = new Set();

// Every level object name the level's rules refer to, present or not.
const ruleNames = async (dir) => new Set((await readRules(dir)).rules.flatMap((r) => ruleRefs(r, ruleExt).map((x) => x.name)));

// Saves as rules.txt and empties an older rules.json. Rules with something the script cannot say (a "who") stay in rules.json.
async function writeRules(dir, doc, label)
{
  const folder = rulesFolderOf(dir);
  const text = rulesScriptWrite(doc);
  const json = await virtualText(RULES_JSON_PATH(folder));

  if (rulesKey(rulesScriptParse(text)) !== rulesKey(doc))
  {
    if (!rulesStayJson.has(folder))
      alert(`Some of ${folder}'s rules came from an older rules.json and use fields the rules script cannot hold, so they stay in rules.json.`);

    rulesStayJson.add(folder);

    return editText(RULES_JSON_PATH(folder), rulesWrite(doc), label);
  }

  await historyGroup(label, async () => {
    await editText(RULES_PATH(folder), text, label);

    if (json)
      await editText(RULES_JSON_PATH(folder), '', label);
  });
}

// The stock mission scripts (and the names they look up) are in the mission overlay; a mod level with its own objectives runs none of them,
// so there only the main code's lookups (the player start and the like, used by every level) count.
const MISSION_OVERLAY_ROM = 0xA5D30;

const scriptFinds = (name, dir = selected) => {
  const end = modLevelOf(dir)?.objectives?.length ? MISSION_OVERLAY_ROM : BLOB_START;

  return !!name && nameUses(romBytes.subarray(0, end), name, 0, 0) > 0;
};

// Renames an object; its rules, camera shot lines and every DAT reference (datRenameItem) follow. Throws a modder-facing reason when it
// can't. Returns how many of each moved.
async function renameObject(it, name, dir = selected)
{
  const old = it.name;
  const moved = { rules: 0, shots: 0, npcs: 0, triggers: 0, links: 0 };
  const lv = dir.split('/').pop();

  if (!/^[!-~]{1,47}$/.test(name))
    throw new Error('A name is 1 to 47 letters, digits or symbols, with no spaces');

  if (old === name)
    return moved;

  if (scriptFinds(old, dir))
    throw new Error(`The game looks "${old}" up by name, so it can't be renamed`);

  if (parseDAT(await rom.read(dir + '/dat')).items.some((x) => x.name === name))
    throw new Error(`"${name}" is already used in this level`);

  await historyGroup(`Name ${name}`, async () => {
    await editAsset(dir + '/dat', (b) => {
      const { bytes, ...r } = datRenameItem(b, it, name);

      Object.assign(moved, r);

      return bytes;
    }, `Name ${name}`);

    if (!old)
      return;

    const doc = await readRules(dir);

    moved.rules = rulesRename(doc, old, name, ruleExt);

    if (moved.rules)
      await writeRules(dir, doc, `Rules: ${old} -> ${name}`);

    const shots = shotRename(await virtualText(SHOT_LINKS), lv, old, name);

    moved.shots = shots.n;

    if (shots.n)
      await editText(SHOT_LINKS, shots.text, `Camera shots: ${old} -> ${name}`);
  });

  if (selected === dir)
    await reselect(it.type, it.index);

  return moved;
}

const levelScripts = new Map();

// What a stock level's own script does, read from the ROM once per level. Levels of the player's own have none.
function levelScript(dir)
{
  const id = dir.match(/\/lv_([0-9a-k])$/)?.[1];

  if (id && !levelScripts.has(id))
    levelScripts.set(id, missionScript(romBytes, parseInt(id, 36)));

  return levelScripts.get(id);
}

const ARROW_FIELDS = { show: ['flag', 'counter', 'atLeast'], hide: ['hideFlag', 'hideCounter', 'below'] };

const ARROW_MODES = {
  show: { always: 'Always', units: 'After units are destroyed', area: 'After an area is entered', mission: 'When the mission says' },
  hide: { always: 'Never', units: 'When units are destroyed', area: 'When an area is entered', mission: 'When the mission says' }
};

// One side of an arrow as the modder sees it. managed: only rules drive it, so an edit can reuse its counter or flag.
function arrowCond(w, part, dv, doc, dir)
{
  const [flagKey, counterKey] = ARROW_FIELDS[part];
  const flag = w[flagKey];
  const counter = w[counterKey];

  if (!flag && !counter)
    return { mode: 'always', units: [], areas: [], flag, counter };

  const src = arrowSources(dv, doc, flag, counter);
  const script = levelScript(dir);
  const scripted = script && !(flag && counter) ? scriptUnits(script, dv, flag, counter) : null;
  const units = scripted ?? src.units;
  const named = flag && counter ? 'mission' : (src.areas.length ? 'area' : (units.length ? 'units' : 'mission'));

  return { mode: named, units, areas: src.areas, flag, counter, managed: src.managed && !scripted && named !== 'mission' };
}

const arrowOwnsRule = (r, flag, counter) => (r.when === 'destroyed' || r.when === 'enter') && r.actions?.length === 1 && ((counter && r.actions[0].do === 'counter' && r.actions[0].counter === counter) || (flag && r.actions[0].do === 'flag' && r.actions[0].flag === flag));

// Sets one side of an arrow: names are the units or the area. The rules that count them and the arrow's numbers change as one undo step.
async function arrowSet(it, part, mode, names, reload = true)
{
  const dir = selected;

  await historyGroup('Objective arrow', async () => {
    const named = [];

    for (const n of mode === 'units' ? names : [])
    {
      const unit = parseDAT(await rom.read(dir + '/dat')).items.find((x) => x.name === n && isUnit(x));
      const name = unit && (await ensureRuleName(dir, unit));

      if (name)
        named.push(name);
    }

    const dv = await rom.read(dir + '/dat');
    const doc = await readRules(dir);
    const items = parseDAT(dv).items;
    const now = items.find((x) => x.type === 7 && x.index === it.index);
    const w = { ...now.waypoint };
    const [flagKey, counterKey, countKey] = ARROW_FIELDS[part];
    const old = arrowCond(w, part, dv, doc, dir);
    const shared = items.some((x) => x !== now && x.waypoint && ((old.flag && [x.waypoint.flag, x.waypoint.hideFlag].includes(old.flag)) || (old.counter && [
      x.waypoint.counter,
      x.waypoint.hideCounter
    ].includes(old.counter))));
    const owned = old.managed && !shared;

    if (owned)
      doc.rules = doc.rules.filter((r) => !arrowOwnsRule(r, old.flag, old.counter));

    const ruleNumbers = (kind) => doc.rules.flatMap((r) => (r.actions ?? []).filter((a) => a.do === kind).map((a) => a[kind]));

    const free = (used) => {

      for (let n = 128; n >= 1; n--)
        if (!used.has(n))
          return n;

      throw new Error('This level has no free counters left');
    };

    let flag = 0;
    let counter = 0;
    let count = 0;

    if (mode === 'units' && named.length)
    {
      counter = owned && old.counter ? old.counter : free(new Set([...levelUsedObjectiveSlots(dv).counters, ...ruleNumbers('counter')]));
      count = named.length;
      named.forEach((object) => doc.rules.push({ when: 'destroyed', object, actions: [{ do: 'counter', counter, add: 1 }] }));
    }

    if (mode === 'area' && names.length)
    {
      const waves = await readWaves(dir);

      flag = owned && old.flag ? old.flag : free(new Set([...levelUsedFlags(dv), ...ruleNumbers('flag'), ...Object.values(waves)]));
      doc.rules.push({ when: 'enter', object: names[0], actions: [{ do: 'flag', flag, value: 1 }] });
    }

    Object.assign(w, { [flagKey]: flag, [counterKey]: counter, [countKey]: count });
    await writeRules(dir, doc, 'Objective arrow');

    const write = (bytes) => arrowWrite(bytes, now, w);

    await (reload ? editItem(now, write, 'Objective arrow', dir) : editAsset(dir + '/dat', write, 'Objective arrow'));
  });

  rulesChanged();
}

// Before an item is deleted: arrows waiting on a unit stop counting it, a side waiting on an area goes back to its default, and arrows following it stay put. A deleted arrow takes its rules with it. Call inside the delete's undo step.
async function arrowsForget(dir, gone)
{
  const state = async () => {

    const dv = await rom.read(dir + '/dat');

    return { dv, doc: await readRules(dir), items: parseDAT(dv).items };
  };

  const start = await state();

  if (gone.waypoint)
  {
    for (const part of ['show', 'hide'])
      if (arrowCond(gone.waypoint, part, start.dv, start.doc, dir).managed)
        await arrowSet(gone, part, 'always', [], false);

    return;
  }

  // A shared name stays in use by the objects left behind.
  if (!gone.name || start.items.filter((x) => x.name === gone.name).length > 1)
    return;

  for (const { index } of start.items.filter((x) => x.waypoint))
    for (const part of ['show', 'hide'])
    {
      const { dv, doc, items } = await state();
      const arrow = items.find((x) => x.type === 7 && x.index === index);
      const cond = arrowCond(arrow.waypoint, part, dv, doc, dir);

      // A stock kill count only needs its target lowered.
      if (cond.mode === 'units' && cond.units.includes(gone.name) && cond.counter && !cond.managed && cond.units.length > 1)
      {
        const d = unitDeath(dv, gone);
        const key = ARROW_FIELDS[part][2];
        const count = arrow.waypoint[key] - Math.max(1, d.counter === cond.counter ? d.add : d.add2);

        await editAsset(dir + '/dat', (bytes) => arrowWrite(bytes, arrow, { ...arrow.waypoint, [key]: count }), 'Objective arrow');
      }
      else if (cond.mode === 'units' && cond.units.includes(gone.name))
        await arrowSet(arrow, part, 'units', cond.units.filter((n) => n !== gone.name), false);

      if (cond.mode === 'area' && cond.areas.includes(gone.name))
        await arrowSet(arrow, part, 'always', [], false);
    }

  for (const arrow of (await state()).items.filter((x) => x.waypoint?.follow === gone.name))
    await editAsset(dir + '/dat', (bytes) => arrowWrite(bytes, arrow, { ...arrow.waypoint, follow: '' }), 'Objective arrow target');
}

// The selected arrow's target and when it shows and hides, in names only: the flags and counters are picked here.
function arrowPanel(it)
{
  const dir = selected;
  const box = elem('div', { className: 'arrowPanel' });
  const pending = {};

  const condRow = (part, cond, items) => {

    const mode = pending[part] ?? cond.mode;
    const modes = Object.entries(ARROW_MODES[part]).filter(([k]) => k !== 'mission' || cond.mode === 'mission');
    const pick = selectOf(modes, mode, { title: part === 'show' ? 'When the arrow starts pointing here' : 'When the arrow stops pointing here' });
    const out = [elem('div', { className: 'shotRow' }, part === 'show' ? 'Shows ' : 'Hides ', pick)];

    pick.onchange = () => {
      if (pick.value === 'always')
        return arrowSet(it, part, 'always', []);

      pending[part] = pick.value;
      render();
    };

    if (mode === 'units')
    {
      const units = pending[part] ? [] : cond.units;

      const chips = units.map((n) => elem('button', {
        className: 'objChip',
        textContent: n,
        title: 'Remove',
        onclick: () => arrowSet(it, part, 'units', units.filter((x) => x !== n))
      }));

      const add = selectOf([
        ['', 'Add a unit…'],
        ...namedByKind(items, (x) => isUnit(x) && !units.includes(x.name))
      ], '', { title: 'A unit that has to be destroyed' });

      add.onchange = () => add.value && arrowSet(it, part, 'units', [...units, add.value]);
      out.push(elem('div', { className: 'arrowChips' }, ...chips, add));
    }

    if (mode === 'area')
    {
      const areas = items.filter((x) => x.name && x.onEnter).map((x) => [x.name, x.name]);
      const area = selectOf([['', 'Pick an area…'], ...areas], pending[part] ? '' : (cond.areas[0] ?? ''), { title: 'The area that has to be entered' });

      area.onchange = () => area.value && arrowSet(it, part, 'area', [area.value]);
      out.push(area);
    }

    return out;
  };

  const render = async () => {

    const [dv, doc] = await Promise.all([rom.read(dir + '/dat'), readRules(dir)]);
    const items = parseDAT(dv).items;
    const now = items.find((x) => x.type === 7 && x.index === it.index);

    if (!now?.waypoint)
      return;

    const w = now.waypoint;

    const follow = selectOf([
      ['', 'Nothing, stays here'],
      ...namedByKind(items, isUnit)
    ], w.follow, { title: 'A unit the arrow keeps pointing at as it moves' });

    follow.onchange = () => editItem(now, (bytes) => arrowWrite(bytes, now, { ...w, follow: follow.value }), 'Objective arrow target', dir);

    box.replaceChildren(
      elem('div', { className: 'shotRow' }, 'Follows ', follow),
      ...condRow('show', arrowCond(w, 'show', dv, doc, dir), items),
      ...condRow('hide', arrowCond(w, 'hide', dv, doc, dir), items)
    );
  };

  render();
  liveRuleViews.add({ box, render });

  return box;
}

// The object a rule attaches to needs a unique name; unnamed or shared ones get one, unless the stock script finds it by name.
// Rules already on a shared name stay on it: in game they fire for every object of that name, not just this one.
async function ensureRuleName(dir, it)
{
  const dv = await rom.read(dir + '/dat');
  const items = parseDAT(dv).items;

  if (it.name && items.filter((x) => x.name === it.name).length === 1)
    return it.name;

  if (scriptFinds(it.name, dir))
  {
    alert(`"${it.name}" is shared with another object and the game looks it up by name, so it cannot be renamed for a rule.`);

    return null;
  }

  const bytes = u8(dv);
  const ruled = await ruleNames(dir);
  const name = uniqueName(bytes, it.name || (it.pathEvent ? 'rule_point' : (it.type === 7 ? 'rule_area' : 'rule_unit')), ruled);

  await editAsset(dir + '/dat', (b) => datNameItem(b, it, name), `Name ${name}`);

  return name;
}

// A picker for one rule parameter, filled from the level; returns the element. set(value) stores it.
async function rulePicker(p, value, set, ctx)
{
  const sel = (opts, cls) => {
    const s = selectOf([['', 'Pick…'], ...opts], value ?? '', { className: cls ?? '' }, '(missing)');

    s.onchange = () => set(s.value === '' ? undefined : (/^(voice|song|enemyKind|objective|number|flag)$/.test(p.picker) ? +s.value : s.value));

    return s;
  };

  const items = ctx.items;
  const named = (f) => items.filter((it) => it.name && f(it)).map((it) => [it.name, it.name]);

  if (p.picker === 'area')
    return sel(named((it) => it.type === 7 && it.onEnter && !it.pathEvent), 'obj');

  if (p.picker === 'path')
    return sel(named((it) => it.type === 3), 'obj');

  if (p.picker === 'pathPoint')
    return sel(named((it) => it.pathEvent), 'obj');

  if (p.picker === 'unit')
    return sel(namedByKind(items, isUnit), 'obj');

  if (p.picker === 'camera')
    return sel(named((it) => it.type === 0 && it.sub === 0x26), 'obj');

  if (p.picker === 'song')
    return sel(songChoices());

  if (p.picker === 'enemyKind')
    return sel(ENEMY_GROUPS.flatMap(([, subs]) => subs.filter((s) => WAVE_FIELDS[s]).map((s) => [s, SUBTYPES[s]])));

  if (p.picker === 'objective')
    return sel((modLevelOf(ctx.dir)?.objectives ?? []).map((o, i) => [i + 1, `${i + 1}: ${o.text}`]));

  if (p.picker === 'voice') {
    const s = elem('select');

    await fillVoiceSelect(s);
    s.value = value != null ? String(value) : '';
    s.onchange = () => set(s.value === '' ? undefined : +s.value);

    return s;
  }

  if (p.picker === 'recording')
  {
    const name = value || `rule_${Math.random().toString(36).slice(2, 8)}`;
    const rec = recordingControls(`mod/assets/level/${rulesFolderOf(ctx.dir)}/${name}`, 'rule', (has) => has && value !== name && set(name));

    return elem('div', null, rec.row, rec.note);
  }

  // A variable name, suggesting the ones the level's rules already set.
  if (p.picker === 'variable')
  {
    const list = elem('datalist', { id: 'ruleVars' + Math.random().toString(36).slice(2, 8) });
    const inp = elem('input', { value: value ?? '', placeholder: 'variable', title: 'A name the rules can check, like bridge_held' });

    list.append(...(ctx.variables ?? []).map((v) => new Option(v)));
    inp.setAttribute('list', list.id);
    inp.onchange = () => set(inp.value.trim() || undefined);

    return elem('span', null, inp, list);
  }

  const kind = p.picker === 'text' ? 'text' : 'number';
  const inp = elem('input', { type: kind, value: value ?? '', placeholder: p.label ?? p.key, min: 0, step: p.picker === 'seconds' ? 0.5 : 1 });

  inp.onchange = () => set(inp.value === '' ? undefined : (kind === 'text' ? inp.value : +inp.value));

  return inp;
}

const optGroup = (label, opts) => elem('optgroup', { label }, ...opts.map(([v, t]) => new Option(t, v)));

// Named objects passing f, one option group per kind (a level can name hundreds), each name once.
function namedByKind(items, f)
{
  const groups = new Map();
  const seen = new Set();

  for (const it of items.filter((x) => x.name && f(x) && !seen.has(x.name)))
  {
    seen.add(it.name);

    const kind = SUBTYPES[it.sub] ?? 'Other';

    groups.set(kind, [...(groups.get(kind) ?? []), [it.name, it.name]]);
  }

  return [...groups].sort(([a], [b]) => a.localeCompare(b)).map(([kind, opts]) => optGroup(kind, opts.sort((a, b) => a[1].localeCompare(b[1]))));
}

// The in-mission cue table's songs as [song id, name], one each.
const songChoices = () => [...new Map(parseMusicCues(romBytes).cues.map((c) => [c.song, c.name])).entries()];

// Actions and custom whens native mods offer (mods/<id>/rules_ext.json in the game folder); ask = pick the folder if none is granted.
let ruleExt = [];

async function loadRuleExt(ask)
{
  let dir = ask ? await gameFolder().catch(() => null) : await idbKv('readonly', (s) => s.get('gameDir')).catch(() => null);

  if (!ask && dir && (await dir.queryPermission({ mode: 'readwrite' })) !== 'granted')
    dir = null;

  const mods = dir && (await dir.getDirectoryHandle('mods').catch(() => null));

  if (!mods)
    return false;

  const out = [];

  for await (const [id, h] of mods.entries())
  {
    const f = h.kind === 'directory' && (await h.getFileHandle('rules_ext.json').then((x) => x.getFile(), () => null));

    if (f)
      out.push(...rulesExtParse(await f.text(), id));
  }

  ruleExt = out;
  rulesChanged();

  return true;
}

// Every shown rules view re-renders after any rules write, so none edits a stale copy of the file.
const liveRuleViews = new Set();

function rulesChanged() {
  stopRulePathPick();

  for (const v of liveRuleViews)
    (v.box.isConnected ? v.render() : liveRuleViews.delete(v));
}

function stopRulePathPick()
{
  if (!rulePathPick)
    return;

  $('selInfo').textContent = rulePathPick.info;
  rulePathPick.button.classList.remove('on');
  rulePathPick = null;
  attachGizmo();
}

const PICK_PATH = {
  tip: 'Pick the path by clicking it in the view',
  prompt: 'Click a flight path in the view. Esc cancels',
  targets: () => visibleMarkers(),
  accepts: (it) => it.type === 3
};

const PICK_UNIT = {
  tip: 'Pick the target by clicking it in the view',
  prompt: 'Click the unit to attack in the view. Esc cancels',
  targets: () => [
    ...root.children.filter((o) => o.userData.placedItem && o.visible),
    ...visibleMarkers().filter((m) => m.userData.item && isUnit(m.userData.item))
  ],
  accepts: isUnit
};

// Picks a named object by clicking it in the view. The move handles go away meanwhile, or a click near them would grab them.
function rulePathButton(set, kind = PICK_PATH)
{
  const button = iconButton('place', kind.tip, () => {
    if (rulePathPick?.button === button)
      return stopRulePathPick();

    objSetPick(null);
    stopPlacing();
    stopRulePathPick();
    rulePathPick = { set, button, kind, info: $('selInfo').textContent };
    button.classList.add('on');
    gizmo.detach();
    $('selInfo').textContent = kind.prompt;
  });

  return button;
}

function rulePathClick(e)
{
  let o = pick(e, rulePathPick.kind.targets())?.object;

  while (o && !o.userData.item && !o.userData.placedItem)
    o = o.parent;

  const it = o?.userData.placedItem ?? o?.userData.item;

  if (!it?.name || !rulePathPick.kind.accepts(it))
    return;

  const { set } = rulePathPick;

  stopRulePathPick();
  set(it.name);
}

// A fly order can only head for a region or an area with a radius; the game sends it at the player for anything else (func_800F5A30).
const PICK_AREA = {
  tip: 'Pick the place by clicking it in the view',
  prompt: 'Click the region or area to fly to in the view. Esc cancels',
  targets: () => visibleMarkers().filter((m) => m.userData.item && PICK_AREA.accepts(m.userData.item)),
  accepts: (it) => it.type === 6 || (it.type === 7 && it.radius != null)
};

// none: the value for "no target" ("-" makes the craft drop the target it has) and its wording.
const ORDER_TARGETS = {
  0x20: { none: ['-', 'Nobody (drops its target)'], kind: PICK_UNIT, title: 'The unit this order sends the craft after' },
  0x4E: { none: ['', 'The player'], kind: PICK_AREA, title: 'Where this order sends the craft' }
};

function orderTargetRow(it)
{
  const { none, kind, title } = ORDER_TARGETS[it.sub];
  const names = [...new Set(editObjects().filter(([, x]) => kind.accepts(x) && x.name).map(([, x]) => x.name))].sort((a, b) => a.localeCompare(b, undefined, { numeric: true }));
  const pairs = [none, ...names.map((n) => [n, n])];
  const box = selectOf(pairs, it.pathEvent.target || none[0], { title }, (v) => v);

  const set = async (name) => {
    await editAsset(selected + '/dat', (bytes) => orderSetTarget(bytes, it, name), `Order target: ${name || none[1]}`);
    await reloadLevel(selected);
    select(editObjects().find(([, x]) => x.type === it.type && x.index === it.index)?.[0] ?? null);
  };

  box.onchange = () => set(box.value);

  const row = elem('div', { className: 'shotRow' });

  row.append('Target', box, rulePathButton(set, kind));

  return row;
}

const PICK_COND = {
  tip: 'Add a check on an object by clicking it in the view',
  prompt: 'Click a unit or trigger area in the view. Esc cancels',
  targets: () => [...PICK_UNIT.targets(), ...visibleMarkers().filter((m) => m.userData.item && isTriggerArea(m.userData.item))],
  accepts: (it) => isUnit(it) || (it.type === 7 && it.onEnter && !it.pathEvent)
};

// What each named object of the level is, for the rules script's checks and completion.
function ruleObjectKinds(items)
{
  const out = new Map();

  for (const it of items.filter((x) => x.name))
  {
    if (it.pathEvent)
      out.set(it.name, 'path');
    else if (it.type === 7 && it.onEnter)
      out.set(it.name, 'area');
    else if (it.type === 0 && it.sub === 0x26)
      out.set(it.name, 'camera');
    else if (isUnit(it))
      out.set(it.name, 'unit');
  }

  return out;
}

// The "only if" box of a rule (or the condition of a "Conditions become true" rule), with a pick-in-view button that adds a check.
function ruleCondRow(r, kinds, ctx, save)
{
  const prefix = ctx.variables.size ? [...ctx.variables].map((v) => `set ${v} 1`).join('\n') + '\n' : '';
  const head = prefix + 'when true: ';
  const own = r.when === 'true';

  const box = codeBox({
    value: r.if ?? '',
    placeholder: own ? 'destroyed(gate) and kills >= 3' : 'Only if… (optional)',
    title: own ? 'The rule fires as soon as this is true' : 'The rule only fires while this is true',
    check: (t) => (t.trim() || own ? condCheck(t, ctx) : []),

    complete: (t, c) => {
      const out = rulesComplete(head + t, head.length + c, { objects: kinds });

      return { ...out, from: out.from - head.length, to: out.to - head.length };
    },

    onChange: (t) => {
      if (t.trim())
        r.if = t.trim();
      else
        delete r.if;

      save('Rule: only if');
    }
  });

  const pick = rulePathButton((name) => {
    const t = box.text().trimEnd();
    const check = kinds.get(name) === 'area' ? `entered(${name})` : `destroyed(${name})`;

    if (t.endsWith('('))
      box.insert(name);
    else
      box.insert((t && !/\b(and|or|not)$/.test(t) ? ' and ' : (t ? ' ' : '')) + check);
  }, PICK_COND);

  return elem('div', { className: 'cond' }, box, pick);
}

// A new action of that kind; Spawn starts from the last one made, on a flight path of this level.
function newRuleAction(type, items)
{
  if (type !== 'spawn')
    return { do: type };

  const paths = items.filter((x) => isFlightPath(x) && x.name);

  return { do: 'spawn', ...lastSpawn, path: paths.some((x) => x.name === lastSpawn.path) ? lastSpawn.path : paths.at(-1)?.name };
}

// owner: { kind: 'area' | 'path' | 'unit', it } on a selected object, or { kind: 'level' } in the Level events window.
function rulesSection(owner) {
  const dir = selected;
  const box = elem('div', { className: 'rulesBox' });

  const render = async () => {
    // A rebuild would take the caret out of a condition being typed; it runs once the box loses focus.
    if (document.activeElement?.classList.contains('codeText') && box.contains(document.activeElement))
      return box.addEventListener('focusout', () => setTimeout(render), { once: true });

    const [doc, dv] = await Promise.all([readRules(dir), rom.read(dir + '/dat')]);
    const items = parseDAT(dv).items;
    const variables = [...new Set(doc.rules.flatMap((r) => (r.actions ?? []).filter((a) => (a.do === 'set' || a.do === 'add') && a.variable).map((a) => a.variable)))];
    const ctx = { dir, items, variables };
    const own = !!modLevelOf(dir)?.objectives?.length;
    const kinds = ruleObjectKinds(items);
    const condCtx = { objects: kinds, objectives: own, variables: new Set(variables) };
    const whens = RULE_WHENS.filter((w) => w.on === owner.kind && (own || !w.objectives));
    const broken = new Set(rulesBroken(doc, items, ruleExt).map((b) => `${b.rule}:${b.field}`));

    // Level events also list rules whose object was deleted, so they can be pointed at another one or removed.
    const orphan = (i) => owner.kind === 'level' && broken.has(`${i}:object`);
    const custom = owner.kind === 'level' ? ruleExt.filter((e) => e.kind === 'event') : [];
    const mine = doc.rules.map((r, i) => [r, i]).filter(([r, i]) => orphan(i) || (owner.kind === 'level' && r.when === 'custom') || (whens.some((w) => w.when === r.when) && (owner.kind === 'level' || r.object === owner.it.name)));
    const save = (label) => writeRules(dir, doc, label).then(rulesChanged);
    const fromMods = custom.length ? [optGroup('From mods', custom.map((e) => [`custom:${e.name}`, `${e.label} (${e.mod})`]))] : [];

    const setWhen = (r, value) => {
      const c = value.startsWith('custom:');

      r.when = c ? 'custom' : value;

      if (c)
        r.custom = value.slice(7);
      else
        delete r.custom;
    };

    box.replaceChildren(elem('b', { className: 'rulesHead', textContent: 'When…' }));

    for (const [r, i] of mine)
    {
      const rule = elem('div', { className: 'rule' });
      const head = elem('div', { className: 'when' });
      const on = RULE_WHENS.find((x) => x.when === r.when)?.on;
      const whenPairs = (orphan(i) ? RULE_WHENS.filter((x) => x.on === on) : whens).map((x) => [x.when, x.label]);
      const w = selectOf([...whenPairs, ...fromMods], r.when === 'custom' ? `custom:${r.custom}` : r.when, null, () => `${r.custom} (needs a mod)`);

      w.onchange = () => {
        setWhen(r, w.value);
        save('Rule: when');
      };

      const every = elem('input', { type: 'checkbox', checked: !!r.every, title: 'Every time (otherwise only the first time)' });

      every.onchange = () => ((r.every = every.checked), save('Rule: every time'));

      const del = iconButton('delete', 'Remove rule', () => (doc.rules.splice(i, 1), save('Remove rule')));

      head.append(w);

      if (orphan(i))
        head.append(await rulePicker({ picker: on === 'path' ? 'pathPoint' : on }, r.object, (v) => ((r.object = v), save('Rule: object')), ctx));

      for (const p of RULE_WHENS.find((x) => x.when === r.when)?.params ?? [])
        head.append(await rulePicker(p, r[p.key], (v) => ((r[p.key] = v), save('Rule: ' + p.key)), ctx));

      const ev = elem('label');

      ev.append(every, ' every time');
      head.append(ev, del);
      rule.append(head, ruleCondRow(r, kinds, condCtx, save));

      if (broken.has(`${i}:object`))
        rule.append(elem('small', { className: 'warn', textContent: `"${r.object}" is gone: pick another or remove the rule.` }));

      const extActs = ruleExt.filter((e) => e.kind === 'action');

      const actChoices = [
        ...RULE_ACTIONS.filter((x) => own || !x.objectives).map((x) => [x.do, x.label]),
        ...(extActs.length ? [optGroup('From mods', extActs.map((e) => [e.name, `${e.label} (${e.mod})`]))] : [])
      ];

      for (const [k, a] of (r.actions ?? []).entries())
      {
        const row = elem('div', { className: 'act' });
        const kind = selectOf(actChoices, a.do, null, '(needs a mod)');

        kind.onchange = () => ((r.actions[k] = newRuleAction(kind.value, items)), save('Rule: action'));

        const rm = iconButton('delete', 'Remove action', () => (r.actions.splice(k, 1), save('Remove action')));

        row.append(kind);

        for (const p of RULE_ACTIONS.find((x) => x.do === a.do)?.params ?? extActs.find((e) => e.name === a.do)?.params ?? [])
        {
          const set = (v) => {
            a[p.key] = v;

            if (a.do === 'spawn')
              lastSpawn[p.key] = v;

            save('Rule: ' + p.key);
          };

          const el = await rulePicker(p, a[p.key], set, ctx);

          if (broken.has(`${i}:actions.${k}.${p.key}`))
            el.classList.add('bad');

          // A created wave is already in the level, so changing these would do nothing.
          el.disabled = a.do === 'spawn' && !!a.flag;
          row.append(el);

          if (p.picker === 'path' && !el.disabled)
            row.append(rulePathButton(set));
        }

        if (a.do === 'spawn')
          row.append(spawnWaveButton(a, dir, () => save('Spawn wave')));

        row.append(rm);
        rule.append(row);
      }

      const addAct = selectOf([['', '+ Add action'], ...actChoices], '', { className: 'addAct' });

      addAct.onchange = () => ((r.actions ??= []).push(newRuleAction(addAct.value, items)), save('Add action'));
      rule.append(addAct);
      box.append(rule);
    }

    const add = selectOf([['', '+ Add rule'], ...whens.map((x) => [x.when, x.label]), ...fromMods], '', { className: 'addRule' });

    add.onchange = async () => {
      const r = { actions: [] };

      setWhen(r, add.value);

      if (owner.kind !== 'level') {
        const name = await ensureRuleName(dir, owner.it);

        if (!name)
          return;

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
function levelRulesPanel()
{
  const dir = selected;
  const box = elem('div', { className: 'levelRules' });
  const filter = elem('input', { type: 'search', placeholder: 'Filter by name, when or action' });
  const badOnly = elem('input', { type: 'checkbox' });
  const badLabel = elem('label');
  const list = elem('div', { className: 'ruleList' });
  const tools = elem('div', { className: 'when' });
  const whenLabel = (r) => RULE_WHENS.find((w) => w.when === r.when)?.label ?? r.when;

  const render = async () => {
    const [doc, dv] = await Promise.all([readRules(dir), rom.read(dir + '/dat')]);
    const items = parseDAT(dv).items;
    const broken = new Set(rulesBroken(doc, items, ruleExt).filter((b) => b.field === 'object').map((b) => b.rule));
    const groups = new Map();

    doc.rules.forEach((r, i) => r.when !== 'custom' && RULE_WHENS.find((w) => w.when === r.when)?.on !== 'level' && (groups.get(r.object) ?? groups.set(r.object, []).get(r.object)).push([r, i]));

    const q = filter.value.trim().toLowerCase();

    list.replaceChildren();

    for (const [name, rules] of [...groups].sort(([a], [b]) => String(a).localeCompare(String(b))))
    {
      const bad = rules.some(([, i]) => broken.has(i));
      const acts = rules.reduce((n, [r]) => n + (r.actions?.length ?? 0), 0);
      const text = rules.map(([r]) => [
        whenLabel(r),
        ...(r.actions ?? []).map((a) => RULE_ACTIONS.find((x) => x.do === a.do)?.label ?? a.do)
      ].join(' ')).join(' ');

      if ((badOnly.checked && !bad) || (q && !`${name} ${text}`.toLowerCase().includes(q)))
        continue;

      const row = elem('button', {
        className: 'ruleRow' + (bad ? ' bad' : ''),
        title: bad ? 'Its object is gone: point it at another or remove it in Level events above' : 'Select it'
      });

      row.append(elem('b', { textContent: name }), ` ${rules.map(([r]) => whenLabel(r)).join('; ')} (${acts} action${acts === 1 ? '' : 's'})`);
      row.onclick = () => bad || selectNamed(name);
      list.append(row);
    }

    if (!list.children.length)
      list.append(elem('small', {
        className: 'shotNote',
        textContent: groups.size ? 'No rules match.' : 'No rules on objects yet: select an area, path point or unit and add one.'
      }));
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

  const panels = elem('div', { className: 'levelRules' }, elem('b', { textContent: 'Level-wide' }), rulesSection({ kind: 'level' }), fromMods, elem('b', { textContent: 'Rules on objects' }), tools, list);
  const script = elem('div', { className: 'ruleScript' });
  const scriptBtn = iconButton('script', 'Edit the rules as text', () => showScript(!scriptBtn.classList.contains('on')));
  let code = null;

  // The whole rules.txt in one box; an older rules.json shows as script and becomes rules.txt on the first save.
  const renderScript = async () => {
    const folder = rulesFolderOf(dir);
    const [text, doc, dv] = await Promise.all([virtualText(RULES_PATH(folder)), readRules(dir), rom.read(dir + '/dat')]);
    const value = text || rulesScriptWrite(doc);

    Object.assign(scriptCtx, {
      objects: ruleObjectKinds(parseDAT(dv).items),
      objectives: !!modLevelOf(dir)?.objectives?.length,
      ext: ruleExt.length ? ruleExt : null
    });

    if (code)
      return code.setValue(value);

    code = codeBox({
      value,
      rows: 18,
      title: 'The level\'s rules as text: when … [if …], one action a line, end',
      check: (t) => rulesScriptCheck(t, scriptCtx),
      complete: (t, c) => rulesComplete(t, c, scriptCtx),

      onChange: async (t) => {
        await historyGroup('Rules script', async () => {
          await editText(RULES_PATH(folder), t, 'Rules script');

          if (await virtualText(RULES_JSON_PATH(folder)))
            await editText(RULES_JSON_PATH(folder), '', 'Rules script');
        });

        rulesChanged();
      }
    });

    script.replaceChildren(code);
  };

  const scriptCtx = {};

  const showScript = (on) => {
    scriptBtn.classList.toggle('on', on);
    store('rs64.rulesScript', on);
    panels.hidden = on;
    script.hidden = !on;

    if (on)
      renderScript();
  };

  box.append(elem('div', { className: 'when' }, scriptBtn), panels, script);
  liveRuleViews.add({ box: list, render });
  liveRuleViews.add({ box: script, render: () => !script.hidden && renderScript() });
  showScript(!!stored('rs64.rulesScript', false));
  render();

  return box;
}

function openRulesPanel() {
  rulesPanelDir = selected;
  $('rulesBody').replaceChildren(levelRulesPanel());
  $('rulesPanel').hidden = false;

  if (window.showDirectoryPicker)
    loadRuleExt(false);
}

setEditIcon($('btnRules'), 'events');
$('btnRules').append('Level events…');
$('btnRules').removeAttribute('aria-label');
$('btnRules').onclick = () => ($('rulesPanel').hidden || rulesPanelDir !== selected ? openRulesPanel() : ($('rulesPanel').hidden = true));
setEditIcon($('rulesClose'), 'close');
$('rulesClose').onclick = () => ($('rulesPanel').hidden = true);

// ---- Water, fog and clouds of the open level: water is on the level's own data, fog and clouds in its environment.json ----

const FOG_REACH = [
  ['scale', 'Move out with the draw distance'],
  ['thin', 'Stay, then thin out toward the view\'s edge'],
  ['stock', 'Stay as a solid wall']
];

const rgbHex = (c) => '#' + c.map((v) => v.toString(16).padStart(2, '0')).join('');
const hexRgb = (s) => [1, 3, 5].map((i) => parseInt(s.slice(i, i + 2), 16));

// A heading with its on/off box to the right.
function lookSection(title, on, change)
{
  const box = elem('input', { type: 'checkbox', checked: on, title: `${title} on or off` });

  box.onchange = () => change(box.checked);

  return elem('div', { className: 'lookHead' }, elem('b', { textContent: title }), box);
}

function lookSlider(label, tip, value, min, max, step, change)
{
  const input = elem('input', { type: 'range', min, max, step, value });
  const out = elem('output', { textContent: value });

  input.oninput = () => (out.textContent = input.value);
  input.onchange = () => change(+input.value);

  return elem('label', { title: tip }, label, input, out);
}

// A colour swatch, with a button back to the default once it has been changed.
function lookColor(label, resetTip, rgb, fallback, change)
{
  const pick = elem('input', { type: 'color', value: rgbHex(rgb ?? fallback) });

  pick.onchange = () => change(hexRgb(pick.value));

  return elem('label', {}, label, pick, ...(rgb ? [iconButton('undo', resetTip, () => change(undefined))] : []));
}

function lookPanel()
{
  const dir = selected;
  const box = elem('div', { className: 'levelRules look' });
  const path = ENV_PATH(rulesFolderOf(dir));

  const changeEnv = async (label, change) => {
    const doc = envParse(await virtualText(path));

    change(doc);
    await editText(path, envWrite(doc), label);
    await reloadLevel(dir);
    await render();
  };

  // Drops a section that holds no settings, so an untouched level stays exactly the game's.
  const setFog = (key, value) => changeEnv('Fog', (doc) => {
    doc.fog = { ...doc.fog, [key]: value };

    if (Object.values(doc.fog).every((v) => v === undefined))
      delete doc.fog;
  });

  const setClouds = (key, value) => changeEnv('Clouds', (doc) => (doc.clouds = { ...doc.clouds, [key]: value }));

  const render = async () => {
    const [dv, doc] = await Promise.all([rom.read(dir + '/dat'), virtualText(path).then(envParse)]);
    const water = datWaterGet(dv);

    const setWater = (patch, label) => editAsset(dir + '/dat', (b) => datWaterSet(b, { ...datWaterGet(b), ...patch }), label).then(() => reloadLevel(dir)).then(render);

    const fogStart = doc.fog?.start ?? (levelFog?.frac ?? 0.9);
    const clouds = doc.clouds;
    const windX = clouds?.wind?.[0] ?? 0;
    const windZ = clouds?.wind?.[1] ?? 0;
    const speed = Math.round(Math.hypot(windX, windZ) * 10) / 10;
    const heading = Math.round((Math.atan2(windZ, windX) * 180 / Math.PI + 360) % 360);
    const setWind = (spd, deg) => setClouds('wind', [+(spd * Math.cos(deg * Math.PI / 180)).toFixed(3), +(spd * Math.sin(deg * Math.PI / 180)).toFixed(3)]);

    // Unset, the game thins fog that starts close and moves the rest out (FogReach::Auto).
    const reach = selectOf(FOG_REACH, doc.fog?.reach && doc.fog.reach !== 'auto' ? doc.fog.reach : (fogStart < 0.85 ? 'thin' : 'scale'));

    reach.onchange = () => setFog('reach', reach.value);

    const parts = [];

    if (water)
    {
      parts.push(lookSection('Water', water.on, (on) => setWater({ on }, 'Water')));

      if (water.on)
        parts.push(
          lookSlider('Water level', 'How high the water comes up the terrain; anything lower becomes water', -water.level, -126, 128, 1, (v) => setWater({ level: -v }, 'Water level')),
          lookSlider('Wave height', 'Stock levels use 2 to 4', water.waves, 0, 12, 1, (v) => setWater({ waves: v }, 'Wave height')),
          lookSlider('Wave speed', 'Stock levels use 0.5 or 0.75', +water.speed.toFixed(2), 0.1, 2, 0.05, (v) => setWater({ speed: v }, 'Wave speed'))
        );
    }

    parts.push(
      elem('div', { className: 'lookHead' }, elem('b', { textContent: 'Fog' })),
      lookSlider('Fog starts at', 'How far out the fog begins, as a share of the view distance', +fogStart.toFixed(2), 0.1, 0.95, 0.05, (v) => setFog('start', v)),
      elem('label', { title: 'What the fog does when the player raises the draw distance' }, 'Far view', reach),
      lookColor('Fog colour', 'Back to the level\'s own colour', doc.fog?.color, levelFog ? hexRgb('#' + levelFog.color.getHexString()) : [128, 128, 128], (c) => setFog('color', c)),
      lookSection('Clouds', !!clouds, (on) => changeEnv('Clouds', (d) => (on ? (d.clouds = {}) : delete d.clouds)))
    );

    if (clouds)
    {
      const pinned = clouds.altitude !== undefined;
      const pin = elem('input', { type: 'checkbox', checked: pinned, title: 'Keep the clouds at one height, for levels with ground. Off, they stay around the player, as on Taloraan' });

      pin.onchange = () => setClouds('altitude', pin.checked ? -20 : undefined);

      parts.push(
        lookSlider('Wind speed', 'How fast the clouds drift; 0 is still', speed, 0, 20, 0.5, (v) => setWind(v, heading)),
        lookSlider('Wind direction', 'Degrees around the map', heading, 0, 359, 1, (v) => setWind(speed, v)),
        elem('label', { title: pin.title }, 'Fixed height', pin)
      );

      if (pinned)
        parts.push(lookSlider('Cloud height', 'Higher is further up', -clouds.altitude, -100, 200, 1, (v) => setClouds('altitude', -v)));

      parts.push(lookColor('Cloud colour', 'Back to the tan Taloraan colour', clouds.tint, [182, 140, 87], (c) => setClouds('tint', c)));
    }

    box.replaceChildren(...parts);
  };

  liveRuleViews.add({ box, render });
  render();

  return box;
}

function openLookPanel() {
  lookPanelDir = selected;
  $('lookBody').replaceChildren(lookPanel());
  $('lookPanel').hidden = false;
}

setEditIcon($('btnLook'), 'globe');
$('btnLook').append('Water, fog and clouds…');
$('btnLook').removeAttribute('aria-label');
$('btnLook').onclick = () => ($('lookPanel').hidden || lookPanelDir !== selected ? openLookPanel() : ($('lookPanel').hidden = true));
setEditIcon($('lookClose'), 'close');
$('lookClose').onclick = () => ($('lookPanel').hidden = true);

// ---- Opening crawls (mod/assets/crawl/<name>/crawl.json): the crawl of the level being edited. A stock chapter level's (0, 5, 10,
// 15) is the mod's replacement for that chapter; a mod level's is the crawl its levels.json names. Until the first edit the game's (or
// the template's) crawl shows, and that edit makes the file. Its backdrop and fly-by craft are the chapter's unless it has its own image.
const CRAWL_FILE = /^mod\/assets\/crawl\/([^/]+)\/crawl\.json$/;
const crawlBackdrop = (name) => `mod/assets/crawl/${name}/backdrop`;
const crawlDat = (name) => `mod/assets/crawl/${name}/dat`;
const CHAPTER_OF = { 0: 0, 5: 1, 10: 2, 15: 3 };
const ROMAN = ['I', 'II', 'III', 'IV'];
let crawlFontData = null;
let crawlKeySel = null;

async function modCrawls() {
  const out = [];

  for (const p of edits.keys()) {
    const m = CRAWL_FILE.exec(p);
    const text = m && (await virtualText(p));

    if (text)
      out.push({ name: m[1], ...crawlParse(text) });
  }

  return out.sort((a, b) => a.name.localeCompare(b.name));
}

const hasBytes = (path) => rom.read(path).then((d) => d.byteLength > 0, () => false);

// The crawl a level opens, or null (a stock level without a chapter, a mod level set to none): { dir, name (its file, or the one the
// first edit makes; manifest names are 15 characters), exists, c, base (the stock planet: backdrop and craft), replaces, level, label }.
async function levelCrawl(dir, adding = false)
{
  const ml = modLevelOf(dir);
  const n = ml ? ml.template : parseInt(dir.split('/lv_')[1], 36);
  const chapter = CHAPTER_OF[n];
  const crawls = await modCrawls();
  const stock = chapter != null ? stockCrawls(frontText)[chapter] : null;
  const blank = { chapter: '', title: '', body: '', planet: chapter ?? 0, base: chapter ?? 0, replaces: -1, camera: [] };

  if (!ml) {
    if (chapter == null)
      return null;

    const mine = crawls.filter((c) => c.replaces === chapter).at(-1);

    return {
      dir,
      name: mine?.name ?? `chapter_${chapter + 1}`,
      exists: !!mine,
      c: mine ?? { ...blank, ...stock },
      base: chapter,
      replaces: chapter,
      level: null,
      label: `Chapter ${ROMAN[chapter]}`
    };
  }

  if (ml.crawl === false && !adding)
    return null;

  const named = typeof ml.crawl === 'string' ? crawls.find((c) => c.name === ml.crawl) : null;
  const inline = ml.crawl && typeof ml.crawl === 'object' ? ml.crawl : null;

  if (!named && !inline && !stock && !adding)
    return null;

  const c = named ?? (inline ? { ...blank, ...inline, planet: inline.planet ?? 0 } : (stock ? { ...blank, ...stock } : blank));
  const taken = new Set(crawls.map((x) => x.name));
  const stem = ml.folder.toLowerCase().replace(/[^a-z0-9_-]+/g, '_').slice(0, 15);
  let name = named?.name ?? stem;

  for (let i = 2; !named && taken.has(name); i++)
    name = stem.slice(0, 15 - String(i).length - 1) + '_' + i;

  return { dir, name, exists: !!named, c, base: c.planet === 'own' ? c.base : c.planet, replaces: -1, level: ml, label: ml.title };
}

// One undo step: more() writes the crawl's own files first, then crawl.json (made on the first edit; planet "own" while it has its own
// backdrop) and, for a mod level, its levels.json entry naming it.
async function saveCrawl(ctx, label, change = {}, more = null) {
  await historyGroup(`Crawl ${ctx.label}: ${label}`, async () => {
    if (more)
      await more();

    const own = await hasBytes(crawlBackdrop(ctx.name));

    await editText(CRAWL_PATH(ctx.name), crawlWrite({
      ...ctx.c,
      ...change,
      planet: own ? 'own' : ctx.base,
      base: ctx.base,
      replaces: ctx.replaces
    }), `Crawl ${ctx.label}: ${label}`);

    if (ctx.level && ctx.level.crawl !== ctx.name) {
      const levels = modLevelsParse(await virtualText(MOD_LEVELS));
      const x = levels.find((l) => l.folder === ctx.level.folder);

      if (x)
      {
        x.crawl = ctx.name;
        await editText(MOD_LEVELS, modLevelsWrite(levels), `Crawl ${ctx.label}: ${label}`);
        await applyModLevels();
      }
    }
  });

  crawlsChanged();
}

// A backdrop from any image the browser opens, scaled to cover 640 x 128; encodeJpeg writes it the way the game's JFIF decoder reads.
async function importCrawlBackdrop(ctx)
{
  const pick = elem('input', { type: 'file', accept: 'image/*' });

  const f = await new Promise((res) => {
    pick.onchange = () => res(pick.files[0]);
    pick.click();
  });

  if (!f)
    return;

  const img = await createImageBitmap(f);
  const cv = new OffscreenCanvas(640, 128);
  const cx = cv.getContext('2d');
  const k = Math.max(640 / img.width, 128 / img.height);

  cx.drawImage(img, (640 - img.width * k) / 2, (128 - img.height * k) / 2, img.width * k, img.height * k);

  const jpeg = encodeJpeg(cx.getImageData(0, 0, 640, 128).data, 640, 128);

  await saveCrawl(ctx, 'backdrop', {}, () => editAsset(crawlBackdrop(ctx.name), () => jpeg, `Crawl ${ctx.label}: backdrop`));
}

// The right-click card for the backdrop: it is a texture like any other.
function crawlBackdropCard()
{
  const ctx = cut?.crawl?.ctx;

  if (!ctx)
    return null;

  const own = cut.crawl.ownBackdrop;
  const path = own ? crawlBackdrop(ctx.name) : `data/backdrop/planet_${ctx.base}`;
  const download = async () => saveBlob(new Blob([u8(await rom.read(path))], { type: 'image/jpeg' }), `${ctx.name}_backdrop.jpg`);

  return {
    title: 'Backdrop',
    rows: [
      ['Image', `640x128 JPEG, ${own ? 'this crawl\'s own' : `${CRAWL_PLANETS[ctx.base]} (backdrop/planet_${ctx.base})`}`],
      ['Shown', 'behind the 3D from the pan down, rising with the camera']
    ],
    actions: [
      ['Download image', 'The backdrop as a JPEG', download],
      [
        'Replace texture from image…',
        'Scaled to cover 640 x 128; Export mod saves it',
        () => importCrawlBackdrop(ctx)
      ],
      ...(own ? [[
        'Use the game\'s backdrop',
        `Back to ${CRAWL_PLANETS[ctx.base]}`,
        () => saveCrawl(ctx, 'game backdrop', {}, () => editAsset(crawlBackdrop(ctx.name), () => new Uint8Array(0), `Crawl ${ctx.label}: game backdrop`))
      ]] : [])
    ]
  };
}

async function crawlFontOf() {
  if (!crawlFontData)
  {
    const f = crawlFont(romBytes, await rom.read('data/fonts/starwars50'));
    const cv = new OffscreenCanvas(f.cell, f.cell * f.glyphs.size);
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
  if (cut?.crawl)
    showCrawl(cut.crawl.which, { t: cut.t, playing: cut.playing });
}

// The crawl window: its text (its camera and routes are edited with the cutscene pencil, in the cutscene inspector and the view).
function openCrawlPanel(ctx)
{
  const c = ctx.c;
  const box = elem('div', { className: 'crawlEdit' });
  const save = (label, change) => saveCrawl(ctx, label, change);
  const chapter = elem('input', { value: c.chapter, placeholder: 'Chapter V' });
  const title = elem('input', { value: c.title, placeholder: 'THE ICE MOON' });
  const body = elem('textarea', { value: c.body, rows: 6 });
  const note = elem('small', { className: 'shotNote' });

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
  box.append(elem('small', {
    className: 'shotNote',
    textContent: status
  }), stacked(fld('Chapter', chapter, 'Small line above the title; may be empty')), stacked(fld('Title', title)), textRow, note);
  $('crawlBody').replaceChildren(box);
  $('crawlPanel').hidden = false;
  placeCrawlPanel();
}

// Beside the cutscene timeline when there is room, else below it (as the cutscene inspector) and left of the inspector while that is
// open, unless dragged by its title.
let crawlPos = stored('rs64.crawlPos', null);

function placeCrawlPanel()
{
  const p = $('crawlPanel');

  if (p.hidden)
    return;

  const main = p.parentElement.getBoundingClientRect();
  const tl = $('cutPanel').getBoundingClientRect();
  const beside = $('cutPanel').hidden || tl.right + 10 + p.offsetWidth <= main.right - 10;
  const ins = $('cutInspector').hidden ? null : $('cutInspector').getBoundingClientRect();
  const left = crawlPos ? THREE.MathUtils.clamp(crawlPos.x, 0, Math.max(0, main.width - p.offsetWidth)) : (ins ? Math.max(0, ins.left - main.left - p.offsetWidth - 10) : null);
  const top = crawlPos ? THREE.MathUtils.clamp(crawlPos.y, 0, Math.max(0, main.height - 80)) : (beside ? 10 : tl.bottom - main.top + 8);

  Object.assign(p.style, {
    top: `${top}px`,
    left: left == null ? '' : `${left}px`,
    right: left == null ? '' : 'auto',
    maxHeight: `${main.height - top - 10}px`
  });
}

new ResizeObserver(placeCrawlPanel).observe($('cutPanel'));

// The window keeps the size it was dragged to (resizing it writes its inline size).
const crawlSize = stored('rs64.crawlSize', null);

if (crawlSize)
  Object.assign($('crawlPanel').style, { width: `${crawlSize.w}px`, height: `${crawlSize.h}px` });

new ResizeObserver(() => {
  const p = $('crawlPanel');

  if (!p.hidden && p.style.height)
    store('rs64.crawlSize', { w: p.offsetWidth, h: p.offsetHeight });
}).observe($('crawlPanel'));

const crawlHead = $('crawlPanel').querySelector('.rulesPanelHead');
crawlHead.title = 'Drag to move, double-click to put it back';
dragByHead(crawlHead, $('crawlPanel'), 'rs64.crawlPos', (p) => (crawlPos = p), placeCrawlPanel);

setEditIcon($('crawlClose'), 'close');
$('crawlClose').onclick = () => ($('crawlPanel').hidden = true);

// Selects the level object of that name (a unit in Objects mode) and moves the view onto it.
function selectNamed(name)
{
  const find = () => editObjects().find(([, x]) => x.name === name)?.[0];
  let o = find();

  if (o && isUnit(editItemOf(o)) && editMode !== 'objects') {
    setEditMode('objects');
    o = find();
  }

  if (!o)
    return;

  const d = o.getWorldPosition(new THREE.Vector3()).sub(controls.target);

  controls.target.add(d);
  camera.position.add(d);
  select(o);
}

// A spawn action's wave: count copies of the kind on the path, waiting for a free flag the action raises.
function spawnWaveButton(a, dir, done) {
  const b = elem('button', {
    textContent: a.flag ? 'Wave ready' : 'Create wave',
    title: a.path || a.flag ? 'Adds the enemies to the level, waiting off-stage until this action brings them in' : 'Pick a flight path first'
  });

  b.disabled = !!a.flag || !a.kind || !a.count || !a.path;

  b.onclick = async () => {
    const t = await enemyTemplate(a.kind);
    const items = parseDAT(await rom.read(dir + '/dat')).items;
    const path = items.find((x) => x.type === 3 && x.name === a.path);

    if (!t || !path)
      return;

    const waves = await readWaves(dir);
    const name = `rule_${a.path}_${Object.keys(waves).length + 1}`;
    const flag = waveFlagFor(waves, name, levelUsedFlags(await rom.read(dir + '/dat')));

    if (!flag)
      return alert('No free mission flag left in this level');

    const tdv = await rom.read(t.dir + '/dat');
    const rec = new Uint8Array(tdv.buffer, tdv.byteOffset + t.item.offset, tdv.getUint32(t.item.offset + 8)).slice();

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

    if (sel)
      await reselect(sel.it.type, sel.it.index);
    else
      await reloadLevel(dir);
  };

  return b;
}

// Who sets off a trigger area: the player or a named unit of the level (a stock list of several stays as it is until changed).
const whoLabel = (names) => names.map((a) => (a === 'ply' ? 'The player' : a)).join(', ') || 'Nothing';

function triggerWhoRow(it) {
  const row = elem('div', { className: 'shotRow' });
  const cur = (it.activators ?? []).join('|');

  rom.read(selected + '/dat').then((dv) => {
    const units = namedByKind(parseDAT(dv).items, (x) => isUnit(x) || (x.type === 0 && x.sub === 0x27));
    const known = cur === 'ply' || units.some((g) => [...g.children].some((o) => o.value === cur));

    const pick = selectOf([
      ...(known ? [] : [[cur, whoLabel(it.activators ?? [])]]),
      ['ply', 'The player'],
      ...units
    ], cur, { title: 'What has to come into this area for it to fire' });

    pick.onchange = () => editItem(it, (b) => triggerSetActivators(b, it, pick.value.split('|').filter(Boolean)), `Set off by: ${it.name || 'trigger'} -> ${whoLabel([pick.value])}`);
    row.append('Set off by ', pick);
  });

  return row;
}

// What uses the selected object: the mod level's objectives that target it and the level events (rules) that name it. Shown only when
// something does, with a way to the window that edits them.
function usedByRow(it)
{
  const dir = selected;
  const row = elem('div', { className: 'shotRow usedBy', hidden: true });

  Promise.all([rom.read(dir + '/dat'), readRules(dir)]).then(([dv, doc]) => {
    const l = modLevelOf(dir);
    const goals = (l?.objectives ?? []).map((o, k) => [o, k]).filter(([o]) => objectiveTargets(dv, o).some((t) => t.offset === it.offset));
    const rules = it.name ? doc.rules.filter((r) => ruleRefs(r, ruleExt).some((x) => x.name === it.name)).length : 0;
    const parts = goals.map(([o, k]) => `objective ${k + 1} (${o.text || 'no text'})`);

    if (rules)
      parts.push(`${rules} level event${rules > 1 ? 's' : ''}`);

    if (!parts.length)
      return;

    row.append(`Used by ${parts.join(', ')}`);

    if (goals.length)
      row.append(iconButton('edit', 'Objectives', () => openObjectives(dir)));

    row.hidden = false;
  });

  return row;
}

// The game measures roaming in 3D, so a ground unit gets only the slice of the ball at ground level; flyers and probe droids use the whole ball.
function roamGroundNote(it)
{
  if (!it.roamUsers?.some((u) => GROUND_NPCS.has(u.sub)) || !(it.radius > 0))
    return '';

  const dy = Math.abs(levelHeight(it.pos[0], it.pos[2]) - it.pos[1]);
  const reach = Math.sqrt(Math.max(it.radius ** 2 - dy ** 2, 0));

  if (reach >= it.radius * 0.9)
    return '';

  const textContent = reach
    ? `The ground is ${dy.toFixed(1)} units from this area's centre, so ground units here can only roam ${reach.toFixed(1)} of its ${it.radius.toFixed(1)} units. Move the centre to ground level to use all of it.`
    : `The ground is ${dy.toFixed(1)} units from this area's centre, past its ${it.radius.toFixed(1)} unit radius, so ground units here will keep turning back. Move the centre to ground level.`;

  return elem('small', { className: 'shotNote', textContent });
}

// A trigger area's reach (record +0x1C, in level units): how close what sets it off has to come.
const TRIGGER_RADIUS = 0x1C;

function setTriggerRadius(bytes, it, r)
{
  const out = bytes.slice();

  new DataView(out.buffer).setFloat32(it.offset + TRIGGER_RADIUS, r);

  return out;
}

function triggerRadiusRow(it)
{
  const inp = elem('input', { type: 'number', min: 0.5, step: 0.5, value: +(it.radius ?? 0).toFixed(2), title: 'How close it has to come, in level units' });

  inp.onchange = () => {
    const r = parseFloat(inp.value);

    if (Number.isFinite(r) && r > 0)
      editItem(it, (b) => setTriggerRadius(b, it, r), `Radius: ${it.name || 'trigger'} -> ${r}`);
  };

  return elem('label', {}, 'Radius ', inp);
}

// Which of the music panel's collapsible parts are open, so an edit (which rebuilds the panel) doesn't close them.
const musicOpen = {};

function musicDetails(key, summary, ...children)
{
  return elem('details', { open: !!musicOpen[key], ontoggle: (e) => (musicOpen[key] = e.target.open) }, elem('summary', { textContent: summary }), ...children);
}

const musicNames = (names) => {
  const list = [...names];

  return list.slice(0, 2).join(', ') + (list.length > 2 ? ` +${list.length - 2}` : '');
};

// Picks a mission flag or counter by what sets it; one only the mission script sets has no name, so it shows its number.
function musicSourcePick(map, value, what, title, apply)
{
  const s = selectOf([
    [0, 'None'],
    ...[...map].sort((a, b) => a[0] - b[0]).map(([k, names]) => [k, musicNames(names)]),
    ['other', 'Other number…']
  ], value, { title }, (v) => `Mission script ${what} ${v}`);

  s.onchange = () => {
    if (s.value !== 'other')
      return apply(+s.value);

    const n = parseInt(prompt(`${what[0].toUpperCase() + what.slice(1)} number (1-128)`, value || 1), 10);

    if (n >= 1 && n <= 128)
      apply(n);
    else
      s.value = String(value);
  };

  return s;
}

// A music region's settings. With no trigger area it is a level setting that the mission script plays by number, so it shows just its songs.
function musicPanel(region)
{
  const box = elem('div', { className: 'musicPanel' });
  const dir = selected;
  const name = region.name || 'music region';
  const m = region.music;
  const cues = [[0, 'Stop music'], [1, 'Keep current song'], ...parseMusicCues(romBytes).cues.map((c) => [c.cue, c.name])];

  if (!m)
    return box;

  const set = (patch, label) => editItem(sel.it, (b) => musicZoneEdit(b, region, patch), `${label}: ${name}`);
  const row = (label, title, ...controls) => elem('div', { className: 'shotRow', title }, `${label} `, ...controls);

  const cue = (key, label, title) => {
    const s = selectOf(cues, m[key], { title }, (v) => `Unknown song ${v}`);

    s.onchange = () => set({ [key]: +s.value }, label);

    return row(label, title, s);
  };

  const number = (value, title, apply, props) => {
    const inp = elem('input', { type: 'number', step: 'any', value: +value.toFixed(3), title, ...props });

    inp.onchange = () => {
      const n = parseFloat(inp.value);

      if (Number.isFinite(n))
        apply(n);
    };

    return inp;
  };

  const seconds = (key, label, title) => row(label, title, number(m[key], title, (n) => set({ [key]: Math.max(n, 0) }, label), { min: 0 }));

  Promise.all([rom.read(dir + '/dat'), readWaves(dir)]).then(([dv, waves]) => {
    if (dir !== selected)
      return;

    const { flags, counters } = musicConditionSources(dv);

    for (const [wave, flag] of Object.entries(waves))
      (flags.get(flag) ?? flags.set(flag, new Set()).get(flag)).add(`Wave ${wave}`);

    if (!region.musicTrigger)
    {
      box.append(...['enter', 'action', 'after', 'leave'].map((k, i) => cue(k, `Script song ${i + 1}`, 'The mission script plays this by number')));

      return;
    }

    const onOff = (value, title, apply) => {
      const s = selectOf([[1, 'is on'], [0, 'is off']], value, { title }, (v) => `equals ${v}`);

      s.onchange = () => apply(+s.value);

      return s;
    };

    const whenBelow = m.whenThreshold < 0;
    const whenCount = Math.abs(m.whenThreshold);
    const whenTitle = 'When the action song plays. With both set, both must hold; with neither, it never plays';

    const compare = selectOf([['ge', 'at least'], ['lt', 'below']], whenBelow ? 'lt' : 'ge', { title: whenTitle });

    compare.onchange = () => set({ whenThreshold: compare.value === 'lt' ? -Math.max(whenCount, 1) : whenCount }, 'Action song counter');

    const playsKind = m.hold > 0 ? 'time' : (m.loops === 1 ? 'forever' : (m.loops === 0 ? 'random' : 'loops'));
    const playsTitle = 'How long the entry song plays before the earlier song or the Then song takes over';

    const plays = selectOf([
      ['forever', 'Until the player leaves'],
      ['time', 'For some seconds'],
      ['loops', 'For some repeats'],
      ['random', 'For a random number of repeats']
    ], playsKind, { title: playsTitle });

    plays.onchange = () => set({
      forever: { hold: 0, loops: 1 },
      time: { hold: m.hold || 10, loops: 1 },
      loops: { hold: 0, loops: Math.max(m.loops, 2) },
      random: { hold: 0, loops: 0, loopsMax: Math.max(m.loopsMax, 2) }
    }[plays.value], 'Entry song length');

    const playsRow = row('Entry song plays', playsTitle, plays);

    if (playsKind === 'time')
      playsRow.append(number(m.hold, 'Seconds', (n) => set({ hold: Math.max(n, 0.01) }, 'Entry song length'), { min: 0 }));
    else if (playsKind === 'loops')
      playsRow.append(number(m.loops, 'Repeats', (n) => set({ loops: Math.max(Math.round(n), 2) }, 'Entry song length'), { min: 2, step: 1 }));
    else if (playsKind === 'random')
      playsRow.append(number(m.loopsMax, 'At most this many repeats', (n) => set({ loopsMax: Math.max(Math.round(n), 1) }, 'Entry song length'), { min: 1, step: 1 }));

    const more = [
      playsRow,
      ...(playsKind === 'loops' || playsKind === 'random' ? [cue('after', 'Then', 'Plays once the entry song has finished its repeats')] : []),
      row(
        'Enter fades',
        'Seconds to fade the old song out and the new one in',
        number(m.enterFadeOut, 'Old song out, seconds', (n) => set({ enterFadeOut: Math.max(n, 0) }, 'Entering fade'), { min: 0 }),
        number(m.enterFadeIn, 'New song in, seconds', (n) => set({ enterFadeIn: Math.max(n, 0) }, 'Entering fade'), { min: 0 })
      ),
      row(
        'Leave fades',
        'Seconds to fade the old song out and the new one in',
        number(m.leaveFadeOut, 'Old song out, seconds', (n) => set({ leaveFadeOut: Math.max(n, 0) }, 'Leaving fade'), { min: 0 }),
        number(m.leaveFadeIn, 'New song in, seconds', (n) => set({ leaveFadeIn: Math.max(n, 0) }, 'Leaving fade'), { min: 0 })
      ),
      seconds('reenter', 'Re-entry delay', 'Seconds after leaving before the region responds again'),
      row('Works for', 'The region removes itself after the player has left it this many times. 0 means it never does', number(m.uses, 'Times the player can leave. 0 means no limit', (n) => set({ uses: Math.max(Math.round(n), 0) }, 'Uses'), { min: 0, step: 1 }), ' leaves'),
      elem('label', { title: 'Entering cancels a timer another region started' }, elem('input', {
        type: 'checkbox',
        checked: m.restart !== 0,
        onchange: (e) => set({ restart: e.target.checked ? 1 : 0 }, 'Cancel timers')
      }), ' Cancels running timers'),
      row(
        'Active when',
        'The whole region does nothing until this holds',
        musicSourcePick(flags, m.gateFlag, 'flag', 'The whole region does nothing until this holds', (v) => set({
          gateFlag: v,
          gateValue: m.gateValue || 1
        }, 'Active when')),
        ...(m.gateFlag ? [onOff(m.gateValue, 'The value it must have', (v) => set({ gateValue: v }, 'Active when'))] : [])
      ),
      row(
        'Active when count',
        'The whole region does nothing until this counter reaches the number',
        musicSourcePick(counters, m.gateCounter, 'counter', 'The whole region does nothing until this counter reaches the number', (v) => set({ gateCounter: v }, 'Active when')),
        ...(m.gateCounter ? [
          ' at least ',
          number(m.gateAtLeast, 'The number to reach', (n) => set({ gateAtLeast: Math.round(n) }, 'Active when'), { step: 1 })
        ] : [])
      )
    ];

    box.append(
      cue('enter', 'On enter', 'What happens to the music when the player enters the inner box'),
      cue('action', 'Action song', 'Replaces the song while the condition below holds'),
      row(
        'When',
        whenTitle,
        musicSourcePick(flags, m.whenFlag, 'flag', whenTitle, (v) => set({ whenFlag: v, whenValue: m.whenValue || 1 }, 'Action song condition')),
        ...(m.whenFlag ? [onOff(m.whenValue, whenTitle, (v) => set({ whenValue: v }, 'Action song condition'))] : [])
      ),
      row(
        'When count',
        whenTitle,
        musicSourcePick(counters, m.whenCounter, 'counter', whenTitle, (v) => set({ whenCounter: v }, 'Action song condition')),
        ...(m.whenCounter ? [
          compare,
          number(whenCount, whenTitle, (n) => set({ whenThreshold: (whenBelow ? -1 : 1) * Math.max(Math.round(n), whenBelow ? 1 : 0) }, 'Action song counter'), { min: 0, step: 1 })
        ] : [])
      ),
      seconds('linger', 'Linger', 'Seconds the action song keeps playing after the player leaves'),
      cue('leave', 'On leave', 'Keep current song brings back the song from before the player entered'),
      ...musicMarginRows(region),
      musicDetails('more', 'More', ...more)
    );
  });

  return box;
}

// The Exit margin stays as typed for the session; a side's offset is always its real gap minus the margin, so shipped regions stay exact.
const musicMargins = new Map();
const MUSIC_SIDES = ['-X', '+X', '-Z', '+Z'];

function musicMarginRows(region)
{
  const trigger = region.musicTrigger;
  const key = `${selected}/${region.index}`;
  const margin = musicMargins.get(key) ?? musicMargin(region, trigger, ...mapSize());
  const gaps = musicGaps(region, trigger);
  const offsets = gaps.map((g) => g - margin);
  const name = region.name || 'music region';

  const write = (m, next, label) => {
    const box = musicRegionBox(region, trigger, next.map((g) => Math.max(g, 0)));

    musicMargins.set(key, m);

    return editItem(sel.it, (b) => {
      const dv = dvOf(b);

      box.pos.concat(box.size).forEach((v, a) => dv.setFloat32(region.offset + (a < 3 ? 0x10 : 0x1C) + a * 4, v));
    }, label);
  };

  const field = (value, title, apply) => {
    const inp = elem('input', { type: 'number', step: 0.25, value: +value.toFixed(3), title });

    inp.onchange = () => {
      const n = parseFloat(inp.value);

      if (Number.isFinite(n))
        apply(n);
    };

    return inp;
  };

  const setMargin = (n) => {
    const m = Math.max(n, 0);

    write(m, offsets.map((o) => m + o), `Exit margin: ${name} -> ${m}`);
  };

  const sides = MUSIC_SIDES.map((s, k) => elem('label', {}, `${s} `, field(offsets[k], `Added to the exit margin on the ${s} side`, (n) => write(margin, gaps.map((g, j) => (j === k ? margin + n : g)), `Side offset: ${name} ${s} -> ${n}`))));
  const custom = offsets.some((o) => Math.abs(o) > 0.005);

  return [
    elem('label', { title: 'How far past the inner trigger area the player must fly before the song stops' }, 'Exit margin ', field(margin, 'How far past the inner trigger area the player must fly before the song stops', setMargin)),
    musicDetails('sides', custom ? 'Side offsets (custom)' : 'Side offsets', elem('div', { className: 'musicSides' }, ...sides))
  ];
}

// The path a wingman spawn or path-only unit flies; noneLabel names the no-path choice, left out for units that must have a path.
function npcPathRow(it, noneLabel)
{
  const dir = selected;
  const row = elem('div', { className: 'shotRow' });
  const set = (name) => editItem(it, (bytes) => npcSetPath(bytes, it, name), `Path: ${it.name} -> ${name}`, dir);

  rom.read(dir + '/dat').then((dv) => row.append('Path ', npcPathSelect(it, dv, '', noneLabel), rulePathButton(set)));

  return row;
}

// Enemies from any level: the level's own record of that kind, else the first in another level (its gate and path reset, as they
// name that level's flags and splines).
async function enemyTemplate(sub)
{
  const from = UNIT_RECORD_FROM[sub] ?? sub;
  const own = parseDAT(await rom.read(selected + '/dat')).items.find((x) => x.type === 0 && x.sub === from);

  if (own)
    return { dir: selected, item: own };

  for (const p of [...rom.assets.keys()].filter((p) => /^data\/level\/lv_.\/dat$/.test(p)).sort())
  {
    const item = parseDAT(await rom.read(p)).items.find((x) => x.type === 0 && x.sub === from);

    if (item)
      return { dir: p.slice(0, -4), item };
  }

  return null;
}

// The Add palette: what the current edit mode can place, by category, filtered by the search box. Picking one arms placement.
const ENEMY_GROUPS = [
  ['Imperial fighters', [0x0C, 0x12, 0x13, 0x29]],
  ['Imperial ground units', [0x1F, 0x0F, 0x47, 0x25, 0x3B, 0x50, 0x0E, 0x37]],
  ['Imperial transports', [0x24, 0x2C, 0x3E, 0x4D, 0x2F]],
  ['Turrets', [0x0D, 0x1E, 0x28, 0x41, 0x42]],
  ['Rebels and civilians', [0x2E, 0x2B, 0x3C, 0x3D, 0x34, 0x2D]],
  ['Structures', [0x2A, 0x32, 0x44, 0x33, 0x36]],
  ['Other', [0x21, 0x40, 0x43, 0x46, 0x48, 0x4C]],
];

// Stock levels use subtype 0x46 for the Sand Crawler, the record the list copies.
const UNIT_LABELS = { 0x46: 'Sand crawler' };

let addEntries = [];

// Add list pictures: each enemy kind is drawn once off screen, one at a time, and kept for the session. null = no model to show, so it keeps its icon.
const enemyThumbs = new Map();
const THUMB_SIZE = 128;
const ADD_THUMB_ROW = 41;

// Three picture rows and the sideways scrollbar, so the bar does not change size as pictures load.
const ADD_BAR_HEIGHT = 209;

// Kinds the level view doesn't draw as models; the files are the ones load_dat_file_assets loads for them.
const THUMB_MODELS = { 0x31: 'imp_stuff/train#tr_container', 0x33: 'imp_stuff/train_pil', 0x36: 'reb_stuff/r_tr_pil', 0x44: 'imp_stuff/lplatform' };
let thumbQueue = Promise.resolve();

const thumbImg = (url) => `<img class="addThumb" src="${url}" alt="">`;

function queueEnemyThumb(key)
{
  enemyThumbs.set(key, null);

  // Reading a picture back is only a wait on the GPU, so the next kind starts during it. One draw a frame keeps the page smooth.
  thumbQueue = thumbQueue.then(async () => {
    const prop = key.startsWith('prop:') ? key.slice(5) : null;
    const drawn = await drawEnemyThumb(prop ? null : +key.slice(6), prop).catch(() => null);

    drawn?.url.then((url) => {
      enemyThumbs.set(key, url);
      $('addList').querySelector(`[data-key="${key}"] .icon`)?.replaceWith(Object.assign(new Image(), { className: 'addThumb', src: url, alt: '' }));
    }).catch(() => {});

    await new Promise(requestAnimationFrame);
  });
}

async function thumbFromTarget(target)
{
  const pixels = await renderer.readRenderTargetPixelsAsync(target, 0, 0, THUMB_SIZE, THUMB_SIZE);
  const drawing = new OffscreenCanvas(THUMB_SIZE, THUMB_SIZE);

  drawing.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(pixels.buffer, pixels.byteOffset, THUMB_SIZE * THUMB_SIZE * 4), THUMB_SIZE, THUMB_SIZE), 0, 0);
  target.dispose();

  return URL.createObjectURL(await drawing.convertToBlob());
}

async function drawEnemyThumb(sub, model0)
{
  const [path, tableObject] = (model0 ? null : (SUBTYPE_MODEL[sub] ?? THUMB_MODELS[sub]))?.split('#') ?? [];
  const base = 'data/' + path;

  // Kinds without a fixed file (the Sand Crawler) name a model from their level's package on the record.
  const named = model0 || (!path && (await enemyTemplate(sub))?.item.model);
  const id = named ? (await buildObjectIndex()).get(named) : (rom.assets.has(base + '_hi_HOB') ? base + '_hi' : base);
  const objName = named || tableObject;

  if (!id || !rom.assets.has(id + '_HOB'))
    return null;

  const model = await loadModel(id);
  const object = (objName && model.hob.objects.find((o) => o.name === objName)) || model.hob.objects[0];
  const mesh = buildObject(object, model, id, { tris: 0, verts: 0, parts: 0 });
  const stage = new THREE.Scene();

  stage.add(mesh);
  mesh.updateWorldMatrix(true, true);

  const sphere = new THREE.Box3().setFromObject(mesh).getBoundingSphere(new THREE.Sphere());

  if (!(sphere.radius > 0))
    return null;

  const lens = new THREE.PerspectiveCamera(26, 1, sphere.radius * 0.1, sphere.radius * 20);

  lens.position.copy(sphere.center).add(new THREE.Vector3(1, -0.55, 1).normalize().multiplyScalar(sphere.radius * 4.6));
  lens.up.set(0, -1, 0);
  lens.lookAt(sphere.center);

  const target = new THREE.RenderTarget(THUMB_SIZE, THUMB_SIZE, { samples: 4 });
  const held = renderer.getRenderTarget();
  const clear = renderer.getClearColor(new THREE.Color());
  const alpha = renderer.getClearAlpha();

  renderer.setRenderTarget(target);
  renderer.setClearColor(0x000000, 0);
  renderer.render(stage, lens);
  renderer.setRenderTarget(held);
  renderer.setClearColor(clear, alpha);

  return { url: thumbFromTarget(target) };
}

function renderAddList()
{
  const q = $('addSearch').value.trim().toLowerCase();
  const el = $('addList');
  const scrolled = el.scrollLeft;

  el.innerHTML = '';

  let group = null;
  let rows = null;

  for (const e of addEntries)
  {
    if (q && !`${e.label} ${e.group} ${e.find ?? ''}`.toLowerCase().includes(q))
      continue;

    if (e.group !== group)
    {
      group = e.group;
      rows = elem('div', { className: 'addGroupRows' });
      el.append(elem('div', { className: 'addGroup' }, elem('b', { textContent: group }), rows));
    }

    const row = elem('button', { className: 'addRow' + (placing === e.key ? ' active' : ''), title: e.tip ?? '' });

    const thumb = enemyThumbs.get(e.key);

    row.dataset.key = e.key;
    row.innerHTML = `${thumb ? thumbImg(thumb) : e.icon ?? ''}<span>${esc(e.label)}</span>`;

    if (thumb === undefined && (e.key.startsWith('enemy:') || e.key.startsWith('prop:')))
      queueEnemyThumb(e.key);

    // A camera is placed at the view, looking where the view looks, so the shot shows what the modder sees.
    row.onclick = () => {
      if (e.key === 'cam')
        return addCameraAtView(addTemplates.get('cam'));

      objSetPick(null);
      placing = placing === e.key ? null : e.key;
      select(null);
      renderAddList();
    };

    rows.append(row);
  }

  if (!el.childElementCount)
    el.append(elem('small', { className: 'shotNote', textContent: q ? 'Nothing matches.' : 'Nothing to add here.' }));

  el.scrollLeft = scrolled;
  fitAddRows();

  // While placing, the list is covered so the next step is clear: click the ground, or cancel.
  $('addOverlay').hidden = !placing;

  if (!placing)
    dropGhost();

  const hint = placingNeedsPath() ? 'Click a path to fly, or the ground for a new path' : 'Click the ground to place';

  $('addOverlayText').textContent = `${hint}: ${addEntries.find((e) => e.key === placing)?.label ?? 'it'}`;
}

const placingNeedsPath = () => !!placing?.startsWith('enemy:') && UNITS_NEED_PATH.has(+placing.slice(6)) && !UNITS_OWN_PATH.has(+placing.slice(6));

// The flight path under the cursor while placing a unit that must fly one.
function placingPath(e)
{
  if (!placingNeedsPath())
    return null;

  let o = pick(e, visibleMarkers().filter((m) => m.userData.item && isFlightPath(m.userData.item)))?.object;

  while (o && !o.userData.item)
    o = o.parent;

  return o?.userData.item ?? null;
}

// While placing, a see-through marker of the kind follows the cursor over the ground, where a click would put it.
let ghost = null;

function dropGhost() {
  ghost?.removeFromParent();
  ghost = null;
}

const placingTemplate = () => (placing && !placing.startsWith('enemy:') ? addTemplates.get(placing)?.it ?? addTemplates.get(placing) : null);

function placeGhost(e, zone)
{
  const markers = root?.getObjectByName('markers');

  if (!placing || !markers || editMode === 'terrain')
    return dropGhost();

  if (zone || ghost?.userData.kind !== placing || ghost.parent !== markers)
  {
    dropGhost();

    const t = placingTemplate();

    const it = !t ? {
      type: 0,
      sub: placing.startsWith('prop:') ? 0x14 : +placing.slice(6),
      name: '',
      pos: [0, 0, 0],
      rot: [0, 0, 0],
      scale: [1, 1, 1]
    } : {
      ...t,
      pos: zone?.pos ?? [0, 0, 0],
      ...(zone && { size: zone.size, rot: zone.rot })
    };

    ghost = makeMarker(it);

    ghost.traverse((o) => {
      o.raycast = () => {};

      if (!o.material)
        return;

      o.material = o.material.clone();
      Object.assign(o.material, { transparent: true, opacity: 0.55, depthWrite: false });
    });

    ghost.userData = { kind: placing };
    markers.add(ghost);

    if (zone)
      return;
  }

  const path = placingPath(e);
  const hit = path ? new THREE.Vector3(...path.points[1]) : cursorGround(e);

  ghost.visible = !!hit;

  if (hit)
    ghost.position.copy(hit);
}

// The ground point under the cursor in game space (the markers' frame), or null.
function cursorGround(e) {
  const markers = root?.getObjectByName('markers');
  const terrain = root?.getObjectByName('terrain');

  if (markers && terrain?.visible && terrain.userData.tiles)
  {
    const p = terrainRay(e, terrain);

    return p ? markers.worldToLocal(terrain.localToWorld(p)) : null;
  }

  const hit = markers && pick(e, terrain ? [terrain] : [root]);

  return hit ? markers.worldToLocal(hit.point.clone()) : null;
}

canvas.addEventListener('pointermove', (e) => zoneDrag || placeGhost(e));

// The median stock size of each zone kind (type 6 subtype), for a zone placed with a click; a copied zone can be the size of a map.
let zoneSizes = null;

async function zoneDefaultSize(sub) {
  if (!zoneSizes)
  {
    const all = new Map();

    for (const p of [...rom.assets.keys()].filter((p) => /^data\/level\/lv_.\/dat$/.test(p)))
    {
      for (const z of parseDAT(await rom.read(p)).items.filter((x) => x.type === 6 && x.size))
        all.set(z.sub, [...(all.get(z.sub) ?? []), z.size]);
    }

    zoneSizes = new Map([...all].map(([s, l]) => [s, [0, 1, 2].map((a) => l.map((v) => v[a]).sort((x, y) => x - y)[l.length >> 1])]));
  }

  return zoneSizes.get(sub);
}

// Zones are dragged out: press on one corner, release on the opposite one (a click places one of the kind's usual size around it).
let zoneDrag = null;

const zoneFrom = (a, b, t) => ({
  pos: [Math.min(a.x, b.x), a.y, Math.min(a.z, b.z)],
  size: [Math.abs(b.x - a.x), t.size[1], Math.abs(b.z - a.z)],
  rot: [0, 0, 0]
});

canvas.addEventListener('pointerdown', (e) => {
  const t = placingTemplate();

  if (e.button !== 0 || t?.type !== 6 || !(editMode === 'events'))
    return;

  const a = cursorGround(e);

  if (!a)
    return;

  zoneDrag = { a, t };
  controls.enabled = false;
  canvas.setPointerCapture(e.pointerId);
  e.stopImmediatePropagation();
}, true);
canvas.addEventListener('pointermove', (e) => {
  if (!zoneDrag)
    return;

  const b = cursorGround(e);

  if (b)
    placeGhost(e, zoneFrom(zoneDrag.a, b, zoneDrag.t));
}, true);
canvas.addEventListener('pointerup', (e) => {
  if (!zoneDrag)
    return;

  e.stopImmediatePropagation();

  const { a, t } = zoneDrag;
  const b = cursorGround(e) ?? a;
  const z = zoneFrom(a, b, t);
  const src = addTemplates.get(placing);

  zoneDrag = null;
  controls.enabled = true;
  placing = null;
  renderAddList();

  if (z.size[0] >= 0.5 || z.size[2] >= 0.5)
    return addItem(src, z.pos, z);

  zoneDefaultSize(t.sub).then((size = t.size) => addItem(src, [a.x - size[0] / 2, a.y, a.z - size[2] / 2], { size, rot: [0, 0, 0] }));
}, true);
$('addSearch').oninput = renderAddList;

// The wheel scrolls the bar sideways, as it has nothing to scroll vertically.
$('addList').addEventListener('wheel', (e) => {
  const el = e.currentTarget;

  if (el.scrollWidth <= el.clientWidth || e.deltaX)
    return;

  el.scrollLeft += e.deltaY;
  e.preventDefault();
}, { passive: false });

// The bar fills the gap between the controls help and the scene panel, whose sizes change with the mode.
function layoutAddBar()
{
  const bar = $('addBar');
  const help = $('help');
  const scene = $('scene');

  bar.style.left = `${help.offsetLeft + help.offsetWidth + 10}px`;
  bar.style.right = `${scene.offsetWidth ? bar.parentElement.clientWidth - scene.offsetLeft + 10 : 10}px`;
}

// Dragging the bar's top edge makes it taller, and each category then runs down more rows, so it gets narrower and more fit across.
function setAddBarHeight(px)
{
  $('addBar').style.height = `${px || ADD_BAR_HEIGHT}px`;
  fitAddRows();
}

function fitAddRows()
{
  const list = $('addList');
  const head = list.querySelector('.addGroup > b');

  if (!head)
    return list.style.removeProperty('--add-rows');

  // A picture row, whether its picture has loaded yet or not.
  const row = addEntries[0]?.key.startsWith('enemy:') ? ADD_THUMB_ROW : list.querySelector('.addRow').offsetHeight + 1;

  list.style.setProperty('--add-rows', Math.max(3, Math.floor((list.clientHeight - head.offsetHeight - 8) / row)));
}

setAddBarHeight(stored('rs64.addHeight', null));
new ResizeObserver(fitAddRows).observe($('addList'));

$('addResize').onpointerdown = (e) => {
  const handle = $('addResize');
  const bar = $('addBar');
  const start = bar.offsetHeight;
  let px = start === ADD_BAR_HEIGHT ? null : start;

  handle.setPointerCapture(e.pointerId);

  handle.onpointermove = (m) => {
    const wanted = start + e.clientY - m.clientY;

    px = wanted < ADD_BAR_HEIGHT + 24 ? null : Math.min(wanted, bar.parentElement.clientHeight - 20);
    setAddBarHeight(px);
  };

  handle.onpointerup = () => {
    handle.onpointermove = handle.onpointerup = null;
    store('rs64.addHeight', px);
  };
};

const addBarObserver = new ResizeObserver(layoutAddBar);

[$('addBar').parentElement, $('help'), $('scene')].forEach((el) => addBarObserver.observe(el));

function stopPlacing()
{
  placing = null;
  renderAddList();
  attachGizmo();
}

$('addCancel').onclick = stopPlacing;

// Enemies keep placing until stopped; clicks wait for each add to land so two never overlap.
async function placeEnemy(kind, at, path)
{
  placeBusy = true;

  try {
    await (kind.startsWith('prop:') ? addProp(kind, at) : addEnemy(+kind.slice(6), at, path));
  } finally {
    placeBusy = false;
    renderAddList();
  }
}

function fillAddEnemies()
{
  const have = new Set();

  root?.traverse((o) => editItemOf(o)?.type === 0 && have.add(editItemOf(o).sub));
  addEntries = [];

  for (const [group, subs] of ENEMY_GROUPS)
  {
    for (const s of subs.filter((x) => have.has(x) || UNITS_ANY_LEVEL.has(x)).sort((a, b) => SUBTYPES[a].localeCompare(SUBTYPES[b])))
    {
      addEntries.push({ key: 'enemy:' + s, group, label: UNIT_LABELS[s] ?? SUBTYPES[s], find: SUBTYPE_MODEL[s], foreign: !have.has(s), icon: icon(TYPES[0]) });
    }
  }

  if (propCatalog)
  {
    for (const [key, c] of propCatalog)
      addEntries.push({
        key,
        group: lvName(c.dir.split('/').pop()),
        label: c.label,
        find: c.model,
        tip: c.model,
        foreign: c.dir !== selected,
        icon: icon(TYPES[0])
      });
  }
  else
  {
    getPropCatalog().then(() => editMode === 'objects' && fillAddEnemies());
  }

  renderAddList();
}

const unitsOf = (bytes) => parseDAT(new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)).items.filter((x) => x.type === 0);

// A placed unit gets a name of its own: a template's may be one a mission script looks up, which would also block deleting it.
function nameUnit(bytes, unit, base)
{
  const code = romBytes.subarray(0, BLOB_START);
  const name = uniqueName(bytes, base.replace(/[^A-Za-z0-9]+/g, '_'), { has: (n) => nameUses(code, n, 0, 0) > 0 });

  return datNameItem(bytes, unit, name);
}

// A whole train from the DAT `from`, every car on the given path, or on a new one at pos.
async function addTrain(sub, from, pos, path)
{
  const dat = selected + '/dat';
  const units = async () => parseDAT(await rom.read(dat)).items.filter((x) => x.type === 0);
  const first = (await units()).length;

  await historyGroup(`Add ${SUBTYPES[sub]}`, async () => {
    await editAsset(dat, (bytes) => {
      let out = datAddTrain(bytes, from, sub, pos);

      for (let i = first; i < unitsOf(out).length; i++)
        out = nameUnit(out, unitsOf(out)[i], SUBTYPES[sub]);

      return out;
    });

    if (!path)
      await newPathFor((await units())[first]);

    const name = path?.name ?? npcPathGet(await rom.read(dat), (await units())[first]);

    await editAsset(dat, (bytes) => {
      let out = bytes;
      const count = parseDAT(new DataView(out.buffer)).items.filter((x) => x.type === 0).length;

      for (let i = first; i < count; i++)
        out = npcSetPath(out, parseDAT(new DataView(out.buffer)).items.find((x) => x.type === 0 && x.index === i), name);

      return out;
    });
  });

  await reselect(0, first);
}

// Buildings, capital ships and the like by model, from every level's package: 'prop:<model>' -> the first record naming it.
let propCatalog = null;

async function getPropCatalog()
{
  if (propCatalog)
    return propCatalog;

  const found = new Map();

  for (const p of [...rom.assets.keys()].filter((p) => /^data\/level\/lv_.\/dat$/.test(p)).sort())
  {
    const dv = await rom.read(p);
    const inPackage = await packageModels(p.slice(0, -4));

    for (const it of parseDAT(dv).items.filter((x) => x.type === 0 && MODEL_UNITS.has(x.sub)))
    {
      const model = unitModelName(dv, it);
      const key = 'prop:' + model;

      // A kind the game loads by subtype (the power transmitter) names a model outside the packages and needs none of them.
      if (!model || found.has(key) || !(inPackage.has(model) || SUBTYPE_MODEL[it.sub]))
        continue;

      const label = (it.name || model).replace(/[_\s]*\d*$/, '').replace(/_/g, ' ') || model;

      found.set(key, { dir: p.slice(0, -4), item: it, model, label });
    }
  }

  return propCatalog = found;
}

const packageCache = new Map();

// Model names in a level folder's own packages.
async function packageModels(dir)
{
  if (packageCache.has(dir))
    return packageCache.get(dir);

  const names = new Set();

  for (const pk of ['opkg', 'opkg2'])
    if (rom.assets.has(`${dir}/${pk}_HOB`))
      parseHOB(await rom.read(`${dir}/${pk}_HOB`)).objects.forEach((o) => names.add(o.name));

  packageCache.set(dir, names);

  return names;
}

// Every level any level of this mod borrows from.
async function borrowedPackages()
{
  const found = new Set();

  for (const p of [...dirty].filter((x) => x.endsWith('/packages.json')))
  {
    try {
      JSON.parse(await virtualText(p) || '{}').levels?.forEach((lv) => found.add(lv));
    } catch { /* an unreadable file borrows nothing */ }
  }

  return found;
}

let textureClashes = null;

// Texture names that the level packages and shared unit models use with different pixels, size or format.
async function getTextureClashes()
{
  if (textureClashes)
    return textureClashes;

  const seen = new Map();
  const clashes = new Set();

  for (const p of [...rom.assets.keys()].filter((x) => /_HMT$/.test(x) && /^data\/(level\/lv_.\/opkg|imp_stuff|gnrc_stuff|reb_stuff|pl_crafts)/.test(x)))
  {
    let textures;

    try {
      textures = parseHMT(await rom.read(p)).textures;
    } catch {
      continue;
    }

    for (const t of textures)
    {
      const key = `${t.w}x${t.rows}.${t.type}.${t.depth}.${t.flags}.${t.rgba.reduce((h, v) => Math.imul(h ^ v, 16777619), 2166136261)}`;

      if (seen.has(t.name) && seen.get(t.name) !== key)
        clashes.add(t.name);

      seen.set(t.name, key);
    }
  }

  return textureClashes = clashes;
}

// The level's packages.json: the given level joins, and a level none of this level's objects still need leaves.
async function syncPackages(dir, add)
{
  const path = PACKAGES_PATH(rulesFolderOf(dir));
  const text = await virtualText(path);
  let current = [];

  try {
    current = JSON.parse(text || '{}').levels ?? [];
  } catch { /* an unreadable file starts over */ }

  const dv = await rom.read(dir + '/dat');
  const named = new Set(parseDAT(dv).items.filter((x) => x.type === 0).map((x) => unitModelName(dv, x)));
  const own = await packageModels(dir);
  const keep = [];

  for (const lv of new Set(add ? [...current, add] : current))
    if ([...await packageModels('data/level/' + lv)].some((m) => named.has(m) && !own.has(m)))
      keep.push(lv);

  // Level order, which is the order the Add list prefers a model that several levels have.
  keep.sort();

  const next = JSON.stringify({ version: 1, levels: keep }) + '\n';

  if (keep.join() === current.join())
    return;

  await editText(path, next, 'Level packages');

  const clashes = await getTextureClashes();
  const borrowed = await borrowedPackages();

  for (const lv of keep.filter((x) => !current.includes(x)))
    await editAsset(`data/level/${lv}/opkg_HMT`, (bytes) => hmtRenameTextures(bytes, lv, clashes));

  for (const lv of current.filter((x) => !borrowed.has(x)))
    await editAsset(`data/level/${lv}/opkg_HMT`, (bytes) => hmtRestoreNames(bytes, edits.get(`data/level/${lv}/opkg_HMT`)));
}

// Borrowed packages, by size on disk. Mos Eisley ran out of memory at about 850 KB while loading them, and the mission allocates more once it runs.
const PACKAGE_BUDGET = 500 * 1024;
const packageSize = (lv) => ['HOB', 'HMT'].reduce((n, x) => n + (rom.assets.get(`data/level/${lv}/opkg_${x}`)?.size ?? 0), 0);

async function packageFits(dir, lv, model)
{
  if ((await packageModels(dir)).has(model) || !(await packageModels('data/level/' + lv)).has(model))
    return true;

  let current = [];

  try {
    current = JSON.parse(await virtualText(PACKAGES_PATH(rulesFolderOf(dir))) || '{}').levels ?? [];
  } catch { /* an unreadable file counts as empty */ }

  return [...new Set([...current, lv])].reduce((n, l) => n + packageSize(l), 0) <= PACKAGE_BUDGET;
}

async function addProp(key, pos)
{
  const c = (await getPropCatalog()).get(key);

  if (!await packageFits(selected, c.dir.split('/').pop(), c.model))
  {
    alert('This level already uses as many models from other levels as the game has memory for. Delete some of them first.');

    return;
  }

  const dv = await rom.read(c.dir + '/dat');
  const rec = new Uint8Array(dv.buffer, dv.byteOffset + c.item.offset, dv.getUint32(c.item.offset + 8)).slice();
  const dat = selected + '/dat';

  await historyGroup(`Add ${c.label}`, async () => {
    await editAsset(dat, (bytes) => {
      const out = datAddEnemy(bytes, rec, pos, undefined, c.dir !== selected);

      return nameUnit(out, unitsOf(out).at(-1), c.label);
    });

    await syncPackages(selected, c.dir.split('/').pop());
  });

  await reselect(0, parseDAT(await rom.read(dat)).items.filter((x) => x.type === 0).length - 1);
}

// path: a flight path of this level for it to start on. A unit that must have a path gets a new one at pos when none is given.
async function addEnemy(sub, pos, path)
{
  const t = await enemyTemplate(sub);

  if (!t)
    return;

  const dv = await rom.read(t.dir + '/dat');
  const rec = new Uint8Array(dv.buffer, dv.byteOffset + t.item.offset, dv.getUint32(t.item.offset + 8)).slice();
  const label = `Add ${SUBTYPES[sub]}`;

  new DataView(rec.buffer).setUint16(0, sub);

  if (TRAIN_UNITS.has(sub))
    return addTrain(sub, u8(dv), path ? path.points[1] : pos, path);

  await historyGroup(label, async () => {
    await editAsset(selected + '/dat', (bytes) => {
      const out = datAddEnemy(bytes, rec, path ? path.points[1] : pos, path?.name, t.dir !== selected);

      return nameUnit(out, unitsOf(out).at(-1), SUBTYPES[sub]);
    }, label);

    const added = parseDAT(await rom.read(selected + '/dat')).items.filter((x) => x.type === 0).at(-1);

    // After the unit reads back, so the guns come after it in the list and it stays the one selected below.
    if (sub === 0x37)
      await editAsset(selected + '/dat', (bytes) => datAddDevastatorGuns(bytes, u8(dv), rec));

    if (UNITS_NEED_PATH.has(sub) && !path)
      await newPathFor(added);
    else
      await reselect(0, added.index);
  });
}

function nameRow(it)
{
  const row = elem('label', { textContent: 'Name' });

  const inp = elem('input', {
    type: 'text',
    value: it.name ?? '',
    maxLength: 47,
    placeholder: '(unnamed)',
    title: 'Rules, camera shots and NPCs flying it follow a new name'
  });

  inp.onchange = () => renameObject(it, inp.value.trim()).catch((e) => {
    alert(e.message);
    inp.value = it.name ?? '';
  });

  row.append(inp);

  return row;
}

const HELP_TOOL_NAMES = { view: 'select', translate: 'move', rotate: 'rotate', scale: 'scale' };

// The shortcut rows that only apply to the current selection.
const HELP_WHEN = {
  selected: () => !!sel,
  tools: () => sel && !sel.fixed && editTools(sel).length,
  axes: () => HELP_WHEN.tools() && !sel.it.pathEvent,
  duplicate: () => sel && !sel.fixed && !sel.it.pathEvent,
  remove: () => sel && canDelete(sel.it),
  path: () => sel && sel.it.type === 3 && sel.it.sub === 3 && sel.it.points?.length >= 4 && !sel.pathNode
};

function syncHelp()
{
  for (const el of document.querySelectorAll('#help [data-when]'))
    el.hidden = !el.dataset.modes.split(' ').includes(editMode ?? 'none') || !HELP_WHEN[el.dataset.when]();

  if (!HELP_WHEN.tools())
    return;

  const on = ['view', ...editTools(sel)];
  const keys = Object.entries(TOOL_KEYS).filter(([, tool]) => on.includes(tool));

  $('helpToolKeys').textContent = keys.map(([code]) => code.slice(3)).join(' ');
  $('helpToolNames').textContent = keys.map(([, tool]) => HELP_TOOL_NAMES[tool]).join(', ');
}

function syncSelPanel()
{
  syncHelp();

  const it = sel?.it;

  $('selInfo').textContent = !sel
    ? `Click ${editMode === 'objects' ? 'an object' : 'an event marker'} to select it`
    : `${it.name || '(unnamed)'}: ${it.type === 0 && it.model ? it.model : TYPES[markerKind(it)]?.label ?? 'item'}` + (sel.fixed ?? '');

  const f = $('selFields');

  f.textContent = '';

  if (sel && !sel.fixed && !sel.pathNode)
    f.append(nameRow(it));

  if (sel?.pathNode)
  {
    const row = elem('label');

    row.textContent = `Along ${sel.pathNode.s.name || 'spline'} %`;

    const inp = elem('input', { type: 'number', step: 'any', min: 0, max: 100, value: +(sel.pathAt * 100).toFixed(2) });

    inp.onchange = () => {
      const n = parseFloat(inp.value);

      if (!Number.isFinite(n))
        return;

      slidePathEvent(THREE.MathUtils.clamp(n / 100, 0, 1));
      commitSel();
    };

    row.append(inp);
    f.append(row);

    if (ORDER_TARGETS[it.sub])
      f.append(orderTargetRow(it));
  }
  else if (sel && !sel.fixed)
    for (const [label, , key] of PLACE_FIELDS)
    {
      // Only placed models use their scale; the other type-0 markers ignore it.
      if (!Array.isArray(it[key]) || (key === 'scale' && !sel.o.userData.placedItem) || (key === 'rot' && isMusicZone(it)))
        continue;

      const row = elem('label');

      row.textContent = label;

      it[key].forEach((v, a) => {
        const inp = elem('input', { type: 'number', step: 'any', value: +v.toFixed(3), title: 'XYZ'[a] });

        inp.onchange = () => {
          const n = parseFloat(inp.value);

          if (!Number.isFinite(n))
            return;

          const d = key === 'pos' ? n - it.pos[a] : 0;

          it[key] = it[key].slice();
          it[key][a] = n;

          if (it.points && d)
            for (const pt of it.points)
              pt[a] += d;

          itemToProxy();
          showItemChange();
          commitSel();
        };

        row.append(inp);
      });

      f.append(row);
    }

  if (sel && editMode === 'events' && isTriggerArea(it))
    f.append(triggerWhoRow(it), triggerRadiusRow(it));

  if (sel && editMode === 'events' && markerKind(it) === 'troam')
    f.append(roamGroundNote(it));

  if (sel && !sel.pathNode && (editMode === 'objects' || editMode === 'events'))
    f.append(usedByRow(it));

  if (sel && editMode === 'events' && isTriggerArea(it) && it.name)
    f.append(shotLinkRow(it), destroyRow(it), waveTriggerRow(it));

  if (sel && editMode === 'objects' && it.type === 0 && WAVE_FIELDS[it.sub])
    f.append(waveEnemyRow(it));

  if (sel && editMode === 'events' && it.type === 0 && it.sub === 0x27)
    f.append(npcPathRow(it, 'None (stays put)'));

  if (sel && editMode === 'objects' && it.type === 0 && UNITS_NEED_PATH.has(it.sub) && !UNITS_OWN_PATH.has(it.sub))
    f.append(npcPathRow(it));

  if (sel && editMode === 'events' && it.type === 7 && it.pathEvent)
    f.append(rulesSection({ kind: 'path', it }));
  else if (sel && editMode === 'events' && it.type === 7 && it.onEnter)
    f.append(rulesSection({ kind: 'area', it }));

  if (sel && editMode === 'objects' && isUnit(it))
    f.append(rulesSection({ kind: 'unit', it }));

  if (sel && editMode === 'events' && it.waypoint)
    f.append(arrowPanel(it));

  const musicRegion = it?.type === 6 ? it.musicRegion ?? (it.sub === 0x10 ? it : null) : null;

  if (sel && editMode === 'events' && musicRegion)
    f.append(musicPanel(musicRegion));

  if (sel && editMode === 'events' && eventHelpUrl(it))
    $('selInfo').append(' ', eventHelpLink(it));

  if (sel && editMode === 'events' && isFlightPath(it) && !sel.pathNode)
    f.append(pathAttackRow(it));

  $('btnDup').disabled = !sel || !!sel.fixed || !!it.pathEvent;
  $('btnDel').disabled = !sel || !canDelete(it);

  $('btnObjectives').hidden = !modLevelOf(selected);

  if (objDir && objDir !== selected)
    closeObjectives();

  if (levelPanelDir !== selected)
    $('levelPanel').hidden = true;

  if (rulesPanelDir !== selected)
    $('rulesPanel').hidden = true;

  if (lookPanelDir !== selected)
    $('lookPanel').hidden = true;

  hud();
}

const MUSIC_NEW_MARGIN = 5;

// Copies an item's record with a new position and appends it as the last of its kind, so no existing index moves.
// src is an item of this level, or { it, dv } from another level's DAT (copied inert). zone: { size, rot } written over a zone's footprint (+0x28) and rotation (+0x1C) when it was dragged out.
async function addItem(src, pos, zone, rot = null)
{
  const it = src.it ?? src;
  const dir = selected;
  const dv = src.dv ?? (await rom.read(dir + '/dat'));
  let rec = new Uint8Array(dv.buffer, dv.byteOffset + it.offset, dv.getUint32(it.offset + 8)).slice();

  if (src.dv)
    rec = recordInert(rec, it);

  pos.forEach((v, a) => new DataView(rec.buffer).setFloat32(0x10 + a * 4, v));

  if (rot)
    rot.forEach((v, a) => new DataView(rec.buffer).setFloat32(0x1C + a * 4, v));

  if (zone)
    zone.size.concat(zone.rot).forEach((v, a) => new DataView(rec.buffer).setFloat32((a < 3 ? 0x28 : 0x1C - 12) + a * 4, v));

  // A copy keeps its source's name; one that rules name (its source deleted or not) gets its own, so it never takes over their rules.
  const ruled = await ruleNames(dir);

  const newest = (bytes) => parseDAT(dvOf(bytes)).items.filter((x) => x.type === it.type).reduce((a, b) => (a && a.index >= b.index ? a : b), null);

  await historyGroup(`Add ${it.name || TYPES[markerKind(it)]?.label || 'item'}`, async () => {
    await editAsset(dir + '/dat', (bytes) => datAddItem(bytes, it.type, rec));

    // A trigger area from another level would still wait for that level's units, and may be sized for them, so the copy is set off by the
    // player and reaches at least 10 units.
    if (src.dv && isTriggerArea(it))
    {
      await editAsset(dir + '/dat', (bytes) => {
        const set = triggerSetActivators(bytes, newest(bytes), ['ply']);

        return setTriggerRadius(set, newest(set), Math.max(it.radius ?? 0, 10));
      });
    }

    if (it.name && ruled.has(it.name))
      await editAsset(dir + '/dat', (bytes) => datNameItem(bytes, newest(bytes), uniqueName(bytes, it.name, ruled)));

    // A linked music region gets its own trigger area; sharing the source's would leave the new region with no way to start its song.
    if (it.type === 6 && it.sub === 0x10 && it.zoneLinks?.length)
    {
      const t = it.musicTrigger ? { it: it.musicTrigger, dv } : addTemplates.get('zmtrig') ?? (await foreignTemplates(dir)).get('zmtrig');
      const tDv = t.dv ?? dv;
      const tRec = new Uint8Array(tDv.buffer, tDv.byteOffset + (t.it ?? t).offset, tDv.getUint32((t.it ?? t).offset + 8)).slice();

      await editAsset(dir + '/dat', (bytes) => musicAddTrigger(bytes, newest(bytes), tRec, MUSIC_NEW_MARGIN, ...mapSize()));
    }
  });

  await reloadLevel(dir);

  const last = editObjects().filter(([, x]) => x.type === it.type && x.sub === it.sub).reduce((a, b) => (a && a[1].index >= b[1].index ? a : b), null);

  if (last)
    select(last[0]);
}

// The view as a camera position in level terms: the eye, and the ZYX rotation (degrees) that turns the camera marker's -Z onto the view's.
function viewAsCameraPose()
{
  const space = root.getObjectByName('markers');
  const worldQ = space.getWorldQuaternion(new THREE.Quaternion());
  const q = worldQ.invert().multiply(camera.getWorldQuaternion(new THREE.Quaternion()));
  const e = new THREE.Euler().setFromQuaternion(q, 'ZYX');

  return {
    pos: space.worldToLocal(camera.getWorldPosition(new THREE.Vector3())).toArray(),
    rot: [e.x, e.y, e.z].map((r) => THREE.MathUtils.radToDeg(r))
  };
}

// A new sequence camera framed like the view, aiming at nothing and leading nowhere, under a name of its own.
async function addSeqCamera(dir, base)
{
  const { it, dv } = (await foreignTemplates(dir)).get('cam');
  const rec = recordInert(new Uint8Array(dv.buffer, dv.byteOffset + it.offset, dv.getUint32(it.offset + 8)).slice(), it);
  const r = dvOf(rec);
  const { pos, rot } = viewAsCameraPose();

  pos.forEach((v, a) => r.setFloat32(0x10 + a * 4, v));
  rot.forEach((v, a) => r.setFloat32(0x1C + a * 4, v));
  r.setFloat32(0x60, 3);

  let made = null;

  await editAsset(dir + '/dat', (bytes) => {
    const newest = (b) => parseDAT(dvOf(b)).items.filter((x) => x.type === 0).reduce((a, x) => (a.index >= x.index ? a : x));
    let out = datAddItem(bytes, 0, rec);
    const name = uniqueName(out, base);

    out = datNameItem(out, newest(out), name);
    out = recordSetName(out, newest(out), 0x64, '-');
    out = recordSetName(out, newest(out), 0x68, '-');
    made = { name, index: newest(out).index };

    return out;
  }, `New camera ${base}`);

  return made;
}

async function addCameraAtView(src)
{
  if (!src)
    return;

  const { pos, rot } = viewAsCameraPose();

  await addItem(src, pos, null, rot);
}

// Dragging an Audio row by its grip (a click on the row still opens it) onto a cutscene's Audio lane. Esc or a drop elsewhere does nothing.
let listDrag = null;

function dragGrip(r)
{
  const g = elem('span', { className: 'grip', draggable: true, title: 'Drag onto the Audio lane of a cutscene you are editing' });

  setEditIcon(g, 'grip');
  g.onclick = (e) => e.stopPropagation();

  g.ondragstart = (e) => {
    listDrag = { audio: r.row, label: r.label };
    e.dataTransfer.effectAllowed = 'copy';
    e.dataTransfer.setData('text/plain', r.label);
  };

  g.ondragend = () => {
    listDrag = null;
    document.querySelectorAll('.dropOk').forEach((x) => x.classList.remove('dropOk'));
  };

  return g;
}

// el takes a dragged row while ok() holds, highlighted under it.
function listDropTarget(el, ok, onDrop) {
  el.ondragover = (e) => {
    if (!ok())
      return;

    e.preventDefault();
    e.dataTransfer.dropEffect = 'copy';
    el.classList.add('dropOk');
  };

  el.ondragleave = () => el.classList.remove('dropOk');

  el.ondrop = (e) => {
    el.classList.remove('dropOk');

    if (!ok())
      return;

    e.preventDefault();
    e.stopPropagation();
    onDrop(e);
  };
}

// A music trigger area is duplicated through its region, which makes the copy's own trigger.
function duplicateSel()
{
  const it = sel.it.musicRegion ?? sel.it;

  addItem(it, [it.pos[0] + 4, it.pos[1], it.pos[2]]);
}

$('btnDup').onclick = () => sel && !sel.fixed && !sel.it.pathEvent && duplicateSel();
$('btnDel').onclick = () => sel && canDelete(sel.it) && deleteItem(sel.it);

// Event kinds any level can add, copied from another level when this one has none (a blank level starts with only its player start):
// the template's stock DAT first, then the stock levels in order.
const FOREIGN_KINDS = ['player', 'wing', 'cam', 'zhi', 'zlo', 'zmus', 'zunit', 'zmtrig', 'tvoice', 'tevent', 'tspawn', 'troam', 'twaypoint'];

async function foreignTemplates(dir)
{
  const tmpl = modLevelOf(dir)?.template;
  const first = tmpl != null ? `data/level/lv_${tmpl.toString(36)}/dat` : null;
  const out = new Map();
  const dats = [...rom.assets.keys()].filter((k) => /^data\/level\/lv_[0-9a-i]\/dat$/.test(k)).sort((a, b) => (b === first) - (a === first) || a.localeCompare(b));

  for (const p of dats) {
    const dv = await rom.read(p);

    for (const it of parseDAT(dv).items)
    {
      const k = String(markerKind(it));

      if (FOREIGN_KINDS.includes(k) && !it.pathEvent && !out.has(k) && (k !== 'zmus' || it.zoneLinks?.length))
        out.set(k, { it, dv });
    }
  }

  return out;
}

async function fillAddKinds() {
  addTemplates = new Map();

  for (const m of root?.getObjectByName('markers')?.children ?? [])
  {
    const it = m.userData.item;

    if (!it || it.pathEvent)
      continue;

    const k = String(markerKind(it));

    if (!addTemplates.has(k) && (k !== 'zmus' || it.zoneLinks?.length))
      addTemplates.set(k, it);
  }

  const local = new Set(addTemplates.keys());
  const dir = selected;

  for (const [k, t] of await foreignTemplates(dir))
    if (!addTemplates.has(k))
      addTemplates.set(k, t);

  if (dir !== selected || editMode !== 'events')
    return;

  addEntries = [];

  for (const [group, kinds] of LEGEND_GROUPS)
  {
    for (const k of kinds ?? [...addTemplates.keys()].filter((x) => x.startsWith('path:') || x === '3'))
      if (addTemplates.has(k))
        addEntries.push({ key: k, group, label: TYPES[k]?.label ?? k, tip: TYPES[k]?.tip, foreign: !local.has(k), icon: TYPES[k] ? icon(TYPES[k]) : '' });
  }

  renderAddList();
}

// Left clicks in Objects / Events mode select (or place) instead of opening the info card.
function editClick(e) {
  if (editMode !== 'objects' && editMode !== 'events')
    return false;

  if (gizmo.axis || placeBusy)
    return true;

  if (rulePathPick)
  {
    rulePathClick(e);

    return true;
  }

  if (objPick)
  {
    if (objPick.row.goal === 'reach')
      objPickArea(e);
    else
      objToggleUnit(editHit(e));

    return true;
  }

  if (placing)
  {
    const path = placingPath(e);

    if (path)
      return placeEnemy(placing, null, path), true;

    const terrain = root.getObjectByName('terrain');
    const hit = pick(e, terrain ? [terrain] : [root]);

    if (!hit)
      return true;

    const kind = placing;
    const at = root.worldToLocal(hit.point.clone()).toArray();

    if (kind.startsWith('enemy:') || kind.startsWith('prop:'))
      return placeEnemy(kind, at), true;

    placing = null;
    renderAddList();
    addItem(sel && String(markerKind(sel.it)) === kind && (kind !== 'zmus' || sel.it.musicTrigger) ? sel.it : addTemplates.get(kind), at);

    return true;
  }

  select(editHit(e));

  return true;
}

const EDIT_MODES = ['terrain', 'objects', 'events'];

// F: frames what is selected in the level or cutscene editor from the current viewing angle. Returns whether there was something to frame.
function focusSelected()
{
  const tr = cutEditing && cut?.file ? cutTrack : null;
  const o = tr ? (tr.actor != null ? cut.nodes[tr.actor] : tr.holder ?? editProxy) : ((editMode === 'objects' || editMode === 'events') && sel?.o);

  if (!o)
    return false;

  if (tr && $('cutCam').checked)
    cutFreeView();

  o.updateWorldMatrix(true, true);

  const box = new THREE.Box3().setFromObject(o);
  const sphere = box.isEmpty() ? new THREE.Sphere(o.getWorldPosition(new THREE.Vector3()), 0.5) : box.getBoundingSphere(new THREE.Sphere());
  const away = camera.position.clone().sub(controls.target).normalize();
  const reach = Math.max(sphere.radius, 0.3) / Math.sin(THREE.MathUtils.degToRad(camera.fov) / 2);

  controls.target.copy(sphere.center);
  camera.position.copy(sphere.center).addScaledVector(away, reach * 1.3);
  controls.update();

  return true;
}

function editKey(e)
{
  // Tab / Shift+Tab cycle the edit modes while editing.
  if (editMode && e.code === 'Tab' && !e.ctrlKey && !e.altKey && !e.metaKey)
  {
    e.preventDefault();
    setEditMode(EDIT_MODES[(EDIT_MODES.indexOf(editMode) + (e.shiftKey ? 2 : 1)) % 3]);

    return true;
  }

  if (editMode === 'terrain')
    return sculptActive() && sculptKey(e);

  if (editMode !== 'objects' && editMode !== 'events')
    return false;

  if (TOOL_KEYS[e.code] && !rmbHeld && !e.ctrlKey && !e.metaKey && !e.altKey)
    setTool(TOOL_KEYS[e.code]);
  else if (e.code === 'KeyX' && !e.ctrlKey && !e.metaKey)
    setEditSpace(editSpace === 'world' ? 'local' : 'world');
  else if (DELETE_KEYS.has(e.key) && sel && canDelete(sel.it))
    deleteItem(sel.it);
  else if (e.code === 'KeyD' && (e.ctrlKey || e.metaKey) && sel && !sel.fixed && !sel.it.pathEvent)
  {
    e.preventDefault();
    duplicateSel();
  }
  else if (e.code === 'Escape') {
    if (rulePathPick)
      return stopRulePathPick(), true;

    if (objPick)
      return objSetPick(null), true;

    if (!placing)
      select(null);

    stopPlacing();
  } else
    return false;

  return true;
}

// Zones in Scale mode also get corner handles: dragging one moves that corner over the ground while the opposite corner stays put. A zone
// spans pos .. pos + R * size (X/Z), as makeMarker draws it.
const cornerMat = new THREE.MeshBasicMaterial({ color: 0xe8b84a, depthTest: false, transparent: true });
const innerCornerMat = new THREE.MeshBasicMaterial({ color: TYPES.zmtrig.color, depthTest: false, transparent: true });
const cornerGeo = new THREE.SphereGeometry(1, 12, 8);
const _cw = new THREE.Vector3();

// Constant on-screen size: about 1.2% of the distance to the camera.
const sizeCorners = () => corners.forEach((h) => h.scale.setScalar(camera.position.distanceTo(h.getWorldPosition(_cw)) * 0.012));
let corners = [];
let cornerDrag = null;

function cornerPoint(it, cu, cv)
{
  const p = new THREE.Vector3(cu * it.size[0], 0, cv * it.size[2]).applyMatrix4(placementRotation(it.rot)).add(new THREE.Vector3(...it.pos));

  p.y = levelHeight(p.x, p.z) - 0.2;

  return p;
}

// A selected path gets a handle per control point: drag moves it over the ground, Shift+drag changes its height, Ctrl+click adds a
// point after it, Alt+click deletes it.
const pathPointMat = new THREE.MeshBasicMaterial({ color: 0x4ac0e8, depthTest: false, transparent: true });

function showCorners()
{
  for (const c of corners)
    c.removeFromParent();

  corners = [];

  if (sel && editMode === 'events' && sel.it.type === 3 && sel.it.sub === 3 && sel.it.points?.length >= 4 && !sel.pathNode)
  {
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

  if (!sel || sel.it.type !== 6 || editTool !== 'scale')
    return;

  for (const [target, mat] of [[sel.it, cornerMat], ...(sel.it.musicTrigger ? [[sel.it.musicTrigger, innerCornerMat]] : [])])
  {
    for (const [cu, cv] of [[0, 0], [1, 0], [1, 1], [0, 1]]) {
      const h = new THREE.Mesh(cornerGeo, mat);

      h.position.copy(cornerPoint(target, cu, cv));
      h.renderOrder = 10;
      h.userData.corner = [cu, cv];
      h.userData.cornerItem = target;
      sel.o.parent.add(h);
      corners.push(h);
    }
  }
}

// Capture-phase listeners run before the orbit controls and the gizmo, so a corner drag neither orbits nor reselects.
canvas.addEventListener('pointerdown', (e) => {
  if (e.button !== 0 || !corners.length)
    return;

  const h = pick(e, corners);

  if (!h)
    return;

  if (h.object.userData.pathPoint !== undefined && (e.ctrlKey || e.altKey))
  {
    e.stopImmediatePropagation();
    e.preventDefault();

    const it = sel.it;
    const k = h.object.userData.pathPoint;
    const pts = it.points.map((p) => p.slice());

    if (e.altKey) {
      if (pts.length <= 4)
        return alert('A path needs at least 4 points.');

      pts.splice(k, 1);
    } else
    {
      const a = pts[k];
      const b = pts[k + 1] ?? a.map((v, ax) => 2 * v - pts[k - 1][ax]);

      pts.splice(k + 1, 0, a.map((v, ax) => (v + b[ax]) / 2));
    }

    return setPathPoints(it, pts, `${e.altKey ? 'Delete' : 'Add'} point ${k + (e.altKey ? 1 : 2)}`);
  }

  if (h.object.userData.pathPoint !== undefined)
  {
    cornerDrag ={ point: h.object.userData.pathPoint, handle: h.object, vertical: e.shiftKey };
    controls.enabled = false;
    canvas.setPointerCapture(e.pointerId);
    e.stopImmediatePropagation();

    return;
  }

  const it = h.object.userData.cornerItem;
  const rot = placementRotation(it.rot);
  const [cu, cv] = h.object.userData.corner;
  const A = new THREE.Vector3((1 - cu) * it.size[0], 0, (1 - cv) * it.size[2]).applyMatrix4(rot).add(new THREE.Vector3(...it.pos));

  cornerDrag = {
    it,
    o: it === sel.it ? sel.o : markerOf(sel.o, it),
    cu,
    cv,
    A,
    rot,
    inv: rot.clone().invert(),
    y: h.object.position.y,
    pos0: new THREE.Vector3(...it.pos)
  };

  controls.enabled = false;
  canvas.setPointerCapture(e.pointerId);
  e.stopImmediatePropagation();
}, true);
canvas.addEventListener('pointermove', (e) => {
  if (!cornerDrag)
    return;

  e.stopImmediatePropagation();

  const r = canvas.getBoundingClientRect();
  const c = cornerDrag;
  const it = c.it ?? sel.it;
  const o = c.o ?? sel.o;

  pointer.set(((e.clientX - r.left) / r.width) * 2 - 1, -((e.clientY - r.top) / r.height) * 2 + 1);
  raycaster.setFromCamera(pointer, camera);

  if (c.point !== undefined)
  {
    // Over the ground: a level plane through the point; height: an upright plane through it facing the camera.
    const at = root.localToWorld(c.handle.position.clone());
    const up = root.localToWorld(new THREE.Vector3(0, 1, 0)).sub(root.localToWorld(new THREE.Vector3()));
    const normal = c.vertical ? camera.getWorldDirection(new THREE.Vector3()).projectOnPlane(up).normalize() : up.normalize();
    const w = raycaster.ray.intersectPlane(new THREE.Plane().setFromNormalAndCoplanarPoint(normal, at), new THREE.Vector3());

    if (!w)
      return;

    const l = root.worldToLocal(w);
    const p = c.handle.position;

    if (c.vertical)
      p.y = l.y;
    else
      p.set(l.x, p.y, l.z);

    it.points[c.point] = p.toArray();

    return;
  }

  const plane = new THREE.Plane().setFromNormalAndCoplanarPoint(new THREE.Vector3(0, 1, 0), root.localToWorld(new THREE.Vector3(0, c.y, 0)));
  const w = raycaster.ray.intersectPlane(plane, new THREE.Vector3());

  if (!w)
    return;

  const d = root.worldToLocal(w).sub(c.A).applyMatrix4(c.inv);

  it.size = [c.cu ? d.x : -d.x, it.size[1], c.cv ? d.z : -d.z];

  const p = new THREE.Vector3(c.cu ? 0 : d.x, 0, c.cv ? 0 : d.z).applyMatrix4(c.rot).add(c.A);

  it.pos = [p.x, c.pos0.y, p.z];

  drapeZone(o, it);

  if (it.musicRegion)
    drapeZone(sel.o, { ...it.musicRegion, ...musicRegionBox(it.musicRegion, it, sel.base.gaps) });

  corners.forEach((h) => h.position.copy(cornerPoint(h.userData.cornerItem, ...h.userData.corner)));
  boxAround(selBox, sel.o);
  syncSelPanel();
}, true);
canvas.addEventListener('pointerup', (e) => {
  if (!cornerDrag)
    return;

  e.stopImmediatePropagation();

  const c = cornerDrag;

  cornerDrag = null;
  controls.enabled = true;

  if (c.point === undefined)
    return commitSel(c.it === sel.it ? sel : { o: c.o, it: c.it, base: sel.base });

  const it = sel.it;

  setPathPoints(it, it.points, `Move point ${c.point + 1}`);
}, true);
setTool('translate');
setEditMode(null);

// Rules naming objects their level no longer has, as "level: name" lines (for the export warning).
async function brokenRuleLines()
{
  const out = [];

  for (const p of modDirty().filter((p) => /^mod\/assets\/level\/[^/]+\/rules\.(txt|json)$/.test(p)))
  {
    const folder = p.split('/')[3];
    const dir = 'data/level/' + folder;
    const items = parseDAT(await rom.read(dir + '/dat')).items;
    const text = await virtualText(p);

    for (const b of rulesBroken(p.endsWith('.txt') ? rulesScriptParse(text) : rulesParse(text), items, ruleExt))
      out.push(`${folder}: ${b.name}`);
  }

  return out;
}

// The export items: every changed path, with the menu file split into its entries (menuItemKeys).
async function exportItems()
{
  const paths = modDirty();

  if (!paths.includes(MENU_FILE))
    return paths;

  const doc = menuParse(await virtualText(MENU_FILE)).doc;

  return doc ? [...paths.filter((p) => p !== MENU_FILE), ...menuItemKeys(doc)] : paths;
}

// The mod's files, named relative to its folder: mod.json and one assets/<path>.bps per edited asset. keys (exportItems) picks what goes in;
// a mod level's .from copies go with levels.json (else with the first Levels change).
async function modFiles(id, name, keys = null)
{
  const all = await exportItems();

  keys ??= all;

  const paths = keys.filter((k) => !/^(menu|page):/.test(k));
  const menuKeys = new Set(keys.filter((k) => /^(menu|page):/.test(k)));
  const firstLevels = all.find((k) => k === 'mod/levels.json') ?? all.find((k) => modChangeKind(k) === 'levels');

  const manifest = {
    game_id: 'rs64',
    id,
    display_name: name,
    description: 'Made with the RS64 Explorer: ' + keys.map((p) => p.replace(/^(data|mod)\//, '')).join(', '),
    version: '1.0.0',
    authors: ['RS64 Explorer'],
    minimum_recomp_version: '0.0.0',
  };

  const files = [{ name: 'mod.json', data: new TextEncoder().encode(JSON.stringify(manifest, null, 2) + '\n') }];

  for (const path of paths)
  {
    const now = await rom.read(path);

    if (path.startsWith('mod/')) {
      // A deleted crawl's files are left empty.
      if (now.byteLength)
        files.push({ name: path.slice(4), data: u8(now).slice() });

      continue;
    }

    // A level's painted textures are a new file, not a patch of one in the game.
    files.push(modAssetFile(path, path.endsWith('_PAINT') ? null : edits.get(path), u8(now)));
  }

  if (menuKeys.size)
  {
    const doc = menuParse(await virtualText(MENU_FILE)).doc;

    files.push({ name: MENU_FILE.slice(4), data: new TextEncoder().encode(menuWrite(menuWriteSubset(doc, (b) => menuKeys.has(menuItemKey(doc, b))))) });
  }

  // A mod level's files: copies of its template's (.from), patched by the .bps above when edited.
  if (!firstLevels || keys.includes(firstLevels))
    for (const [path, from] of rom.aliases)
      files.push({ name: `assets/${path.replace(/^data\//, '')}.from`, data: new TextEncoder().encode(from.replace(/^data\//, '') + '\n') });

  return [...files, ...(await cutDerivedModelFiles(paths))];
}

const MENU_FILE = 'mod/roguesq_menu.json';
const KIND_LABEL = { levels: 'Levels', models: 'Models', audio: 'Audio', ui: 'UI', fonts: 'Fonts', other: 'Other' };

// (user, used) pairs between export items: a cutscene or level that shows a model the project changes, and menu entries that need
// each other (menuLinks). Found the way the scenes load their models: a cutscene actor as showCutscene, a level item as the level view.
async function exportLinks(items)
{
  const set = new Set(items);
  const out = [];
  const modelFiles = (id) => ['_HOB', '_HMT'].map((ext) => id + ext).filter((p) => set.has(p));
  const changedModels = new Set(items.filter((p) => modChangeKind(p) === 'models').map((p) => p.replace(/_(HOB|HMT)$/, '')));

  if (changedModels.size)
  {
    await cutScanPathModels();

    const index = await buildObjectIndex();

    for (const p of items.filter((x) => x.startsWith('data/cuts/')))
    {
      for (const a of parseCutscene(await rom.read(p)).actors)
      {
        const base = cutBase(a.name);
        const tabled = cutAsset(base).path && 'data/' + cutAsset(base).path;
        const id = cutPathModelId(a.name) ?? (tabled && rom.assets.has(tabled + '_HOB') ? tabled : index.get(base));

        for (const m of id && changedModels.has(id) ? modelFiles(id) : [])
          out.push([p, m]);
      }
    }

    for (const dir of new Set(items.map((p) => p.match(/^data\/level\/[^/]+/)?.[0]).filter(Boolean)))
    {
      const dv = await rom.read(dir + '/dat').catch(() => null);
      const mine = items.filter((p) => p.startsWith(dir + '/'));

      for (const it of dv ? parseDAT(dv).items : [])
      {
        const slot = it.type === 0 && it.sub === 0x06 && it.name.replace(/[*#]\d+$/, '');
        const sub = it.type === 0 && SUBTYPE_MODEL[it.sub]?.split('#')[0];
        const ids = [it.model && index.get(it.model), sub && 'data/' + sub, sub && `data/${sub}_hi`, slot && 'data/pl_crafts/' + slot, slot && index.get(slot)];

        for (const id of ids.filter((x) => x && changedModels.has(x)))
          for (const m of modelFiles(id))
            for (const p of mine)
              out.push([p, m]);
      }
    }
  }

  if (items.some((p) => /^(menu|page):/.test(p)))
    out.push(...menuLinks(menuParse(await virtualText(MENU_FILE)).doc));

  // Soft paint: a level's painted textures, its _TEX and its map only work together.
  for (const p of items.filter((x) => x.endsWith('_PAINT')))
    for (const other of [p.replace(/_PAINT$/, '_TEX'), p.replace(/_PAINT$/, '/hmp')].filter((x) => set.has(x)))
      out.push([p, other]);

  return [...new Map(out.map((l) => [l.join('\n'), l])).values()];
}

$('btnMod').onclick = async () => {
  const broken = await brokenRuleLines();

  if (broken.length && !confirm(`Some level rules name objects that are gone and will be skipped in the game:\n${broken.join('\n')}\n\nExport anyway?`))
    return;

  const items = await exportItems();
  const doc = await readJson(EXPORT_GROUPS, () => ({ groups: [], assign: {}, kindNames: {} }));
  const base = (projectHandle?.name ?? 'my-rs64-mod').replace(/\.rs64proj$/, '');
  const kindNames = Object.fromEntries(MOD_KINDS.map((k) => [k, doc.kindNames?.[k] ?? `${base} - ${KIND_LABEL[k]}`]));
  const plan = exportPlan(items, doc, await exportLinks(items), kindNames);

  if (plan.length <= 1)
  {
    const name = prompt('Mod folder name (letters, digits and dashes)', plan[0]?.name ?? base);

    if (name)
      await saveMods([{ name, keys: items }]);

    return;
  }

  exportDialog(items, doc, base);
};

const EXPORT_GROUPS = 'explorer/export-groups.json';
const modFolderId = (name) => name.toLowerCase().replace(/[^a-z0-9-]+/g, '-').replace(/-+/g, '-').replace(/^-|-$/g, '') || 'my-rs64-mod';

// One zip holding a folder per mod.
async function saveMods(mods)
{
  const files = [];

  for (const m of mods) {
    const id = modFolderId(m.name);

    for (const f of await modFiles(id, m.name, m.keys))
      files.push({ name: `${id}/${f.name}`, data: f.data });
  }

  saveBlob(new Blob([zipStore(files)], { type: 'application/zip' }), `${mods.length === 1 ? modFolderId(mods[0].name) : modFolderId(mods[0].name.replace(/ - [^-]*$/, ''))}.zip`);
}

// The split export: a row per mod (exportPlan) with its name, change count and what it merged; each change can move to another group,
// "+ Group" adds one. Groups, assignments and renamed kind groups are kept in the project (EXPORT_GROUPS).
async function exportDialog(items, doc, base)
{
  const dlg = $('exportDialog') ?? document.body.appendChild(elem('dialog', { id: 'exportDialog' }));
  const links = await exportLinks(items);
  const skip = new Set();

  doc.groups ??= [];
  doc.assign ??= {};

  let allInOne = false;
  const kindNames = () => Object.fromEntries(MOD_KINDS.map((k) => [k, doc.kindNames?.[k] ?? `${base} - ${KIND_LABEL[k]}`]));
  const keyLabel = (k) => k.replace(/^menu:/, 'Menu button ').replace(/^page:/, 'Menu page ').replace(/^(data|mod)\//, '');

  const render = () => {
    const plan = exportPlan(items, doc, links, kindNames());
    const names = kindNames();
    const groupNames = [...new Set([...MOD_KINDS.map((k) => names[k]), ...doc.groups.map((g) => g.name)])];

    const rows = plan.map((e) => {
      const tick = elem('input', { type: 'checkbox', checked: !skip.has(e.name), title: 'Export this mod' });

      tick.onchange = () => (tick.checked ? skip.delete(e.name) : skip.add(e.name));

      const name = elem('input', { value: e.name, title: 'The mod\'s name (its folder is made from it)' });

      name.onchange = () => {
        const kind = MOD_KINDS.find((k) => names[k] === e.name);
        const g = doc.groups.find((x) => x.name === e.name);

        if (!name.value.trim() || groupNames.includes(name.value))
          return render();

        if (kind)
          doc.kindNames = { ...doc.kindNames, [kind]: name.value };

        if (g) {
          g.name = name.value;

          for (const k of Object.keys(doc.assign))
            if (doc.assign[k] === e.name)
              doc.assign[k] = name.value;
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
      const note = e.merged.length ? elem('small', {
        className: 'shotNote',
        textContent: `${e.name} uses changes from ${e.merged.join(', ')}: they export as one mod.`
      }) : '';

      const del = doc.groups.some((g) => g.name === e.name) && iconButton('delete', 'Remove this mod (its changes go back to the mod for their kind)', () => {
        doc.groups = doc.groups.filter((g) => g.name !== e.name);
        render();
      });

      return elem('div', { className: 'exportRow' }, elem('div', { className: 'exportHead' }, tick, name, del || ''), note, more);
    });

    const addGroup = elem('button', { textContent: '+ Mod', title: 'Add another mod to export, then move changes into it' });

    addGroup.onclick = () => {
      const n = prompt('Name of the new mod');

      if (!n || groupNames.includes(n))
        return;

      doc.groups.push({ name: n });
      render();
    };

    const one = elem('input', { type: 'checkbox', checked: allInOne });

    one.onchange = () => {
      allInOne = one.checked;
      render();
    };

    const go = elem('button', { textContent: 'Export', title: 'Download the mods as one zip' });
    const cancel = elem('button', { textContent: 'Cancel' });

    cancel.onclick = () => dlg.close();

    go.onclick = async () => {
      const picked = plan.filter((e) => !skip.has(e.name));

      if (!picked.length)
        return;

      await editText(EXPORT_GROUPS, JSON.stringify(doc, null, 1), 'Export groups');
      dlg.close();
      await saveMods(allInOne ? [{ name: base, keys: picked.flatMap((e) => e.keys) }] : picked);
    };

    dlg.replaceChildren(elem('h3', { textContent: 'Export mods' }),
      elem('small', {
        className: 'shotNote',
        textContent: 'Each kind of change exports as its own mod, so players can turn them on separately. Changes that need each other stay in one mod.'
      }),
      ...rows, elem('div', { className: 'exportFoot' }, addGroup, elem('label', null, one, ` All in one mod (${base})`), elem('span', { className: 'spacer' }), cancel, go));
  };

  render();

  if (!dlg.open)
    dlg.showModal();
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

const GAME_EXE = 'RogueSquadron64Recomp.exe';
const PREVIEW_MOD = 'rs64-explorer-preview';
const PLAY_LINK_FILE = 'explorer-link.txt';

async function gameFolder()
{
  let dir = await idbKv('readonly', (s) => s.get('gameDir')).catch(() => null);

  if (dir && (await dir.queryPermission({ mode: 'readwrite' })) !== 'granted' && (await dir.requestPermission({ mode: 'readwrite' })) !== 'granted')
    dir = null;

  if (dir)
    return dir;

  dir = await showDirectoryPicker({ id: 'rs64-game', mode: 'readwrite' });

  const hasExe = await dir.getFileHandle(GAME_EXE).then(() => true, () => false);
  const hasMods = await dir.getDirectoryHandle('mods').then(() => true, () => false);

  if (!hasExe && !hasMods)
    throw new Error(`Pick the folder that holds ${GAME_EXE} (for a build, build/Debug or build/Release)`);

  await idbKv('readwrite', (s) => s.put(dir, 'gameDir')).catch(() => {});

  return dir;
}

// The game's --boot-target for what is open: a cutscene file plays on its own, a level (or one of its sequences) starts its mission.
function bootTarget()
{
  const mc = modLevels.find((l) => selected === `cut:data/cuts/${l.folder}_intro` || selected === `cut:data/cuts/${l.folder}_extro`);

  if (mc)
    return `cutscene:${mc.id},${selected.endsWith('_intro') ? 0 : 1}`;

  const c = selected?.match(/^cut:data\/cuts\/id(\d+)_(intro|extro|special)$/);

  if (c)
    return `cutscene:${c[1]},${['intro', 'extro', 'special'].indexOf(c[2])}`;

  const ml = selectedLevel && modLevelOf(selected);

  if (ml)
    return `level:${ml.id}`;

  const lv = selectedLevel && selected.match(/\/lv_([0-9a-z])(:\d+)?$/);

  return lv ? `level:${parseInt(lv[1], 36)}` : '';
}

$('btnPlay').onclick = async () => {
  const st = $('playStatus');

  st.hidden = false;

  if (!window.showDirectoryPicker)
  {
    st.textContent = 'Writing into the game folder needs Chrome or Edge; use Export mod instead';

    return;
  }

  try {
    const dir = await gameFolder();
    const mods = await dir.getDirectoryHandle('mods', { create: true });

    // Rewritten from scratch so an edit that was undone leaves no patch behind.
    await mods.removeEntry(PREVIEW_MOD, { recursive: true }).catch(() => {});

    if (modDirty().length)
    {
      const root = await mods.getDirectoryHandle(PREVIEW_MOD, { create: true });

      for (const f of await modFiles(PREVIEW_MOD, 'RS64 Explorer preview'))
      {
        let d = root;
        const parts = f.name.split('/');

        for (const p of parts.slice(0, -1))
          d = await d.getDirectoryHandle(p, { create: true });

        const w = await (await d.getFileHandle(parts.at(-1), { create: true })).createWritable();

        await w.write(f.data);
        await w.close();
      }
    }

    const target = bootTarget();
    const cmd = `.\\${GAME_EXE}` + (target ? ` --boot-target=${target}` : '');
    const count = modDirty().length;
    const wrote = `${count ? `${count} edited asset${count > 1 ? 's' : ''} written to` : 'No edits; removed'} ${esc(dir.name)}\\mods\\${PREVIEW_MOD}.`;

    // The game writes "ready" in this file once it has registered its link. Until then the file asks it to.
    const link = await dir.getFileHandle(PLAY_LINK_FILE, { create: true });

    if ((await (await link.getFile()).text()).trim() === 'ready')
    {
      startingGame = true;
      location.href = `rs64recomp:${target}`;
      setTimeout(() => (startingGame = false), 1000);
      st.innerHTML = `${wrote}<br>Starting the game. If nothing opens, run this in ${esc(dir.name)}:<br><code>${esc(cmd)}</code>`;

      return;
    }

    await navigator.clipboard.writeText(cmd).catch(() => {});

    st.innerHTML = `${wrote}<br>Run this once in ${esc(dir.name)} (copied). After that, Play in game starts the game for you:<br><code>${esc(cmd)}</code>`;
  } catch (e) {
    st.textContent = e.name === 'AbortError' ? '' : e.message;
    st.hidden = e.name === 'AbortError';
  }
};

// ---------- What do you want to make? ----------

// Each goal opens where it starts and points at the control to use next, with its wiki guide.
const GUIDE_URL = (page) => `https://github.com/MikeSemicolonD/RogueSquadron64Recomp/wiki/${page}`;

const openLevel0 = async () => {
  setMode('levels');
  await showLevel('data/level/lv_0');
};

const GOALS = [
  {
    label: 'Make a new level',
    page: 'Make-a-Custom-Level',
    next: 'Pick New blank level (or New level from this one) under the list, then open Level settings… for its title and craft, and Objectives… in the edit panel for how it is won.',
    go: async () => (await openLevel0(), 'btnBlankLevel')
  },
  {
    label: 'Change a stock level',
    page: 'Make-a-Level-Mod',
    next: 'Pick a level in the list, then click the pencil to edit its ground, objects and events.',
    go: async () => (await openLevel0(), 'editTab')
  },
  {
    label: 'Repaint or replace a ship',
    page: 'Make-a-Model-Mod',
    next: 'Right-click a part to repaint it, or bring in your own model with Import craft model.',
    go: async () => {
      setMode('models');
      await showModel('data/pl_crafts/xwing');

      return 'btnImport';
    }
  },
  {
    label: 'Edit a cutscene',
    page: 'Make-a-Cutscene-Mod',
    next: 'Pick the intro or ending from the Cutscene list, then click the pencil on the timeline.',
    go: async () => (await openLevel0(), 'cutPick')
  },
  {
    label: 'Change the menus',
    page: 'Make-a-Menu-Mod',
    next: 'Pick a menu in the list, then add buttons or change what they do.',
    go: async () => {
      await menuRefreshPages();
      setMode('menus');

      return 'list';
    }
  }
];

function pointAt(id)
{
  const el = $(id);

  if (!el)
    return;

  el.scrollIntoView({ block: 'nearest' });
  el.classList.remove('pointHere');
  void el.offsetWidth;
  el.classList.add('pointHere');
}

function showGoals(goal = null)
{
  const p = $('goalPanel');

  const close = iconButton('close', 'Close', () => (p.hidden = true));

  p.replaceChildren(elem('div', { className: 'goalHead' }, elem('b', { textContent: goal ? goal.label : 'What do you want to make?' }), close));

  if (goal)
  {
    const back = elem('button', { className: 'linkBtn', textContent: 'Something else', onclick: () => showGoals() });

    p.append(elem('p', { textContent: goal.next }), elem('div', { className: 'goalFoot' }, elem('a', {
      href: GUIDE_URL(goal.page),
      target: '_blank',
      rel: 'noopener',
      textContent: 'Step-by-step guide'
    }), back));
  }
  else
    for (const g of GOALS)
      p.append(elem('button', {
        className: 'goalBtn',
        textContent: g.label,
        onclick: async () => {
          showGoals(g);
          pointAt(await g.go());
        }
      }));

  p.hidden = false;
}

setEditIcon($('goalsBtn'), 'goals');
$('goalsBtn').onclick = () => ($('goalPanel').hidden ? showGoals() : ($('goalPanel').hidden = true));
