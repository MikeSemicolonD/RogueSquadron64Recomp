// node tools/web/rs64-explorer/tests/test_arrows.mjs <rom.z64>   (US v1.0 ROM)
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import assert from 'node:assert/strict';
createRequire(import.meta.url)('../rs64assets.js');
const { RS64 } = globalThis;
const romPath = process.argv[2];
assert.ok(romPath, 'usage: test_arrows.mjs <rom.z64>');
const buf = readFileSync(romPath);
const rom = await RS64.openRom(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.length));
const bytesOf = (dv) => new Uint8Array(dv.buffer.slice(dv.byteOffset, dv.byteOffset + dv.byteLength));
const arrowsOf = (bytes) => RS64.parseDAT(new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)).items.filter((i) => i.waypoint);
const noRules = { rules: [] };

const dv = await rom.read('data/level/lv_1/dat');
const orig = bytesOf(dv);
const before = arrowsOf(orig);
const target = before[0];
assert.equal(target.waypoint.follow, 'Reb_trans1');

// A rewrite changes that arrow alone and round-trips every field, with or without a follow target.
const w = { follow: 'Reb_trans2', flag: 3, counter: 120, atLeast: 4, hideFlag: 7, hideCounter: 121, below: 2 };
const out = RS64.arrowWrite(orig, target, w);
const after = arrowsOf(out);
assert.deepEqual(after[0].waypoint, w);
assert.deepEqual(after.slice(1).map((a) => a.waypoint), before.slice(1).map((a) => a.waypoint));
assert.deepEqual(after[0].pos, target.pos);

const none = { ...w, follow: '' };
assert.equal(arrowsOf(RS64.arrowWrite(orig, target, none))[0].waypoint.follow, '');

// A named arrow keeps its name.
const named = RS64.datNameItem(orig, target, 'Arrow_1');
const withName = arrowsOf(named)[0];
assert.equal(withName.name, 'Arrow_1');
const renamedOut = arrowsOf(RS64.arrowWrite(named, withName, w))[0];
assert.equal(renamedOut.name, 'Arrow_1');
assert.deepEqual(renamedOut.waypoint, w);

// Sources come from the level data and from rules; only rules count as managed.
const mos = await rom.read('data/level/lv_0/dat');
const fromData = RS64.arrowSources(mos, noRules, 0, 1);
assert.equal(fromData.units.length, 21);
assert.equal(fromData.managed, false);

const doc = { rules: [{ when: 'destroyed', object: 'A', actions: [{ do: 'counter', counter: 127, add: 1 }] }, { when: 'enter', object: 'Zone', actions: [{ do: 'flag', flag: 126, value: 1 }] }] };
assert.deepEqual(RS64.arrowSources(mos, doc, 0, 127), { units: ['A'], areas: [], managed: true });
assert.deepEqual(RS64.arrowSources(mos, doc, 126, 0), { units: [], areas: ['Zone'], managed: true });

// Counters an arrow uses are taken, so objectives pick others.
const used = RS64.levelUsedObjectiveSlots(new DataView(out.buffer, out.byteOffset, out.byteLength));
assert.ok(used.counters.has(120) && used.counters.has(121));

// A level script is read from the ROM: Mos Eisley's arrow flag is raised once these units are dead.
const romBytes = new Uint8Array(buf.buffer, buf.byteOffset, buf.length);
const script = RS64.missionScript(romBytes, 0);
const dead = RS64.scriptUnits(script, mos, 51, 0);
for (const name of ['A1_Probe_Droid', 'Fodder_Droid_01', 'Fodder_Droid_02'])
  assert.ok(dead.includes(name), name);

assert.equal(RS64.scriptUnits(script, mos, 0, 1).length, 21);
assert.equal(RS64.scriptUnits(script, mos, 126, 0), null);

// Any other ROM reads as no script rather than as nonsense.
assert.equal(RS64.missionScript(new Uint8Array(romBytes.length), 0).flags.size, 0);

console.log('test_arrows: ok');
