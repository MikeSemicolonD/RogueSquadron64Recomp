'use strict';
// ---------- audio ----------
// parseMenuVoice table order, matched against the user's listening notes for each part.
const CRAFT_NAMES = ['Snowspeeder', 'X-wing', 'A-wing', 'Y-wing', 'V-wing', 'Millennium Falcon', 'TIE Interceptor'];
const PILOT_NAMES = ['Wedge', 'Dack', 'Hobbie', 'Zev', 'Wes', 'Kasan'];
let missionNames = [], frontText = [];
// A voice line's words: the SELECT LEVEL briefings (0x279 + level) are their level's description (front_TXT 0x21 + level), the rest are in voice_TXT.
const voiceWords = (v) => (v >= 0x279 && v <= 0x28B ? frontText[0x21 + v - 0x279] : voiceLine(v));
const titleCase = (s) => s.toLowerCase().replace(/(^|[\s-])([a-z]+)/g, (m, a, w) => a + (/^[iv]+$/.test(w) ? w.toUpperCase() : a && /^(at|on|of|the|for|from|above)$/.test(w) ? w : w[0].toUpperCase() + w.slice(1)));
// lv_X: X is the level index in base 36 (lv_a = level 10).
const lvName = (lv) => missionNames[parseInt(lv.slice(3), 36)] ?? lv;
let audioBank = null, audioCtx = null, audioSel = null, playToken = 0;
const svgIcon = (d) => `<svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true"><path fill="currentColor" d="${d}"/></svg>`;
const ICON = { loop: svgIcon('M4 5h7V3l3 3-3 3V7H4a1 1 0 0 0-1 1v1H1V8a3 3 0 0 1 3-3zm8 6H5v2l-3-3 3-3v2h7a1 1 0 0 0 1-1V7h2v1a3 3 0 0 1-3 3z'), play: svgIcon('M4 2.5v11l9-5.5z'), pause: svgIcon('M3.5 2.5h3v11h-3zM9.5 2.5h3v11h-3z'), stop: svgIcon('M3 3h10v10H3z'),
  speaker: svgIcon('M2 6h3l4-3.5v11L5 10H2zM11 5.2a4 4 0 0 1 0 5.6l-.9-.9a2.7 2.7 0 0 0 0-3.8zM12.8 3.4a6.5 6.5 0 0 1 0 9.2l-.9-.9a5.2 5.2 0 0 0 0-7.4z'),
  muted: svgIcon('M2 6h3l4-3.5v11L5 10H2zM10.6 5.5l.9-.9 1.7 1.7 1.7-1.7.9.9-1.7 1.7 1.7 1.7-.9.9-1.7-1.7-1.7 1.7-.9-.9 1.7-1.7z'),
  solo: svgIcon('M8 2a6 6 0 0 0-6 6v4.5A1.5 1.5 0 0 0 3.5 14H5V9H3.3V8a4.7 4.7 0 0 1 9.4 0v1H11v5h1.5a1.5 1.5 0 0 0 1.5-1.5V8a6 6 0 0 0-6-6z') };
// The sidebar is a drawer; whether it is open is remembered per browser.
function syncNav() {
  const open = !document.body.classList.contains('nav-closed'), t = open ? 'Hide the sidebar' : 'Show the sidebar', b = $('navToggle');
  b.innerHTML = svgIcon(open ? 'M10.5 3.5 6 8l4.5 4.5-1 1L4 8l5.5-5.5z' : 'M5.5 3.5 10 8l-4.5 4.5 1 1L12 8 6.5 2.5z');
  b.title = t;
  b.setAttribute('aria-label', t);
  b.setAttribute('aria-expanded', open);
}
$('navToggle').onclick = () => {
  document.body.classList.toggle('nav-closed');
  store('rs64.navClosed', document.body.classList.contains('nav-closed') ? 1 : 0);
  syncNav();
};
document.body.classList.toggle('nav-closed', stored('rs64.navClosed', 0) === 1);
syncNav();
const pcmCache = new Map();

