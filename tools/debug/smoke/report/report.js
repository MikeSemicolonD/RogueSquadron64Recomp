// Smoke report: builds the page from window.SMOKE_RESULTS (results.js, written by smoke.py compare next to a copy of this page).
// Each row is {name, pct, problems, baseline?, current?, diff?, frame_ms?}; a row with problems failed.

const data = window.SMOKE_RESULTS;
const $ = (id) => document.getElementById(id);
const state = { section: null };

const sectionOf = (row) => (row.name.match(/^[a-z]+/) || ['other'])[0];
const failed = (row) => row.problems.length > 0;

// Failures first; among them the ones with no pixel figure (frame rate, memory, camera, missing), then the most pixels changed.
function byChange(a, b)
{
  const fa = failed(a);
  const fb = failed(b);

  if (fa !== fb)
    return fa ? -1 : 1;

  const pa = a.pct === null ? Infinity : a.pct;
  const pb = b.pct === null ? Infinity : b.pct;

  return pb - pa || a.name.localeCompare(b.name);
}

function el(tag, props, ...children)
{
  const e = Object.assign(document.createElement(tag), props);

  e.append(...children.filter((c) => c !== null && c !== undefined));

  return e;
}

// Folder names can hold '#' (menu_options__showroom__#0), which a URL would read as a fragment.
const url = (path) => path.split('/').map(encodeURIComponent).join('/');

// A row's pictures as [label, path] for the ones it has.
const pictures = (row) => [
  ['Baseline', row.baseline],
  ['Current', row.current],
  ['Changed pixels', row.diff]
].filter(([, path]) => path);

function shot(row, label, path)
{
  const img = el('img', { src: url(path), loading: 'lazy', alt: label, title: 'Open in the viewer' });

  img.onclick = () => openViewer(row, label);

  return el('figure', {}, el('figcaption', { textContent: label }), img);
}

// The viewer: one picture, or baseline and current split by a slider, zoomed with the wheel and panned by dragging.
const viewing = { fit: false, row: null, mode: '', scale: 1, x: 0, y: 0 };

function viewerModes(row)
{
  const modes = pictures(row).map(([label]) => label);

  if (row.baseline && row.current)
    modes.push('Compare');

  return modes;
}

function placeLayer()
{
  $('vLayer').style.transform = `translate(${viewing.x}px, ${viewing.y}px) scale(${viewing.scale})`;
  $('vLayer').classList.toggle('pixels', viewing.scale >= 2);
  $('vScale').textContent = `${Math.round(viewing.scale * 100)}%`;
}

function fitViewer()
{
  const img = $('vA');
  const stage = $('vStage').getBoundingClientRect();

  if (!img.naturalWidth)
    return;

  viewing.scale = Math.min(stage.width / img.naturalWidth, stage.height / img.naturalHeight);
  viewing.x = (stage.width - img.naturalWidth * viewing.scale) / 2;
  viewing.y = (stage.height - img.naturalHeight * viewing.scale) / 2;
  placeLayer();
}

// Zooms by `factor` keeping the stage point (px, py) still.
function zoomViewer(factor, px, py)
{
  const scale = Math.min(32, Math.max(0.05, viewing.scale * factor));

  viewing.x = px - ((px - viewing.x) * scale) / viewing.scale;
  viewing.y = py - ((py - viewing.y) * scale) / viewing.scale;
  viewing.scale = scale;
  placeLayer();
}

function showMode(mode)
{
  const compare = mode === 'Compare';
  const path = compare ? viewing.row.baseline : pictures(viewing.row).find(([label]) => label === mode)[1];

  viewing.mode = mode;
  $('vA').src = url(path);
  $('vB').hidden = !compare;
  $('vLine').hidden = !compare;
  $('vSplitBar').hidden = !compare;

  if (compare)
    $('vB').src = url(viewing.row.current);

  for (const b of $('vModes').children)
    b.classList.toggle('on', b.textContent === mode);

  splitViewer();
}

function splitViewer()
{
  const split = $('vSplit').value;

  $('vB').style.clipPath = `inset(0 0 0 ${split}%)`;
  $('vLine').style.left = `${split}%`;
}

