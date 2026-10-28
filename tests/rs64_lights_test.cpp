#include "../lib/rt64/src/hle/rt64_rs64_lights.h"
#include "check.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace rs64lights;

static bool near(float a, float b, float tol) {
    return std::fabs(a - b) <= tol * std::max(1.0f, std::fabs(b));
}

static Mat4 perspective(float fovY, float aspect, float zn, float zf) {
    const float f = 1.0f / std::tan(fovY * 0.5f);
    Mat4 p{};
    p.m[0][0] = f / aspect;
    p.m[1][1] = f;
    p.m[2][2] = zf / (zf - zn);
    p.m[2][3] = 1.0f;
    p.m[3][2] = -zn * zf / (zf - zn);
    return p;
}

static void testInverse() {
    Mat4 a = perspective(1.0f, 4.0f / 3.0f, 10.0f, 20000.0f);
    a.m[3][0] = 3.0f;
    a.m[1][0] = 0.25f;
    Mat4 inv;
    CHECK(inverse(a, inv));
    const Mat4 id = mul(a, inv);
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            CHECK(std::fabs(id.m[r][c] - (r == c ? 1.0f : 0.0f)) < 1e-4f);
        }
    }
    Mat4 singular{};
    CHECK(!inverse(singular, inv));
}

static void testScreenRoundTrip() {
    const Mat4 vp = perspective(1.1f, 4.0f / 3.0f, 10.0f, 20000.0f);
    ScreenMap map{};
    map.vpScale[0] = 160.0f;
    map.vpScale[1] = 120.0f;
    map.vpScale[2] = 0.5f;
    map.vpTranslate[0] = 160.0f;
    map.vpTranslate[1] = 120.0f;
    map.vpTranslate[2] = 0.5f;
    map.res[0] = 320.0f;
    map.res[1] = 240.0f;
    map.screenScale[0] = 0.75f;
    map.screenScale[1] = 1.0f;
    map.screenOffset[0] = 0.0015f;
    map.screenOffset[1] = -0.002f;
    Mat4 inv;
    CHECK(viewFromScreen(vp, map, inv));
    const float pts[4][3] = { { 10.0f, 5.0f, 200.0f }, { -300.0f, 80.0f, 1500.0f }, { 0.0f, 0.0f, 30.0f }, { 900.0f, -700.0f, 12000.0f } };
    for (const auto& p : pts) {
        float s[3];
        projectToScreen(vp, map, p, s);
        const float sv[4] = { s[0], s[1], s[2], 1.0f };
        float v[4];
        transform(sv, inv, v);
        CHECK(near(v[0] / v[3], p[0], 1e-3f));
        CHECK(near(v[1] / v[3], p[1], 1e-3f));
        CHECK(near(v[2] / v[3], p[2], 1e-3f));
    }
}

static void testSpriteLight() {
    Candidate c;
    CHECK(spriteLight(0xFFA040FFu, 50.0f, 6.0f, 1200.0f, c));
    CHECK(c.kind == KindSprite);
    CHECK(near(c.color[0], 1.0f, 1e-5f));
    CHECK(c.color[1] < 1.0f && c.color[2] < c.color[1]);
    CHECK(near(c.radius, 300.0f, 1e-5f));
    CHECK(c.intensity > 0.0f && c.intensity <= 1.0f);
    CHECK(!spriteLight(0x404040FFu, 50.0f, 6.0f, 1200.0f, c));
    CHECK(!spriteLight(0xFFFFFF00u, 50.0f, 6.0f, 1200.0f, c));
    CHECK(!spriteLight(0xFFFFFFFFu, 0.0f, 6.0f, 1200.0f, c));
}

static void testSpriteLightRadiusCap() {
    Candidate c;
    CHECK(spriteLight(0xFFA040FFu, 900.0f, 6.0f, 1200.0f, c));
    CHECK(near(c.radius, 1200.0f, 1e-5f));
}

static void testSameMatrix() {
    const Mat4 a = perspective(1.0f, 4.0f / 3.0f, 10.0f, 20000.0f);
    Mat4 b = a;
    CHECK(sameMatrix(a, b, 1e-5f));
    b.m[3][2] *= 1.0f + 1e-7f;
    CHECK(sameMatrix(a, b, 1e-5f));
    b.m[0][0] *= 1.1f;
    CHECK(!sameMatrix(a, b, 1e-5f));
    const Candidate c;
    CHECK(c.viewIndex == 0);
}

static void testModelSunDir() {
    // A model rotated 90 degrees about z and scaled 2x: its up axis (-y in the y-down convention) maps to camera +x.
    Mat4 m{};
    m.m[0][1] = 2.0f;
    m.m[1][0] = -2.0f;
    m.m[2][2] = 2.0f;
    m.m[3][0] = 100.0f;
    m.m[3][3] = 1.0f;
    const float straightDown[3] = { 0.0f, 1.0f, 0.0f };
    float out[3];
    CHECK(modelSunDir(straightDown, m, out));
    CHECK(near(out[0], 1.0f, 1e-5f) && near(out[1], 0.0f, 1e-5f) && near(out[2], 0.0f, 1e-5f));
    // Identity model: same as the camera-space sun.
    const Mat4 id = identity();
    CHECK(modelSunDir(straightDown, id, out));
    CHECK(near(out[1], -1.0f, 1e-5f));
    Mat4 zero{};
    CHECK(!modelSunDir(straightDown, zero, out));
}

static void testReceiverSkipped() {
    const float up[4] = { 0.0f, -0.99f, -0.14f, 0.985f };
    const float floorN[3] = { 0.0f, -0.99f, -0.14f };
    const float tiltedN[3] = { 0.0f, -0.94f, -0.34f };
    const float wallN[3] = { 1.0f, 0.0f, 0.0f };
    CHECK(receiverSkipped(floorN, up));
    CHECK(!receiverSkipped(tiltedN, up));
    CHECK(!receiverSkipped(wallN, up));
    const float off[4] = { 0.0f, -0.99f, -0.14f, 0.0f };
    CHECK(!receiverSkipped(floorN, off));
}

