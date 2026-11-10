// node tools/web/rs64-explorer/tests/test_rules_script.mjs   (no ROM needed)
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import assert from 'node:assert/strict';

createRequire(import.meta.url)('../rs64assets.js');

const { RS64 } = globalThis;
const cases = JSON.parse(readFileSync(new URL('../../../../tests/level_rules_script_cases.json', import.meta.url), 'utf8'));
let failed = 0;

const check = (name, fn) => {
  try {
    fn();
    console.log('ok  ', name);
  } catch (e) {
    failed++;
    console.log('FAIL', name, e.message);
  }
};

const lineOf = (text, at) => text.slice(0, at).split('\n').length;
const caseOf = (name) => cases.find((c) => c.name.startsWith(name));
const strip = (doc) => doc.rules.map(({ line, ...r }) => r);

// The shared cases list what the game accepts; unset variables and object names are the runner's checks, not the parser's.
const parserOnly = { objects: null, objectives: true, strictVariables: false };

// Every line the game reports is marked, and the rules with no mark are exactly the ones the game accepts.
check('shared_cases', () => {
  for (const c of cases)
  {
    const doc = RS64.rulesScriptParse(c.text);
    const marked = new Set(RS64.rulesScriptCheck(c.text, parserOnly).map((m) => lineOf(c.text, m.from)));

    for (const l of c.errorLines)
      assert.ok(marked.has(l), `${c.name}: line ${l} not marked`);

    const clean = doc.rules.filter((r, i) => {
      const last = (doc.rules[i + 1]?.line ?? Infinity) - 1;

      return !r.raw && ![...marked].some((l) => l >= r.line && l <= last);
    });

    assert.equal(clean.length, c.rules, c.name);
  }
});

check('round_trip', () => {
  for (const name of ['full_example', 'every_action', 'every_header', 'script_names_with_spaces'])
  {
    const doc = RS64.rulesScriptParse(caseOf(name).text);

    assert.deepEqual(strip(RS64.rulesScriptParse(RS64.rulesScriptWrite(doc))), strip(doc), name);
  }

  const doc = RS64.rulesScriptParse(caseOf('full_example').text);

  assert.equal(doc.rules[0].comment, '# Hold the bridge once the gate is down');
  assert.equal(doc.rules[0].if, 'destroyed(gate) and not objective_done(2)');
  assert.deepEqual(doc.rules[0].actions[0], { do: 'message', seconds: 6, text: 'Hold the bridge' });
  assert.deepEqual(doc.rules[2], { when: 'after', seconds: 120, every: true, actions: [{ do: 'voice', id: 627 }], line: 12 });
  assert.equal(doc.rules[3].when, 'true');
});

// Rules the explorer makes one click at a time print, read back the same, and are marked until finished.
check('lenient_half_made_rules', () => {
  const rules = [
    { when: 'after', actions: [{ do: 'voice' }] },
    { when: 'healthBelow', object: 'u', actions: [] },
    { when: 'true', actions: [{ do: 'set', variable: 'x' }] }
  ];

  const text = RS64.rulesScriptWrite({ version: 1, rules });
  const marks = RS64.rulesScriptCheck(text, parserOnly);

  assert.deepEqual(strip(RS64.rulesScriptParse(text)), rules);
  assert.ok(marks.some((m) => lineOf(text, m.from) === lineOf(text, text.indexOf('voice'))), 'voice line marked');
  assert.ok(marks.some((m) => lineOf(text, m.from) === lineOf(text, text.indexOf('when health'))), 'health line marked');
});

check('raw_kept', () => {
  const bad = 'when explode the base\n    wiat 4\nend';
  const text = `when start\n    win\nend\n\n${bad}\n`;
  const doc = RS64.rulesScriptParse(text);

  assert.equal(doc.rules.length, 2);
  assert.equal(doc.rules[1].raw, bad);
  assert.ok(RS64.rulesScriptWrite(doc).includes(bad));

  const action = RS64.rulesScriptParse('when start\n    wiat 4\nend\n');

  assert.ok(RS64.rulesScriptWrite(action).includes('    wiat 4\n'));
});

check('check_marks_unknown_variable', () => {
  const text = 'when start\n    set kills 1\nend\n\nwhen true: kils > 1\n    win\nend\n';
  const marks = RS64.rulesScriptCheck(text, { objects: null, objectives: true });

  assert.equal(marks.length, 1);
  assert.equal(text.slice(marks[0].from, marks[0].to), 'kils');
  assert.match(marks[0].reason, /kils/);

  const one = RS64.condCheck('kils > 1 and destroyed(gate)', { objects: new Map([['gate', 'unit']]), objectives: true, variables: new Set(['kills']) });

  assert.deepEqual(one.map((m) => [m.from, m.to]), [[0, 4]]);
});

check('check_marks_objects_and_objectives', () => {
  const objects = new Map([['gate', 'unit'], ['bridge_zone', 'area']]);
  const text = 'when enter gate if destroyed(bridge_zone) and objective_done(1)\n    destroy nothing_here\nend\n';
  const marks = RS64.rulesScriptCheck(text, { objects, objectives: false }).map((m) => text.slice(m.from, m.to));

  assert.deepEqual(marks.sort(), ['bridge_zone', 'gate', 'nothing_here', 'objective_done'].sort());
});

check('complete_by_slot', () => {
  const ctx = { objects: new Map([['gate', 'unit'], ['garage_zone', 'area'], ['bridge_zone', 'area']]), objectives: true };
  const head = 'when start\n    set bridge_held true\nend\n';
  const labels = (text) => RS64.rulesComplete(text, text.length, ctx).items.map((i) => i.label);

  assert.deepEqual(labels(head + 'when true: destroyed(ga'), ['gate']);
  assert.deepEqual(labels(head + 'when enter ga'), ['garage_zone']);
  assert.ok(labels(head + 'when enter bridge_zone if bri').includes('bridge_held'));
  assert.ok(labels(head + 'when true: bri').includes('bridge_held'));

  const text = head + 'when true: destroyed(ga';
  const done = RS64.rulesComplete(text, text.length, ctx);

  assert.equal(text.slice(done.from, done.to), 'ga');
});

check('refs_and_rename', () => {
  assert.equal(RS64.condRename('destroyed(gate) and gate_open', 'gate', 'door'), 'destroyed(door) and gate_open');
  assert.equal(RS64.condRename('health( gate ) < 5', 'gate', 'door'), 'health( door ) < 5');

  const doc = { rules: [{ when: 'true', if: 'destroyed(gate) and entered(zone)', actions: [] }] };

  assert.deepEqual(RS64.ruleRefs(doc.rules[0]).map((r) => [r.field, r.name]), [['if', 'gate'], ['if', 'zone']]);
  assert.equal(RS64.rulesRename(doc, 'gate', 'door'), 1);
  assert.equal(doc.rules[0].if, 'destroyed(door) and entered(zone)');
});

console.log(failed ? `${failed} failed` : 'all ok');
process.exit(failed ? 1 : 0);