function openViewer(row, mode)
{
  viewing.row = row;
  $('vTitle').textContent = row.name;

  $('vModes').replaceChildren(...viewerModes(row).map((m) => {
    const b = el('button', {
      textContent: m,
      title: m === 'Compare' ? 'Baseline on the left, current on the right of the slider' : `Show the ${m.toLowerCase()} picture`
    });

    b.onclick = () => showMode(m);

    return b;
  }));

  // Fit once when the viewer opens; switching pictures keeps the zoom so the same spot can be compared.
  viewing.fit = true;

  $('vA').onload = () => {
    if (viewing.fit)
      fitViewer();

    viewing.fit = false;
  };

  $('viewer').hidden = false;
  showMode(mode);
}

function closeViewer()
{
  $('viewer').hidden = true;
  $('vA').removeAttribute('src');
  $('vB').removeAttribute('src');
}

function setupViewer()
{
  const stage = $('vStage');
  let drag = null;

  stage.onwheel = (e) => {
    const box = stage.getBoundingClientRect();

    e.preventDefault();
    zoomViewer(e.deltaY < 0 ? 1.15 : 1 / 1.15, e.clientX - box.left, e.clientY - box.top);
  };

  stage.onpointerdown = (e) => {
    drag = { x: e.clientX - viewing.x, y: e.clientY - viewing.y };
    stage.setPointerCapture(e.pointerId);
  };

  stage.onpointermove = (e) => {
    if (!drag)
      return;

    viewing.x = e.clientX - drag.x;
    viewing.y = e.clientY - drag.y;
    placeLayer();
  };

  stage.onpointerup = () => {
    drag = null;
  };

  stage.ondblclick = fitViewer;

  const centre = (factor) => {
    const box = stage.getBoundingClientRect();

    zoomViewer(factor, box.width / 2, box.height / 2);
  };

  $('vIn').onclick = () => centre(1.5);
  $('vOut').onclick = () => centre(1 / 1.5);
  $('vFit').onclick = fitViewer;
  $('vClose').onclick = closeViewer;
  $('vSplit').oninput = splitViewer;

  $('viewer').onclick = (e) => {
    if (e.target === $('viewer'))
      closeViewer();
  };

  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape' && !$('viewer').hidden)
      closeViewer();
  });
}

function rowView(row)
{
  const bad = failed(row);
  const head = el('div', { className: 'rowHead' }, el('span', { className: 'status', textContent: bad ? 'FAIL' : 'ok' }), el('b', { textContent: row.name }));

  if (row.pct !== null)
    head.append(el('span', { className: 'pct', textContent: `${row.pct.toFixed(3)}% of pixels changed` }));

  if (row.frame_ms)
    head.append(el('span', { className: 'fps', textContent: `${(1000 / row.frame_ms.avg).toFixed(0)} fps, p99 ${row.frame_ms.p99.toFixed(1)} ms` }));

  const view = el('article', { className: bad ? 'row fail' : 'row' }, head);

  if (bad)
    view.append(el('div', { className: 'problems', textContent: row.problems.join('; ') }));

  const shots = pictures(row).map(([label, path]) => shot(row, label, path));

  if (shots.length)
    view.append(el('div', { className: 'shots' }, ...shots));

  return view;
}

// The level performance table from run-level-perf.ps1 opens as its own card next to the smoke sections.
const PERF_CARD = 'levelperf';

function card(key, label, detail, failedPart)
{
  const bar = el('div', { className: 'bar' }, el('i', { style: `width: ${100 * failedPart}%` }));
  const c = el('button', {
    className: 'sec',
    title: `Show only ${label.toLowerCase()}`
  }, el('b', { textContent: label }), el('small', { textContent: detail }), bar);

  c.classList.toggle('on', state.section === key);

  c.onclick = () => {
    state.section = state.section === key ? null : key;
    render();
  };

  return c;
}

function sectionsView()
{
  const counts = new Map();

  for (const row of data ? data.rows : [])
  {
    const c = counts.get(sectionOf(row)) || { total: 0, failed: 0 };

    c.total++;
    c.failed += failed(row) ? 1 : 0;
    counts.set(sectionOf(row), c);
  }

  const cards = [...counts].sort((a, b) => b[1].failed - a[1].failed || a[0].localeCompare(b[0])).map(([name, c]) => card(name, name, `${c.failed} of ${c.total} failed`, c.failed / c.total));
  const perf = window.LEVEL_PERF;

  if (perf)
  {
    cards.push(card(PERF_CARD, 'Level performance', `${perf.rows.length} levels`, 0));
  }

  $('sections').replaceChildren(...cards);
}