static void testSunCameraDir() {
    const float identity[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    const float L[3] = { 0.374f, 0.624f, 0.686f };
    float out[3];
    // Row-vector view matrix (v * M) -> column rotation R (out = R * v), scale removed.
    Mat4 view = {};
    view.m[0][2] = -2.0f;
    view.m[1][1] = 2.0f;
    view.m[2][0] = 2.0f;
    view.m[3][0] = 500.0f;
    view.m[3][3] = 1.0f;
    float R[9];
    CHECK(viewRotation(view, R));
    CHECK(near(R[2 * 3 + 0], -1.0f, 1e-6f) && near(R[1 * 3 + 1], 1.0f, 1e-6f) && near(R[0 * 3 + 2], 1.0f, 1e-6f) && (R[0] == 0.0f));
    Mat4 empty = {};
    CHECK(!viewRotation(empty, R));
    CHECK(sunCameraDir(L, identity, out));
    CHECK(near(out[0], 0.374f, 1e-3f) && near(out[1], -0.624f, 1e-3f) && near(out[2], 0.686f, 1e-3f));
    // 90 degrees about Y in the game's column convention: x' = z, z' = -x.
    const float rotY[9] = { 0, 0, 1, 0, 1, 0, -1, 0, 0 };
    const float L2[3] = { 1, 0, 0 };
    CHECK(sunCameraDir(L2, rotY, out));
    CHECK(near(out[0], 0.0f, 1e-5f) && near(out[2], -1.0f, 1e-5f));
    const float zero[3] = { 0, 0, 0 };
    CHECK(!sunCameraDir(zero, identity, out));
    // A horizon sun is lifted to the minimum elevation (y-down world); a higher one is left alone.
    const float tan25 = std::tan(25.0f * 3.14159265f / 180.0f);
    CHECK(sunCameraDir(L2, identity, out, tan25));
    CHECK(near(out[1], -std::sin(25.0f * 3.14159265f / 180.0f), 1e-5f) && near(out[0], std::cos(25.0f * 3.14159265f / 180.0f), 1e-5f));
    CHECK(sunCameraDir(L, identity, out, tan25));
    CHECK(near(out[1], -0.624f, 1e-3f));
    // Level 6 stores a sun below the horizon (unused by the game); it is mirrored above it (y-down world: negative y is up).
    const float below[3] = { 0.0f, -0.832f, 0.555f };
    CHECK(sunCameraDir(below, identity, out));
    CHECK(out[1] < 0.0f);
    CHECK(near(out[1], -0.832f, 1e-3f) && near(out[2], 0.555f, 1e-3f));
}

static void testGatherCasterIndices() {
    const uint32_t faces[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    const IndexRange ranges[3] = { { 3, 1 }, { 9, 1 }, { 9, 2 } };
    std::vector<uint32_t> out;
    const size_t n = gatherCasterIndices(faces, 12, ranges, 3, out);
    CHECK(n == 6);
    CHECK(out.size() == 6);
    CHECK(out[0] == 3 && out[2] == 5 && out[3] == 9 && out[5] == 11);
    CHECK(gatherCasterIndices(faces, 12, nullptr, 0, out) == 0);
}

static void testFnv1a64() {
    const uint8_t a = 'a';
    CHECK(fnv1a64(&a, 1) == 0xaf63dc4c8601ec8cull);
    CHECK(fnv1a64(nullptr, 0) == 0xcbf29ce484222325ull);
}

static void testTileCells() {
    const uint16_t tiles[4] = { 3, 5, 3, 0 };
    std::vector<int32_t> cells;
    buildTileCells(tiles, 4, cells);
    CHECK(cells.size() == 6);
    CHECK(cells[0] == 3);
    CHECK(cells[5] == 1);
    CHECK(cells[3] == -2);
    CHECK(cells[1] == -1);
}

static void testTerrainTileMesh() {
    int8_t h[25];
    for (int k = 0; k < 25; ++k) {
        h[k] = (int8_t)(k - 12);
    }
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    terrainTileMesh(h, 2, 1024, -512, 1.0f, pos, idx);
    CHECK(pos.size() == 81 * 3);
    CHECK(idx.size() == 8 * 8 * 6);
    CHECK(pos[0] == 1024.0f && pos[1] == -192.0f && pos[2] == -512.0f);
    const size_t v = (2 * 9 + 2) * 3;
    CHECK(pos[v] == 1152.0f && pos[v + 1] == -96.0f && pos[v + 2] == -384.0f);
    CHECK(pos[3] == 1088.0f && pos[4] == -184.0f);
    CHECK(idx[0] == 0 && idx[1] == 10 && idx[2] == 9 && idx[3] == 0 && idx[4] == 1 && idx[5] == 10);
    terrainTileMesh(h, 1, 0, 0, 1.0f, pos, idx);
    CHECK(pos.size() == (81 + 25) * 3);
    CHECK(idx[8 * 8 * 6] == 81);
}

static void testBuildTerrainMesh() {
    TerrainMap m;
    m.width = 2;
    m.height = 1;
    m.heights.assign(2 * 25, 0);
    m.heights[25] = 10;
    m.present = { 1, 1 };
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    buildTerrainMesh(m, 1, pos, idx);
    CHECK(pos.size() == 2 * 25 * 3);
    CHECK(idx.size() == 2 * 16 * 6);
    CHECK(pos[25 * 3] == 512.0f && pos[25 * 3 + 1] == 160.0f && pos[25 * 3 + 2] == 0.0f);
    CHECK(idx[16 * 6] == 25);
    m.present[0] = 0;
    buildTerrainMesh(m, 1, pos, idx);
    CHECK(pos.size() == 25 * 3 && pos[0] == 512.0f);

    // A shore cell adds a flat quad TerrainWaterDip units under the water level (heights grow downward), facing up like the ground (same winding).
    m.present = { 1, 1 };
    m.waterKind = { 0, 2 };
    m.waterLevel = 10;
    buildTerrainMesh(m, 1, pos, idx);

    // Every margin cell is water too (the map never ends in a void), row-major from the margin's corner, the outer ring reaching TerrainWaterFarCells
    // further; the ground cell in the map is not.
    const size_t spanW = size_t(2 * TerrainWaterMargin + 2);
    const size_t span = spanW * size_t(2 * TerrainWaterMargin + 1);
    const size_t shore = 50 + 4 * (size_t(TerrainWaterMargin) * spanW + size_t(TerrainWaterMargin));
    const size_t shoreIdx = 2 * 16 * 6 + 6 * (shore - 50) / 4;

    CHECK(pos.size() == (2 * 25 + 4 * (span - 1)) * 3);
    CHECK(pos[50 * 3] == float(-(TerrainWaterMargin + TerrainWaterFarCells) * 512) && pos[50 * 3 + 2] == float(-(TerrainWaterMargin + TerrainWaterFarCells) * 512));
    CHECK(pos[53 * 3] == float((1 - TerrainWaterMargin) * 512));
    CHECK(pos[shore * 3] == 512.0f && pos[shore * 3 + 1] == float((10 + TerrainWaterDip) * 16) && pos[shore * 3 + 2] == 0.0f);
    CHECK(pos[(shore + 3) * 3] == 1024.0f && pos[(shore + 3) * 3 + 2] == 512.0f);
    CHECK(idx[shoreIdx] == shore && idx[shoreIdx + 1] == shore + 3 && idx[shoreIdx + 2] == shore + 2);
}

static void testTerrainHitColors() {

    // Two present cells, the second also water: header, both ground cells in order, then the water quad.
    TerrainMap m;
    m.width = 2;
    m.height = 1;
    m.heights.assign(2 * 25, 0);
    m.present = { 1, 1 };
    m.waterKind = { 0, 2 };
    m.colors.assign(2 * 75, 255);

    for (int k = 0; k < 25; ++k) {
        m.colors[75 + k * 3] = 102;
    }

    const std::vector<uint32_t> texAvg = { 0x00FF8040u, 0x00808080u };
    std::vector<uint32_t> out;

    terrainHitColors(m, 1, texAvg, out);

    m.waterLevel = 10;
    terrainHitColors(m, 1, texAvg, out);

    float surfaceY = 0.0f;

    memcpy(&surfaceY, &out[3], sizeof(float));
    const size_t spanW = size_t(2 * TerrainWaterMargin + 2);

    // The water quads: the margin (each the nearest edge cell's colour) around the shore cell.
    CHECK(out.size() == 4 + 2 + spanW * size_t(2 * TerrainWaterMargin + 1) - 1);
    CHECK(out[0] == 32 && out[1] == 64 && out[2] == 2);
    CHECK(surfaceY == 160.0f - TerrainWaterLift);
    CHECK(out[4] == 0x00FF8040u);
    CHECK(out[5] == ((128u * 102u + 127u) / 255u | (128u << 8) | (128u << 16)));
    CHECK(out[6] == out[4] && out[6 + size_t(TerrainWaterMargin) * spanW + size_t(TerrainWaterMargin)] == out[5]);

    m.waterKind = { 0, 0 };
    terrainHitColors(m, 1, texAvg, out);
    CHECK((out.size() == 6) && (out[1] == 0));
}

static void testSolveTerrainOrigin() {
    // Level 0 measurement: cell (34,16) drawn at x=0 z=1024, (35,16) at 512/1024, (33,17) at -512/1536.
    TerrainMap m;
    m.width = 72;
    m.height = 72;
    m.tileCell.assign(1000, -1);
    m.tileCell[650] = 16 * 72 + 34;
    m.tileCell[651] = 16 * 72 + 35;
    m.tileCell[704] = 17 * 72 + 33;
    m.tileCell[3] = -2;
    const TerrainSample s[5] = { { 0, 1024, 650 }, { 512, 1024, 651 }, { -512, 1536, 704 }, { 0, 0, 3 }, { 0, 0, 999 } };
    int32_t ox = 0, oz = 0;
    CHECK(solveTerrainOrigin(m, s, 5, ox, oz) == 3);
    CHECK(ox == 34 * 512 && oz == 16 * 512 - 1024);
    CHECK(solveTerrainOrigin(m, nullptr, 0, ox, oz) == 0);
    const TerrainSample tie[2] = { { 0, 1024, 650 }, { 0, 1024, 651 } };
    CHECK(solveTerrainOrigin(m, tie, 2, ox, oz) == 0);
    const TerrainSample shared[2] = { { 0, 0, 3 }, { 512, 0, 3 } };
    CHECK(solveTerrainOrigin(m, shared, 2, ox, oz) == 0);
}

static void testTerrainInstanceTransform() {
    Mat4 m = identity();
    m.m[0][0] = 0.0f;
    m.m[0][2] = -1.0f;
    m.m[2][0] = 1.0f;
    m.m[2][2] = 0.0f;
    m.m[3][0] = 5.0f;
    m.m[3][1] = -7.0f;
    m.m[3][2] = 100.0f;
    float t[3][4];
    terrainInstanceTransform(m, 17408, 7168, t);
    const float pm[3] = { 17408.0f + 300.0f, 40.0f, 7168.0f - 200.0f };
    const float rec[4] = { pm[0] - 17408.0f, pm[1], pm[2] - 7168.0f, 1.0f };
    float want[4];
    transform(rec, m, want);
    for (int j = 0; j < 3; ++j) {
        CHECK(near(t[j][0] * pm[0] + t[j][1] * pm[1] + t[j][2] * pm[2] + t[j][3], want[j], 1e-4f));
    }
}

static void testTerrainMapKey() {
    const uint16_t tiles[3] = { 1, 2, 1 };
    uint8_t table[60] = {};
    const uint64_t a = terrainMapKey(tiles, 3, table, sizeof(table), 0x802F2634u, 3, 1);
    CHECK(a == terrainMapKey(tiles, 3, table, sizeof(table), 0x802F2634u, 3, 1));
    table[40] = 7;
    CHECK(a != terrainMapKey(tiles, 3, table, sizeof(table), 0x802F2634u, 3, 1));
    table[40] = 0;
    CHECK(a != terrainMapKey(tiles, 3, table, sizeof(table), 0x802F3CF4u, 3, 1));
}

static void testSolveTerrainOriginWithPrevious() {
    TerrainMap m;
    m.width = 72;
    m.height = 72;
    m.tileCell.assign(1000, -1);
    m.tileCell[650] = 16 * 72 + 34;
    m.tileCell[651] = 16 * 72 + 35;
    const TerrainSample one[1] = { { 0, 1024, 650 } };
    int32_t ox = 0, oz = 0;
    CHECK(solveTerrainOrigin(m, one, 1, ox, oz) == 0);
    CHECK(solveTerrainOrigin(m, one, 1, ox, oz, true, 34 * 512, 16 * 512 - 1024) == 1);
    CHECK(ox == 34 * 512 && oz == 16 * 512 - 1024);
    CHECK(solveTerrainOrigin(m, one, 1, ox, oz, true, 0, 0) == 0);
    const TerrainSample split[2] = { { 0, 1024, 650 }, { 0, 1024, 651 } };
    CHECK(solveTerrainOrigin(m, split, 2, ox, oz, true, 34 * 512, 16 * 512 - 1024) == 0);
    // Frames that draw only flat far tiles have no votes; the previous origin carries over.
    ox = oz = 0;
    CHECK(solveTerrainOrigin(m, nullptr, 0, ox, oz, true, 1024, 2048) == 1);
    CHECK(ox == 1024 && oz == 2048);
    CHECK(solveTerrainOrigin(m, nullptr, 0, ox, oz) == 0);
}

static void testNoteTerrainTransform() {
    TerrainFrame t;
    noteTerrainTransform(t, 13, true);
    noteTerrainTransform(t, 14, true);
    noteTerrainTransform(t, 14, true);
    CHECK(t.transformIndex == 13 && !t.mixed);
    CHECK(t.transforms.size() == 2);
    CHECK(isTerrainTransform(t, 13) && isTerrainTransform(t, 14) && !isTerrainTransform(t, 15));
    noteTerrainTransform(t, 20, false);
    CHECK(t.mixed);
    CHECK(!isTerrainTransform(t, 20));
}

static void testSceneCaptureEnabled() {
    Config c{};
    CHECK(!sceneCaptureEnabled(c));
    c.shadows = true;
    CHECK(sceneCaptureEnabled(c));
    c.shadows = false;
    c.enabled = true;
    c.lightShadows = true;
    CHECK(sceneCaptureEnabled(c));
    c.lightShadows = false;
    CHECK(!sceneCaptureEnabled(c));
    c.enabled = false;
    c.fogShafts = true;
    CHECK(sceneCaptureEnabled(c));
}

static void testSceneWanted() {
    CHECK(sceneWanted(true, false, false, false, false));
    CHECK(sceneWanted(false, true, true, true, false));
    CHECK(!sceneWanted(false, true, false, true, false));
    CHECK(!sceneWanted(false, true, true, false, false));
    CHECK(!sceneWanted(false, true, true, true, true));
    CHECK(!sceneWanted(false, false, true, true, false));
}

static void testLightRaySpan() {
    float tMax = 0.0f, terrainTMin = 0.0f;
    bool traceTerrain = false;
    CHECK(lightRaySpan(500.0f, 1000.0f, 0.1f, 2.0f, 1000.0f, 0.03f, tMax, terrainTMin, traceTerrain));
    CHECK(near(tMax, 400.0f, 1e-5f) && near(terrainTMin, 30.0f, 1e-5f) && traceTerrain);
    // A surface at the light's centre (explosion in the ground): nothing left to trace, the light stays visible.
    CHECK(!lightRaySpan(90.0f, 1000.0f, 0.1f, 2.0f, 1000.0f, 0.03f, tMax, terrainTMin, traceTerrain));
    // Far receiver: the terrain start passes the ray end, so only the drawn casters are traced.
    CHECK(lightRaySpan(500.0f, 1000.0f, 0.1f, 2.0f, 20000.0f, 0.03f, tMax, terrainTMin, traceTerrain));
    CHECK(!traceTerrain);
    // A fireball's own extent (shadowStart) beats the radius fraction: wreckage inside the blast never blocks its light.
    CHECK(lightRaySpan(500.0f, 1000.0f, 0.1f, 2.0f, 1000.0f, 0.03f, tMax, terrainTMin, traceTerrain, 400.0f));
    CHECK(near(tMax, 100.0f, 1e-5f));
    CHECK(lightRaySpan(500.0f, 1000.0f, 0.1f, 2.0f, 1000.0f, 0.03f, tMax, terrainTMin, traceTerrain, 50.0f));
    CHECK(near(tMax, 400.0f, 1e-5f));
}

static void testSpriteShadowStart() {
    Candidate c;
    CHECK(spriteLight(0xFFE0A0FFu, 120.0f, 3.0f, 1200.0f, c));
    CHECK(c.shadowStart == 120.0f);
    CHECK(c.radius == 360.0f);
    Candidate l;
    CHECK(l.shadowStart == 0.0f);
}

static void testNearestLights() {
    const float d[6] = { 50.0f, 10.0f, 30.0f, 10.0f, 5.0f, 70.0f };
    uint32_t out[4];
    CHECK(nearestLights(d, 6, 4, out) == 4);
    CHECK(out[0] == 4 && out[1] == 1 && out[2] == 3 && out[3] == 2);
    CHECK(nearestLights(d, 2, 4, out) == 2);
    CHECK(out[0] == 1 && out[1] == 0);
    CHECK(nearestLights(d, 0, 4, out) == 0);
}

static void testMenuSun() {
    float v[3] = { 9.0f, 9.0f, 9.0f };
    CHECK(parseVec3("0.3,0.85,-0.45", v));
    CHECK(near(v[0], 0.3f, 1e-6f) && near(v[1], 0.85f, 1e-6f) && near(v[2], -0.45f, 1e-6f));
    CHECK(!parseVec3("1,2", v));
    CHECK(!parseVec3(nullptr, v));
    CHECK(!parseVec3("", v));
    // Account menu (id 1): overlay screen 1 = mission select, 0 = hangar, whatever the sub-step (0 on first arrival and after backing out).
    CHECK(menuScene(0u, 0x801B3C90u, 1, 1) == MenuSceneMissionSelect);
    CHECK(menuScene(0u, 0x801B3C90u, 1, 0) == MenuSceneHangar);
    CHECK(menuScene(0x8025CAC0u, 0x801B3C90u, 1, 1) == MenuSceneNone);
    CHECK(menuScene(0u, 0u, 1, 1) == MenuSceneNone);
    CHECK(menuScene(0u, 0x12345678u, 1, 1) == MenuSceneNone);
    // Name entry (screen 2) and the main menu (id 0) show neither scene.
    CHECK(menuScene(0u, 0x801B3C90u, 1, 2) == MenuSceneNone);
    CHECK(menuScene(0u, 0x800A5FD4u, 0, 2) == MenuSceneNone);
    CHECK(menuScene(0u, 0x800A5FD4u, 0, 0) == MenuSceneNone);
    // Back from an aborted mission: menu overlay, no menu data, stale mission root; the screen alone picks the scene (3 = results).
    CHECK(menuScene(0x8025CAC0u, 0u, 0, 0, true) == MenuSceneHangar);
    CHECK(menuScene(0x8025CAC0u, 0u, 0, 1, true) == MenuSceneMissionSelect);
    CHECK(menuScene(0x8025CAC0u, 0u, 0, 3, true) == MenuSceneNone);
    CHECK(menuScene(0x8025CAC0u, 0u, 0, 0, false) == MenuSceneNone);
    CHECK(menuScene(0u, 0x800A5FD4u, 0, 0, true) == MenuSceneNone);
    // Boot sequence (N64 logo, attribution): no level light, no mission, no menu scene -> authored key light.
    CHECK(introScene(false, 0u, MenuSceneNone));
    CHECK(!introScene(true, 0u, MenuSceneNone));
    CHECK(!introScene(false, 0x8025CAC0u, MenuSceneNone));
    CHECK(!introScene(false, 0u, MenuSceneHangar));
    const float* h = hangarSunDir();
    CHECK(h[1] > 0.5f * std::sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]));
}

