// Smoke report: shows one run of dumps/smoke. Served by `smoke.py serve` it asks the server, which compares the run when asked; opened from disk it reads
// dumps/smoke/index.js and the run's results.js and levelperf.js, which `smoke.py compare` and run-level-perf.ps1 write.
// Each row is {name, pct, problems, known, baseline?, current?, diff?, frame_ms?}; a row with problems failed, one with only known problems
// (each {problem, reason}, explained by the manifest's [[known]] entries) did not.

let data = null;
const $ = (id) => document.getElementById(id);
const live = location.protocol.startsWith('http');
const DUMPS = '../../../../dumps/smoke/';

// Row picture paths are relative to the run's folder.
let runFolder = '';
const state = { section: null };

const sectionOf = (row) => (row.name.match(/^[a-z]+/) || ['other'])[0];
const failed = (row) => row.problems.length > 0;
const knownOnly = (row) => !failed(row) && (row.known || []).length > 0;

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
const url = (path) => (runFolder + path).split('/').map(encodeURIComponent).join('/');

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
  const known = row.known || [];
  const isNew = known.some((k) => k.problem === 'new, not in the baseline');
  const status = bad ? 'FAIL' : isNew ? 'new' : known.length ? 'known' : 'ok';
  const head = el('div', { className: 'rowHead' }, el('span', { className: 'status', textContent: status }), el('b', { textContent: row.name }));

  if (row.pct !== null)
    head.append(el('span', { className: 'pct', textContent: `${row.pct.toFixed(3)}% of pixels changed` }));

  if (row.frame_ms)
    head.append(el('span', { className: 'fps', textContent: `${(1000 / row.frame_ms.avg).toFixed(0)} fps, p99 ${row.frame_ms.p99.toFixed(1)} ms` }));

  const view = el('article', { className: bad ? 'row fail' : known.length ? 'row known' : 'row' }, head);

  if (bad)
    view.append(el('div', { className: 'problems', textContent: row.problems.join('; ') }));

  for (const k of known)
    view.append(el('div', { className: 'knownNote', textContent: isNew ? `${k.reason}.` : `Known: ${k.problem}. ${k.reason}` }));

  const shots = pictures(row).map(([label, path]) => shot(row, label, path));

  if (shots.length)
    view.append(el('div', { className: 'shots' }, ...shots));

  return view;
}

// The level performance table from run-level-perf.ps1 opens as its own card next to the smoke sections.
const PERF_CARD = 'levelperf';

// The perf and memory sections as tables (data.speed), next to the level performance card.
const SPEED_CARD = 'speed';

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
    const c = counts.get(sectionOf(row)) || { total: 0, failed: 0, known: 0 };

    c.total++;
    c.failed += failed(row) ? 1 : 0;
    c.known += knownOnly(row) ? 1 : 0;
    counts.set(sectionOf(row), c);
  }

  const detail = (c) => `${c.failed} of ${c.total} failed` + (c.known ? `, ${c.known} known` : '');
  const cards = [...counts].sort((a, b) => b[1].failed - a[1].failed || a[0].localeCompare(b[0])).map(([name, c]) => card(name, name, detail(c), c.failed / c.total));
  const perf = window.LEVEL_PERF;

  const speed = data && data.speed;
  const speedRows = speed ? [...speed.perf, ...speed.memory] : [];

  if (speedRows.length)
  {
    const bad = speedRows.filter((r) => r.failed).length;

    cards.push(card(SPEED_CARD, 'Frame rate & memory', `${bad} of ${speedRows.length} failed`, bad / speedRows.length));
  }

  if (perf)
  {
    cards.push(card(PERF_CARD, 'Level performance', `${perf.rows.length} levels`, 0));
  }

  $('sections').replaceChildren(...cards);
}

