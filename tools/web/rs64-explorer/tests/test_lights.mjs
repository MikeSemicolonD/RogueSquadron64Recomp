// node tools/web/rs64-explorer/tests/test_lights.mjs   (no ROM needed)
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import assert from 'node:assert/strict';
createRequire(import.meta.url)('../rs64assets.js');
const { lightsParse, lightsWrite, hobMeshKey, meshKeyName, lightsMeshMatches, spriteKeys } = globalThis.RS64;
// rmobil's 7-node "vwing" keys apart from the 3-node V-wing (same table as rs64lights::meshKeyName).
assert.equal(meshKeyName('vwing', 7), 'rmobil');
assert.equal(meshKeyName('vwing', 3), 'vwing');
assert.equal(meshKeyName('xwing', 7), 'xwing');

// The shipped model lights round-trip unchanged in content.
const shipped = readFileSync(new URL('../../../../mods/model-lights/roguesq_lights.json', import.meta.url), 'utf8');
const doc = lightsParse(shipped);
assert.deepEqual(lightsParse(lightsWrite(doc)), doc);
assert.ok(doc.presets.find((p) => p.mesh === 'red_laser' && p.cameraUnits));

// An empty or missing file is an empty library; presets always get a light list.
assert.deepEqual(lightsParse(''), { presets: [] });
assert.deepEqual(lightsParse('{"presets":[{"name":"a"}]}').presets[0].lights, []);

// Keys and matching mirror the game (rt64_gbi_f3dfactor5.cpp, PresetLights::meshMatches).
assert.equal(hobMeshKey('xwing', 0, 1, 2), 'xwing#s0.p1.2');
assert.ok(lightsMeshMatches('red_laser', 'red_laser#s2.p0.0'));
assert.ok(lightsMeshMatches('xwing#s0.p1', 'xwing#s0.p1.2'));
assert.ok(lightsMeshMatches('xwing#s0.p1.2', 'xwing#s0.p1.2'));
assert.ok(!lightsMeshMatches('red_laser', 'red_laser_dst#s0.p0.0'));
assert.ok(!lightsMeshMatches('xwing#s0.p1', 'xwing#s0.p10.0'));
assert.ok(!lightsMeshMatches('', 'xwing#s0.p1.2'));
// Several keys: a player craft part and its wingman model's parts on the same joint.
assert.ok(lightsMeshMatches('xwing#s0.p1|wmxwng#s0.p3', 'wmxwng#s0.p3.0'));
assert.ok(lightsMeshMatches('xwing#s0.p1|wmxwng#s0.p3', 'xwing#s0.p1.2'));
assert.ok(!lightsMeshMatches('xwing#s0.p1|wmxwng#s0.p3', 'wmxwng#s0.p30.0'));
assert.ok(!lightsMeshMatches('xwing#s0.p1||', 'wmxwng#s0.p1.0'));

// Sprite keys are the game's texture names (logged in game as [rt-lights] sprite spr:explo_tex2): explosion sheets by file position, others by frame name.
const frames = [{ name: 'explo000' }, { name: 'explo001' }, { name: 'explo002' }];
assert.deepEqual(spriteKeys('data/explo2_IMGS', frames), ['spr:explo_tex0', 'spr:explo_tex1', 'spr:explo_tex2']);
assert.deepEqual(spriteKeys('data/explo2hi_IMGS', frames.slice(0, 1)), ['spr:explh_tex0']);
assert.deepEqual(spriteKeys('data/part_spr', [{ name: 'srp_s_01' }]), ['spr:srp_s_01']);
assert.ok(lightsMeshMatches(spriteKeys('data/explo2_IMGS', frames).join('|'), 'spr:explo_tex2'));
assert.ok(!lightsMeshMatches('spr:explo_tex1', 'spr:explo_tex10'));
console.log('ok   lights');