async function loadAudioBank() {
  const speechDv = await rom.read('data/speech');
  const vt = parseVoiceTable(romBytes, await rom.read('data/voice_TXT'));
  const vids = new Map();
  vt.voiceMap.forEach((s, v) => vids.set(s, [...(vids.get(s) ?? []), v]));
  const cats = globalThis.RS64_VOICE_CATS ?? {};
  const voice = parseSpeech(speechDv).filter((e) => e.index).map((e) => {
    const [cat, levels, src] = cats[e.index] ?? ['Uncategorised', [], 'text'];
    const text = vt.strings[e.index] ?? '';
    return { key: 'v' + e.index, kind: 'voice', index: e.index, label: `${e.index} ${text.trim() || (levels.length ? `(${cat.split(': ').pop()}, ${levels.map(lvName).join(', ')})` : '(no subtitle)')}`, text, rate: e.freq, voiceIds: vids.get(e.index) ?? [], levels, cat, src, entry: e };
  });
  // Multi-part craft descriptions and pilot bios play as one row, in the order the menus play them.
  const mv = parseMenuVoice(romBytes), byIndex = new Map(voice.map((r) => [r.index, r])), grouped = new Set();
  const groups = [[mv.crafts, CRAFT_NAMES, 'Hangar: craft descriptions'], [mv.roster, PILOT_NAMES, 'Pilot roster bios']].flatMap(([lists, names, cat]) => lists.map((ids, i) => {
    const parts = ids.map((v) => byIndex.get(vt.voiceMap[v])).filter(Boolean);
    parts.forEach((p) => grouped.add(p));
    return { key: `g${cat[0]}${i}`, kind: 'group', index: parts[0].index, label: `${names[i]} (${parts.length} parts)`, text: parts.map((p) => p.text.trim()).filter(Boolean).join('\n'),
      rate: parts[0].rate, voiceIds: ids, levels: [], cat, src: 'code', parts };
  }));
  const samples = parseSdir(await rom.read('data/sound/sdir_SND')).filter((e) => e.num).map((e) => ({
    key: 's' + e.index, kind: 'sample', index: e.index, label: `sample ${e.index}${e.loopLen ? ' (loop)' : ''}`, text: '', rate: e.rate, levels: [], cat: 'Samples', src: 'code', entry: e }));
  const fill = (id, vals, text = (v) => v) => {
    const sel = $(id);
    sel.length = 1;
    for (const v of vals) sel.append(new Option(text(v), v));
  };
  // One row per cutscene that plays its dialogue at the event times.
  const cuts = [];
  for (const a of rom.assets.values()) {
    const m = a.dir === 'data/cuts' && /^id(\d+)_(intro|extro|special)$/.exec(a.name);
    if (!m) continue;
    const c = parseCutsceneVoice(await rom.read(a.path));
    const parts = c.events.map((ev) => ({ row: byIndex.get(vt.voiceMap[ev.voiceId]), at: ev.at, voiceId: ev.voiceId })).filter((p) => p.row);
    if (!parts.length) continue;
    const kind = { intro: 'intro', extro: 'outro' }[m[2]] ?? 'medal ceremony', lv = 'lv_' + (+m[1]).toString(36);
    cuts.push({ key: 'c' + a.name, kind: 'cut', index: +m[1], label: m[2] === 'special' ? 'Medal ceremony' : lvName(lv),
      title: m[2] === 'special' ? 'Medal ceremony' : `${kind[0].toUpperCase() + kind.slice(1)}: ${lvName(lv)}`,
      text: parts.map((p) => p.row.text.trim()).filter(Boolean).join('\n'), rate: 16000, voiceIds: c.events.map((e) => e.voiceId),
      levels: m[2] === 'special' ? [] : [lv], cat: m[2] === 'special' ? 'Full cutscene: medal ceremony' : `Full cutscene: ${kind}s`, src: 'code', parts, length: c.length });
  }
  cuts.sort((a, b) => a.index - b.index);
  const sdirAll = parseSdir(await rom.read('data/sound/sdir_SND')), samp = u8(await rom.read('data/sound/samp_SND'));
  const sfxBank = { pool: parsePool(await rom.read('data/sound/pool_SND')), sdir: new Map(sdirAll.map((e) => [e.id, e])), samp, tables: parseMixTables(romBytes) };
  // Callers come from tools/web/rs64-explorer-data/gen_voice_cats.mjs (constant ids passed to the game's SFX start); the rest are picked at runtime.
  // Cutscenes play sound effects through their own table (event type 3 -> u16 at 0x800AF6E8, weapon fire's p3), which the caller scan
  // does not see, so each cutscene that plays an effect is listed as one of its users.
  const cutUse = new Map();
  for (const a of rom.assets.values()) {
    if (a.dir !== 'data/cuts' || !/^id\d+_/.test(a.name)) continue;
    for (const e of parseCutscene(await rom.read(a.path)).events) {
      const id = e.type === 3 ? (romBytes[CUT_SFX + e.p[0] * 2] << 8) | romBytes[CUT_SFX + e.p[0] * 2 + 1] : e.type === 8 && e.p[3] & 0xffff ? e.p[3] & 0xffff : -1;
      if (id >= 0) (cutUse.get(id) ?? cutUse.set(id, new Set()).get(id)).add(a.name);
    }
  }
  const sfx = parseSfxTable(await rom.read('data/sound/proj_SND')).sort((a, b) => a.id - b.id).map((s) => {
    const callers = [...(globalThis.RS64_SFX_CALLERS?.[s.id] ?? []), ...(cutUse.get(s.id) ?? [])];
    return { key: 'x' + s.id, kind: 'sfx', index: s.id, callers, text: '', rate: 22050, levels: [], cat: 'SFX', src: 'code', entry: s,
      label: `sfx ${hex(s.id)}${s.macro & 0x8000 ? ' (layer)' : ''}${callers.length ? ` · ${callers[0]}${callers.length > 1 ? ` +${callers.length - 1}` : ''}` : ''}` };
  });
  // Songs: the ROM song table names sound/<name>_SNG; a _SNG missing from it has no instrument group.
  const songIds = new Map(parseSongTable(romBytes).map((s) => [s.name, s.id]));
  const musicCtx = { ...sfxBank, groups: parseSongGroups(await rom.read('data/sound/proj_SND')) };
  const music = [...rom.assets.values()].filter((a) => /_SNG$/.test(a.name)).map((a) => {
    const name = a.name.slice(0, -4), id = songIds.get(name) ?? null;
    return { key: 'm' + name, kind: 'music', index: id, name, path: a.path, label: id == null ? `${name} (not referenced)` : name, text: '', rate: 22050, levels: [], cat: 'Music', src: 'code' };
  }).sort((a, b) => (a.index ?? 999) - (b.index ?? 999));
  fill('audCat', [...new Set(voice.map((r) => r.cat))].sort().concat('Music', 'SFX', 'Samples'));
  fill('audLevel', [...new Set(voice.flatMap((r) => r.levels))].sort((a, b) => parseInt(a.slice(3), 36) - parseInt(b.slice(3), 36)), (lv) => `${parseInt(lv.slice(3), 36)} ${lvName(lv)}`);
  return { speech: u8(speechDv), samp, sfxBank, sfx, music, musicCtx, voice: [...groups, ...cuts, ...voice.filter((r) => !grouped.has(r))], samples, voiceMap: vt.voiceMap, byIndex };
}
// When each cutscene line actually plays (speech queue, playSpeechClip / tickSpeechSubsystem): a line queued while nothing has
// played for 0.25 s waits the forced 0.5 s lead-in (music duck); a line queued during another waits for it to end. Every clip is
// then heard 0.29 s after it starts (hand-off measured in recomp captures of all 33 cutscenes).
function cutSchedule(r) {
  let end = -1e9;
  return r.parts.map((p) => {
    const pcm = rowPcm(p.row), at = (p.at > end + 0.25 ? p.at + 0.5 : Math.max(p.at, end)) + 0.29;
    end = at + pcm.length / p.row.rate;
    return { ...p, pcm, at, end };
  });
}
function rowPcm(r) {
  if (!pcmCache.has(r.key) && r.kind === 'cut') {
    const parts = cutSchedule(r);
    const len = Math.max(r.length, ...parts.map((p) => p.end));
    const mix = new Float32Array(Math.ceil(len * r.rate));
    for (const p of parts) {
      const step = p.row.rate / r.rate, o = Math.round(p.at * r.rate);
      for (let i = 0; o + i < mix.length && i * step < p.pcm.length - 1; i++) {
        const x = i * step, k = Math.floor(x);
        mix[o + i] += p.pcm[k] + (p.pcm[k + 1] - p.pcm[k]) * (x - k);
      }
    }
    pcmCache.set(r.key, Int16Array.from(mix, (x) => Math.max(-32768, Math.min(32767, Math.round(x)))));
  }
  if (!pcmCache.has(r.key) && r.kind === 'group') {
    // tickMultiPartVoiceLine waits 0.7 s after each part.
    const gap = Math.round(0.7 * r.rate), pcms = r.parts.map(rowPcm);
    const out = new Int16Array(pcms.reduce((n, p) => n + p.length, 0) + gap * (pcms.length - 1));
    let o = 0;
    for (const p of pcms) {
      out.set(p, o);
      o += p.length + gap;
    }
    pcmCache.set(r.key, out);
  }
  if (!pcmCache.has(r.key)) {
    const e = r.entry;
    pcmCache.set(r.key, r.kind === 'voice' ? decodeMort(audioBank.speech, e.offset, e.size)
      : r.kind === 'sfx' ? renderSfx(audioBank.sfxBank, e).pcm : decodeMusyxAdpcm(audioBank.samp, e.off, e.num));
  }
  return pcmCache.get(r.key);
}
// player = { key, buf, loop: [start, end] | null, offset (s), src (null while paused), startedAt, stems? }. Buffer sources cannot pause,
// so pause remembers the offset and resume starts new sources from it. A song plays one source per track (stems: [{ track, buf, start }])
// through a gain per track (gains), so mute / solo only change gains.
let player = null;
const syncPlayButton = () => {
  const b = $('audPlay');
  if (!b) return;
  const playing = !!(player?.src && player.key === b.dataset.key);
  b.innerHTML = playing ? ICON.pause : ICON.play;
  b.title = playing ? 'Pause' : 'Play';
  b.setAttribute('aria-label', b.title);
};
function startPlayer() {
  const p = player, src = p.stems ? stemSources(p) : audioCtx.createBufferSource();
  if (!p.stems) {
    src.buffer = p.buf;
    if (p.loop) [src.loop, src.loopStart, src.loopEnd] = [true, ...p.loop];
    src.connect(audioCtx.destination);
  }
  src.onended = () => {
    if (player === p && p.src === src) player = null;
    syncPlayButton();
  };
  if (!p.stems) src.start(0, p.offset);
  p.startedAt = audioCtx.currentTime - p.offset;
  p.src = src;
  syncPlayButton();
}
// Starts every track that still sounds after the offset, aligned on the song clock; the returned handle stops them all and ends with the last.
function stemSources(p) {
  const now = audioCtx.currentTime, srcs = [];
  for (const st of p.stems) {
    if (st.start + st.buf.duration <= p.offset) continue;
    const s = audioCtx.createBufferSource();
    s.buffer = st.buf;
    s.connect(p.gains.get(st.track));
    s.start(now + Math.max(0, st.start - p.offset), Math.max(0, p.offset - st.start));
    srcs.push(Object.assign(s, { end: st.start + st.buf.duration }));
  }
  const last = srcs.reduce((a, s) => (!a || s.end > a.end ? s : a), null);
  // A song with no sounding track left still needs an end event: a silent one-frame source.
  const tail = last ?? Object.assign(audioCtx.createBufferSource(), { buffer: audioCtx.createBuffer(1, 1, audioCtx.sampleRate) });
  if (!last) { tail.connect(audioCtx.destination); tail.start(); }
  return { stop: () => (last ? srcs : [tail]).forEach((s) => s.stop()), set onended(fn) { tail.onended = fn; } };
}
const trackGain = (r, t) => (t === 'reverb' ? 1 : r.solo != null ? +(r.solo === t) : +!r.skip.has(t));
// Position being heard now: the context clock runs ahead of the speakers by the output latency.
const playerTime = () => (player.src ? Math.max(player.offset, audioCtx.currentTime - player.startedAt - (audioCtx.outputLatency || audioCtx.baseLatency || 0)) : player.offset);
function pauseAudio() {
  const p = player;
  if (!p?.src) return;
  let t = playerTime();
  if (p.loop && t > p.loop[1]) t = p.loop[0] + ((t - p.loop[0]) % (p.loop[1] - p.loop[0]));
  p.offset = Math.min(t, p.buf.duration);
  p.src.onended = null;
  p.src.stop();
  p.src = null;
  syncPlayButton();
}
function stopAudio() {
  playToken++;
  if (player?.src) {
    player.src.onended = null;
    player.src.stop();
  }
  player = null;
  syncPlayButton();
}
// Called from a click so the context may start (autoplay policy).
async function playRow(r, loop = false, offset = 0) {
  stopAudio();
  const token = playToken;
  audioCtx ??= new AudioContext();
  await audioCtx.resume();
  if (token !== playToken) return;
  let buf;
  if (r.kind === 'music') {
    // Songs play each track's stem through its own gain; the output clips at full scale like the game's 16-bit mixer.
    const gains = new Map(r.stems.map((st) => [st.track, audioCtx.createGain()]));
    for (const [t, g] of gains) { g.gain.value = trackGain(r, t); g.connect(audioCtx.destination); }
    player = { key: r.key, buf: { duration: r.render.length }, loop: null, offset: Math.min(offset, r.render.length), src: null, stems: r.stems, gains };
    return startPlayer();
  } else {
    const pcm = rowPcm(r);
    if (!pcm.length) return;
    buf = audioCtx.createBuffer(1, pcm.length, r.rate);
    const ch = buf.getChannelData(0);
    for (let i = 0; i < pcm.length; i++) ch[i] = pcm[i] / 32768;
  }
  const e = r.entry;
  const looped = loop && r.kind === 'sample' && e.loopLen && e.loopStart + e.loopLen <= buf.length;
  player = { key: r.key, buf, loop: looped ? [e.loopStart / r.rate, (e.loopStart + e.loopLen) / r.rate] : null, offset: Math.min(offset, buf.duration), src: null };
  startPlayer();
}
// Play / pause toggle for the panel: resumes where it paused, or starts the row fresh.
async function togglePlay(r, loop) {
  if (player?.key !== r.key) return playRow(r, loop);
  if (player.src) return pauseAudio();
  await audioCtx.resume();
  startPlayer();
}
function saveBlob(blob, name) {
  // Chrome saves a detached link or an already-revoked blob under the blob's UUID with no extension, so attach it and revoke late.
  const a = elem('a');
  a.href = URL.createObjectURL(blob);
  a.download = name;
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 60000);
}
// Plays voice ids one after another with the menus' 0.7 s gap.
function playVoiceIds(ids) {
  const rows = [...new Set(ids.map((v) => audioBank?.byIndex.get(audioBank.voiceMap[v])).filter(Boolean))];
  let at = 0;
  const parts = rows.map((row) => {
    const p = { row, at };
    at += rowPcm(row).length / row.rate + 0.7;
    return p;
  });
  if (parts.length) playRow({ key: 'tv' + ids.join(','), kind: 'cut', rate: 16000, length: 0, parts });
}
const triggerVoiceIds = (it, lv) => window.RS64.triggerVoiceIds(it, globalThis.RS64_TRIGGER_VOICES?.[lv]);
// ---------- music dashboard ----------
let musicSel = null, musicRaf = 0;
const LANE_H = 26;
// Song renders run on Web Workers built from the library source (works from file://); each renders a share of the tracks and the dry
// partial mixes are summed (voices only add, so the sum equals a one-thread render).
// One track alone, dry, trimmed to where it sounds, L/R scaled to -1..1 for an AudioBuffer. A reverb song keeps the track's reverb send
// (aux, 16-bit units): the reverb runs once on the summed send (reverbStem), since it rounds to 16 bits inside and is not exact per track.
function renderStem(ctx, song, songId, track, all) {
  const v = RS64.renderSong(ctx, song, songId, { dry: true, skipTracks: new Set(all.filter((t) => t !== track)) });
  const aux = v.reverb ? v.aux : null, on = (i) => v.L[i] || v.R[i] || (aux && aux[i]);
  let a = 0, b = v.L.length;
  while (a < b && !on(a)) a++;
  while (b > a && !on(b - 1)) b--;
  const L = v.L.slice(a, b), R = v.R.slice(a, b);
  for (let i = 0; i < L.length; i++) { L[i] /= 32768; R[i] /= 32768; }
  return { ...v, L, R, aux: aux && aux.slice(a, b), start: a / v.rate, track };
}
// The song reverb on the summed send of the tracks that are on, as its own stem (always on).
function reverbStem(r) {
  const n = Math.ceil(r.render.length * r.render.rate), aux = new Float32Array(n), L = new Float32Array(n), R = new Float32Array(n);
  for (const st of r.stems) {
    if (!st.aux || !trackGain(r, st.track)) continue;
    const o = Math.round(st.start * r.render.rate);
    for (let i = 0; i < st.aux.length && o + i < n; i++) aux[o + i] += st.aux[i];
  }
  RS64.applyMusyxReverb(L, R, aux, r.render.reverb);
  const buf = new AudioBuffer({ numberOfChannels: 2, length: n, sampleRate: r.render.rate });
  for (let i = 0; i < n; i++) { L[i] /= 32768; R[i] /= 32768; }
  buf.copyToChannel(L, 0);
  buf.copyToChannel(R, 1);
  return { track: 'reverb', buf, start: 0 };
}
// Rebuilds the reverb stem after a mute / solo change and swaps it into a running player.
function refreshReverb(r) {
  if (!r.render.reverb) return;
  r.stems = [...r.stems.filter((st) => st.track !== 'reverb'), reverbStem(r)];
  if (player?.key !== r.key) return;
  const playing = !!player.src;
  if (playing) pauseAudio();
  player.stems = r.stems;
  if (playing) startPlayer();
}
function musicWorkerMain() {
  let ctx = null;
  onmessage = (e) => {
    const m = e.data;
    if (m.ctx) { ctx = m.ctx; return; }
    postMessage({ id: m.id, render: renderStem(ctx, m.song, m.songId, m.track, m.all) });
  };
}
let musicPool = null, musicJob = 0;
function musicWorkers() {
  if (musicPool) return musicPool;
  musicPool = [];
  try {
    const url = URL.createObjectURL(new Blob([`${rs64Lib}\nrs64Lib();\n${renderStem}\n(${musicWorkerMain})();`], { type: 'text/javascript' }));
    const n = Math.max(2, Math.min(8, (navigator.hardwareConcurrency || 4) - 1));
    for (let i = 0; i < n; i++) musicPool.push(Object.assign(new Worker(url), { ctxFor: null }));
  } catch (e) {
    console.warn('music workers unavailable, rendering on the page', e);
  }
  return musicPool;
}
// Every track rendered alone, spread over the workers (a queue: each worker takes the next track when done), or on the page without workers.
async function renderSongStems(ctx, song, songId, onProgress) {
  const ws = musicWorkers(), all = [...new Set(song.tracks.map((t) => t.index))], out = [];
  if (!ws.length) {
    for (const t of all) {
      out.push(renderStem(ctx, song, songId, t, all));
      onProgress?.(out.length / all.length);
      await new Promise((res) => setTimeout(res, 0));
    }
    return out;
  }
  const queue = [...all];
  await Promise.all(ws.map((w) => (async () => {
    if (w.ctxFor !== ctx) {
      w.postMessage({ ctx: { pool: ctx.pool, sdir: ctx.sdir, samp: ctx.samp, tables: ctx.tables, groups: ctx.groups } });
      w.ctxFor = ctx;
    }
    while (queue.length) {
      const track = queue.shift(), id = ++musicJob;
      out.push(await new Promise((resolve, reject) => {
        const onMsg = (e) => {
          if (e.data.id !== id) return;
          w.removeEventListener('message', onMsg);
          resolve(e.data.render);
        };
        w.addEventListener('message', onMsg);
        w.onerror = (e) => reject(e);
        w.postMessage({ id, song, songId, track, all });
      }));
      onProgress?.(out.length / all.length);
    }
  })()));
  return out;
}
// Renders a music row once into per-track stems (r.render, r.stems), shared by the music dashboard and the cutscene player.
async function songStems(r, onProgress) {
  if (r.stems) return r;
  r.pending ??= (async () => {
    r.song = parseSong(await rom.read(r.path));
    r.skip = new Set();
    const parts = await renderSongStems(audioBank.musicCtx, r.song, r.index ?? -1, onProgress).catch((e) => {
      console.warn('worker render failed, rendering on the page', e);
      musicPool = [];
      return renderSongStems(audioBank.musicCtx, r.song, r.index ?? -1, onProgress);
    });
    const { L, R, aux, ...meta } = parts[0];
    r.render = { ...meta, unresolved: new Set(parts.flatMap((p) => [...p.unresolved])) };
    r.stems = parts.filter((p) => p.L.length).map((p) => {
      const buf = new AudioBuffer({ numberOfChannels: 2, length: p.L.length, sampleRate: p.rate });
      buf.copyToChannel(p.L, 0);
      buf.copyToChannel(p.R, 1);
      return { track: p.track, buf, start: p.start, aux: p.aux };
    });
    if (r.render.reverb) r.stems.push(reverbStem(r));
    return r;
  })();
  return r.pending;
}
async function showMusic(r) {
  audioSel = r.key;
  if (musicSel !== r) $('musicNote').hidden = true;
  musicSel = r;
  $('audioPanel').hidden = true;
  const P = $('musicPanel');
  P.hidden = false;
  renderList();
  if (!r.stems) {
    stopAudio();
    $('musicHead').innerHTML = `<b>${esc(r.label)}</b> <span class="meta" id="musicProg">rendering…</span>`;
    await songStems(r, (f) => { const el = $('musicProg'); if (el) el.textContent = `rendering tracks… ${Math.round(f * 100)}%`; });
    if (musicSel !== r) return;
  }
  const s = r.render, clock = (x) => `${Math.floor(x / 60)}:${(x % 60).toFixed(1).padStart(4, '0')}`;
  const unres = [...s.unresolved], silent = s.notes.filter((n) => n.silent).length;
  $('musicHead').innerHTML = `<b>${esc(r.label)}</b> <span class="meta">song ${r.index ?? '-'} · group ${s.group ?? '-'} · ${[...new Set(s.bpm)].join(' / ')} BPM · ${clock(s.length)}`
    + `${s.loopAt != null ? ` · loops at ${clock(s.loopAt)}` : ''} · reverb: ${s.reverb ? esc(s.reverb.name) : 'none (in-mission)'}`
    + `${silent ? ` · ${silent} note${silent === 1 ? '' : 's'} outside the instrument's key range` : ''}</span>`
    + (unres.length ? `<div class="warn">Not yet confirmed: ${unres.map(esc).join('; ')}</div>` : '');
  // Buttons: the shared player plus WAV.
  const B = $('musicBtns');
  B.innerHTML = '';
  const mk = (title, html, fn) => B.appendChild(elem('button', { title, innerHTML: html, onclick: fn }));
  const play = mk('Play', ICON.play, () => togglePlay(r));
  play.id = 'audPlay';
  play.dataset.key = r.key;
  mk('Stop', ICON.stop, stopAudio);
  const loop = mk(r.loop ? 'Loop on' : 'Loop off', ICON.loop, () => {
    r.loop = !r.loop;
    loop.classList.toggle('on', r.loop);
    loop.title = r.loop ? 'Loop on' : 'Loop off';
    loop.setAttribute('aria-pressed', r.loop);
  });
  loop.classList.toggle('on', !!r.loop);
  loop.setAttribute('aria-pressed', !!r.loop);
  mk('Save a WAV of the tracks that are on', 'Save WAV', () => {
    const n = Math.ceil(s.length * s.rate), L = new Float32Array(n), R = new Float32Array(n), c = (v) => Math.max(-32768, Math.min(32767, Math.round(v * 32768)));
    for (const st of r.stems) {
      if (!trackGain(r, st.track)) continue;
      const a = st.buf.getChannelData(0), b = st.buf.getChannelData(1), o = Math.round(st.start * s.rate);
      for (let i = 0; i < a.length && o + i < n; i++) { L[o + i] += a[i]; R[o + i] += b[i]; }
    }
    const w = new Int16Array(n * 2);
    for (let i = 0; i < n; i++) [w[2 * i], w[2 * i + 1]] = [c(L[i]), c(R[i])];
    saveBlob(new Blob([wavBytes(w, s.rate, 2)], { type: 'audio/wav' }), r.name + '.wav');
  });
  B.append(elem('span', { style: 'flex:1' }));
  mk('Fit the whole song (mouse wheel over the timeline zooms)', 'Fit', () => zoomMusic(r, 0)).id = 'musicFit';
  syncPlayButton();
  drawMusic(r);
}
const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
// MusyX key 60 is middle C (C4).
const noteName = (k) => `${NOTE_NAMES[k % 12]}${Math.floor(k / 12) - 1}`;
const sampleHue = (id) => `hsl(${((id ?? 0) * 47) % 360} 65% 60%)`;
// A song's instrument number (MIDI program) maps, per sound group, to one of three MusyX kinds.
const targetText = (t) => (t == null ? 'none'
  : t & 0x8000 ? `layer ${hex(t)}`
  : t & 0x4000 ? `keymap ${hex(t)}` : `sound/macro ${hex(t)}`);