static void testCasterFragment() {
    CHECK(casterFragment(true, true, 4));
    CHECK(casterFragment(false, true, 30));
    CHECK(!casterFragment(false, true, 4));
    CHECK(!casterFragment(false, false, 30));
}

static void testLastPassPerTarget() {
    int a = 0, b = 0;
    const void* targets[4] = { &a, &a, &b, &a };
    bool on[4] = { true, true, true, false };
    lastPassPerTarget(targets, on, 4);
    CHECK(!on[0] && on[1] && on[2] && !on[3]);
    bool on2[2] = { true, false };
    const void* t2[2] = { &a, &a };
    lastPassPerTarget(t2, on2, 2);
    CHECK(on2[0] && !on2[1]);
}

static void testConsensusUp() {
    // Floor and platform (level) outvote one large tilted prop.
    const float ups[9] = { 0.0f, -0.96f, -0.28f, 0.0f, -0.89f, -0.457f, 0.0f, -0.96f, -0.28f };
    const uint32_t weights[3] = { 40, 84, 80 };
    float up[3];
    CHECK(consensusUp(ups, weights, 3, 0.9994f, up) && near(up[2], -0.28f, 1e-6f));
    CHECK(!consensusUp(ups, weights, 0, 0.9994f, up));
}

static void testDedupeLights() {
    Resolved l[4];
    for (Resolved& r : l) {
        r.color[0] = r.color[1] = r.color[2] = 1.0f;
        r.intensity = 0.8f;
    }
    l[1].pos[0] = 0.5f;
    l[1].intensity = 0.9f;
    l[2].pos[0] = 50.0f;
    l[3].color[0] = 0.5f;
    CHECK(dedupeLights(l, 4, 1.0f) == 3);
    CHECK(near(l[0].intensity, 0.9f, 1e-6f) && near(l[1].pos[0], 50.0f, 1e-6f) && near(l[2].color[0], 0.5f, 1e-6f));

    // An engine glow's sprite light on an exhaust light goes; a sprite inside a non-engine mesh light's reach (the hologram) stays.
    Resolved e[3];
    e[0].kind = KindMesh;
    e[0].engine = true;
    e[0].radius = 800.0f;
    e[1].kind = KindSprite;
    e[1].pos[0] = 300.0f;
    e[1].color[0] = 0.5f;
    e[2].kind = KindSprite;
    e[2].pos[0] = 500.0f;
    e[2].color[0] = 0.25f;
    CHECK(dedupeLights(e, 3, 1.0f) == 2);
    CHECK(near(e[1].pos[0], 500.0f, 1e-6f));
    Resolved h[2];
    h[0].kind = KindMesh;
    h[0].radius = 20000.0f;
    h[1].kind = KindSprite;
    h[1].pos[0] = 300.0f;
    h[1].color[0] = 0.5f;
    CHECK(dedupeLights(h, 2, 1.0f) == 2);
}

static void testConsensusCameraDelta() {
    // Two static objects agree on a 10-unit camera move; the moving ship (same weight as one of them) is outvoted.
    Mat4 cam = identity();
    cam.m[3][0] = 10.0f;
    Mat4 ship = identity();
    ship.m[3][1] = 40.0f;
    const Mat4 deltas[3] = { cam, cam, ship };
    const uint32_t weights[3] = { 50, 30, 50 };
    Mat4 out;
    CHECK(consensusCameraDelta(deltas, weights, 3, 0.002f, 2.0f, out) && near(out.m[3][0], 10.0f, 1e-6f) && near(out.m[3][1], 0.0f, 1e-6f));

    // No candidate carries half the weight: no history.
    const Mat4 split[2] = { cam, ship };
    const uint32_t even[2] = { 50, 50 };
    CHECK(consensusCameraDelta(split, even, 2, 0.002f, 2.0f, out));
    const uint32_t uneven[2] = { 40, 60 };
    CHECK(consensusCameraDelta(split, uneven, 2, 0.002f, 2.0f, out) && near(out.m[3][1], 40.0f, 1e-6f));
    const Mat4 three[3] = { cam, ship, identity() };
    const uint32_t thirds[3] = { 30, 30, 40 };
    CHECK(!consensusCameraDelta(three, thirds, 3, 0.002f, 2.0f, out));
}

