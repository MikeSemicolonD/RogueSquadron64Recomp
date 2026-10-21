// rs64_dump.js: dump RDRAM (8 MB, big-endian) at game milestones for tools/validate/rdram_golden_diff.py
var OUT = "E:/Projects/RogueSquadron64Recomp/dumps/pj64/";
var cine = 0, menu = 0, done = {};
function dump(tag) {
    if (done[tag]) return;
    done[tag] = true;
    var buf = mem.getblock(0x80000000, 0x800000);
    console.log("[rs64_dump] getblock ok len=" + buf.length + " for " + tag);
    fs.writefile(OUT + "rdram_" + tag + ".bin", buf);
    console.log("[rs64_dump] wrote " + tag);
}
// State-matched goldens for tools/validate/state_diff.ps1. Mirrors the host game-state classifier
// (src/game/game_state.cpp + state_model.toml): dump rdram_state_<id>.bin once per logical state so
// the PJ64 golden lines up with the recomp's ROGUESQ_DUMP_RDRAM_ON_STATE dump.
var stateDone = {};
function inKseg0(p) { return (p >>> 0) >= 0x80000000 && (p >>> 0) < 0x80800000; }
function classifyState() {
    var objs = mem.u8[0x80130B17];                 // gMissionState+0x07 numMissionObjectives
    var flags = mem.u32[0x80130B50] >>> 0;         // gGameSettings+0x10
    if (objs != 0) return (flags & 0x20) ? "attract.demo" : "mission";
    if (inKseg0(mem.u32[0x800B0934])) return "cinematic";   // cineStatePtr
    if (inKseg0(mem.u32[0x800CE730])) {            // gCurrentMenuData
        var id = mem.u8[0x800CE734];               // menu id
        if (id == 1) { var sub = mem.u8[0x800CE626];
            if (sub == 0) return "menu.account.enter_name";
            if (sub == 1) return "menu.account.level_select";
            if (sub == 2) return "menu.account.craft_select";
            return "menu.account"; }
        var nm = { 0: "menu.main", 2: "menu.options", 3: "menu.game_settings", 4: "menu.elite_rogues",
                   5: "menu.controller_settings", 6: "menu.sound_settings", 7: "menu.passcodes",
                   8: "menu.biographies", 10: "menu.media", 11: "menu.media.showroom" };
        return nm[id] || "menu";
    }
    return "unknown";
}
function dumpState() {
    var id = classifyState();
    if (id == "unknown" || stateDone[id]) return;
    stateDone[id] = true;
    fs.writefile(OUT + "rdram_state_" + id + ".bin", mem.getblock(0x80000000, 0x800000));
    console.log("[rs64_dump] state " + id + " -> rdram_state_" + id + ".bin");
}
console.log("[rs64_dump] start; fs.exists(OUT)=" + fs.exists(OUT));
fs.writefile(OUT + "marker_abs.txt", "abs");
console.log("[rs64_dump] abs marker written");
fs.writefile("rs64_marker_rel.txt", "rel");
console.log("[rs64_dump] rel marker written");
events.onexec(0x8003DFA0, function () { console.log("[rs64_dump] mainGameLoop hit"); });
events.onexec(0x800A5D80, function () { cine++; if (cine == 1) dump("cine_iter1"); if (cine == 120) dump("cine_iter120"); });
events.onexec(0x800C58A0, function () { menu++; var a1 = cpu.gpr.a1; console.log("[rs64_dump] menuOverlayInit #" + menu + " a1=" + a1); if (menu == 1) dump("menuinit1_screen" + a1); dumpState(); });
events.onexec(0x8008DA00, function () { dump("first_musyx_tick"); });
var frame = 0;
events.onexec(0x800AA658, function () {                 // tickCutsceneActionSlots: once per cinematic frame
    dumpState();
    frame++;
    if (frame == 1) dump("cine_frame1");
    if (frame == 78) dump("cine_frame78");
    if (frame == 79) dump("cine_frame79");
    if (frame == 120) dump("cine_frame120");
    if (frame == 300) dump("cine_frame300");
    if (frame == 600) dump("cine_frame600");
    if (frame == 900) dump("cine_frame900");
});
var laOn = false, laGf = 0;
events.onexec(0x80000B20, function () { console.log("[rs64_dump] loadOverlay a0=" + cpu.gpr.a0); if (cpu.gpr.a0 == 2 && !laOn) { laOn = true; laGf = 0; } });
// Graphics-frame counter after the cinematic overlay first loads (la_gf goldens use it).
// Effect burst: the first randomlyScaledColoredEffectNpcHandler call past the LucasArts flyover arms a
// dump of every graphics frame for FX_BURST frames (particles live a fraction of a second).
var FX_AFTER_GF = 600, FX_BURST = 36, fxArmed = -1, fxSeen = {};
function fxHit(name) {
    if (!laOn) return;
    if (!fxSeen[name]) { fxSeen[name] = true; console.log("[rs64_dump] first " + name + " at gf " + laGf); }
    if (fxArmed < 0 && laGf > FX_AFTER_GF) { fxArmed = laGf; console.log("[rs64_dump] fx burst armed by " + name + " at gf " + laGf); }
}
events.onexec(0x80060900, function () { fxHit("coloredEffect"); });
events.onexec(0x80060690, function () { fxHit("plainEffect"); });
events.onexec(0x8007413C, function () { fxHit("explosionCtl"); });
events.onexec(0x8000C07C, function () {
    dumpState();   // per graphics frame: catches mission / attract states
    if (!laOn) return;
    laGf++;
    if (laGf % 200 == 0) console.log("[rs64_dump] gf " + laGf);
    if (laGf >= 840 && laGf <= 1120 && laGf % 4 == 0) dump("fx_gf" + laGf);
});
// Sky/horizon handler DRAW branch (cinematicSplineWalkerNpcHandler a1==4, 0x8005C378). Mirrors the
// recomp probe: dump a golden at draw #200 (Tatooine demo, ~77s) so we can compare hardware's sky
// modelview (0x80700040) against ours.
var sky = 0;
events.onexec(0x8005C378, function () {
    sky++;
    if (sky == 60) dump("sky_frame60"); if (sky == 200) dump("sky_frame200");
    // Tatooine attract-demo goldens for the LOD investigation: two moments (different camera
    // distances → different per-object LOD levels). Full RDRAM, so meshInstance+0xA / obj+0x19A /
    // camera globals (0x8011A7D4/0x801163C0/0x8011A8CC, D_8011D088) are all captured.
    if (sky == 120) dump("tatooine_demo_f120");
    if (sky == 300) dump("tatooine_demo_f300");
});
console.log("[rs64_dump] hooks installed");

