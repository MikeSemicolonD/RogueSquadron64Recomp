// ---- Fonts tab: the game's fonts and a language mod's glyph packs (lang/glyphs/<font>.bin, see docs/guides/modding-languages.md) ----
// A font shows the game's own glyphs (from data/fonts/<name>, read-only: ASCII always draws with them) and the mod's pack glyphs, which
// can be drawn from a font file for every character the language's text needs and touched up by hand. The language's files live in
// the project as mod/lang/... and Export mod writes them out.
let fontSel = null, glyphSel = null, glyphLevel = 15, fontSheets = new Map();
const glyphPackPath = (name) => `mod/lang/glyphs/${name}.bin`;
const LANG_TEXT = ['front', 'game', 'voice'].map((n) => `mod/lang/${n}.json`);
const fontOf = (name) => gameFonts(romBytes).find((f) => f.name === name);
async function readPack(f) {
  const bytes = await rom.read(glyphPackPath(f.name)).then(u8, () => new Uint8Array(0));
  return (bytes.length && glyphPackParse(bytes)) || { cellW: f.cellW + (f.cellW & 1), cellH: f.cellH, glyphs: [] };
}
const writePack = (f, pack, label) => editAsset(glyphPackPath(f.name), () => glyphPackWrite({ ...pack, glyphs: [...pack.glyphs].sort((a, b) => a.cp - b.cp) }), `${f.name}: ${label}`);
async function langCharsNow() {
  return langChars(await Promise.all(LANG_TEXT.map((p) => virtualText(p))));
}
// The game's glyph strip as one canvas (cell wide, one cell per glyph down), I4 high nibble first, white on transparent.
async function fontSheet(f) {
  if (!fontSheets.has(f.name)) {
    const dv = await rom.read('data/fonts/' + f.name), w = dv.getUint16(4), h = dv.getUint16(6), px = u8(dv).subarray(FONT_STRIP_HEADER);
    const cv = document.createElement('canvas'), img = new ImageData(w, h);
    cv.width = w;
    cv.height = h;
    for (let i = 0; i < w * h; i++) img.data.set([255, 255, 255, (((px[i >> 1] ?? 0) >> (i & 1 ? 0 : 4)) & 15) * 17], i * 4);
    cv.getContext('2d').putImageData(img, 0, 0);
    fontSheets.set(f.name, cv);
  }
  return fontSheets.get(f.name);
}
function glyphCanvas(cellW, cellH, pixels, scale) {
  const cv = elem('canvas', { width: cellW, height: cellH }), img = new ImageData(cellW, cellH);
  for (let i = 0; i < cellW * cellH; i++) img.data.set([255, 255, 255, pixels[i] * 17], i * 4);
  cv.getContext('2d').putImageData(img, 0, 0);
  Object.assign(cv.style, { width: `${cellW * scale}px`, height: `${cellH * scale}px` });
  return cv;
}
// One character drawn with the loaded font file into a cell: sized so the font's ascent and descent fill the drawn height, squeezed to
// the cell width if wider, left-aligned (the game draws a glyph at the pen and advances by its width), alpha quantized to 16 levels.
function rasterGlyph(family, cp, f, cellW) {
  const cv = new OffscreenCanvas(cellW, f.cellH), cx = cv.getContext('2d');
  cx.font = `100px "${family}"`;
  const ref = cx.measureText('Mg'), size = (100 * f.drawn) / ((ref.fontBoundingBoxAscent + ref.fontBoundingBoxDescent) || 100);
  cx.font = `${size}px "${family}"`;
  const w = Math.max(1, cx.measureText(String.fromCodePoint(cp)).width), sx = Math.min(1, cellW / w);
  cx.fillStyle = '#fff';
  cx.setTransform(sx, 0, 0, 1, 0, 0);
  cx.fillText(String.fromCodePoint(cp), 0, (size * ref.fontBoundingBoxAscent) / 100);
  const a = cx.getImageData(0, 0, cellW, f.cellH).data, pixels = new Uint8Array(cellW * f.cellH);
  for (let i = 0; i < pixels.length; i++) pixels[i] = Math.round(a[i * 4 + 3] / 17);
  return { cp, advance: Math.min(cellW, Math.ceil(w * sx) + 1), pixels };
}
const pickFiles = (accept, multiple = false, folder = false) => new Promise((res) => {
  const inp = elem('input', { type: 'file', accept, multiple, webkitdirectory: folder });
  inp.onchange = () => res([...inp.files]);
  inp.click();
});
// The font file each game font was drawn from stays in the project (explorer/, never exported), so characters the text gains later are drawn
// from it on import. fontSkipped lists, per font, the text's characters its file has no glyph for.
const FONT_SOURCES = 'explorer/glyph-fonts.json', fontSourcePath = (file) => `explorer/glyph-fonts/${file}`;
let fontSkipped = {};
const readSources = () => readJson(FONT_SOURCES, () => ({}));
// Draws `cps` (or, with replace false, only those the pack lacks) into each font's pack from one font file.
async function drawGlyphs(fonts, fileName, data, cps, replace) {
  // Only characters the file has glyphs for: the browser would draw any other with a system font, which a mod must not ship.
  const covers = fontCovers(data), family = 'rs64build' + Date.now(), face = new FontFace(family, data);
  await face.load();
  document.fonts.add(face);
  for (const f of fonts) {
    const pack = await readPack(f), have = new Set(pack.glyphs.map((g) => g.cp)), want = replace ? cps : cps.filter((cp) => !have.has(cp));
    const drawn = want.filter(covers).map((cp) => rasterGlyph(family, cp, f, pack.cellW)), got = new Set(drawn.map((g) => g.cp));
    fontSkipped[f.name] = { file: fileName, cps: cps.filter((cp) => !covers(cp)) };
    if (drawn.length) await writePack(f, { ...pack, glyphs: [...pack.glyphs.filter((g) => !got.has(g.cp)), ...drawn] }, `drawn from ${fileName}`);
  }
  document.fonts.delete(face);
}
async function drawPacksFrom(fonts) {
  const [file] = await pickFiles('.ttf,.otf');
  if (!file) return;
  const data = new Uint8Array(await file.arrayBuffer());
  if (!fontCovers(data)) return alert(`${file.name} is not a .ttf or .otf font this can read (a TrueType collection or a compressed font): convert it to .ttf first.`);
  await historyGroup(`Draw glyphs from ${file.name}`, async () => {
    const sources = await readSources();
    for (const f of fonts) sources[f.name] = file.name;
    await editAsset(fontSourcePath(file.name), () => data, `Keep ${file.name}`);
    await writeJson(FONT_SOURCES, sources, `Draw from ${file.name}`, 1);
    await drawGlyphs(fonts, file.name, data, await langCharsNow(), true);
  });
  showFont(fontSel);
}
// After the text changes: each font with a remembered file gets glyphs for the characters it now lacks.
async function drawNewChars() {
  const sources = await readSources(), cps = await langCharsNow(), byFile = new Map();
  for (const f of gameFonts(romBytes).filter((x) => x.extended && sources[x.name])) byFile.set(sources[f.name], [...(byFile.get(sources[f.name]) ?? []), f]);
  for (const [file, fonts] of byFile) {
    const data = await rom.read(fontSourcePath(file)).then(u8, () => null);
    if (data?.length) await drawGlyphs(fonts, file, data, cps, false);
  }
}
// A language mod's folder (or its lang folder) into the project: lang/*.json, the lang/glyphs/*.bin packs and their license texts.
async function importLangFiles() {
  const files = (await pickFiles('', true, true))
    .map((f) => ({ f, rel: /(?:^|\/)(lang\/(?:[^/]+\.json|glyphs\/[^/]+\.(?:bin|txt)))$/.exec(f.webkitRelativePath.replace(/\\/g, '/'))?.[1] }))
    .filter(({ rel }) => rel);
  if (!files.length) return alert('No language files there: pick a language mod\'s folder, the one holding lang/.');
  await historyGroup('Import language files', async () => {
    for (const { f, rel } of files) {
      const path = 'mod/' + rel, bytes = new Uint8Array(await f.arrayBuffer());
      if (f.name.endsWith('.bin') && !glyphPackParse(bytes)) {
        alert(`${f.name} is not a glyph pack (RSG1) the game reads; skipped.`);
        continue;
      }
      await editAsset(path, () => bytes, `Import ${f.name}`);
    }
    await drawNewChars();
  });
  if (fontSel) showFont(fontSel);
}
async function showFont(name) {
  fontSel = name;
  renderList();
  const f = fontOf(name), panel = $('fontPanel');
  if (!f) return;
  const pack = await readPack(f), sheet = await fontSheet(f), cps = await langCharsNow(), scale = f.cellH <= 15 ? 3 : 2;
  const have = new Set(pack.glyphs.map((g) => g.cp)), missing = cps.filter((c) => !have.has(c));
  const tool = (icon, tip, fn) => {
    const b = elem('button', { title: tip });
    setEditIcon(b, icon);
    b.onclick = fn;
    return b;
  };
  const tools = elem('div', { className: 'fontTools' }, tool('open', 'Import a language mod: pick its folder (the one holding lang/)', importLangFiles));
  if (f.extended) tools.append(tool('upload', 'Draw this font\'s glyphs from a font file (.ttf or .otf): every character the text uses, and later ones when you import the text again', () => drawPacksFrom([f])),
    tool('modExport', 'Draw every font\'s glyphs from one font file (a pixel font often reads better at the small sizes: draw those again one by one)', () => drawPacksFrom(gameFonts(romBytes).filter((x) => x.extended))));
  const stock = elem('div', { className: 'glyphGrid' }), mine = elem('div', { className: 'glyphGrid' });
  [...f.charset].forEach((ch, i) => {
    const cv = elem('canvas', { width: f.cellW, height: f.cellH });
    cv.getContext('2d').drawImage(sheet, 0, i * f.cellH, f.cellW, f.cellH, 0, 0, f.cellW, f.cellH);
    Object.assign(cv.style, { width: `${f.cellW * scale}px`, height: `${f.cellH * scale}px` });
    stock.append(elem('div', { className: 'glyphCell', title: `${ch} (the game's, ${f.widths[i]} px wide)` }, cv, elem('small', { textContent: ch })));
  });
  for (const g of [...pack.glyphs].sort((a, b) => a.cp - b.cp)) {
    const cell = elem('div', { className: 'glyphCell mine' + (g.cp === glyphSel ? ' sel' : ''), title: `${String.fromCodePoint(g.cp)} U+${g.cp.toString(16).toUpperCase().padStart(4, '0')}, ${g.advance} px wide: click to edit` },
      glyphCanvas(pack.cellW, pack.cellH, g.pixels, scale), elem('small', { textContent: String.fromCodePoint(g.cp) }));
    cell.onclick = () => {
      glyphSel = g.cp;
      showFont(name);
    };
    mine.append(cell);
  }
  const parts = [elem('b', { textContent: f.name }), tools, elem('hr', { className: 'fontDivider' })];
  const chars = (list) => list.slice(0, 80).map((c) => String.fromCodePoint(c)).join('') + (list.length > 80 ? '…' : '');
  if (missing.length && f.extended) parts.push(elem('small', { className: 'shotNote', textContent: `Missing: ${chars(missing)}. In game these use the same character from another font's pack, scaled, or a box when no pack has it.` }));
  const skipped = fontSkipped[f.name];
  if (skipped?.cps.length) parts.push(elem('small', { className: 'shotNote', textContent: `${skipped.file} has no glyph for ${skipped.cps.length} of them, so they were not drawn: ${chars(skipped.cps)}` }));
  const g = glyphSel != null && pack.glyphs.find((x) => x.cp === glyphSel);
  if (g) parts.push(glyphEditor(f, pack, g), elem('hr', { className: 'fontDivider' }));
  if (f.extended && (pack.glyphs.length || cps.length)) parts.push(elem('h4', { textContent: 'This mod\'s glyphs' }), pack.glyphs.length ? mine : elem('small', { className: 'shotNote', textContent: 'None yet: draw them from a font file, or import a pack.' }));
  parts.push(elem('h4', { textContent: 'The game\'s glyphs' }), stock);
  panel.replaceChildren(...parts);
  panel.hidden = false;
}
// One glyph at 14x: left-drag paints the picked level, right-drag erases; the rows below the drawn height show dimmed (never drawn).
function glyphEditor(f, pack, g) {
  const z = 14, cv = elem('canvas', { width: pack.cellW * z, height: pack.cellH * z, className: 'glyphEdit' }), cx = cv.getContext('2d'), px = g.pixels.slice();
  Object.assign(cv.style, { width: `${cv.width}px`, height: `${cv.height}px` });
  const draw = () => {
    cx.fillStyle = '#000';
    cx.fillRect(0, 0, cv.width, cv.height);
    for (let i = 0; i < px.length; i++) {
      const v = px[i] * 17;
      cx.fillStyle = `rgb(${v},${v},${v})`;
      cx.fillRect((i % pack.cellW) * z, Math.floor(i / pack.cellW) * z, z - 1, z - 1);
    }
    cx.fillStyle = 'rgba(255,79,122,.6)';
    cx.fillRect(0, f.drawn * z, cv.width, cv.height - f.drawn * z);
    cx.fillStyle = '#ff4f7a';
    cx.fillRect(0, f.drawn * z - 1, cv.width, 2);
    cx.fillStyle = '#ff4f7a';
    cx.fillRect(g.advance * z - 1, 0, 2, cv.height);
  };
  let painting = null;
  const paintAt = (e) => {
    const r = cv.getBoundingClientRect(), x = Math.floor(((e.clientX - r.left) / r.width) * pack.cellW), y = Math.floor(((e.clientY - r.top) / r.height) * pack.cellH);
    if (x < 0 || y < 0 || x >= pack.cellW || y >= pack.cellH) return;
    px[y * pack.cellW + x] = painting;
    draw();
  };
  cv.oncontextmenu = (e) => e.preventDefault();
  cv.onpointerdown = (e) => {
    painting = e.button === 2 ? 0 : glyphLevel;
    cv.setPointerCapture(e.pointerId);
    paintAt(e);
  };
  cv.onpointermove = (e) => painting != null && paintAt(e);
  cv.onpointerup = () => {
    painting = null;
    if (px.every((v, i) => v === g.pixels[i])) return;
    writePack(f, { ...pack, glyphs: pack.glyphs.map((x) => (x === g ? { ...g, pixels: px } : x)) }, `paint ${String.fromCodePoint(g.cp)}`).then(() => showFont(f.name));
  };
  const level = elem('input', { type: 'range', min: 0, max: 15, value: glyphLevel, title: 'Brightness the left button paints with, from 0 (clear) to 15 (solid)' }), swatch = elem('span', { className: 'inkSwatch' });
  const showLevel = () => (swatch.style.background = `rgb(${glyphLevel * 17},${glyphLevel * 17},${glyphLevel * 17})`);
  level.oninput = () => {
    glyphLevel = +level.value;
    showLevel();
  };
  showLevel();
  const advance = elem('input', { type: 'number', min: 0, max: pack.cellW, value: g.advance, title: 'How far the next character starts after this one, in pixels (the red line)' });
  advance.onchange = () => writePack(f, { ...pack, glyphs: pack.glyphs.map((x) => (x === g ? { ...g, advance: Math.max(0, Math.min(pack.cellW, +advance.value | 0)) } : x)) }, `width ${String.fromCodePoint(g.cp)}`).then(() => showFont(f.name));
  const del = elem('button', { title: 'Delete this glyph (the game then draws the same character from another font\'s pack, or a box)' });
  setEditIcon(del, 'delete');
  del.onclick = () => {
    glyphSel = null;
    writePack(f, { ...pack, glyphs: pack.glyphs.filter((x) => x !== g) }, `delete ${String.fromCodePoint(g.cp)}`).then(() => showFont(f.name));
  };
  draw();
  return elem('div', { className: 'glyphEditor' }, elem('div', { className: 'glyphEditTop' }, cv, elem('div', { className: 'glyphEditTools' },
    elem('div', { className: 'glyphEditHead' }, elem('span', { className: 'glyphEditChar', textContent: String.fromCodePoint(g.cp) }),
      elem('span', { className: 'glyphEditCode', textContent: `U+${g.cp.toString(16).toUpperCase().padStart(4, '0')}` }), del),
    elem('div', { className: 'glyphEditControls' }, elem('label', null, 'Ink ', level, swatch), elem('label', null, 'Character width ', advance)))),
    elem('small', { className: 'shotNote', textContent: 'Left-drag paints, right-drag erases.' + (f.drawn < pack.cellH ? ` The game shows only the top ${f.drawn} of the ${pack.cellH} rows: the red rows at the bottom are cut off, so anything painted there never shows.` : '') }));
}