static void testMenuHoloLight() {
    const float pos[3] = { 0.0f, 22.0f, 11068.0f };
    Candidate c;
    menuHoloLight(pos, 5000.0f, 2.5f, 1.0f, c);
    CHECK(c.kind == KindMesh && c.transformIndex == CameraSpace);
    CHECK(c.color[2] > c.color[0] && near(c.radius, 5000.0f, 1e-5f) && near(c.intensity, 2.5f, 1e-5f));
    CHECK(near(c.falloff, 1.0f, 1e-6f));
    // Other lights keep the squared falloff.
    Candidate d;
    CHECK(near(d.falloff, 2.0f, 1e-6f));
    Resolved r;
    CHECK(near(r.falloff, 2.0f, 1e-6f));
    CHECK(near(c.local[1], 22.0f, 1e-5f) && near(c.local[2], 11068.0f, 1e-5f));
}

static void testFlickerGain() {
    // No flicker is steady; flicker dims within [1 - flicker, 1], differs per seed, and moves smoothly (no jump within a step).
    CHECK(flickerGain(0.0f, 7, 1.234) == 1.0f);
    bool differs = false;
    for (int k = 0; k < 200; ++k) {
        const double t = k * 0.013;
        const float a = flickerGain(0.6f, 7, t), b = flickerGain(0.6f, 8, t);
        CHECK(a >= 0.4f - 1e-6f && a <= 1.0f + 1e-6f);
        CHECK(std::fabs(flickerGain(0.6f, 7, t + 0.001) - a) < 0.05f);
        differs = differs || (std::fabs(a - b) > 1e-3f);
    }
    CHECK(differs);
    CHECK(flickerGain(2.0f, 7, 0.5) >= -1e-6f);
}

static void testLampMatch() {
    // A lamp light keeps when a mark's reach (an ellipse, per-axis) covers it, and takes the nearest such mark; none covering drops it.
    const float marks[3][2] = { { 0.0f, 0.0f }, { 0.5f, 0.5f }, { 0.52f, 0.5f } };
    const float reach[6] = { 0.1f, 0.2f, 0.1f, 0.1f, 0.1f, 0.1f };
    size_t best = 99;
    const float onA[2] = { 0.05f, 0.15f };
    CHECK(lampMatch(onA, marks, reach, 3, best) && best == 0);
    const float wideX[2] = { 0.15f, 0.0f };
    CHECK(!lampMatch(wideX, marks, reach, 3, best));
    const float nearC[2] = { 0.53f, 0.5f };
    CHECK(lampMatch(nearC, marks, reach, 3, best) && best == 2);
    CHECK(!lampMatch(onA, marks, reach, 0, best));
}

static void testMeshKeyName() {
    // pl_crafts/rmobil's 7-node "vwing" keys as rmobil; the 3-node V-wing and every other object keep their names.
    CHECK(std::strcmp(meshKeyName("vwing", 7), "rmobil") == 0);
    CHECK(std::strcmp(meshKeyName("vwing", 3), "vwing") == 0);
    CHECK(std::strcmp(meshKeyName("xwing", 7), "xwing") == 0);
}

static void testLaserBolt() {
    CHECK(laserBolt("red_laser#s2.p0.0"));
    CHECK(laserBolt("green_laser#s0.p0.0"));
    CHECK(laserBolt("ion_laser#s0.p0.0"));
    CHECK(!laserBolt("red_laser_dst#s0.p0.0"));
    CHECK(!laserBolt("ph_torp#s0.p0.0"));
    CHECK(!laserBolt("xwing#s0.p0.0"));
    CHECK(!laserBolt(nullptr));
}

static void testSelectLights() {
    CHECK(selectLights(nullptr, 0, nullptr, MaxLights) == 0);
    std::vector<Resolved> in(40);
    for (size_t i = 0; i < in.size(); ++i) {
        in[i].pos[2] = 100.0f * (float)(in.size() - i);
        in[i].radius = 10.0f;
        in[i].intensity = 1.0f;
    }
    in[39].intensity = 0.0f;
    std::vector<Resolved> out(MaxLights);
    const size_t n = selectLights(in.data(), in.size(), out.data(), MaxLights);
    CHECK(n == MaxLights);
    CHECK(near(out[0].pos[2], 200.0f, 1e-5f));
    for (size_t i = 1; i < n; ++i) {
        CHECK(out[i].pos[2] >= out[i - 1].pos[2]);
    }
}

static void testFogFactor() {
    const Mat4 id = identity();
    const float mid[3] = { 0.0f, 0.0f, 0.5f };
    CHECK(near(fogFactor(id, mid, 256.0f, -64.0f), (0.5f * 256.0f - 64.0f) / 255.0f, 1e-5f));
    const float behind[3] = { 0.0f, 0.0f, -0.5f };
    CHECK(near(fogFactor(id, behind, 256.0f, 10.0f), 10.0f / 255.0f, 1e-5f));
    const float beyond[3] = { 0.0f, 0.0f, 2.0f };
    CHECK(fogFactor(id, beyond, 256.0f, -64.0f) == 1.0f);

    // Camera point: clip.w == 0 must give the RSP limit (offset only), never NaN.
    const Mat4 proj = perspective(1.0f, 1.0f, 10.0f, 10000.0f);
    const float eye[3] = { 0.0f, 0.0f, 0.0f };
    CHECK(fogFactor(proj, eye, 600.0f, -300.0f) == 0.0f);
    CHECK(near(fogFactor(proj, eye, 600.0f, 100.0f), 100.0f / 255.0f, 1e-5f));

    // Along the view axis: 0 before the fog starts, monotonic, 1 at the far plane.
    const float atNear[3] = { 0.0f, 0.0f, 10.0f };
    const float atFar[3] = { 0.0f, 0.0f, 10000.0f };
    CHECK(fogFactor(proj, atNear, 600.0f, -300.0f) == 0.0f);
    CHECK(fogFactor(proj, atFar, 600.0f, -300.0f) == 1.0f);
    float prev = 0.0f;
    for (float z = 10.0f; z <= 10000.0f; z += 37.0f) {
        const float q[3] = { 30.0f, -20.0f, z };
        const float f = fogFactor(proj, q, 600.0f, -300.0f);
        CHECK(std::isfinite(f) && (f >= prev - 1e-6f) && (f >= 0.0f) && (f <= 1.0f));
        prev = f;
    }
}

static void testFogSteps() {
    float t[12];
    fogSteps(12, 0.0f, t);
    for (uint32_t i = 0; i < 12; ++i) {
        CHECK(near(t[i], (i + 1) / 12.0f, 1e-6f));
    }
    fogSteps(12, 0.5f, t);
    CHECK(near(t[0], 0.5f / 12.0f, 1e-6f));
    CHECK(t[11] == 1.0f);
    for (uint32_t i = 0; i < 11; ++i) {
        CHECK(near(t[i], (i + 0.5f) / 12.0f, 1e-6f));
        CHECK((t[i] > 0.0f) && (t[i] < t[i + 1]));
    }
    float one[1];
    fogSteps(1, 0.7f, one);
    CHECK(one[0] == 1.0f);
}

static CasterMode opaqueMode() {
    CasterMode m{};
    m.zUpd = true;
    m.triangles = 4;
    return m;
}

static void testCasterClass() {
    CHECK(casterClass(opaqueMode()) == CasterOpaque);
    CasterMode m = opaqueMode();
    m.alphaCompare = true;
    CHECK(casterClass(m) == CasterCutout);
    m = opaqueMode();
    m.cvgXAlpha = true;
    CHECK(casterClass(m) == CasterCutout);
    // Anything today's caster test rejects stays out, cutout or not.
    bool CasterMode::*const off[] = { &CasterMode::primDepth, &CasterMode::xlu, &CasterMode::fillOrCopy, &CasterMode::extended };
    for (auto f : off) {
        m = opaqueMode();
        m.*f = true;
        CHECK(casterClass(m) == CasterNone);
        m.alphaCompare = true;
        CHECK(casterClass(m) == CasterNone);
    }
    m = opaqueMode();
    m.zUpd = false;
    m.alphaCompare = true;
    CHECK(casterClass(m) == CasterNone);
    m = opaqueMode();
    m.triangles = 0;
    CHECK(casterClass(m) == CasterNone);
}

static void testCutoutThreshold() {
    CHECK(cutoutThreshold(true, 0.25f) == 0.25f);
    CHECK(cutoutThreshold(false, 0.25f) == 0.5f);
}