function render()
{
  const perfOpen = state.section === PERF_CARD;

  sectionsView();
  $('perf').hidden = !perfOpen;
  $('toolbar').hidden = perfOpen || !data;
  $('rows').hidden = perfOpen || !data;

  if (perfOpen)
  {
    perfView();

    return;
  }

  if (!data)
    return;

  const failedOnly = $('failedOnly').checked;
  const sort = $('sort').value === 'name' ? (a, b) => a.name.localeCompare(b.name) : byChange;
  const rows = data.rows.filter((r) => (!failedOnly || failed(r)) && (!state.section || sectionOf(r) === state.section)).sort(sort);

  $('rows').replaceChildren(...rows.map(rowView));
  $('shown').textContent = `${rows.length} shown`;
}

// The per-level table from run-level-perf.ps1 (levelperf.js next to this page); a column header sorts by it, again to reverse.
// [label, what it means, sort value, shown text].
const perfColumns = [
  ['Level', 'The level whose mission recording was replayed', (r) => parseInt(r.Level, 10), (r) => r.Level],
  ['Frames', 'Game frames in the mission recording', (r) => r.Frames],
  [
    'Duration',
    'Real time the replay took, from mission start to the end of the recording',
    (r) => r.Secs,
    (r) => `${Math.floor(r.Secs / 60)}:${String(r.Secs % 60).padStart(2, '0')}`
  ],
  ['Game fps', 'Game frames per second of real time. A level that keeps up reads 30 times the replay speed.', (r) => r.GameFps, (r) => r.GameFps.toFixed(1)],
  ['Walk ms/f', 'Average milliseconds per frame the renderer spent reading the game\'s display lists', (r) => r.WalkMs, (r) => r.WalkMs.toFixed(2)],
  ['Hitches', 'Frames that took more than 25 ms to reach the screen after the one before', (r) => r.Hitches],
  ['RAM MB', 'The game\'s memory use (working set) at mission start and at the end', (r) => r.RamEndMB, (r) => `${r.RamStartMB} → ${r.RamEndMB}`],
  ['RAM growth MB', 'Memory use at the end minus at the start', (r) => r.RamEndMB - r.RamStartMB]
];

const perfSort = { col: 0, desc: false };

function perfView()
{
  const perf = window.LEVEL_PERF;
  const [, , key] = perfColumns[perfSort.col];
  const rows = [...perf.rows].sort((a, b) => (perfSort.desc ? key(b) - key(a) : key(a) - key(b)));

  const head = perfColumns.map(([label, meaning], i) => {
    const mark = i === perfSort.col ? (perfSort.desc ? ' ▼' : ' ▲') : '';
    const th = el('th', { textContent: label + mark, title: meaning });

    th.onclick = () => {
      perfSort.desc = perfSort.col === i ? !perfSort.desc : i !== 0;
      perfSort.col = i;
      perfView();
    };

    return th;
  });

  const body = rows.map((r) => el('tr', {}, ...perfColumns.map(([, , value, text]) => el('td', { textContent: text ? text(r) : value(r) }))));
  const note = `Level recordings at ${perf.speed}x, ${perf.made}, commit ${perf.commit}. ${30 * perf.speed} game fps means the level kept up.`;

  $('perf').replaceChildren(el('h2', { textContent: 'Level performance' }), el('small', { textContent: note }), el('table', {}, el('thead', {}, el('tr', {}, ...head)), el('tbody', {}, ...body)));
}

$('themeBtn').onclick = () => {
  const now = document.documentElement.dataset.theme ?? (matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark');

  document.documentElement.dataset.theme = now === 'light' ? 'dark' : 'light';

  try {
    localStorage.setItem('rs64.theme', JSON.stringify(document.documentElement.dataset.theme));
  }
  catch {
    // A page opened from disk may have no storage; the theme then lasts until reload.
  }
};

// Every platform compared in this run (../platforms.js); the picker opens that platform's own report.
const platforms = window.SMOKE_PLATFORMS || (data ? [data.platform] : []);

if (data)
{
  $('platform').replaceChildren(...platforms.map((p) => el('option', { value: p, textContent: p, selected: p === data.platform })));
  $('platform').onchange = (e) => location.assign(`../${encodeURIComponent(e.target.value)}/report.html`);
  $('platform').hidden = false;
}

setupViewer();
$('sort').onchange = render;
$('failedOnly').onchange = render;

if (data)
{
  const bad = data.rows.filter(failed).length;

  $('summary').textContent = `${bad} of ${data.rows.length} failed`;
  $('made').textContent = `${data.tag} / ${data.platform}, compared ${data.made}`;
}
else if (window.LEVEL_PERF)
  state.section = PERF_CARD;
else
  $('empty').hidden = false;

render();