// Samples-list row for a sample id (sdir id -> the row keyed by its sdir index).
const sampleRowOf = (id) => audioBank?.samples.find((x) => x.entry.id === id);
function noteCard(n) {
  const voices = n.silent ? '<div class="voiceRow" style="color:var(--dim)">silent: the instrument has no sound for this key</div>'
    : n.voices.map((v) => {
      const row = sampleRowOf(v.sample);
      return `<div class="voiceRow"><span class="swatch" style="background:${sampleHue(v.sample)}"></span>sound/macro ${hex(v.macro)} · plays ${noteName(v.note)}`
        + ` · sample ${row ? row.index : '-'}${row ? ` (${(row.entry.num / row.entry.rate).toFixed(2)} s${row.entry.loopLen ? ', loops' : ''})` : ''}</div>`;
    }).join('');
  return `<div class="hd">${noteName(n.note)} <span style="color:var(--dim);font-weight:400">· key ${n.note}</span></div>`
    + `<dl><dt>Time</dt><dd>${n.t.toFixed(2)} s, ${n.dur.toFixed(2)} s long</dd><dt>Velocity</dt><dd>${n.vel}</dd>`
    + `<dt>Instrument</dt><dd>#${n.program} → ${targetText(n.target)}</dd></dl>${voices}`;
}
// Inspector for a clicked note: its card plus, per voice, play the raw sample or open it in the Samples list.
function showNoteInspector(n) {
  const el = $('musicNote');
  el.innerHTML = `<div>${noteCard(n)}</div>`;
  const acts = elem('div');
  for (const v of n.voices) {
    const row = sampleRowOf(v.sample);
    if (!row) continue;
    const line = elem('div', { className: 'voiceRow' });
    line.innerHTML = `<span class="swatch" style="background:${sampleHue(v.sample)}"></span>sample ${row.index}`;
    const play = elem('button', { innerHTML: ICON.play, title: 'Play this sample (raw, native pitch)' });
    play.setAttribute('aria-label', play.title);
    play.onclick = () => playRow(row);
    const open = elem('button', { textContent: 'Show in Samples', title: 'Open this sample in the Samples list' });
    open.onclick = () => {
      $('audCat').value = 'Samples';
      $('audLevel').value = '';
      showAudio(row);
      $('list').querySelector('.row.sel')?.scrollIntoView({ block: 'center' });
    };
    line.append(play, open);
    acts.append(line);
  }
  el.append(acts);
  el.hidden = false;
}
// Timeline zoom (1 = whole song fits): keeps the time under anchorPx (px from the timeline's left edge) in place.
function zoomMusic(r, factor, anchorPx) {
  const view = $('musicLanes'), vw = view.clientWidth - CTL_W;
  anchorPx ??= vw / 2;
  const t = (view.scrollLeft + anchorPx) / r.pxs;
  const z = factor ? (r.zoom ?? 1) * factor : 1;
  r.zoom = z < 1.01 ? 1 : Math.min(400, z);
  drawMusic(r);
  view.scrollLeft = Math.max(0, t * r.pxs - anchorPx);
  r.redraw();
}
const CTL_W = 190;
function drawMusic(r) {
  const s = r.render, tracks = [...new Set(r.song.tracks.map((t) => t.index))];
  const lane = new Map(tracks.map((t, i) => [t, i])), scroll = $('musicScroll'), view = $('musicLanes');
  const VW = Math.max(100, view.clientWidth - CTL_W), pxs = (r.pxs = (VW - 4) / Math.max(1, s.length) * (r.zoom ?? 1));
  const W = Math.ceil(s.length * pxs), H = tracks.length * LANE_H;
  $('musicFit').hidden = (r.zoom ?? 1) === 1;
  // Lane controls: track, channel, program, mute / solo (re-render without the muted tracks).
  const C = $('musicCtl');
  C.innerHTML = '';
  for (const t of tracks) {
    const tr = r.song.tracks.find((x) => x.index === t), first = s.notes.find((n) => n.track === t), prog = first?.program;
    const kind = first?.target == null ? '' : first.target & 0x8000 ? 'layer' : first.target & 0x4000 ? 'keymap' : 'sound';
    const d = elem('div', { className: 'lane' });
    d.innerHTML = `<span title="Track ${t} of the song file · MIDI channel ${tr.channel + 1} · instrument #${prog ?? '-'}">`
      + `T${t} · ch${tr.channel + 1}${prog != null ? ` · #${prog} ${kind}` : ''}</span>`;
    for (const k of ['M', 'S']) {
      const on = k === 'M' ? r.skip.has(t) && r.solo == null : r.solo === t;
      const title = k === 'M' ? (on ? 'Unmute' : 'Mute') : on ? 'Unsolo' : 'Solo';
      const b = elem('button', { className: 'laneBtn' + (on ? ' on' : ''), title,
        innerHTML: k === 'M' ? (on ? ICON.muted : ICON.speaker) : ICON.solo });
      b.setAttribute('aria-label', title);
      b.setAttribute('aria-pressed', on);
      b.onclick = () => {
        if (k === 'M') { if (on) r.skip.delete(t); else r.skip.add(t); r.solo = null; }
        else { r.solo = on ? null : t; r.skip = new Set(on ? [] : tracks.filter((x) => x !== t)); }
        if (player?.key === r.key) for (const [x, g] of player.gains) g.gain.value = trackGain(r, x);
        refreshReverb(r);
        drawMusic(r);
      };
      d.append(b);
    }
    C.append(d);
  }
  // Canvases cover only the visible window (a long song zoomed in would exceed the browser's canvas size); the scroll div is a spacer.
  const cv = $('musicCanvas'), hc = $('musicHeadCanvas');
  for (const c of [cv, hc]) [c.width, c.height] = [VW, H];
  scroll.style.width = W + 'px';
  scroll.style.height = H + 'px';
  const g = cv.getContext('2d');
  // Pitch range per lane, notes coloured by their first sample, silent notes hollow, CC7 as a thin line at the lane bottom.
  const range = new Map();
  for (const n of s.notes) { const x = range.get(n.track) ?? [127, 0]; range.set(n.track, [Math.min(x[0], n.note), Math.max(x[1], n.note)]); }
  const cc7 = tracks.map((t) => s.ctl.filter((c) => c.channel === r.song.tracks.find((x) => x.index === t).channel && c.cc === 7));
  const barH = Math.max(3, Math.min(6, Math.round(pxs / 40)));
  r.redraw = () => {
    const x0 = view.scrollLeft;
    cv.style.left = hc.style.left = x0 + 'px';
    g.clearRect(0, 0, VW, H);
    tracks.forEach((t, i) => { g.fillStyle = ink(i % 2 ? 0.03 : 0.06); g.fillRect(0, i * LANE_H, VW, LANE_H); });
    // Second ticks when zoomed in far enough to read them.
    if (pxs >= 30) {
      g.fillStyle = ink(0.08);
      for (let sec = Math.ceil(x0 / pxs); sec * pxs < x0 + VW; sec++) g.fillRect(sec * pxs - x0, 0, 1, H);
    }
    for (const n of s.notes) {
      const x = n.t * pxs - x0, w = Math.max(2, n.dur * pxs);
      if (x > VW || x + w < 0) continue;
      const i = lane.get(n.track), [lo, hi] = range.get(n.track), y = i * LANE_H + 3 + (hi > lo ? ((hi - n.note) / (hi - lo)) * (LANE_H - 6 - barH) : (LANE_H - 6 - barH) / 2);
      if (n.silent) { g.strokeStyle = '#888'; g.strokeRect(x, y, w, barH); continue; }
      g.fillStyle = sampleHue(n.voices[0]?.sample);
      g.fillRect(x, y, w, barH);
    }
    g.strokeStyle = 'rgba(240,162,74,.6)';
    cc7.forEach((pts, i) => {
      if (!pts.length) return;
      const y0 = (i + 1) * LANE_H - 1;
      g.beginPath();
      g.moveTo(0, y0 - (pts[0].value / 127) * 4);
      for (const p of pts) g.lineTo(p.t * pxs - x0, y0 - (p.value / 127) * 4);
      g.lineTo(VW, y0 - (pts[pts.length - 1].value / 127) * 4);
      g.stroke();
    });
    if (s.loopAt != null) { g.fillStyle = ink(0.4); g.fillRect(s.loopAt * pxs - x0, 0, 1, H); }
  };
  view.onscroll = r.redraw;
  r.redraw();
  // Wheel over the timeline zooms around the cursor; Shift + wheel or a horizontal swipe still scrolls.
  hc.onwheel = (ev) => {
    if (ev.shiftKey || Math.abs(ev.deltaX) > Math.abs(ev.deltaY)) return;
    ev.preventDefault();
    zoomMusic(r, ev.deltaY < 0 ? 1.25 : 0.8, ev.clientX - hc.getBoundingClientRect().left);
  };
  // Hover: the note under the cursor. Playhead: follows the player.
  const tip = $('musicTip');
  const noteAt = (ev) => {
    const b = hc.getBoundingClientRect(), t = (ev.clientX - b.left + view.scrollLeft) / pxs, track = tracks[Math.floor((ev.clientY - b.top) / LANE_H)];
    return { t, n: s.notes.find((x) => x.track === track && t >= x.t && t <= x.t + Math.max(x.dur, 3 / pxs)) };
  };
  hc.onmousemove = (ev) => {
    const { n } = noteAt(ev);
    if (!n) { tip.hidden = true; return; }
    tip.innerHTML = noteCard(n);
    const x = Math.min(ev.clientX + 14, innerWidth - tip.offsetWidth - 8), y = Math.min(ev.clientY + 14, innerHeight - tip.offsetHeight - 8);
    Object.assign(tip.style, { left: x + 'px', top: y + 'px' });
    tip.hidden = false;
  };
  hc.onmouseleave = () => (tip.hidden = true);
  // Click: seek to the clicked time; on a note, also open the inspector with its samples.
  hc.style.cursor = 'pointer';
  hc.onclick = (ev) => {
    const { t: at, n } = noteAt(ev);
    if (n) showNoteInspector(n);
    const t = Math.max(0, Math.min(s.length, at));
    if (player?.key === r.key && !player.src) player.offset = t;
    else playRow(r, false, t);
  };
  cancelAnimationFrame(musicRaf);
  const hg = hc.getContext('2d');
  const tick = () => {
    hg.clearRect(0, 0, VW, H);
    if (player?.key === r.key) {
      hg.fillStyle = ink(1);
      hg.fillRect(playerTime() * pxs - view.scrollLeft, 0, 1, H);
    }
    // Loop: at the song's loop point (or its end) jump back to where the game restarts it (header +0x14).
    // shortcut: notes still ringing at the loop point are cut, not carried over; checked once per frame.
    if (r.loop && player?.key === r.key && player.src && audioCtx.currentTime - player.startedAt >= (s.loopAt ?? Math.max(...r.stems.map((st) => st.start + st.buf.duration)) - 0.03)) {
      pauseAudio();
      player.offset = s.loopAt != null ? s.loopFrom : 0;
      startPlayer();
    }
    if (musicSel === r) musicRaf = requestAnimationFrame(tick);
  };
  tick();
}