static void testBuildCutoutScene() {
    const uint32_t faces[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    // Second range runs past the face buffer and is dropped, like gatherCasterIndices.
    const CutoutRange ranges[3] = { { { 3, 1 }, 7, 70 }, { { 9, 5 }, 8, 80 }, { { 6, 2 }, 9, 90 } };
    std::vector<uint32_t> idx;
    std::vector<CutoutEntry> table;
    CHECK(buildCutoutScene(faces, 12, ranges, 3, idx, table) == 9);
    CHECK(idx.size() == 9 && idx[0] == 3 && idx[3] == 6 && idx[8] == 11);
    CHECK(table.size() == 2);
    CHECK(table[0].firstTriangle == 0 && table[0].instanceIndex == 7 && table[0].tileIndex == 70);
    CHECK(table[1].firstTriangle == 1 && table[1].instanceIndex == 9 && table[1].tileIndex == 90);
    idx.clear();
    table.clear();
    CHECK(buildCutoutScene(faces, 12, nullptr, 0, idx, table) == 0);
    CHECK(idx.empty() && table.empty());
}

static void testCutoutDrawFor() {
    const CutoutEntry t[3] = { { 0, 1, 1, 0 }, { 4, 2, 2, 0 }, { 10, 3, 3, 0 } };
    CHECK(cutoutDrawFor(0, t, 3) == 0);
    CHECK(cutoutDrawFor(3, t, 3) == 0);
    CHECK(cutoutDrawFor(4, t, 3) == 1);
    CHECK(cutoutDrawFor(9, t, 3) == 1);
    CHECK(cutoutDrawFor(10, t, 3) == 2);
    CHECK(cutoutDrawFor(500, t, 3) == 2);
    CHECK(cutoutDrawFor(0, t, 0) == UINT32_MAX);
}

static void testCutoutDenied() {
    CHECK(cutoutDenied("i_ltsrc_hi#s0.p0.0"));
    CHECK(cutoutDenied("i_lnd_hi#s1.p2.3"));
    CHECK(!cutoutDenied("i_ltsrc_hi_x#s0.p0.0"));
    CHECK(!cutoutDenied("gt_strun#s0.p0.0"));
    CHECK(!cutoutDenied("dewback#s0.p0.0"));
    CHECK(!cutoutDenied(nullptr));
}

static void testCutoutMipGradient() {
    // TextureSampler's custom mip path: mip = 0.5*log2(|g*tcScale|^2) - 0.25 must land on level 0 (zero gradients gave log2(0) = NaN alpha).
    for (float tcScale : { 0.25f, 1.0f, 4.0f }) {
        const float g = cutoutMipGradient(tcScale) * tcScale;
        const float mip = 0.5f * std::log2(g * g) - 0.25f;
        CHECK(std::isfinite(mip) && (mip >= -1e-4f) && (mip < 0.05f));
    }
}

static void testCasterAfterDeny() {
    // Lasers: no class casts.
    CHECK(casterAfterDeny(CasterOpaque, true, false, true) == CasterNone);
    CHECK(casterAfterDeny(CasterCutout, true, false, true) == CasterNone);
    // Glow cards: only their cutout faces are dropped; an opaque part still casts.
    CHECK(casterAfterDeny(CasterOpaque, false, true, true) == CasterOpaque);
    CHECK(casterAfterDeny(CasterCutout, false, true, true) == CasterNone);
    // Cutouts off: cutouts never cast, opaque untouched.
    CHECK(casterAfterDeny(CasterCutout, false, false, false) == CasterNone);
    CHECK(casterAfterDeny(CasterOpaque, false, true, false) == CasterOpaque);
    CHECK(casterAfterDeny(CasterCutout, false, false, true) == CasterCutout);
}

static void testEmitterLookupEnabled() {
    Config c{};
    CHECK(!emitterLookupEnabled(c));
    c.fogShafts = true;
    CHECK(emitterLookupEnabled(c));
    c.fogShafts = false;
    c.shadows = true;
    CHECK(emitterLookupEnabled(c));
    c.shadows = false;
    c.enabled = true;
    c.lightShadows = false;
    CHECK(emitterLookupEnabled(c));
}

static void testFogVisT() {
    // The visibility ray starts short of the sample so the receiver itself never blocks the last step.
    CHECK(near(fogVisT(1.0f, 0.002f), 0.998f, 1e-6f));
    CHECK(near(fogVisT(0.5f, 0.002f), 0.499f, 1e-6f));
    CHECK(fogVisT(0.25f, 0.0f) == 0.25f);
    CHECK(fogVisT(1.0f, 0.002f) < 1.0f);
}

static void testFogSkyDepth() {
    // Sky pin 0x7FBE/0x7FBF (of 32767) and cleared depth are sky; anything nearer, including depth just above 0.998, is a receiver.
    const float sky = fogSkyDepth();
    CHECK(sky <= 0x7FBE / 32767.0f);
    CHECK(sky > 0x7FBD / 32767.0f);
    CHECK(0.9980f < sky);
}

static void testInterleavedNoise() {
    CHECK(interleavedNoise(10.5f, 20.5f) == interleavedNoise(10.5f, 20.5f));
    CHECK(interleavedNoise(10.5f, 20.5f) != interleavedNoise(11.5f, 20.5f));
    double sum = 0.0;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const float n = interleavedNoise(x + 0.5f, y + 0.5f);
            CHECK((n >= 0.0f) && (n < 1.0f));
            sum += n;
        }
    }
    const double mean = sum / (64.0 * 64.0);
    CHECK((mean > 0.45) && (mean < 0.55));
}

