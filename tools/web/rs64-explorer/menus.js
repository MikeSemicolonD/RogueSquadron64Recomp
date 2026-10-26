// ---- Menus tab: a menu mod's roguesq_menu.json (docs/guides/adding-menus-and-buttons.md) on a preview of the game's menus ----
// The list holds the front-end and pause menus and the mod's pages; the preview draws a menu as the game builds it (menuLayout over the
// game's own entries and default buttons) in the front-end font; the side panel edits the picked button. New buttons go into the picked
// export group (EXPORT_GROUPS, levels.js).
let menuSel = null, menuPick = null, menuLang = 'en', menuGroup = '';
const MENU_TYPES = ['action', 'toggle', 'cycle', 'slider', 'submenu', 'label'];
const MENU_KEYS = {
  action: ['quit', 'reset_controls', 'open_mods_folder', 'touch_layout', 'none'],
  toggle: ['fullscreen', 'gyro', 'widescreen', 'frame_interpolation', 'cutscene_draw_distance', 'framebuffer_writeback', 'rt_lights', 'rt_shadows', 'rt_soft_shadows', 'rt_fog_shafts', 'rumble'],
  cycle: ['draw_distance', 'resolution', 'antialiasing', 'target_fps', 'texture_filter', 'upscale_filter'],
  slider: ['draw_distance', 'mouse_sensitivity', 'mouse_smoothing', 'deadzone'],
};
// Preview geometry in the 640 x 480 screen: list_base per menu from the menu builder; measured in the game: a title's top is 56 + its y (the game's own
// titles have y 10) and its centre 320 + 1.25 x, the main menu's left edge, BACK's extra gap and the pause layout.
const MENU_TITLE_BASE = 56, MENU_TITLE_Y = 10, MENU_TITLE_X_SCALE = 1.25, MENU_BACK_GAP = 22, MENU_MAIN_LEFT = 249, MENU_PAUSE_PITCH = 17.6, MENU_PAUSE_NAME_X = 236, MENU_PAUSE_VALUE_X = 378;
const MENU_LIST_BASE = { main_menu: 165, options: 30, controller_settings: -90 };
// The built-in conditions (button_visible in menu_config.cpp; docs/adding-menus-and-buttons.md "requires"), with what each checks.
const MENU_CONDITIONS = {
  desktop: 'Only on desktop (Windows, Linux, macOS), not on Android',
  front_end: 'Only when this page is opened from the front end, not from the pause menu: for settings the game reads only when a level loads',
  ray_query: 'Only when the GPU supports ray queries (needed for the ray-tracing settings)',
  expansion_pak: 'Only with the Expansion Pak (the game\'s high-resolution mode needs it)',
  frame_interpolation: 'Only while frame interpolation is on',
};
const MENU_CONDITION_LABEL = { desktop: 'Using Desktop', front_end: 'Opened From Front End', ray_query: 'Supports RT', expansion_pak: 'Uses Expansion Pak', frame_interpolation: 'Uses Frame Interpolation' };
const MENU_TEXT_FIELDS = { action: ['label', 'confirm'], toggle: ['label_on', 'label_off'], cycle: ['label', 'label_other'], slider: ['label', 'label_off', 'label_on'], submenu: ['label', 'title'], label: ['label'] };
const MENU_FIELD_NAME = { label: 'Label', label_on: 'Label when on', label_off: 'Label when off', confirm: 'Confirm question', label_other: 'Label for a custom value', title: 'Page title' };
const MENU_KEY_FIELD = { action: 'action', toggle: 'toggle', cycle: 'cycle', slider: 'slider', submenu: 'action' };
const LANG_MENU = 'mod/lang/menu.json';
const readMenu = async () => menuParse(await virtualText(MENU_FILE));
const writeMenu = (doc, label) => editText(MENU_FILE, menuWrite(doc), label);
// Codes a label can carry: the game's three plus the project's language mod.
async function menuLangs() {
  const mod = await readJson('mod/lang/language.json', () => null);
  return ['en', 'de', 'fr', ...(mod?.code ? [mod.code] : [])];
}
const menuPagesOf = (doc) => doc.buttons.filter((b) => b.type === 'submenu' && b.page && !NATIVE_MENUS[b.page]);
// The list's pages, each under the menu that opens it (depth = pages above it).
let menuPageCache = [];
async function menuRefreshPages() {
  const { doc } = await readMenu(), openers = doc ? menuPagesOf(doc) : [], out = [];
  const depth = (page, seen = new Set()) => {
    const o = openers.find((b) => b.page === page);
    return !o || NATIVE_MENUS[o.menu] || seen.has(page) ? 0 : 1 + depth(o.menu, seen.add(page));
  };
  for (const o of openers) if (!out.some((p) => p.page === o.page)) out.push({ page: o.page, from: o.menu, depth: depth(o.page) });
  menuPageCache = out;
}
// A label in the picked language: a text key's translation (lang/menu.json for a language mod's code), the map's own entry, then en.
function menuText(v, langMenu) {
  if (v == null || typeof v === 'string') return v ?? '';
  return (v.key && langMenu?.[v.key]) ?? v[menuLang] ?? v.en ?? Object.entries(v).find(([k, t]) => k !== 'key' && typeof t === 'string')?.[1] ?? '';
}
function menuRowText(b, langMenu) {
  const t = (f) => menuText(b[f], langMenu);
  if (b.type === 'toggle') return t('label_off') || t('label_on');
  if (b.type === 'cycle') return `${t('label')}: ...`;
  return t('label');
}
// One line of menu text in the front-end font (italic35), white on transparent, then tinted.
// One line of menu text in a game font, tinted: style = { font, sx, sy (per font pixel, in the 640 x 480 screen), spacing (font px after each glyph),
// space (a space's advance, font px), shadow (1 px dark copy down-right, as the pause menus draw) }.
const MENU_FRONT_STYLE = { font: 'italic35', sx: 1.09, sy: 0.91, spacing: 0, space: 11, shadow: false };
const MENU_PAUSE_STYLE = { font: 'subtitle8', sx: 2, sy: 2, spacing: 1, space: 4, shadow: true };
async function menuLineCanvas(text, color, style, scale = 1) {
  const f = gameFonts(romBytes).find((x) => x.name === style.font), sheet = await fontSheet(f), chars = [...String(text).toUpperCase()];
  const at = chars.map((c) => f.charset.indexOf(c)), adv = (i, c) => (i >= 0 ? f.widths[i] + style.spacing : c === ' ' ? style.space : f.cellW / 2);
  const width = Math.max(1, Math.ceil(at.reduce((w, i, k) => w + adv(i, chars[k]), 0))) + 1, height = f.drawn + 1;
  const glyphs = elem('canvas', { width, height }), gx = glyphs.getContext('2d');
  let x = 0;
  at.forEach((i, k) => {
    if (i >= 0) gx.drawImage(sheet, 0, i * f.cellH, f.cellW, f.drawn, Math.round(x), 0, f.cellW, f.drawn);
    x += adv(i, chars[k]);
  });
  const tint = (c) => {
    const cv = elem('canvas', { width, height }), cx = cv.getContext('2d');
    cx.drawImage(glyphs, 0, 0);
    cx.globalCompositeOperation = 'source-in';
    cx.fillStyle = c;
    cx.fillRect(0, 0, width, height);
    return cv;
  };
  const out = elem('canvas', { width, height }), ox = out.getContext('2d');
  if (style.shadow) ox.drawImage(tint('#1a0d00'), 1, 1);
  ox.drawImage(tint(color), 0, 0);
  Object.assign(out.style, { width: `${width * style.sx * scale}px`, height: `${height * style.sy * scale}px` });
  return out;
}
async function showMenu(name) {
  menuSel = name;
  await menuRefreshPages();
  renderList();
  const panel = $('menuPanel'), { doc, error } = await readMenu();
  if (error) {
    panel.replaceChildren(elem('b', { textContent: 'roguesq_menu.json' }), elem('small', { className: 'shotNote', textContent: `The menu file can't be read: ${error}` }));
    panel.hidden = false;
    return;
  }
  const native = NATIVE_MENUS[name], page = !native && menuPagesOf(doc).find((b) => b.page === name);
  const full = menuWithDefaults(doc), langMenu = await readJson(LANG_MENU, () => ({}));
  const { rows, dropped } = menuLayout(full, name, native?.entries ?? [], () => true, native?.max ?? 7);
  const groups = await readJson(EXPORT_GROUPS, () => ({ groups: [], assign: {} }));
  // Toolbar: import, preview language, the group new buttons go in, add a button.
  const tool = (icon, tip, fn) => {
    const b = elem('button', { title: tip });
    setEditIcon(b, icon);
    b.onclick = fn;
    return b;
  };
  const lang = elem('select', { title: 'Language the preview and the label fields show' }, ...(await menuLangs()).map((c) => new Option(c, c)));
  lang.value = menuLang;
  lang.onchange = () => {
    menuLang = lang.value;
    showMenu(name);
  };
  const group = elem('select', { title: 'Which exported mod new buttons belong to (Export mod can move them later)' }, new Option('UI mod (default)', ''), ...(groups.groups ?? []).map((g) => new Option(g.name, g.name)), new Option('New mod...', '+'));
  group.value = menuGroup;
  group.onchange = async () => {
    if (group.value === '+') {
      const n = prompt('Name of the new mod');
      if (n && !(groups.groups ?? []).some((g) => g.name === n)) await editText(EXPORT_GROUPS, JSON.stringify({ ...groups, groups: [...(groups.groups ?? []), { name: n }] }, null, 1), `Group ${n}`);
      menuGroup = n || menuGroup;
      return showMenu(name);
    }
    menuGroup = group.value;
  };
  const tools = elem('div', { className: 'fontTools' }, tool('open', 'Import menu files: any menu JSON (and menu_text.json), added on top of this mod\'s menu as the game layers mods', importMenuFile),
    elem('label', null, 'Language ', lang), elem('label', null, 'Mod for new buttons ', group));
  if (!native?.locked && !native?.limit) tools.append(tool('add', 'Add a button at the end of this menu', () => addMenuButton(name, {})));
  // The preview: the game's 640 x 480 screen (4:3). Front-end lines sit where setupMenuData puts them: top = 240 + list_base + 36 i - 18 n + y
  // (gCurrentMenuData +0x96, docs/guides/adding-menus-and-buttons.md), centred plus x (the main menu is left-aligned at 249 + x). Pause menus are the HUD's:
  // subtitle8 text at 2x (HUD font id 4, 0x80139014), one line per 17.6 px with the block centred, the game's toggles as a name at x 236 and a value at x 378 (measured in the game).
  const pause = name.startsWith('pause') || NATIVE_MENUS[page?.menu]?.pause, style = pause ? MENU_PAUSE_STYLE : MENU_FRONT_STYLE;
  const box = elem('div', { className: 'menuPreview' });
  const title = native ? menuText(native.title) : menuText(page.title ?? page.label, langMenu);
  const place = (el, x, top, left = false) => {
    Object.assign(el.style, { left: `${left ? x : 320 + x}px`, top: `${top}px`, transform: left ? 'none' : '' });
    box.append(el);
  };
  const shown = [...rows, ...(page ? [{ native: true, alias: null, entry: { label: { en: 'BACK' } } }] : [])];
  const base = MENU_LIST_BASE[name] ?? 40, lift = name === 'main_menu' && shown.length > 3 ? 36 * (shown.length - 3) : 0;
  const pauseTop = 240 - ((shown.length + 1) * MENU_PAUSE_PITCH) / 2;
  // The game draws its own BACK lower than the formula (measured on OPTIONS).
  // A page is the host's layout (build_page): entries with the same row share a line, each gap_after pushes the lines below it down, the list moves
  // up by half the gaps, and BACK takes the line after the last with every gap above it.
  const pageLine = new Map();
  if (page && !pause) {
    let line = -1, gap = 0, lineGap = 0;
    const gaps = shown.reduce((s, r) => s + (r.button?.gap_after ?? 0), 0);
    shown.forEach((r, v) => {
      if (!(r.button?.row && v > 0 && shown[v - 1].button?.row === r.button.row)) {
        line++;
        lineGap = gap;
      }
      pageLine.set(r, 36 * line + lineGap - Math.trunc(gaps / 2));
      gap += r.button?.gap_after ?? 0;
    });
  }
  const lineTop = (r) => (pause ? pauseTop + MENU_PAUSE_PITCH * (shown.indexOf(r) + 1)
    : page ? 240 + base + pageLine.get(r) - 18 * shown.length + (r.native ? 0 : r.button.y ?? 0)
      : 240 + base + 36 * shown.indexOf(r) - 18 * shown.length + (r.native ? r.y ?? 0 : r.button.y ?? 0) - lift + (r.native && r.alias === 'back' ? native?.backGap ?? MENU_BACK_GAP : 0));
  // A page's title belongs to the button that opens it (title, title_y); a front-end menu's is the file's "titles" entry (label, x, y, which
  // the game writes over its own). Clicking the title picks it, dragging it moves it. The pause menus' titles are the HUD's and stay.
  const own = !pause && native ? doc.titles?.[name] : null, editable = page || (!pause && native);
  const shownTitle = own?.label ? menuText(own.label, langMenu) : title, tx = MENU_TITLE_X_SCALE * (own?.x ?? 0), ty = page ? page.title_y ?? 0 : own?.y ?? MENU_TITLE_Y;
  const titleKey = page ? page.id : 'title:' + name;
  const titleRow = elem('div', { className: 'menuLine' + (editable ? ' menuRow' + (menuPick === titleKey ? ' sel' : '') : ''),
    title: page ? `The page title (set on ${page.id}): click to edit, drag up or down to move it` : editable ? 'The menu\'s title: click to change it, drag to move it' : 'The pause menu\'s own title' },
    shownTitle ? await menuLineCanvas(shownTitle, '#e8b84a', style) : elem('span', { className: 'menuGap', textContent: editable ? 'title' : '' }));
  if (editable) {
    titleRow.onpointerdown = (e) => {
      if (e.button !== 0) return;
      const x0 = e.clientX, y0 = e.clientY;
      let moved = false;
      titleRow.setPointerCapture(e.pointerId);
      titleRow.onpointermove = (m) => {
        moved ||= Math.abs(m.clientX - x0) + Math.abs(m.clientY - y0) > 2;
        if (moved) titleRow.style.transform = `translate(calc(-50% + ${page ? 0 : m.clientX - x0}px), ${m.clientY - y0}px)`;
      };
      titleRow.onpointerup = async (u) => {
        titleRow.onpointermove = titleRow.onpointerup = null;
        menuPick = titleKey;
        if (!moved) return showMenu(name);
        const dx = Math.round(u.clientX - x0), dy = Math.round(u.clientY - y0);
        if (page) {
          return editMenuButton(page.id, (b) => {
            const v = (b.title_y ?? 0) + dy;
            if (v) b.title_y = v;
            else delete b.title_y;
          }, `Move ${name} title`);
        }
        // The game writes a given offset as is (0 too), so both stay once moved; x moves 1.25 px per unit.
        await editMenuTitle(name, (t) => {
          t.x = (t.x ?? 0) + Math.round(dx / MENU_TITLE_X_SCALE);
          t.y = (t.y ?? MENU_TITLE_Y) + dy;
        }, `Move ${name} title`);
      };
    };
  }
  if (shownTitle || editable) place(titleRow, tx, pause ? pauseTop : MENU_TITLE_BASE + ty);
  const drawRow = async (r, cut) => {
    const text = r.native ? menuText(r.entry.label) : menuRowText(r.button, langMenu), key = r.native ? 'native:' + (r.alias ?? '') : r.id;
    // The game's entries and the port's built-in buttons (LANGUAGE, QUIT) not yet overridden in the file are grey; the mod's are yellow.
    const builtin = !r.native && menuIsBuiltin(doc, r.id);
    const color = key === menuPick ? '#ffffff' : r.native ? (r.entry.placeholder ? '#5a5a5a' : '#8a8a8a') : builtin ? '#8a8a8a' : '#e8b84a';
    const row = elem('div', { className: 'menuRow' + (key === menuPick ? ' sel' : '') + (cut ? ' cut' : '') + (r.native ? ' native' : ''), title: r.entry?.placeholder ? 'The mission\'s objectives show here' : r.native ? (r.alias ? `The game's ${r.alias}: click to hide or replace it` : 'The game\'s own entry (no alias, so it stays)') : `${r.id}: click to edit, drag to move` + (cut ? '. The game cuts this: the menu is full' : '') });
    row.append(r.native && r.entry.spacer ? elem('span', { className: 'menuGap', textContent: pause ? '' : 'gap' }) : await menuLineCanvas(text || ' ', color, style, r.button?.scale ?? 1));
    if (pause && r.native && r.entry.value) {
      const v = await menuLineCanvas(r.entry.value, color, style);
      v.classList.add('menuValue');
      v.style.left = `${MENU_PAUSE_VALUE_X - MENU_PAUSE_NAME_X}px`;
      row.append(v);
    }
    row.onclick = () => {
      menuPick = key;
      showMenu(name);
    };
    // Left-drag a mod button to move it: the preview is the game's 640 x 480, so the pointer's travel is its x / y offset change.
    if (!r.native && !native?.locked && !cut && !pause) {
      row.onpointerdown = (e) => {
        if (e.button !== 0) return;
        const x0 = e.clientX, y0 = e.clientY, base = row.style.transform;
        let moved = false;
        row.setPointerCapture(e.pointerId);
        row.onpointermove = (m) => {
          const dx = m.clientX - x0, dy = m.clientY - y0;
          moved ||= Math.abs(dx) + Math.abs(dy) > 2;
          if (moved) row.style.transform = `${base} translate(${dx}px, ${dy}px)`;
        };
        row.onpointerup = async (u) => {
          row.onpointermove = row.onpointerup = null;
          if (!moved) return;
          const dx = Math.round(u.clientX - x0), dy = Math.round(u.clientY - y0);
          menuPick = r.id;
          await editMenuButton(r.id, (b) => {
            for (const [k, d] of [['x', dx], ['y', dy]]) {
              const v = (b[k] ?? 0) + d;
              if (v) b[k] = v;
              else delete b[k];
            }
          }, `Move ${r.id}`);
        };
      };
      row.onclick = (e) => {
        if (row.style.transform.includes('translate(')) return e.preventDefault();
        menuPick = key;
        showMenu(name);
      };
    }
    if (cut) return cutList.append(row);
    if (key === menuPick && !native?.locked && !native?.limit && (!r.native || r.alias)) menuInsertButtons(name, r, row);
    row.classList.add('menuLine');
    const x = r.native ? r.entry.x ?? 0 : r.button.x ?? 0;
    if (pause && r.native && r.entry.value) place(row, MENU_PAUSE_NAME_X, lineTop(r), true);
    else if (native?.align === 'left') place(row, MENU_MAIN_LEFT + x, lineTop(r), true);
    else place(row, x, lineTop(r));
  };
  const cutList = elem('div', { className: 'menuCut' });
  for (const r of shown) await drawRow(r, false);
  for (const r of dropped) await drawRow(r, true);
  // Slots used at the toolbar's end; what to do is its tooltip.
  const used = rows.length + dropped.length + (page ? 1 : 0), max = (native?.max ?? 7) + (page ? 1 : 0);
  const hint = native?.locked ?? native?.note ?? (dropped.length ? `The game cuts ${dropped.length} entr${dropped.length > 1 ? 'ies' : 'y'} (struck through, below): hide or replace one of the game's entries to make room.`
    : name === 'pause' ? 'Objective lines are placeholders. ' + 'Click a button to edit it.' : 'Click a button to edit it, or one of the game\'s entries to hide or replace it.');
  tools.append(elem('div', { className: 'menuStatus' }, elem('span', { className: 'menuSlots' + (used > max ? ' full' : ''), textContent: `${used} of ${max} slots`, title: hint })));
  // The side panel: the picked entry's actions (and a mod button's settings); every entry, hidden ones too, is listed at the far right.
  const pick = menuPick && !menuPick.startsWith('native:') && full.buttons.find((b) => b.id === menuPick);
  const pickedNative = menuPick?.startsWith('native:') && [...shown, ...dropped].find((r) => r.native && 'native:' + (r.alias ?? '') === menuPick);
  let side = menuPick === 'title:' + name && native && !pause ? menuTitlePanel(name, doc, langMenu) : '';
  if (pick) side = elem('div', null, menuActionBar(menuActions(name, { native: false, id: pick.id, button: pick }, rows)), await menuInspector(name, doc, pick, groups, langMenu));
  else if (pickedNative) side = elem('div', { className: 'menuInspector' }, elem('b', { textContent: menuText(pickedNative.entry.label) ? `${menuText(pickedNative.entry.label)} button` : 'Gap' }),
    pickedNative.alias && !native?.locked && native?.limit !== 'one' ? menuActionBar(menuActions(name, pickedNative)) : elem('small', { className: 'shotNote', textContent: native?.locked ?? native?.note ?? 'This entry has no alias, so mods can\'t hide or move it.' }));
  panel.replaceChildren(elem('b', { textContent: native ? name.replace(/_/g, ' ') : `Page: ${name}` }), tools, elem('hr', { className: 'fontDivider' }),
    elem('div', { className: 'menuEditor' }, elem('div', null, box, cutList), side, menuEntryList(name, doc, shown, dropped, langMenu)));
  panel.hidden = false;
}
// A new button: unique id, the picked group, a starting label; extra holds the placement (replace / before / after).
async function addMenuButton(menu, extra) {
  const { doc } = await readMenu();
  let n = doc.buttons.length + 1;
  while (doc.buttons.some((b) => b.id === `button_${n}`)) n++;
  const b = { id: `button_${n}`, menu, type: 'action', action: 'none', label: 'NEW BUTTON', ...extra };
  if (b.type !== 'action') delete b.action;
  await historyGroup(`Add ${b.id}`, async () => {
    await writeMenu({ ...doc, buttons: [...doc.buttons, b] }, `Add ${b.id}`);
    if (menuGroup) {
      const g = await readJson(EXPORT_GROUPS, () => ({ groups: [], assign: {} }));
      await editText(EXPORT_GROUPS, JSON.stringify({ ...g, assign: { ...(g.assign ?? {}), [menuItemKey(doc, b)]: menuGroup } }, null, 1), `Group ${b.id}`);
    }
  });
  menuPick = b.id;
  showMenu(menu);
}
// A front-end menu's title override ("titles": { <menu>: { label, x, y } }); an empty one is removed.
async function editMenuTitle(menu, change, label) {
  const { doc } = await readMenu(), titles = { ...(doc.titles ?? {}) }, t = { ...(titles[menu] ?? {}) };
  change(t);
  if (Object.keys(t).length) titles[menu] = t;
  else delete titles[menu];
  if (Object.keys(titles).length) doc.titles = titles;
  else delete doc.titles;
  await writeMenu(doc, label);
  showMenu(menuSel);
}
// The picked menu title: its text in the preview language (the globe has every language), x / y, and Reset.
function menuTitlePanel(menu, doc, langMenu) {
  const t = doc.titles?.[menu] ?? {}, map = typeof t.label === 'object' && t.label ? t.label : t.label == null ? {} : { en: t.label };
  const field = (label, input, tip) => elem('label', { className: 'menuField', title: tip ?? '' }, elem('span', { textContent: label }), input);
  const text = elem('input', { value: map[menuLang] ?? '', placeholder: `(${menuText(NATIVE_MENUS[menu].title) || 'no title'})` });
  text.onchange = () => editMenuTitle(menu, (x) => {
    const next = { ...map, [menuLang]: text.value };
    if (!text.value) delete next[menuLang];
    const keys = Object.keys(next);
    if (!keys.length) delete x.label;
    else x.label = keys.length === 1 && keys[0] === 'en' ? next.en : next;
  }, `${menu} title`);
  const globe = elem('button', { title: 'Every language\'s text for this title, and its text key' });
  setEditIcon(globe, 'globe');
  globe.onclick = () => menuTranslations(menu, 'title:' + menu, 'label');
  const num = (k) => {
    const n = elem('input', { type: 'number', value: t[k] ?? '', placeholder: k === 'y' ? `${MENU_TITLE_Y} (the game's)` : '0' });
    n.onchange = () => editMenuTitle(menu, (x) => (n.value === '' ? delete x[k] : (x[k] = +n.value)), `${menu} title ${k}`);
    return n;
  };
  const reset = elem('button', { textContent: 'Use the game\'s title', title: 'Remove this menu\'s title override' });
  reset.onclick = () => editMenuTitle(menu, (x) => Object.keys(x).forEach((k) => delete x[k]), `${menu} title reset`);
  return elem('div', { className: 'menuInspector' }, elem('b', { textContent: 'Menu title' }), field('Text', elem('span', { className: 'menuText' }, text, globe)),
    field('X offset', num('x'), 'Right of the centre, in the game\'s units: 1 moves it 1.25 px (or drag the title)'), field('Y offset', num('y'), 'Pixels down from the top; the game\'s own titles use 10 (or drag the title)'), reset);
}
// A built-in button (QUIT, LANGUAGE, ...) the file does not override yet.
const menuIsBuiltin = (doc, id) => MENU_DEFAULT_BUTTONS.some((d) => d.id === id) && !doc.buttons.some((b) => b.id === id);
// A built-in button (QUIT, LANGUAGE, ...) not yet in the file is copied in on its first edit: the same id overrides it in the game.
async function editMenuButton(id, change, label) {
  const { doc } = await readMenu(), builtin = MENU_DEFAULT_BUTTONS.find((x) => x.id === id);
  let b = doc.buttons.find((x) => x.id === id);
  if (!b && builtin) doc.buttons.push((b = structuredClone(builtin)));
  if (!b) return;
  change(b);
  await writeMenu(doc, label);
  showMenu(menuSel);
}
// What the picked entry offers, as [label, tip, action]: the game's entries can be hidden, replaced or have buttons added beside them; a mod
// button can have one added after it, open its page or be deleted.
// The x / y / scale a new button takes from the entry it is placed by, so it lines up with it.
function menuPlacementOf(r) {
  const from = r.native ? { x: r.entry.x, y: r.y } : { x: r.button.x, y: r.button.y, scale: r.button.scale };
  return Object.fromEntries(Object.entries(from).filter(([, v]) => v));
}
function menuActions(menu, r, rows = []) {
  const at = menuPlacementOf(r);
  // Sound Settings takes only sliders, each in a native entry's place: the game's volume bar with the mod's own value.
  if (r.native && r.alias && NATIVE_MENUS[menu]?.limit === 'slider') {
    return [['Replace with a slider', 'A slider in its place, drawn with the game\'s volume bar; set what it changes and its range', () => addMenuButton(menu, {
      replace: r.alias, type: 'slider', slider: '', label: menuText(r.entry.label), label_off: 'MIN', label_on: 'MAX', min: 0, max: 100, step: 10 })]];
  }
  if (r.native && r.alias) {
    return [['', 'Hide: remove it from this menu (Show again in the entry list brings it back)', async () => {
      const { doc } = await readMenu();
      await writeMenu({ ...doc, hide: { ...(doc.hide ?? {}), [menu]: [...new Set([...(doc.hide?.[menu] ?? []), r.alias])] } }, `Hide ${r.alias}`);
      menuPick = null;
      showMenu(menu);
    }, 'eye'], ['Replace with a new button', 'A new button in its place; the game\'s entry goes', () => addMenuButton(menu, { replace: r.alias, ...at })]];
  }
  if (r.native) return [];
  // Move up / down: its line index among the menu's lines (the drag sets x / y instead).
  const line = rows.findIndex((x) => x.id === r.id);
  const move = (d) => editMenuButton(r.id, (b) => {
    for (const k of ['before', 'after', 'order']) delete b[k];
    b.index = line + d;
  }, `Move ${r.id} ${d < 0 ? 'up' : 'down'}`);
  return [...(line > 0 ? [['', 'Move up one line', () => move(-1), 'up']] : []), ...(line >= 0 && line < rows.length - 1 ? [['', 'Move down one line', () => move(1), 'down']] : []),
    ...(r.button.type === 'submenu' && r.button.page ? [['Open its page', '', () => showMenu(r.button.page)]] : []),
    ['', 'Delete: remove this button from the menu file', async () => {
      const { doc } = await readMenu();
      await writeMenu({ ...doc, buttons: doc.buttons.filter((b) => b.id !== r.id) }, `Delete ${r.id}`);
      menuPick = null;
      showMenu(menu);
    }, 'delete']];
}
// Items are [label, tip, action, icon]; an icon button shows the icon and keeps the tip as its title.
const menuActionBar = (items) => elem('div', { className: 'menuActions' }, ...items.map(([t, tip, fn, icon]) => {
  const b = elem('button', { textContent: t, title: tip });
  if (icon) setEditIcon(b, icon);
  b.onclick = fn;
  return b;
}));
// The + buttons above and below the picked entry: a new button before or after it, lined up with it.
function menuInsertButtons(menu, r, row) {
  const at = menuPlacementOf(r), anchor = r.native ? r.alias : r.id;
  for (const [side, place] of [['before', 'menuPlusAbove'], ['after', 'menuPlusBelow']]) {
    const b = elem('button', { className: 'menuPlus ' + place, title: `Add a button ${side} this one` });
    setEditIcon(b, 'add');
    b.onpointerdown = (e) => e.stopPropagation();
    b.onclick = (e) => {
      e.stopPropagation();
      addMenuButton(menu, { [side]: anchor, ...at });
    };
    row.append(b);
  }
}
// Every entry of the menu in the game's order, tagged (the game's, cut, hidden), clicking one picks it; a hidden one has Show again.
function menuEntryList(menu, doc, shown, dropped, langMenu) {
  const showAgain = (a) => async () => {
    const { doc: d } = await readMenu(), rest = (d.hide?.[menu] ?? []).filter((x) => x !== a), hide = { ...(d.hide ?? {}), [menu]: rest };
    if (!rest.length) delete hide[menu];
    await writeMenu({ ...d, hide }, `Show ${a}`);
    showMenu(menu);
  };
  const item = (r, tag) => {
    const key = r.native ? 'native:' + (r.alias ?? '') : r.id;
    const text = r.native ? menuText(r.entry.label) || '(gap)' : menuRowText(r.button, langMenu) || r.id;
    const row = elem('div', { className: 'menuEntry' + (key === menuPick ? ' sel' : '') + (tag ? ' ' + tag : ''), title: r.native ? (r.alias ?? 'no alias') : r.id },
      elem('span', { textContent: text }), elem('small', { textContent: tag === 'hidden' ? 'hidden' : tag === 'cut' ? 'cut' : r.native ? 'game' : menuIsBuiltin(doc, r.id) ? 'built-in' : '' }));
    // A mod button's languages: every code any of its text fields has (a plain string is English).
    if (!r.native) {
      const codes = new Set(Object.keys(MENU_FIELD_NAME).flatMap((f) => (r.button[f] == null ? [] : typeof r.button[f] === 'string' ? ['en'] : Object.keys(r.button[f]).filter((k) => k !== 'key'))));
      const globe = elem('span', { className: 'menuLangs', title: `${codes.size} language${codes.size === 1 ? '' : 's'}: ${[...codes].join(', ') || 'none'}` });
      setEditIcon(globe, 'globe');
      globe.append(elem('b', { textContent: codes.size }));
      row.insertBefore(globe, row.querySelector('small'));
    }
    if (tag === 'hidden') {
      const b = elem('button', { textContent: 'Show again' });
      b.onclick = showAgain(r.alias);
      row.append(b);
    } else {
      row.onclick = () => {
        menuPick = key;
        showMenu(menu);
      };
    }
    return row;
  };
  const hidden = (NATIVE_MENUS[menu]?.entries ?? []).filter((e) => e.alias && (doc.hide?.[menu] ?? []).includes(e.alias)).map((entry) => ({ native: true, alias: entry.alias, entry }));
  return elem('div', { className: 'menuEntries' }, elem('b', { textContent: 'Entries' }), ...shown.map((r) => item(r)), ...dropped.map((r) => item(r, 'cut')), ...hidden.map((r) => item(r, 'hidden')));
}
// The picked button's settings; each change is one edit of the menu file (or of lang/menu.json for a keyed label in a language mod's code).
async function menuInspector(menu, doc, b, groups, langMenu) {
  const field = (label, input, tip) => elem('label', { className: 'menuField', title: tip ?? '' }, elem('span', { textContent: label }), input);
  const set = (k, v, label) => editMenuButton(b.id, (x) => (v === '' || v == null ? delete x[k] : (x[k] = v)), label ?? `${b.id}: ${k}`);
  const type = elem('select', null, ...MENU_TYPES.map((t) => new Option(t, t)));
  type.value = b.type ?? 'action';
  type.onchange = () => editMenuButton(b.id, (x) => {
    const old = MENU_KEY_FIELD[x.type ?? 'action'], key = old && x[old];
    if (old) delete x[old];
    const text = x.label ?? x.label_off ?? x.label_on;
    for (const f of Object.keys(MENU_FIELD_NAME)) if (!MENU_TEXT_FIELDS[type.value].includes(f)) delete x[f];
    x.type = type.value;
    if (MENU_KEY_FIELD[x.type] && key) x[MENU_KEY_FIELD[x.type]] = key;
    // The old text starts the new type's first field.
    if (text !== undefined && x[MENU_TEXT_FIELDS[x.type][0]] === undefined) x[MENU_TEXT_FIELDS[x.type][0]] = text;
    if (x.type === 'submenu' && !x.page) x.page = x.id;
  }, `${b.id}: type`);
  const parts = [elem('b', { textContent: b.id }), field('Type', type)];
  const kf = MENU_KEY_FIELD[b.type ?? 'action'];
  if (kf && b.type !== 'submenu') {
    // The built-in keys for its type, or Custom: @name (a function the mod's own code exports) or config:<option id>, typed in.
    const tip = 'What it runs or changes: a built-in key, @name for a function your own code mod exports, or config:<option id> for one of your mod\'s config options';
    const builtins = MENU_KEYS[b.type ?? 'action'] ?? [], cur = b[kf] ?? '', custom = cur !== '' && !builtins.includes(cur);
    const pick = elem('select', { title: tip }, new Option('(none chosen)', ''), ...builtins.map((k) => new Option(k, k)), new Option('Custom...', '+'));
    pick.value = custom ? '+' : cur;
    const typed = elem('input', { value: custom ? cur : '', placeholder: '@name or config:<option id>', title: tip, hidden: !custom });
    pick.onchange = () => {
      if (pick.value !== '+') return set(kf, pick.value);
      typed.hidden = false;
      typed.focus();
    };
    typed.onchange = () => set(kf, typed.value.trim());
    parts.push(field((b.type ?? 'action') === 'action' ? 'Action' : 'Setting', elem('span', { className: 'menuKeyPick' }, pick, typed), tip));
  }
  if (b.type === 'submenu') {
    // Where it goes: one of the mod's pages, one of the game's front-end menus, or a new page (named here; its buttons use that name as their menu).
    const pages = [...new Set(menuPagesOf(doc).map((x) => x.page))], gameMenus = ['main_menu', 'options', 'game_settings', 'sound_settings', 'controller_settings'];
    const pg = elem('select', { title: 'The menu this button opens' },
      elem('optgroup', { label: 'This mod\'s pages' }, ...pages.map((p) => new Option(p, p))),
      elem('optgroup', { label: 'The game\'s menus' }, ...gameMenus.map((m) => new Option(m.replace(/_/g, ' '), m))), new Option('New page...', '+'));
    pg.value = b.page ?? '';
    pg.onchange = () => {
      if (pg.value !== '+') return set('page', pg.value);
      const n = prompt('Name of the new page (letters, digits and _)', `${b.id}_page`)?.trim().toLowerCase().replace(/[^a-z0-9_]+/g, '_');
      if (!n || NATIVE_MENUS[n]) return showMenu(menu);
      set('page', n);
    };
    const ty = elem('input', { type: 'number', value: b.title_y ?? '', title: 'The page title\'s y offset in pixels (or drag the title in the page\'s preview)' });
    ty.onchange = () => set('title_y', ty.value === '' || +ty.value === 0 ? '' : +ty.value);
    parts.push(field('Opens page', pg), field('Title Y', ty, 'The page title\'s y offset in pixels (or drag the title in the page\'s preview)'));
  }
  if (b.type === 'slider') for (const k of ['min', 'max', 'step']) {
    const n = elem('input', { type: 'number', value: b[k] ?? '' });
    n.onchange = () => set(k, n.value === '' ? '' : +n.value);
    parts.push(field(k[0].toUpperCase() + k.slice(1), n));
  }
  // Text: the picked language's entry, and a text key other mods can translate.
  const langs = await menuLangs(), modCode = langs.length > 3 && menuLang === langs[3];
  const textField = (f) => {
    const v = b[f], map = typeof v === 'object' && v ? v : v == null ? {} : { en: v };
    const keyed = modCode && map.key;
    const input = elem('input', { value: keyed ? langMenu[map.key] ?? '' : map[menuLang] ?? '', placeholder: f === 'title' && !map[menuLang] && !map.en ? `(${menuText(b.label, langMenu)})` : menuLang === 'en' ? '' : `(${map.en ?? ''})` });
    const gaps = elem('small', { className: 'shotNote' });
    const warn = () => {
      const g = modCode ? [] : [...new Set(menuFontGaps(input.value))];
      gaps.textContent = g.length ? `The menu font can't draw ${g.join(' ')}: they show blank` : '';
    };
    input.oninput = warn;
    warn();
    input.onchange = async () => {
      if (keyed) {
        await editText(LANG_MENU, JSON.stringify({ ...langMenu, [map.key]: input.value }, null, 2), `${map.key}: ${menuLang}`);
        return showMenu(menu);
      }
      const next = { ...map, [menuLang]: input.value };
      if (!input.value) delete next[menuLang];
      const keys = Object.keys(next);
      set(f, !keys.length ? '' : keys.length === 1 && keys[0] === 'en' ? next.en : next, `${b.id}: ${f}`);
    };
    const globe = elem('button', { title: `Every language's text for this, and its text key${map.key ? ` (${map.key})` : ''}` });
    setEditIcon(globe, 'globe');
    globe.onclick = () => menuTranslations(menu, b.id, f);
    return [field(MENU_FIELD_NAME[f] ?? f, elem('span', { className: 'menuText' }, input, globe)), gaps];
  };
  for (const f of (MENU_TEXT_FIELDS[b.type ?? 'action'] ?? ['label']).filter((f) => f !== 'confirm')) parts.push(...textField(f));
  // The confirm question belongs to the action (asked before it runs), so it sits in the action's own section once there is one.
  const runs = (b.type ?? 'action') === 'action' && ((b.action && b.action !== 'none') || b.confirm != null);
  const actionEdit = runs && elem('div', { className: 'menuInspector menuActionEdit' }, elem('b', { textContent: 'Action Edit' }), ...textField('confirm'),
    elem('small', { className: 'shotNote', textContent: 'Asked before the action runs; leave it empty to run it straight away.' }));
  // front_end only means something on a custom page, which can open from the front end and the pause menu alike; the game's own menus are one or
  // the other. It stays listed where a button already has it, so it can be cleared.
  const onPage = !NATIVE_MENUS[b.menu ?? 'main_menu'], had = [b.requires ?? []].flat();
  const conds = Object.entries(MENU_CONDITIONS).filter(([c]) => c !== 'front_end' || onPage || had.includes(c));
  const putConds = (next) => set('requires', next.length ? (next.length === 1 ? next[0] : next) : '', `${b.id}: shows if`);
  // Conditions the list doesn't name (a mod config option, a native mod's own, a negated one) show as typed, ticked, so they can be cleared.
  const extra = had.filter((c) => !(c in MENU_CONDITIONS)).map((c) => [c, c.startsWith('!') ? `Only when ${c.slice(1)} does not hold` : 'Only when this holds']);
  const req = elem('div', { className: 'menuConds' }, ...[...conds, ...extra].map(([c, tip]) => {
    const box = elem('input', { type: 'checkbox', checked: had.includes(c) });
    box.onchange = () => putConds(box.checked ? [...had, c] : had.filter((x) => x !== c));
    return elem('label', { title: tip }, box, ' ' + (MENU_CONDITION_LABEL[c] ?? c));
  }));
  const typedTip = 'config:<mod id>:<option id> holds while that mod config option is set (not its first choice, or non-zero); a native mod\'s condition by name; ! in front for the opposite';
  const typedCond = elem('input', { placeholder: 'config:<mod id>:<option id>, or !name', title: typedTip });
  typedCond.onchange = () => {
    const c = typedCond.value.trim();
    if (c && !had.includes(c)) putConds([...had, c]);
  };
  req.append(typedCond);
  parts.push(field('Shows only if', req, 'Conditions that must all hold; native mods can add their own (type them in the file)'));
  // Text scale is a float (the game's entry_size_scaler); 1 is the menu's own size and is left out of the file.
  // Row, gap and scale work only on the mod's own pages (the page builder reads them); x / y work on front-end menus and pages, not the pause menus.
  const onOwnPage = !NATIVE_MENUS[b.menu ?? 'main_menu'], inPause = (b.menu ?? '').startsWith('pause');
  const layoutFields = [['row', 'Row', 'Entries with the same row share one line'], ['gap_after', 'Gap after', 'Extra pixels below the line'],
    ['scale', 'Text scale', '1 is the menu\'s own size; 0.5 is half, 1.5 half again'], ['x', 'X offset', 'Pixels right of where the game puts it (drag the button in the preview)'], ['y', 'Y offset', 'Pixels down (drag the button in the preview)']];
  for (const [k, label, tip] of layoutFields.filter(([k]) => (['row', 'gap_after', 'scale'].includes(k) ? onOwnPage : !inPause) || b[k] != null)) {
    const n = elem('input', { type: k === 'row' ? 'text' : 'number', value: b[k] ?? (k === 'scale' ? 1 : ''), title: tip, ...(k === 'scale' ? { step: 0.05, min: 0.1 } : {}) });
    n.onchange = () => set(k, n.value === '' || (k === 'scale' && +n.value === 1) ? '' : k === 'row' ? n.value : k === 'scale' ? Math.max(0.1, +n.value) : +n.value);
    parts.push(field(label, n, tip));
  }
  const g = elem('select', { title: 'Which exported mod this button belongs to' }, new Option('UI mod (default)', ''), ...(groups.groups ?? []).map((x) => new Option(x.name, x.name)));
  const itemKey = menuItemKey(doc, b);
  g.value = groups.assign?.[itemKey] ?? '';
  g.onchange = async () => {
    const assign = { ...(groups.assign ?? {}) };
    if (g.value) assign[itemKey] = g.value;
    else delete assign[itemKey];
    await editText(EXPORT_GROUPS, JSON.stringify({ ...groups, assign }, null, 1), `${b.id}: group`);
  };
  parts.push(field('Exported in mod', g, itemKey.startsWith('page:') ? 'A page\'s buttons export together' : ''));
  const main = elem('div', { className: 'menuInspector' }, ...parts);
  return actionEdit ? elem('div', { className: 'menuInspectorPair' }, main, actionEdit) : main;
}
// One text field in every language: rows for en / de / fr, the project's language mod and any other code the field has, + to add a code, and
// the text key other mods translate it by. A language mod's code with a key edits its lang/menu.json, as the game reads it there first.
async function menuTranslations(menu, id, f) {
  const dlg = $('menuLangDialog') ?? document.body.appendChild(elem('dialog', { id: 'menuLangDialog' }));
  // id is a button id, or title:<menu> for a menu's title override.
  const { doc } = await readMenu(), titleOf = id.startsWith('title:') ? id.slice(6) : null;
  const b = titleOf ? doc.titles?.[titleOf] ?? {} : doc.buttons.find((x) => x.id === id) ?? MENU_DEFAULT_BUTTONS.find((x) => x.id === id);
  if (!b) return;
  const v = b[f], map = typeof v === 'object' && v ? { ...v } : v == null ? {} : { en: v };
  const langs = await menuLangs(), modCode = langs.length > 3 ? langs[3] : null, langMenu = await readJson(LANG_MENU, () => ({}));
  const save = async (next, label) => {
    const keys = Object.keys(next);
    const put = (x) => {
      if (!keys.length) delete x[f];
      else x[f] = keys.length === 1 && keys[0] === 'en' ? next.en : next;
    };
    await (titleOf ? editMenuTitle(titleOf, put, label) : editMenuButton(id, put, label));
    menuTranslations(menu, id, f);
  };
  const codes = [...new Set([...langs, ...Object.keys(map).filter((k) => k !== 'key')])];
  const row = (code) => {
    const viaKey = code === modCode && map.key;
    const input = elem('input', { value: viaKey ? langMenu[map.key] ?? '' : map[code] ?? '', placeholder: code === 'en' ? '' : `(${map.en ?? ''})` });
    const gaps = elem('small', { className: 'shotNote' });
    const warn = () => {
      const g = code === modCode ? [] : [...new Set(menuFontGaps(input.value))];
      gaps.textContent = g.length ? `The menu font can't draw ${g.join(' ')}` : '';
    };
    input.oninput = warn;
    warn();
    input.onchange = async () => {
      if (viaKey) {
        await editText(LANG_MENU, JSON.stringify({ ...langMenu, [map.key]: input.value }, null, 2), `${map.key}: ${code}`);
        return menuTranslations(menu, id, f);
      }
      const next = { ...map, [code]: input.value };
      if (!input.value) delete next[code];
      save(next, `${id}: ${f} ${code}`);
    };
    const del = elem('button', { title: `Remove the ${code} text` });
    setEditIcon(del, 'delete');
    del.onclick = () => {
      const next = { ...map };
      delete next[code];
      save(next, `${id}: ${f} ${code} removed`);
    };
    return elem('div', { className: 'menuLangRow' }, elem('b', { textContent: code }), input, map[code] !== undefined ? del : elem('span'), gaps);
  };
  const key = elem('input', { value: map.key ?? '', placeholder: 'none', title: 'Other mods translate this text by this name: a language mod in its lang/menu.json, any mod in a menu_text.json' });
  key.onchange = () => {
    const next = { ...map, key: key.value.trim() };
    if (!next.key) delete next.key;
    save(next, `${id}: ${f} key`);
  };
  const code = elem('input', { placeholder: 'code, e.g. es', size: 8, title: 'An ISO 639-1 code, as a language mod uses' });
  const add = elem('button', { textContent: '+ Language' });
  add.onclick = () => {
    const c = code.value.trim().toLowerCase();
    if (c && !codes.includes(c)) save({ ...map, [c]: map.en ?? '' }, `${id}: ${f} ${c}`);
  };
  const close = elem('button', { textContent: 'Done' });
  close.onclick = () => dlg.close();
  dlg.replaceChildren(elem('h3', { textContent: `${MENU_FIELD_NAME[f] ?? f}: ${id}` }),
    elem('label', { className: 'menuLangRow' }, elem('b', { textContent: 'Text key' }), key),
    elem('small', { className: 'shotNote', textContent: 'A text key lets other mods translate this label. A missing language shows the English text.' }),
    ...codes.map(row), elem('div', { className: 'exportFoot' }, code, add, elem('span', { className: 'spacer' }), close));
  if (!dlg.open) dlg.showModal();
}
// Any menu JSON, whatever its name: each picked file that reads as a menu file (buttons, hide or titles) is layered onto the project's menu file
// in the order picked, as the game layers mods (menuMerge); a menu_text.json-shaped file ({ code: { key: text } }) merges into mod/menu_text.json.
// Files that are neither are listed, not applied. One undo step.
async function importMenuFile() {
  const files = await pickFiles('.json', true);
  if (!files.length) return;
  const menus = [], texts = [], skipped = [];
  for (const f of files) {
    const text = await f.text(), { doc, error } = menuParse(text), raw = (() => { try { return JSON.parse(text); } catch { return null; } })();
    const isMenu = !error && raw && ['buttons', 'hide', 'titles'].some((k) => k in raw);
    const isText = !isMenu && raw && typeof raw === 'object' && Object.values(raw).length && Object.values(raw).every((v) => v && typeof v === 'object' && Object.values(v).every((s) => typeof s === 'string'));
    if (isMenu) menus.push(doc);
    else if (isText) texts.push(raw);
    else skipped.push(`${f.name}${error ? `: ${error}` : ': not a menu file'}`);
  }
  if (!menus.length && !texts.length) return alert(`Nothing to import:\n${skipped.join('\n')}`);
  await historyGroup(`Import ${files.map((f) => f.name).join(', ')}`, async () => {
    if (menus.length) await writeMenu(menuMerge([(await readMenu()).doc ?? { buttons: [] }, ...menus]), 'Import menu files');
    if (texts.length) {
      const cur = await readJson('mod/menu_text.json', () => ({})), merged = { ...cur };
      for (const t of texts) for (const [code, keys] of Object.entries(t)) merged[code] = { ...(merged[code] ?? {}), ...keys };
      await editText('mod/menu_text.json', JSON.stringify(merged, null, 2), 'Import menu text');
    }
  });
  if (skipped.length) alert(`Imported; skipped:\n${skipped.join('\n')}`);
  menuPick = null;
  showMenu(menuSel ?? 'main_menu');
}