function render()
{
  const perfOpen = state.section === PERF_CARD || state.section === SPEED_CARD;

  sectionsView();
  $('perf').hidden = !perfOpen;
  $('toolbar').hidden = perfOpen || !data;
  $('rows').hidden = perfOpen || !data;

  if (perfOpen)
  {
    if (state.section === SPEED_CARD)
      speedView();
    else
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

// The perf section (one row per level and video preset) and the memory section, each beside the baseline; a header's title says what the column means.
function speedTable(columns, rows)
{
  const head = columns.map(([label, meaning]) => el('th', { textContent: label, title: meaning }));
  const body = rows.map((r) => el('tr', { className: r.failed ? 'bad' : '' }, ...columns.map(([, , text]) => el('td', { textContent: text(r) }))));

  return el('table', {}, el('thead', {}, el('tr', {}, ...head)), el('tbody', {}, ...body));
}

function speedView()
{
  const s = data.speed;
  const num = (v, digits) => (v === null || v === undefined ? '-' : v.toFixed(digits));
  const status = (r) => (r.failed ? 'FAIL' : 'ok');
  const change = (r) => (r.baseFps && r.fps ? `${((r.baseFps / r.fps - 1) * 100).toFixed(0)}%` : '-');

  const perfColumns = [
    ['Target', 'The level and video preset played at real speed', (r) => r.name.replace(/^perf_/, '')],
    ['Baseline fps', 'Average frames shown per second in the baseline run', (r) => num(r.baseFps, 0)],
    ['fps', 'Average frames shown per second in this run', (r) => num(r.fps, 0)],
    ['Frame time', `Change in average frame time; over +${s.slowerLimit * 100}% fails`, change],
    ['p99 ms', 'Baseline and current 99th-percentile frame time: the slowest frames', (r) => `${num(r.baseP99, 1)} → ${num(r.p99, 1)}`],
    ['', 'Whether this target passed', status]
  ];

  const memoryColumns = [
    ['Target', 'The level replayed several times in one launch', (r) => r.name.replace(/^memory_/, '')],
    ['Baseline MB/loop', 'Median memory growth per replay in the baseline run', (r) => num(r.baseGrowth, 1)],
    ['MB/loop', `Median memory growth per replay in this run; over ${s.growthLimit} MB fails`, (r) => num(r.growth, 1)],
    ['End MB', 'Memory in use (private bytes) after the last replay', (r) => num(r.endMB, 0)],
    ['', 'Whether this target passed', status]
  ];

  $('perf').replaceChildren(
    el('h2', { textContent: 'Frame rate' }),
    speedTable(perfColumns, s.perf),
    el('h2', { textContent: 'Memory' }),
    speedTable(memoryColumns, s.memory)
  );
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

// A script from disk, where fetch can't read files; a missing one leaves its global unset.
function loadScript(src)
{
  return new Promise((done) => {
    const s = el('script', { src: `${src}?v=${Date.now()}` });

    s.onload = done;
    s.onerror = done;
    document.head.append(s);
  });
}

async function loadRuns()
{
  if (live)
    return (await fetch('/api/runs')).json();

  await loadScript(`${DUMPS}index.js`);

  return window.SMOKE_RUNS || [];
}

// The run's results and level performance table as [results, levelPerf], either possibly null.
async function loadRun(run)
{
  const folder = `${run.tag}/${run.platform}/`;

  if (live)
  {
    runFolder = `/runs/${folder}`;
    const answer = await (await fetch(`/api/run?tag=${encodeURIComponent(run.tag)}&platform=${encodeURIComponent(run.platform)}`)).json();

    return [answer.results, answer.levelPerf];
  }

  runFolder = DUMPS + folder;
  await loadScript(`${runFolder}results.js`);
  await loadScript(`${runFolder}levelperf.js`);

  return [window.SMOKE_RESULTS || null, window.LEVEL_PERF || null];
}

// ?run=<tag>/<platform> picks the run; without it, current/windows when there is one.
async function start()
{
  setupViewer();
  $('sort').onchange = render;
  $('failedOnly').onchange = render;

  const runs = await loadRuns();
  const key = (r) => `${r.tag}/${r.platform}`;
  const asked = new URLSearchParams(location.search).get('run');
  const run = runs.find((r) => key(r) === asked) || runs.find((r) => key(r) === 'current/windows') || runs[0];

  if (!run)
  {
    $('empty').hidden = false;
    render();

    return;
  }

  $('platform').replaceChildren(...runs.map((r) => el('option', { value: key(r), textContent: `${r.tag} / ${r.platform}`, selected: r === run })));
  $('platform').onchange = (e) => location.assign(`?run=${encodeURIComponent(e.target.value)}`);
  $('platform').hidden = false;
  $('summary').textContent = live ? 'Comparing…' : '';

  const [results, levelPerf] = await loadRun(run);

  data = results;
  window.LEVEL_PERF = levelPerf;

  if (data)
  {
    const bad = data.rows.filter(failed).length;

    $('summary').textContent = `${bad} of ${data.rows.length} failed`;
    $('made').textContent = `${data.tag} / ${data.platform}, compared ${live ? 'live' : data.made}`;
  }
  else if (levelPerf)
  {
    $('summary').textContent = '';
    state.section = PERF_CARD;
  }
  else
  {
    $('summary').textContent = '';
    $('empty').hidden = false;
  }

  render();
}

start();