function showAudio(r) {
  if (r.kind === 'music') return showMusic(r);
  musicSel = null;
  $('musicPanel').hidden = true;
  audioSel = r.key;
  const p = $('audioPanel');
  const e = r.entry, dur = (n) => (n / r.rate).toFixed(2) + ' s';
  // A cutscene lists one line per row: when it plays, its subtitle, then its sample and voice id.
  const clock = (s) => `${Math.floor(s / 60)}:${(s % 60).toFixed(1).padStart(4, '0')}`;
  const rows = r.kind === 'cut'
    ? [...cutSchedule(r).map((q) => [clock(q.at), `${esc(q.row.text.trim() || '(no subtitle)')} <small style="color:var(--dim)">sample ${q.row.index} · voice ${q.voiceId}</small>`]),
      ['Length', clock(r.length)], ['Category', esc(r.cat)]]
    : r.kind === 'sfx'
    ? [['SFX id', hex(e.id)], ['Used by', r.callers.length ? r.callers.join(', ') : 'no constant caller (id chosen at runtime)'],
      [e.macro & 0x8000 ? 'Layer' : 'Macro', hex(e.macro)], ['Key / velocity', `${e.key} / ${e.vel}`],
      ['Render', 'approximate: SoundMacro subset, pan and vibrato skipped']]
    : r.kind === 'group'
    ? [['Samples', r.parts.map((p) => p.index).join(', ')], ['Category', esc(r.cat)], ['Voice ids', r.voiceIds.join(' ')], ['Rate', r.rate + ' Hz']]
    : r.kind === 'voice'
    ? [['Sample', r.index], ['Category', esc(r.cat) + (r.src === 'code' ? '' : ` (by ${r.src})`)], ['Levels', r.levels.join(', ') || '-'], ['Voice ids', r.voiceIds.join(' ') || '-'], ['Rate', r.rate + ' Hz'], ['MORT flag', e.flag]]
    : [['Sample', r.index], ['Id', hex(e.id)], ['Rate', r.rate + ' Hz'], ['Root key', e.root], ['Length', dur(e.num)], ['Loop', e.loopLen ? `${e.loopStart} + ${e.loopLen}` : 'none']];
  p.innerHTML = `<b>${r.kind === 'group' || r.kind === 'sfx' || r.kind === 'cut' ? esc(r.title ?? r.label) : (r.kind === 'voice' ? 'Voice line ' : 'MusyX sample ') + r.index}</b>` + (r.text.trim() && r.kind !== 'cut' ? `<p>${esc(r.text)}</p>` : '') + rows.map(([k, v]) => `<span>${k}</span><span>${v}</span>`).join('');
  const btns = elem('div');
  btns.className = 'btns';
  const btn = (label, fn) => btns.appendChild(elem('button', { textContent: label, onclick: fn }));
  const loop = r.kind === 'sample' && e.loopLen ? elem('input', { type: 'checkbox' }) : null;
  const play = btn('', () => togglePlay(r, loop?.checked));
  play.id = 'audPlay';
  play.dataset.key = r.key;
  const stop = btn('', stopAudio);
  stop.innerHTML = ICON.stop;
  stop.title = 'Stop';
  stop.setAttribute('aria-label', 'Stop');
  btn('Save WAV', () => saveBlob(new Blob([wavBytes(rowPcm(r), r.rate)], { type: 'audio/wav' }), r.kind === 'group' || r.kind === 'cut' ? (r.title ?? r.label).split(' (')[0].replace(/\W+/g, '_') + '.wav' : `${r.kind}_${r.kind === 'sfx' ? hex(r.index) : r.index}.wav`));
  if (loop) {
    const l = elem('label');
    l.append(loop, ' Loop');
    btns.append(l);
  }
  p.append(btns);
  p.hidden = false;
  syncPlayButton();
  renderList();
  playRow(r, false);
}
// Object name -> model id across every HOB, built once per ROM: placed items often name a model that lives outside the level's
// opkg (atst, i_lnd_hi in lplatform, train pillars, other levels' shared buildings).
let objectHome = null;
function buildObjectIndex() {
  objectHome ??= (async () => {
    const map = new Map();
    for (const a of rom.assets.values()) {
      if (!a.name.endsWith('_HOB') || a.size <= 8 || !rom.assets.has(a.path.replace(/_HOB$/, '_HMT'))) continue;
      try {
        for (const o of parseHOB(await rom.read(a.path)).objects) if (!map.has(o.name)) map.set(o.name, a.path.replace(/_HOB$/, ''));
      } catch { /* a malformed or encrypted HOB just isn't indexed */ }
    }
    return map;
  })();
  return objectHome;
}
async function loadModel(path) {
  if (!modelCache.has(path)) {
    const [hobDv, hmtDv] = await Promise.all([rom.read(path + '_HOB'), rom.read(path + '_HMT')]);
    const hob = parseHOB(hobDv), hmt = parseHMT(hmtDv);
    const outside = new Set();
    for (const o of hob.objects) for (const lod of o.lods) for (const parts of lod) for (const p of parts) for (const f of p.faces) {
      if (f.uvs && f.uvs.some(([u, v]) => u < -0.01 || u > 1.01 || v < -0.01 || v > 1.01)) outside.add(f.mat);
    }
    const clamp = new Set(hmt.materials.map((_, i) => i).filter((i) => !outside.has(i)));
    modelCache.set(path, { hob, hmt, clamp });
  }
  return modelCache.get(path);
}