static void testLitInScatter() {
    const float fog[4] = { 0.1f, 0.3f, 0.6f, 0.8f };
    const float all[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    const float none[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const float half[4] = { 1.0f, 0.0f, 1.0f, 0.0f };
    CHECK(near(litInScatter(fog, all, 4), 0.8f, 1e-6f));
    CHECK(litInScatter(fog, none, 4) == 0.0f);
    CHECK(near(litInScatter(fog, half, 4), 0.4f, 1e-6f));
    const float noFog[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    CHECK(litInScatter(noFog, all, 4) == 0.0f);
    CHECK(litInScatter(fog, all, 0) == 0.0f);
    // Fog that drops along the ray (reverse fog) adds nothing, as in the shader; it never subtracts.
    const float reverse[4] = { 0.5f, 0.2f, 0.6f, 0.4f };
    CHECK(near(litInScatter(reverse, all, 4), 0.5f + 0.4f, 1e-6f));
}

static void testFogShaftsVisible() {
    const float black[3] = { 0.0f, 0.0f, 0.0f };
    const float dim[3] = { 0.001f, 0.002f, 0.0f };
    const float grey[3] = { 0.5f, 0.5f, 0.5f };
    CHECK(!fogShaftsVisible(black, 0.35f));
    CHECK(!fogShaftsVisible(dim, 0.35f));
    CHECK(!fogShaftsVisible(grey, 0.0f));
    CHECK(fogShaftsVisible(grey, 0.35f));
}

static void testConsiderFogCall() {
    const float grey[3] = { 0.5f, 0.5f, 0.5f };
    const float blue[3] = { 0.2f, 0.3f, 0.9f };
    FogParams best;
    CHECK(!best.valid);
    considerFogCall(best, 0, 100.0f, -50.0f, grey);
    CHECK(!best.valid);
    considerFogCall(best, 40, 100.0f, -50.0f, grey);
    CHECK(best.valid && (best.tris == 40) && (best.mul == 100.0f) && (best.offset == -50.0f));
    considerFogCall(best, 30, 200.0f, -10.0f, blue);
    CHECK(best.tris == 40);
    considerFogCall(best, 40, 200.0f, -10.0f, blue);
    CHECK(best.mul == 100.0f);
    considerFogCall(best, 90, 200.0f, -10.0f, blue);
    CHECK((best.tris == 90) && (best.offset == -10.0f) && (best.color[2] == 0.9f));
}

static void testWorkloadFog() {
    const float grey[3] = { 0.5f, 0.5f, 0.5f };
    FogCache cache;
    FogParams first;
    considerFogCall(first, 50, 100.0f, -50.0f, grey);
    CHECK(workloadFog(cache, 7, first).tris == 50);
    // Later pair of the same workload without a fogged main draw (radar fragment): reuse the pick.
    const FogParams empty;
    CHECK(workloadFog(cache, 7, empty).valid);
    CHECK(workloadFog(cache, 7, empty).tris == 50);
    FogParams bigger;
    considerFogCall(bigger, 80, 300.0f, -20.0f, grey);
    CHECK(workloadFog(cache, 7, bigger).mul == 300.0f);
    // New workload: the old pick never leaks.
    CHECK(!workloadFog(cache, 8, empty).valid);
}

static void testSunDiskDir() {
    const float sun[3] = { 0.30f, -0.85f, 0.43f };
    const float len = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
    const float s[3] = { sun[0] / len, sun[1] / len, sun[2] / len };
    const float tanA = std::tan(1.0f * 3.14159265f / 180.0f);
    float d[3];
    sunDiskDir(s, tanA, 0, 1, 0.37f, d);
    CHECK(near(d[0], s[0], 1e-6f) && near(d[1], s[1], 1e-6f) && near(d[2], s[2], 1e-6f));
    float prev[3] = {};
    for (uint32_t i = 0; i < 4; ++i) {
        sunDiskDir(s, tanA, i, 4, 0.25f, d);
        const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        CHECK(near(l, 1.0f, 1e-5f));
        const float cosToSun = d[0] * s[0] + d[1] * s[1] + d[2] * s[2];
        CHECK(cosToSun >= std::cos(std::atan(tanA)) - 1e-6f);
        CHECK(cosToSun < 1.0f - 1e-9f);
        if (i > 0) {
            CHECK((d[0] != prev[0]) || (d[1] != prev[1]) || (d[2] != prev[2]));
        }
        prev[0] = d[0];
        prev[1] = d[1];
        prev[2] = d[2];
    }
    float a[3], b[3];
    sunDiskDir(s, tanA, 1, 4, 0.10f, a);
    sunDiskDir(s, tanA, 1, 4, 0.10f, b);
    CHECK((a[0] == b[0]) && (a[1] == b[1]) && (a[2] == b[2]));
    sunDiskDir(s, tanA, 1, 4, 0.60f, b);
    CHECK((a[0] != b[0]) || (a[2] != b[2]));
    // A sun along +x takes the other tangent seed.
    const float sx[3] = { 1.0f, 0.0f, 0.0f };
    sunDiskDir(sx, tanA, 2, 4, 0.0f, d);
    CHECK(near(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), 1.0f, 1e-5f) && (d[0] < 1.0f));
}

static void testPenumbraPixels() {
    const float t = std::tan(1.0f * 3.14159265f / 180.0f);
    const float p1 = penumbraPixels(100.0f, t, 1000.0f, 500.0f, 64.0f);
    CHECK(p1 > 0.0f);
    CHECK(penumbraPixels(200.0f, t, 1000.0f, 500.0f, 64.0f) > p1);
    CHECK(penumbraPixels(100.0f, 2.0f * t, 1000.0f, 500.0f, 64.0f) > p1);
    CHECK(penumbraPixels(100.0f, t, 2000.0f, 500.0f, 64.0f) < p1);
    CHECK(penumbraPixels(1.0e6f, t, 10.0f, 500.0f, 8.0f) == 8.0f);
    CHECK(penumbraPixels(0.0f, t, 1000.0f, 500.0f, 64.0f) == 0.0f);
    CHECK(penumbraPixels(100.0f, t, 0.0f, 500.0f, 64.0f) == 0.0f);
}

static void testPixelsPerUnit() {
    // View rolled 90 degrees about z, times a perspective with y scale 2: the scale must not depend on the camera's orientation.
    Mat4 view = {};
    view.m[0][1] = 1.0f;
    view.m[1][0] = -1.0f;
    view.m[2][2] = 1.0f;
    view.m[3][3] = 1.0f;
    Mat4 proj = {};
    proj.m[0][0] = 1.5f;
    proj.m[1][1] = 2.0f;
    proj.m[2][2] = 1.0f;
    proj.m[2][3] = -1.0f;
    const Mat4 vp = mul(view, proj);
    CHECK(near(pixelsPerUnit(vp, 480.0f), 2.0f * 240.0f, 1e-3f));
    CHECK(near(pixelsPerUnit(proj, 480.0f), 2.0f * 240.0f, 1e-3f));
}

// Tatooine cell (41, 13) and its neighbours from the PJ64 golden rdram_tatooine_demo_f300; expected = the game's cell+0x54 colours (LOD 1: even samples).
static void testTerrainCellColorsGolden() {

    static const int8_t kHeights[9][25] = {
        { -26, -27, -28, -30, -32, -26, -27, -29, -32, -36, -28, -29, -31, -35, -40, -30, -32, -35, -39, -44, -33, -36, -40, -45, -52 },
        { -32, -36, -39, -44, -48, -36, -39, -44, -50, -55, -40, -43, -49, -56, -63, -44, -49, -56, -64, -72, -52, -59, -67, -77, -85 },
        { -48, -54, -60, -66, -71, -55, -61, -66, -72, -77, -63, -69, -75, -81, -85, -72, -80, -86, -92, -95, -85, -94, -99, -102, -104 },
        { -33, -36, -40, -45, -52, -38, -43, -48, -55, -63, -46, -53, -60, -67, -76, -56, -64, -72, -81, -91, -68, -77, -86, -95, -105 },
        { -52, -59, -67, -77, -85, -63, -72, -81, -91, -100, -76, -86, -97, -105, -112, -91, -101, -109, -116, -120, -105, -113, -119, -123, -125 },
        { -85, -94, -99, -102, -104, -100, -106, -110, -112, -111, -112, -116, -118, -119, -116, -120, -123, -124, -122, -118, -125, -127, -125, -123, -121 },
        { -68, -77, -86, -95, -105, -82, -92, -101, -109, -117, -98, -107, -114, -120, -124, -111, -117, -123, -126, -128, -121, -125, -127, -127, -127 },
        { -105, -113, -119, -123, -125, -117, -122, -126, -128, -127, -124, -127, -127, -127, -127, -128, -127, -127, -127, -127, -127, -127, -127, -127, -127 },
        { -125, -127, -125, -123, -121, -127, -126, -125, -125, -124, -127, -126, -126, -126, -126, -127, -127, -126, -126, -126, -127, -127, -127, -126, -126 }
    };
    static const int kSamples[9] = { 0, 2, 4, 10, 12, 14, 20, 22, 24 };
    static const uint8_t kExpected[9][3] = { { 255, 255, 236 }, { 255, 255, 235 }, { 255, 255, 235 }, { 255, 255, 234 }, { 255, 255, 234 }, { 255, 255, 236 }, { 255, 255, 235 }, { 255, 255, 235 }, { 255, 255, 215 } };

    TerrainMap m;

    m.width = 3;
    m.height = 3;
    m.heights.assign(kHeights[0], kHeights[0] + 9 * 25);

    TerrainShade s;
    const float ambient[3] = { 117.0f, 96.0f, 74.0f };
    const float sunRgb[3] = { 203.0f, 190.0f, 163.0f };
    const float sunDir[3] = { 0.3742697238922119f, 0.6237828731536865f, 0.6861611604690552f };

    std::copy(ambient, ambient + 3, s.ambient);
    std::copy(sunRgb, sunRgb + 3, s.sunRgb);
    std::copy(sunDir, sunDir + 3, s.sunDir);
    s.cellScale = 12.8f;

    uint8_t out[25 * 3];

    terrainCellColors(m, 1, 1, s, nullptr, 0, out);

    for (int i = 0; i < 9; ++i) {
        for (int c = 0; c < 3; ++c) {
            CHECK(out[kSamples[i] * 3 + c] == kExpected[i][c]);
        }
    }
}

// Lights add on top of the sun term: a point light only lights receivers it faces within its radius, a box light fades towards its edge.
static void testTerrainTextureAnims() {

    auto table = [](std::initializer_list<uint16_t> v) {

        std::vector<uint8_t> t;

        for (uint16_t x : v) {
            t.insert(t.end(), { uint8_t(x >> 8), uint8_t(x), 0, 0 });
        }

        return t;
    };

    // Ids 0-1 used by the map; id 2 is a plain entry no cell uses, so the blocks start at 3.
    const std::vector<uint8_t> t = table({ 0xC8, 0xC9, 0xCA, 0x0001, 3, 8, 0xC9, 0x10, 0x11, 0xFFFF });
    const std::vector<TerrainTexAnim> a = terrainTextureAnims(t.data(), uint32_t(t.size()), 0x200, 2);

    CHECK(a.size() == 1);
    CHECK(a[0].id == 1 && a[0].fps == 8);
    CHECK((a[0].banks == std::vector<uint32_t>{ 0xC9, 0x10, 0x11 }));

    const std::vector<uint8_t> plain = table({ 0xC8, 0xC9, 0xFFFF });

    CHECK(terrainTextureAnims(plain.data(), uint32_t(plain.size()), 0x200, 2).empty());
    CHECK(terrainTextureAnims(t.data(), uint32_t(t.size()), 0x11, 2).empty());
}

static void testTerrainVertexColorLights() {

    TerrainShade s;

    s.ambient[0] = s.ambient[1] = s.ambient[2] = 10.0f;

    const float up[3] = { 0.0f, 1.0f, 0.0f };
    const float p[3] = { 5.0f, 0.0f, 5.0f };
    TerrainLight point;

    point.rgb[0] = 100.0f;
    point.pos[0] = 5.0f;
    point.pos[1] = -1.0f;
    point.pos[2] = 5.0f;
    point.radius = 2.0f;

    uint8_t out[3];

    terrainVertexColor(s, up, p, &point, 1, out);
    CHECK(out[0] == 10 + 50);
    CHECK(out[1] == 10);

    point.pos[1] = 1.0f;
    terrainVertexColor(s, up, p, &point, 1, out);
    CHECK(out[0] == 10);

    TerrainLight box;

    box.box = true;
    box.rgb[2] = 100.0f;
    box.pos[0] = 5.0f;
    box.pos[2] = 6.0f;
    box.radius = 4.0f;
    terrainVertexColor(s, up, p, &box, 1, out);
    CHECK(out[2] == 10 + 86);

    box.pos[2] = 9.0f;
    terrainVertexColor(s, up, p, &box, 1, out);
    CHECK(out[2] == 10);
}

static void testBuildTerrainNormals() {
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    std::vector<float> nrm;
    int8_t flat[25] = {};
    terrainTileMesh(flat, 1, 0, 0, 1.0f, pos, idx);
    buildTerrainNormals(pos, idx, nrm);
    CHECK(nrm.size() == pos.size());
    for (size_t v = 0; v < nrm.size() / 3; ++v) {
        CHECK(near(nrm[v * 3 + 0], 0.0f, 1e-6f) && near(nrm[v * 3 + 1], 1.0f, 1e-6f) && near(nrm[v * 3 + 2], 0.0f, 1e-6f));
    }
    // Rising along +x: normals lean toward -x, stay up.
    int8_t ramp[25];
    for (int k = 0; k < 25; ++k) {
        ramp[k] = int8_t((k % 5) * 4);
    }
    pos.clear();
    idx.clear();
    terrainTileMesh(ramp, 1, 0, 0, 1.0f, pos, idx);
    buildTerrainNormals(pos, idx, nrm);
    CHECK((nrm[3 * 6 + 0] < -0.1f) && (nrm[3 * 6 + 1] > 0.5f));
    // A slope tile next to a flat tile: both copies of a seam vertex get the same welded normal.
    pos.clear();
    idx.clear();
    int8_t slope[25] = {};
    for (int r = 0; r < 5; ++r) {
        slope[r * 5 + 3] = 8;
    }
    terrainTileMesh(slope, 1, 0, 0, 1.0f, pos, idx);
    const size_t firstB = pos.size() / 3;
    terrainTileMesh(flat, 1, TerrainTileSpan, 0, 1.0f, pos, idx);
    buildTerrainNormals(pos, idx, nrm);
    // Tile A's vertex (row 2, col 4) and tile B's (row 2, col 0) share x = 512, z = 256.
    const size_t a = 2 * 5 + 4, b = firstB + 2 * 5 + 0;
    CHECK(pos[a * 3] == pos[b * 3] && pos[a * 3 + 2] == pos[b * 3 + 2]);
    CHECK((nrm[a * 3] == nrm[b * 3]) && (nrm[a * 3 + 1] == nrm[b * 3 + 1]) && (nrm[a * 3 + 2] == nrm[b * 3 + 2]));
    CHECK(nrm[a * 3] > 0.05f);
}

static void testCofactorNormal() {
    // Non-uniform scale x2: the transformed normal stays perpendicular to the transformed tangent.
    const float m[3][4] = { { 2.0f, 0.0f, 0.0f, 5.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f } };
    const float n[3] = { 0.70710678f, -0.70710678f, 0.0f };
    float o[3];
    cofactorNormal(m, n, o);
    CHECK(near(o[0] * 2.0f + o[1] * 1.0f, 0.0f, 1e-5f));
    CHECK(near(o[0] * o[0] + o[1] * o[1] + o[2] * o[2], 1.0f, 1e-5f));
    // Rotation 90 degrees about z: same as rotating the normal.
    const float r[3][4] = { { 0.0f, -1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f } };
    const float up[3] = { 0.0f, 1.0f, 0.0f };
    cofactorNormal(r, up, o);
    CHECK(near(o[0], -1.0f, 1e-6f) && near(o[1], 0.0f, 1e-6f));
}

static void testTerminatorShadow() {
    CHECK(terminatorShadow(0.0f, -0.3f, 0.25f) == 1.0f);
    CHECK(terminatorShadow(0.0f, 0.0f, 0.25f) == 1.0f);
    CHECK(terminatorShadow(1.0f, 0.8f, 0.25f) == 1.0f);
    CHECK(terminatorShadow(0.0f, 0.8f, 0.25f) == 0.0f);
    CHECK(near(terminatorShadow(0.0f, 0.1f, 0.25f), 0.6f, 1e-6f));
    CHECK(near(terminatorShadow(0.5f, 0.1f, 0.25f), 0.8f, 1e-6f));
    CHECK(terminatorShadow(0.0f, 0.05f, 0.25f) > terminatorShadow(0.0f, 0.15f, 0.25f));
}

static void testLevelSunWorld() {
    // The level light record's horizontal direction is reversed (Tatooine and level 6 baked shading); elevation is kept.
    const float rec[3] = { 0.3f, -0.8f, 0.5f };
    float w[3];
    levelSunWorld(rec, w);
    CHECK((w[0] == -0.3f) && (w[1] == -0.8f) && (w[2] == -0.5f));
}

static void testDepthSlope() {
    // Ground seen at a shallow angle: view distance grows 3% per pixel; both sides agree.
    CHECK(near(depthSlope(1000.0f, 970.0f, 1030.0f), 30.0f, 1e-4f));
    // Depth edge on one side (object in front): the smaller one-sided step wins.
    CHECK(near(depthSlope(1000.0f, 400.0f, 1030.0f), 30.0f, 1e-4f));
    CHECK(near(depthSlope(1000.0f, 970.0f, 5000.0f), 30.0f, 1e-4f));
    // Sky neighbour: ignore that side.
    CHECK(near(depthSlope(1000.0f, 1.0e30f, 1030.0f), 30.0f, 1e-4f));
    CHECK(depthSlope(1000.0f, 1.0e30f, 1.0e30f) == 0.0f);
    // Along the slope the sample matches the plane and keeps full weight; off-plane still drops.
    const float s = depthSlope(1000.0f, 970.0f, 1030.0f);
    CHECK(shadowDepthWeight(1000.0f + 5.0f * s, 1150.0f, 0.02f) == 1.0f);
    CHECK(shadowDepthWeight(1000.0f + 5.0f * s, 1000.0f, 0.02f) == 0.0f);
}

static void testBlurTapCount() {
    CHECK(blurTapCount(0.0f) == 1);
    CHECK(blurTapCount(4.0f) == 2);
    CHECK(blurTapCount(14.0f) == 7);
    CHECK(blurTapCount(32.0f) == 16);
    CHECK(blurTapCount(100.0f) == 16);
}

static void testAffine() {
    // Rotate 90 degrees about y and translate: inverse then compose gives identity; compose applies the right-hand transform first.
    const float m[3][4] = { { 0.0f, 0.0f, 1.0f, 10.0f }, { 0.0f, 2.0f, 0.0f, -5.0f }, { -1.0f, 0.0f, 0.0f, 3.0f } };
    float inv[3][4];
    CHECK(affineInverse(m, inv));
    float id[3][4];
    affineCompose(m, inv, id);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            CHECK(near(id[i][j], (i == j) ? 1.0f : 0.0f, 1e-5f));
        }
    }
    const float t[3][4] = { { 1.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f } };
    float c[3][4];
    affineCompose(m, t, c);
    // Point 0 -> t -> (1,0,0) -> m -> (10, -5, 2).
    CHECK(near(c[0][3], 10.0f, 1e-6f) && near(c[1][3], -5.0f, 1e-6f) && near(c[2][3], 2.0f, 1e-6f));
    const float sing[3][4] = {};
    CHECK(!affineInverse(sing, inv));
}