// per-frame dt inputs of cinematicComputeDt (0x800AF360): after timeSnapshotFiller returns (pc 0x800AF374),
// period @sp+0x2C, elapsed @sp+0x30. CSV: frame,period,elapsed,gateCtr,viCount,ctr8013889C
var dtLines = "", dtN = 0;
events.onexec(0x800AF374, function () {
    if (dtN >= 200) return;
    var sp = cpu.gpr.sp;
    dtLines += dtN + "," + mem.f32[sp + 0x2C] + "," + mem.f32[sp + 0x30] + "," + mem.u32[0x800B0B28] + "," + mem.u32[0x8011A890] + "," + mem.u32[0x8013889C] + "," + mem.u32[0x80138ED8] + "," + mem.u32[0x8011A900] + "," + mem.u32[0x8011A904] + "," + mem.u8[0x8011A8C8] + "," + mem.u8[0x8011A8C9] + "\n";
    dtN++;
    if (dtN == 130 || dtN == 200) { fs.writefile(OUT + "cine_dt_trace.csv", dtLines); console.log("[rs64_dump] wrote cine_dt_trace.csv (" + dtN + " frames)"); }
});

// every timeSnapshotFiller (0x8000BC00) call: which caller (ra) and the CPU count, to see who consumes the frame interval
var tsfN = 0;
events.onexec(0x8000BC00, function () {
    if (tsfN >= 400 || dtN >= 200) return;
    tsfN++;
    var cnt = 0; try { cnt = cpu.cop0.count; } catch (e) { cnt = -1; }
    dtLines += "tsf," + tsfN + "," + cpu.gpr.ra.toString(16) + "," + cnt + "," + dtN + "\n";
});
// PI DMA log (register level, catches libultra and custom DMA): on write to PI_WR_LEN (0xA460000C, cart->RAM)
// or PI_RD_LEN (0xA4600008). Logs only transfers whose cart address is inside samp_SND (ROM 0x2B60B8..0x583D90);
// counts everything else. CSV: frameIdx,reg,cartAddr,dramAddr,len,pc
var dmaLines = "", dmaN = 0, dmaOther = 0;
function piLog(reg, e) {
    var cart = mem.u32[0xA4600004] & 0x0FFFFFFF, dram = mem.u32[0xA4600000] & 0x00FFFFFF;
    var len = -1; try { len = e.value; } catch (x) {}
    var pc = -1; try { pc = e.pc; } catch (x) {}
    if (cart >= 0x2B60B8 && cart < 0x583D90) {
        if (dmaN >= 4000) return;
        dmaN++;
        dmaLines += dtN + "," + reg + "," + cart.toString(16) + "," + dram.toString(16) + "," + len + "," + pc.toString(16) + "\n";
        if (dmaN % 100 == 0) fs.writefile(OUT + "dma_trace.csv", dmaLines + "other," + dmaOther + "\n");
    } else { dmaOther++; }
}
events.onwrite(0xA460000C, function (e) { piLog("wr", e); });
events.onwrite(0xA4600008, function (e) { piLog("rd", e); });
events.onexec(0x800C58A0, function () { if (menu >= 2) fs.writefile(OUT + "dma_trace.csv", dmaLines + "other," + dmaOther + "\n"); });