function renderList() {
  const q = $('filter').value.toLowerCase();
  const list = $('list');
  list.innerHTML = '';
  if (!rom) return;
  const groups = new Map();
  if (mode === 'models') {
    for (const a of rom.assets.values()) {
      if (!a.name.endsWith('_HOB') || !rom.assets.has(a.path.replace(/_HOB$/, '_HMT'))) continue;
      // Skip placeholder models with no objects (every level ships an 8-byte opkg2_HOB).
      if (a.size <= 8) continue;
      const id = a.path.replace(/_HOB$/, '');
      const dir = a.dir.replace(/^data\/?/, '') || 'data';
      // A level's opkg_HOB is a package of separate buildings and props (preloaded on ROM open), listed one row per object.
      const pkg = /\/opkg\d*$/.test(id) && modelCache.get(id);
      const rows = pkg ? pkg.hob.objects.map((o, i) => ({ id, obj: i, label: o.name || 'object ' + i })) : [{ id, label: a.name.replace(/_HOB$/, '') }];
      for (const r of rows) {
        if (q && !(id + '/' + r.label).toLowerCase().includes(q)) continue;
        if (!groups.has(dir)) groups.set(dir, []);
        groups.get(dir).push(r);
      }
    }
  } else if (mode === 'effects') {
    for (const [dir, ids] of Object.entries(EFFECTS)) for (const id of ids) {
      const label = SIMS[id]?.label ?? PART_LABELS[id] ?? id.split('/').pop();
      if (!(SIMS[id] || rom.assets.has(id.split('#')[0])) || (q && !(id + ' ' + label).toLowerCase().includes(q))) continue;
      if (!groups.has(dir)) groups.set(dir, []);
      groups.get(dir).push({ id, label, tip: SIMS[id]?.tip });
    }
  } else if (mode === 'fonts') {
    for (const f of gameFonts(romBytes)) {
      if (q && !f.name.includes(q)) continue;
      const dir = f.extended ? 'fonts' : 'fonts (no language glyphs)';
      if (!groups.has(dir)) groups.set(dir, []);
      groups.get(dir).push({ id: f.name, label: `${f.name}  ${f.cellW} x ${f.cellH}`, tip: `${f.count} glyphs, ${f.drawn} of ${f.cellH} rows drawn` });
    }
  } else if (mode === 'audio') {
    if (!audioBank) {
      list.textContent = 'No audio in this ROM';
      return;
    }
    const cat = $('audCat').value, lv = $('audLevel').value;
    for (const r of cat === 'Samples' ? audioBank.samples : cat === 'SFX' ? audioBank.sfx : cat === 'Music' ? audioBank.music : cat || lv ? audioBank.voice
      : [...audioBank.voice, ...audioBank.music, ...audioBank.sfx, ...audioBank.samples]) {
      if (cat && r.cat !== cat) continue;
      if (lv && !r.levels.includes(lv)) continue;
      if (q && !`${r.label} ${(r.voiceIds ?? []).join(' ')} ${(r.callers ?? []).join(' ')}`.toLowerCase().includes(q)) continue;
      if (!groups.has(r.cat)) groups.set(r.cat, []);
      groups.get(r.cat).push({ id: r.key, label: r.label.length > 70 ? r.label.slice(0, 67) + '...' : r.label, tip: r.text.trim() || r.label, row: r });
    }
  } else {
    for (const a of rom.assets.values()) {
      if (a.name !== 'dat') continue;
      const id = a.dir;
      if (q && !id.toLowerCase().includes(q)) continue;
      const ml = modLevelOf(id), dir = ml ? 'mod levels' : id.includes('/level/') ? 'missions' : 'frontend';
      if (!groups.has(dir)) groups.set(dir, []);
      const lv = id.split('/').pop();
      groups.get(dir).push({ id, label: ml ? ml.title : /^lv_/.test(lv) && lvName(lv) !== lv ? `${lv} ${lvName(lv)}` : lv,
        tip: ml ? `Mod level ${ml.folder}, plays as ${lvName('lv_' + ml.template.toString(36))}` : undefined });
    }
    // A level's cutscenes are picked in its Scene panel; only the logo sequence (id19) has no level.
    if (rom.assets.has('data/cuts/id19_special') && (!q || 'factor 5 logo'.includes(q))) groups.set('other', [{ id: 'cut:data/cuts/id19_special', label: 'Factor 5 logo' }]);
  }
  for (const [dir, rows] of groups) {
    const h = elem('div');
    h.className = 'dir';
    h.textContent = dir;
    list.append(h);
    for (const r of rows) {
      const d = elem('div');
      d.className = 'row' + ((mode === 'audio' ? r.id === audioSel : mode === 'fonts' ? r.id === fontSel : r.id === selected && (r.obj == null || r.obj === selectedObj)) ? ' sel' : '');
      d.textContent = r.label;
      d.title = r.tip ?? (r.obj == null ? r.id : `${r.id} / ${r.label}`);
      d.onclick = () => (mode === 'models' ? showModel(r.id, r.obj ?? -1) : mode === 'effects' ? showEffect(r.id) : mode === 'audio' ? showAudio(r.row) : mode === 'fonts' ? showFont(r.id) : r.id.startsWith('cut:') ? showCutscene(r.id.slice(4)) : showLevel(r.id));
      list.append(d);
    }
  }
}

function setContext(level, fx = false) {
  $('opts').classList.toggle('level', level);
  $('opts').classList.toggle('fx', fx);
  $('scene').hidden = !level;
  $('editPanel').hidden = !level;
  if (!level && editMode) setEditMode(null);
  $('fxPanel').hidden = !fx;
}