static void testFrustumExitT() {
    // Simple perspective along -z (row-vector): x' = x, y' = y, w' = -z.
    Mat4 sfv = {};
    sfv.m[0][0] = 1.0f;
    sfv.m[1][1] = 1.0f;
    sfv.m[2][3] = -1.0f;
    sfv.m[2][2] = 1.0f;
    // From (0,0,-10) moving +x: leaves the x = w plane at x = 10.
    const float o[3] = { 0.0f, 0.0f, -10.0f };
    const float dx[3] = { 1.0f, 0.0f, 0.0f };
    CHECK(near(frustumExitT(sfv, o, dx), 10.0f, 1e-4f));
    // Moving straight away (-z) never leaves the side planes.
    const float dz[3] = { 0.0f, 0.0f, -1.0f };
    CHECK(frustumExitT(sfv, o, dz) > 1.0e20f);
    // Moving toward the camera (+z): w reaches 0 at t = 10 first or the sides; exits by t = 10.
    const float dback[3] = { 0.0f, 0.0f, 1.0f };
    CHECK(frustumExitT(sfv, o, dback) <= 10.0f + 1e-4f);
    // Origin already outside: 0.
    const float out[3] = { 20.0f, 0.0f, -10.0f };
    CHECK(frustumExitT(sfv, out, dz) == 0.0f);
}

static void testRaySphereSpan() {
    const float o[3] = { 0.0f, 0.0f, 0.0f };
    const float d[3] = { 1.0f, 0.0f, 0.0f };
    const float c[3] = { 10.0f, 0.0f, 0.0f };
    float t0, t1;
    CHECK(raySphereSpan(o, d, c, 2.0f, t0, t1));
    CHECK(near(t0, 8.0f, 1e-5f) && near(t1, 12.0f, 1e-5f));
    const float miss[3] = { 10.0f, 5.0f, 0.0f };
    CHECK(!raySphereSpan(o, d, miss, 2.0f, t0, t1));
}

static void testTmemAverage() {
    std::vector<uint8_t> tmem(4096, 0);
    // CI4, palette 1: index 3 = opaque red (RGBA5551 0xF801), index 0 = transparent.
    const uint32_t pal = 0x800 + (1 * 16 + 3) * 8;
    tmem[pal] = 0xF8;
    tmem[pal + 1] = 0x01;
    // 8x2 texels at tmem word 0, line 1 word (8 bytes = 16 CI4 texels per row): left half index 3, right half index 0.
    for (int r = 0; r < 2; ++r) {
        for (int b = 0; b < 4; ++b) {
            tmem[r * 8 + b] = (b < 2) ? 0x33 : 0x00;
        }
    }
    float rgb[3];
    float cov = 0.0f;
    CHECK(tmemAverage(tmem.data(), 2, 0, 0, 1, 8, 2, 1, rgb, cov));
    CHECK(near(rgb[0], 1.0f, 1e-3f) && near(rgb[1], 0.0f, 1e-3f) && near(rgb[2], 0.0f, 1e-3f));
    CHECK(near(cov, 0.5f, 1e-3f));
    // RGBA16 solid opaque green 4x1.
    std::fill(tmem.begin(), tmem.end(), 0);
    for (int i = 0; i < 4; ++i) {
        tmem[i * 2] = 0x07;
        tmem[i * 2 + 1] = 0xC1;
    }
    CHECK(tmemAverage(tmem.data(), 0, 2, 0, 1, 4, 1, 0, rgb, cov));
    CHECK(near(rgb[1], 1.0f, 1e-3f) && near(rgb[0], 0.0f, 1e-3f) && near(cov, 1.0f, 1e-3f));
    // IA8: intensity 0xF, alpha 0x8 -> white at ~half coverage.
    std::fill(tmem.begin(), tmem.end(), 0);
    tmem[0] = 0xF8;
    CHECK(tmemAverage(tmem.data(), 3, 1, 0, 1, 1, 1, 0, rgb, cov));
    CHECK(near(rgb[0], 1.0f, 1e-3f) && near(cov, 8.0f / 15.0f, 1e-3f));
    // Fully transparent: no colour.
    std::fill(tmem.begin(), tmem.end(), 0);
    CHECK(!tmemAverage(tmem.data(), 0, 2, 0, 1, 4, 1, 0, rgb, cov));
    // RGBA32 is not handled.
    CHECK(!tmemAverage(tmem.data(), 0, 3, 0, 1, 4, 1, 0, rgb, cov));
}

static void testTransformScale() {
    Mat4 m = identity();
    CHECK(near(transformScale(m), 1.0f, 1e-6f));
    // Rotation about y with uniform scale 0.0125 (a craft model in camera space): the row length is the scale.
    const float c = std::cos(0.7f) * 0.0125f, s = std::sin(0.7f) * 0.0125f;
    m.m[0][0] = c;
    m.m[0][2] = -s;
    m.m[2][0] = s;
    m.m[2][2] = c;
    m.m[1][1] = 0.0125f;
    m.m[3][0] = 500.0f;
    CHECK(near(transformScale(m), 0.0125f, 1e-6f));
}

static void testCosineHemisphereDir() {
    const float n[3] = { 0.0f, 1.0f, 0.0f };
    float d[3];
    float prev[3] = {};
    for (uint32_t i = 0; i < 4; ++i) {
        cosineHemisphereDir(n, i, 4, 0.3f, d);
        CHECK(near(d[0] * d[0] + d[1] * d[1] + d[2] * d[2], 1.0f, 1e-5f));
        CHECK(d[1] > 0.0f);
        if (i > 0) {
            CHECK((d[0] != prev[0]) || (d[2] != prev[2]));
        }
        prev[0] = d[0];
        prev[1] = d[1];
        prev[2] = d[2];
    }
    float a[3], b[3];
    cosineHemisphereDir(n, 1, 4, 0.3f, a);
    cosineHemisphereDir(n, 1, 4, 0.7f, b);
    CHECK((a[0] != b[0]) || (a[2] != b[2]));
}