// Thread/message order trace (hardware ground truth): every osSendMesg/osRecvMesg/osYieldThread/osStartThread/
// __osEnqueueAndYield entry during cinematic frames FRAME_LO..FRAME_HI, with the running OSThread id.
// CSV: frameIdx,event,threadId,queue,flags,ra
var FRAME_LO = 60, FRAME_HI = 70, mtLines = "", mtN = 0;
function curThread() { var t = mem.u32[0x80039190]; return t ? mem.u32[t + 0x14] : -1; }
function mt(ev, q, fl) {
    if (dtN < FRAME_LO || dtN > FRAME_HI || mtN >= 20000) return;
    mtN++;
    mtLines += dtN + "," + ev + "," + curThread() + "," + q.toString(16) + "," + fl + "," + cpu.gpr.ra.toString(16) + "\n";
    if (mtN % 500 == 0 || dtN == FRAME_HI) fs.writefile(OUT + "mesg_trace.csv", mtLines);
}
events.onexec(0x80033410, function () { mt("send", cpu.gpr.a0, cpu.gpr.a2); });
events.onexec(0x800331D0, function () { mt("recv", cpu.gpr.a0, cpu.gpr.a2); });
events.onexec(0x80037510, function () { mt("yield", 0, 0); });
events.onexec(0x800344E0, function () { mt("start", cpu.gpr.a0, 0); });
events.onexec(0x8002BC1C, function () { mt("enqyield", cpu.gpr.a0, 0); });
events.onexec(0x8002BD74, function () { mt("dispatch", 0, 0); });
// init-path entry counts
var initN = {registerSiCallback:0,setDisplayMode:0,initAudioSubsystem:0,initMusyXVoiceTable:0,initSpeechSubsystem:0,mainBootstrapWorker:0};
events.onexec(0x80007910, function () { console.log("[initcount] registerSiCallback #" + (++initN.registerSiCallback) + " ra=" + cpu.gpr.ra.toString(16) + " cb=" + cpu.gpr.a0.toString(16)); });
events.onexec(0x8008EA14, function () { console.log("[initcount] setDisplayMode #" + (++initN.setDisplayMode) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x80091B3C, function () { console.log("[initcount] initAudioSubsystem #" + (++initN.initAudioSubsystem) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x8009BBA0, function () { console.log("[initcount] initMusyXVoiceTable #" + (++initN.initMusyXVoiceTable) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x80066A90, function () { console.log("[initcount] initSpeechSubsystem #" + (++initN.initSpeechSubsystem) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x80000C68, function () { console.log("[initcount] mainBootstrapWorker #" + (++initN.mainBootstrapWorker) + " ra=" + cpu.gpr.ra.toString(16)); });
var initM = {initVideoSubsystem:0,clearFourWordTable:0,reapplyDisplayMode:0,reapplyAudioConfig:0,initN64RspMixer:0};
events.onexec(0x8001A098, function () { console.log("[initcount] initVideoSubsystem #" + (++initM.initVideoSubsystem) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x80007980, function () { console.log("[initcount] clearFourWordTable #" + (++initM.clearFourWordTable) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x8008EB5C, function () { console.log("[initcount] reapplyDisplayMode #" + (++initM.reapplyDisplayMode) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x80091C88, function () { console.log("[initcount] reapplyAudioConfig #" + (++initM.reapplyAudioConfig) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x8009189C, function () { console.log("[initcount] initN64RspMixer #" + (++initM.initN64RspMixer) + " ra=" + cpu.gpr.ra.toString(16)); });

// Write watch on the SI callback table 0x8011A8A4..0x8011A8B3: every store with pc and new value.
var tw = 0;
function siTableWrite(e) {
    if (tw++ > 60) return;
    var pc = -1, val = -1; try { pc = e.pc; } catch (x) {} try { val = e.value; } catch (x) {}
    console.log("[sitable-write] addr=" + e.address.toString(16) + " val=" + val.toString(16) + " pc=" + pc.toString(16));
}
events.onwrite(new AddressRange(0x8011A8A4, 0x8011A8B4), siTableWrite);
var unregN = 0;
events.onexec(0x8000794C, function () { if (unregN++ < 12) console.log("[unregcount] findAndZeroTableSlotMatching #" + unregN + " a0=" + cpu.gpr.a0.toString(16) + " ra=" + cpu.gpr.ra.toString(16)); });
var smN = 0;
events.onexec(0x80091EB0, function () { if (smN++ < 8) console.log("[shutdownmix] waitForMusyXAudioTaskDone #" + smN + " ra=" + cpu.gpr.ra.toString(16)); });
var vwN = 0;
events.onexec(0x800B9354, function () { if (vwN++ < 6) console.log("[vwinginit] initVwingHudAndSlotState #" + vwN + " ra=" + cpu.gpr.ra.toString(16) + " a0=" + cpu.gpr.a0.toString(16)); });
// nop-audit: what hardware does at each instruction the recomp nops (value/address at exec)
var npN = {};
var npLines = "";
function npLog(k, msg) { npN[k] = (npN[k]||0)+1; if (npN[k] <= 8) npLines += k + " #" + npN[k] + " " + msg + "\n"; }
events.onexec(0x800668cc, function () { npLog("waitForAnyAudioSlot@800668CC", "beqz        $v0, . + 4 + (-0x4 << 2) | v0=" + (cpu.gpr.v0>>>0).toString(16) + " zero=" + (0).toString(16)); });
events.onexec(0x800c593c, function () { var a = (cpu.gpr.sp + (28)) >>> 0; npLog("menuOverlayInit@800C593C", "sw          $s1, 0x1C($sp) | addr=" + a.toString(16) + " mem=" + mem.u32[a].toString(16) + " s1=" + (cpu.gpr.s1>>>0).toString(16) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x80007a78, function () { var a = (cpu.gpr.v1 + (0)) >>> 0; npLog("heapWalker@80007A78", "lw          $v1, 0x0($v1) | addr=" + a.toString(16) + " mem=" + mem.u32[a].toString(16) + " v1=" + (cpu.gpr.v1>>>0).toString(16) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x8001c3c4, function () { npLog("dpInterruptHandlerThread@8001C3C4", "beqz        $v1, . + 4 + (0x5 << 2) | v1=" + (cpu.gpr.v1>>>0).toString(16) + " zero=" + (0).toString(16)); });
events.onexec(0x80091798, function () { npLog("musyxSynthFrame@80091798", "jalr        $v0 | target=" + (cpu.gpr.v0>>>0).toString(16)); });
events.onexec(0x80022660, function () { var a = (cpu.gpr.v1 + (16)) >>> 0; npLog("findOrCreateMaterial@80022660", "lw          $v0, 0x10($v1) | addr=" + a.toString(16) + " mem=" + mem.u32[a].toString(16) + " v0=" + (cpu.gpr.v0>>>0).toString(16) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x80022094, function () { var a = (cpu.gpr.s0 + (8)) >>> 0; npLog("tickTextureMaterialExpiry@80022094", "lhu         $v1, 0x8($s0) | addr=" + a.toString(16) + " mem=" + mem.u16[a].toString(16) + " v1=" + (cpu.gpr.v1>>>0).toString(16) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x800220d0, function () { var a = (cpu.gpr.v1 + (0)) >>> 0; npLog("tickTextureMaterialExpiry@800220D0", "sw          $v0, 0x0($v1) | addr=" + a.toString(16) + " mem=" + mem.u32[a].toString(16) + " v0=" + (cpu.gpr.v0>>>0).toString(16) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x800220f8, function () { var a = (cpu.gpr.v1 + (4)) >>> 0; npLog("tickTextureMaterialExpiry@800220F8", "sw          $v0, 0x4($v1) | addr=" + a.toString(16) + " mem=" + mem.u32[a].toString(16) + " v0=" + (cpu.gpr.v0>>>0).toString(16) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x8002210c, function () { var a = (cpu.gpr.v0 + (4)) >>> 0; npLog("tickTextureMaterialExpiry@8002210C", "sw          $s0, 0x4($v0) | addr=" + a.toString(16) + " mem=" + mem.u32[a].toString(16) + " s0=" + (cpu.gpr.s0>>>0).toString(16) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x800222c4, function () { var a = (cpu.gpr.v0 + (0)) >>> 0; npLog("findAndUnlinkSmallestEntry@800222C4", "sw          $a0, 0x0($v0) | addr=" + a.toString(16) + " mem=" + mem.u32[a].toString(16) + " a0=" + (cpu.gpr.a0>>>0).toString(16) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x800079b8, function () { var a = (cpu.gpr.v1 + (0)) >>> 0; npLog("heapFreeListInsert@800079B8", "lw          $v1, 0x0($v1) | addr=" + a.toString(16) + " mem=" + mem.u32[a].toString(16) + " v1=" + (cpu.gpr.v1>>>0).toString(16) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x800079bc, function () { var a = (cpu.gpr.v1 + (0)) >>> 0; npLog("heapFreeListInsert@800079BC", "lw          $v0, 0x0($v1) | addr=" + a.toString(16) + " mem=" + mem.u32[a].toString(16) + " v0=" + (cpu.gpr.v0>>>0).toString(16) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x800185e8, function () { npLog("initVideoBootWrapper@800185E8", "beqz        $v0, . + 4 + (0xC << 2) | v0=" + (cpu.gpr.v0>>>0).toString(16) + " zero=" + (0).toString(16)); });
events.onexec(0x800644e0, function () { var a = (cpu.gpr.s0 + (20)) >>> 0; npLog("rebuildActiveDisplayObjectList@800644E0", "lw          $v0, 0x14($s0) | addr=" + a.toString(16) + " mem=" + mem.u32[a].toString(16) + " v0=" + (cpu.gpr.v0>>>0).toString(16) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x8000d324, function () { var a = (cpu.gpr.v0 + (0)) >>> 0; npLog("heapFreeListDequeue@8000D324", "lw          $v0, 0x0($v0) | addr=" + a.toString(16) + " mem=" + mem.u32[a].toString(16) + " v0=" + (cpu.gpr.v0>>>0).toString(16) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x80010830, function () { var a = (cpu.gpr.v0 + (0)) >>> 0; npLog("submitSceneNodeRender@80010830", "lw          $v0, 0x0($v0) | addr=" + a.toString(16) + " mem=" + mem.u32[a].toString(16) + " v0=" + (cpu.gpr.v0>>>0).toString(16) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x80016f18, function () { var a = (cpu.gpr.v0 + (0)) >>> 0; npLog("setupCameraMatrices@80016F18", "lw          $v0, 0x0($v0) | addr=" + a.toString(16) + " mem=" + mem.u32[a].toString(16) + " v0=" + (cpu.gpr.v0>>>0).toString(16) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x80010840, function () { var a = (cpu.gpr.v0 + (4)) >>> 0; npLog("submitSceneNodeRender@80010840", "sw          $zero, 0x4($v0) | addr=" + a.toString(16) + " mem=" + mem.u32[a].toString(16) + " zero=" + (0).toString(16) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x80016f28, function () { var a = (cpu.gpr.v0 + (4)) >>> 0; npLog("setupCameraMatrices@80016F28", "sw          $zero, 0x4($v0) | addr=" + a.toString(16) + " mem=" + mem.u32[a].toString(16) + " zero=" + (0).toString(16) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x80001d84, function () { var a = (cpu.gpr.a1 + (0)) >>> 0; npLog("rs_free@80001D84", "lhu         $v1, 0x0($a1) | addr=" + a.toString(16) + " mem=" + mem.u16[a].toString(16) + " v1=" + (cpu.gpr.v1>>>0).toString(16) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x80002434, function () { var a = (cpu.gpr.v0 + (16)) >>> 0; npLog("coalesceFreeHeapBlocks@80002434", "lhu         $v1, 0x10($v0) | addr=" + a.toString(16) + " mem=" + mem.u16[a].toString(16) + " v1=" + (cpu.gpr.v1>>>0).toString(16) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x80047538, function () { var a = (cpu.gpr.a1 + (0)) >>> 0; npLog("updateActiveGridCellState@80047538", "lhu         $s4, 0x0($a1) | addr=" + a.toString(16) + " mem=" + mem.u16[a].toString(16) + " s4=" + (cpu.gpr.s4>>>0).toString(16) + " ra=" + cpu.gpr.ra.toString(16)); });
events.onexec(0x800C58A0, function () { if (menu >= 2) { var s = ""; for (var k in npN) s += k + "," + npN[k] + "\n"; fs.writefile(OUT + "nopaudit_counts.csv", s); fs.writefile(OUT + "nopaudit_log.txt", npLines); } });
