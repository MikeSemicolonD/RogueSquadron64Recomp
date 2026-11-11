#include "../lib/rt64/src/hle/rt64_rs64_vr.h"
#include "check.h"
#include <cmath>
#include <cstdio>

using namespace rs64vr;
using rs64lights::Mat4;

static bool near(float a, float b, float tol) {
    return std::fabs(a - b) <= tol * std::max(1.0f, std::fabs(b));
}

static bool matrixNear(const Mat4& a, const Mat4& b, float tol) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (!near(a.m[r][c], b.m[r][c], tol)) {
                return false;
            }
        }
    }
    return true;
}

// Game projection as setupCameraMatrices builds it: forward is +Z in camera space, w = z.
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

static void testIdentityPose() {
    const Pose pose{ 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f };
    CHECK(matrixNear(eyeView(pose, 50.0f), rs64lights::identity(), 1e-6f));
}

static void testEyeOffsetScales() {
    const Pose rightEye{ 0.0f, 0.0f, 0.0f, 1.0f, 0.032f, 0.0f, 0.0f };
    const float origin[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float out[4];
    rs64lights::transform(origin, eyeView(rightEye, 50.0f), out);
    CHECK(near(out[0], -1.6f, 1e-5f));
    CHECK(near(out[1], 0.0f, 1e-5f));
    CHECK(near(out[2], 0.0f, 1e-5f));
}

static void testYaw90() {
    CHECK(matrixNear(yawView(0.0f), rs64lights::identity(), 1e-6f));
    const float ahead[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    float out[4];
    rs64lights::transform(ahead, yawView(90.0f), out);

    // Turning left puts what was ahead on the right (game x points right).
    CHECK(near(out[0], 1.0f, 1e-5f));
    CHECK(std::fabs(out[2]) < 1e-5f);
}

static void testHeadView() {
    CHECK(matrixNear(headView(35.0f, 0.0f), yawView(35.0f), 1e-6f));
    const float ahead[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    float out[4];
    rs64lights::transform(ahead, headView(0.0f, 90.0f), out);

    // Looking up puts what was ahead below (game y points down).
    CHECK(near(out[1], 1.0f, 1e-5f));
    CHECK(std::fabs(out[2]) < 1e-5f);
}

static void testSymmetricFovMatchesGame() {
    const float fovY = 0.9f;
    const float aspect = 4.0f / 3.0f;
    const Mat4 game = perspective(fovY, aspect, 10.0f, 20000.0f);
    const float a = std::atan(aspect * std::tan(fovY * 0.5f));
    const float b = fovY * 0.5f;
    CHECK(matrixNear(eyeProjection(game, { -a, a, b, -b }), game, 1e-5f));
}

static void testAsymmetricFovKeepsDepth() {
    const Mat4 game = perspective(0.9f, 4.0f / 3.0f, 10.0f, 20000.0f);
    const Fov fov{ -0.9f, 0.7f, 0.8f, -0.85f };
    const Mat4 eye = eyeProjection(game, fov);
    for (int r = 0; r < 4; ++r) {
        CHECK(eye.m[r][2] == game.m[r][2]);
        CHECK(eye.m[r][3] == game.m[r][3]);
    }

    const float leftEdge[4] = { std::tan(fov.left), 0.0f, 1.0f, 1.0f };
    float clip[4];
    rs64lights::transform(leftEdge, eye, clip);
    CHECK(near(clip[0] / clip[3], -1.0f, 1e-5f));

    const float topEdge[4] = { 0.0f, std::tan(fov.up), 1.0f, 1.0f };
    rs64lights::transform(topEdge, eye, clip);
    CHECK(near(clip[1] / clip[3], 1.0f, 1e-5f));
}

int main() {
    testIdentityPose();
    testEyeOffsetScales();
    testYaw90();
    testHeadView();
    testSymmetricFovMatchesGame();
    testAsymmetricFovKeepsDepth();
    std::printf("rs64_vr_test: ok\n");
    return 0;
}