static void testAoWeightAndSchlick() {
    CHECK(aoWeight(0.0f, 10.0f) == 1.0f);
    CHECK(aoWeight(10.0f, 10.0f) == 0.0f);
    CHECK(near(aoWeight(2.5f, 10.0f), 0.75f, 1e-6f));
    CHECK(aoWeight(20.0f, 10.0f) == 0.0f);
    CHECK(near(schlick(1.0f, 0.04f), 0.04f, 1e-6f));
    CHECK(near(schlick(0.0f, 0.04f), 1.0f, 1e-6f));
    CHECK(schlick(0.3f, 0.04f) > schlick(0.8f, 0.04f));
}

static void testRoughReflectDir() {
    const float v[3] = { 0.0f, -0.70710678f, 0.70710678f };
    const float n[3] = { 0.0f, 1.0f, 0.0f };
    float r[3];
    roughReflectDir(v, n, 0.0f, 0.4f, r);
    CHECK(near(r[1], 0.70710678f, 1e-5f) && near(r[2], 0.70710678f, 1e-5f));
    const float tanCone = std::tan(20.0f * 3.14159265f / 180.0f);
    roughReflectDir(v, n, tanCone, 0.4f, r);
    const float c = r[1] * 0.70710678f + r[2] * 0.70710678f;
    CHECK(c >= std::cos(std::atan(tanCone)) - 1e-5f);
    CHECK(near(r[0] * r[0] + r[1] * r[1] + r[2] * r[2], 1.0f, 1e-5f));
}

static void testHitColorTable() {
    const HitColorEntry t[3] = { { 0, packHitColor(1.0f, 0.0f, 0.0f, false) }, { 10, packHitColor(0.0f, 1.0f, 0.0f, true) }, { 25, packHitColor(0.0f, 0.0f, 1.0f, false) } };
    CHECK(hitColorFor(0, t, 3) == 0);
    CHECK(hitColorFor(9, t, 3) == 0);
    CHECK(hitColorFor(10, t, 3) == 1);
    CHECK(hitColorFor(99, t, 3) == 2);
    CHECK(hitColorFor(0, t, 0) == SIZE_MAX);
    CHECK((t[1].rgba >> 24) == 255);
    CHECK((t[0].rgba >> 24) != 255);
    CHECK((t[0].rgba & 0xFF) == 255);
    // Flying craft (player, wingmen, enemy fighters) stay out of shadow history; the name is the part before '#'.
    CHECK(movingCraft("snowspeeder#s0.p3.0"));
    CHECK(movingCraft("xwing#s0.p1.2"));
    CHECK(movingCraft("tie_fighter#s1.p0.0"));
    CHECK(movingCraft("wmvwng#s0.p0.0") && movingCraft("wmxwng#s0.p2.1"));
    CHECK(!movingCraft("wmvwngx#s0.p0.0"));
    CHECK(!movingCraft("xwing_hangar#s0.p0.0"));
    CHECK(!movingCraft("platform#s0.p0.0"));
    CHECK(!movingCraft("snowspeeder"));
    CHECK(!movingCraft(nullptr));
    // Floor draws keep their colour and carry tag 253 (reflection rays skip them).
    const uint32_t f = markFloor(t[0].rgba);
    CHECK(((f >> 24) == 253) && ((f & 0xFFFFFF) == (t[0].rgba & 0xFFFFFF)));
    const float up[3] = { 0.0f, -1.0f, 0.0f };
    const float dish[3] = { 0.5f, -9.0f, 0.2f };
    const float hull[3] = { 6.0f, -1.0f, 3.0f };
    const float zero[3] = { 0.0f, 0.0f, 0.0f };
    CHECK(floorFacing(dish, up, 0.9f));
    CHECK(!floorFacing(hull, up, 0.9f));
    CHECK(!floorFacing(zero, up, 0.9f));
    // Either winding: the sum's sign depends on how the mesh was wound.
    const float flipped[3] = { 0.0f, 4.0f, 0.0f };
    CHECK(floorFacing(flipped, up, 0.9f));
    const float wall[3] = { 0.0f, 0.0f, 5.0f };
    CHECK(!floorFacing(wall, up, 0.9f));
}

static void testDrawAverageColor() {
    const float vtx[3] = { 0.5f, 0.5f, 0.5f };
    const float tex[3] = { 1.0f, 0.5f, 0.0f };
    const float white[3] = { 1.0f, 1.0f, 1.0f };
    const float black[3] = { 0.0f, 0.0f, 0.0f };
    float o[3];
    // Textured: the prelit vertex colour is baked shading, not albedo, so the texture alone carries the colour.
    drawAverageColor(vtx, tex, white, o);
    CHECK(near(o[0], 1.0f, 1e-6f) && near(o[1], 0.5f, 1e-6f) && (o[2] == 0.0f));
    const float darkVtx[3] = { 0.1f, 0.1f, 0.1f };
    drawAverageColor(darkVtx, tex, white, o);
    CHECK(near(o[0], 1.0f, 1e-6f));
    // Untextured: the vertex colour is the material colour.
    drawAverageColor(vtx, nullptr, white, o);
    CHECK(near(o[0], 0.5f, 1e-6f));
    drawAverageColor(vtx, tex, black, o);
    CHECK(near(o[0], 1.0f, 1e-6f));
    const float tint[3] = { 0.5f, 1.0f, 1.0f };
    drawAverageColor(vtx, tex, tint, o);
    CHECK(near(o[0], 0.5f, 1e-6f) && near(o[1], 0.5f, 1e-6f));
    CHECK(std::isfinite(o[0]) && std::isfinite(o[1]) && std::isfinite(o[2]));
}

static void testComposeFactor() {
    const float none[3] = { 0.0f, 0.0f, 0.0f };
    const float big[3] = { 5.0f, 0.2f, 0.0f };
    float f[4];
    composeFactor(1.0f, 0.0f, none, 0.45f, 0.5f, f);
    CHECK(near(f[3], 0.55f, 1e-6f) && (f[0] == 0.0f));
    composeFactor(0.0f, 1.0f, big, 0.45f, 0.5f, f);
    CHECK(near(f[3], 0.5f, 1e-6f));
    CHECK(near(f[0], 0.5f, 1e-6f) && near(f[1], 0.1f, 1e-6f));
    CHECK(f[0] <= 1.0f);
}

static void testPassGateAndReflectionReceiver() {
    CHECK(!tracePassWanted(false, false, false, false));
    CHECK(tracePassWanted(false, true, false, false));
    CHECK(tracePassWanted(false, false, false, true));
    const float floorSkip[4] = { 0.0f, 1.0f, 0.0f, 0.995f };
    const float up[3] = { 0.0f, 1.0f, 0.0f };
    const float wall[3] = { 1.0f, 0.0f, 0.0f };
    const float none[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    CHECK(reflectionReceiver(floorSkip, up));
    CHECK(!reflectionReceiver(floorSkip, wall));
    CHECK(!reflectionReceiver(none, up));
}

static void testShadowDepthWeight() {
    CHECK(shadowDepthWeight(1000.0f, 1000.0f, 0.02f) == 1.0f);
    CHECK(shadowDepthWeight(1000.0f, 1030.0f, 0.02f) == 0.0f);
    const float w1 = shadowDepthWeight(1000.0f, 1005.0f, 0.02f);
    const float w2 = shadowDepthWeight(1000.0f, 1010.0f, 0.02f);
    CHECK((w1 > w2) && (w2 > 0.0f) && (w1 < 1.0f));
    CHECK(shadowDepthWeight(1000.0f, 1.0e30f, 0.02f) == 0.0f);
    CHECK(shadowDepthWeight(0.0f, 0.0f, 0.02f) == 0.0f);
}

int main() {
    testInverse();
    testScreenRoundTrip();
    testSpriteLight();
    testSpriteLightRadiusCap();
    testSameMatrix();
    testSunCameraDir();
    testModelSunDir();
    testReceiverSkipped();
    testGatherCasterIndices();
    testFnv1a64();
    testTileCells();
    testTerrainTileMesh();
    testBuildTerrainMesh();
    testTerrainHitColors();
    testSolveTerrainOrigin();
    testTerrainInstanceTransform();
    testNoteTerrainTransform();
    testTerrainMapKey();
    testSolveTerrainOriginWithPrevious();
    testSceneCaptureEnabled();
    testSceneWanted();
    testLightRaySpan();
    testNearestLights();
    testMenuSun();
    testCasterFragment();
    testLastPassPerTarget();
    testConsensusUp();
    testConsensusCameraDelta();
    testDedupeLights();
    testMenuHoloLight();
    testFlickerGain();
    testLampMatch();
    testLaserBolt();
    testMeshKeyName();
    testSelectLights();
    testFogFactor();
    testFogSteps();
    testFogVisT();
    testEmitterLookupEnabled();
    testCasterClass();
    testCutoutThreshold();
    testBuildCutoutScene();
    testCutoutDrawFor();
    testCutoutDenied();
    testCutoutMipGradient();
    testCasterAfterDeny();
    testFogShaftsVisible();
    testFogSkyDepth();
    testInterleavedNoise();
    testLitInScatter();
    testConsiderFogCall();
    testWorkloadFog();
    testSunDiskDir();
    testPenumbraPixels();
    testShadowDepthWeight();
    testPixelsPerUnit();
    testTerrainCellColorsGolden();
    testTerrainVertexColorLights();
    testTerrainTextureAnims();
    testBuildTerrainNormals();
    testCofactorNormal();
    testTerminatorShadow();
    testLevelSunWorld();
    testSpriteShadowStart();
    testDepthSlope();
    testBlurTapCount();
    testAffine();
    testFrustumExitT();
    testRaySphereSpan();
    testTmemAverage();
    testTransformScale();
    testCosineHemisphereDir();
    testAoWeightAndSchlick();
    testRoughReflectDir();
    testHitColorTable();
    testDrawAverageColor();
    testComposeFactor();
    testPassGateAndReflectionReceiver();
    std::printf("rs64_lights_test: OK\n");
    return 0;
}
