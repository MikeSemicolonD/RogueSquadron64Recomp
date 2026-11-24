#include "plume_webgpu_as.h"
#include "check.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace plume::webgpu_as;

static bool near(float a, float b, float eps)
{
    return std::fabs(a - b) <= eps;
}

// A median-split BLAS in the packed layout, built on the CPU: stands in for the GPU builder until Task 3 checks that one the same way.
struct CpuBuilder {
    const std::vector<float> &pos;
    const std::vector<uint32_t> &idx;
    std::vector<uint32_t> order;
    std::vector<Node> nodes;
    std::vector<Tri> tris;

    CpuBuilder(const std::vector<float> &p, const std::vector<uint32_t> &i) : pos(p), idx(i) {}

    void bounds(uint32_t t, float lo[3], float hi[3]) const
    {

        for (int a = 0; a < 3; ++a) {
            lo[a] = 1e30f;
            hi[a] = -1e30f;
        }

        for (int k = 0; k < 3; ++k) {
            for (int a = 0; a < 3; ++a) {
                const float v = pos[idx[t * 3 + k] * 3 + a];
                lo[a] = std::min(lo[a], v);
                hi[a] = std::max(hi[a], v);
            }
        }
    }

    // Returns the child word for the range [b, e).
    uint32_t build(uint32_t b, uint32_t e, float lo[3], float hi[3])
    {

        if (e - b == 1)
        {
            const uint32_t t = order[b];
            Tri tri = {};

            for (int a = 0; a < 3; ++a) {
                tri.v0[a] = pos[idx[t * 3 + 0] * 3 + a];
                tri.v1[a] = pos[idx[t * 3 + 1] * 3 + a];
                tri.v2[a] = pos[idx[t * 3 + 2] * 3 + a];
            }

            tri.primitiveIndex = t;
            bounds(t, lo, hi);
            tris.push_back(tri);
            return kLeafBit | uint32_t(tris.size() - 1);
        }

        float clo[3] = { 1e30f, 1e30f, 1e30f };
        float chi[3] = { -1e30f, -1e30f, -1e30f };

        for (uint32_t i = b; i < e; ++i) {
            float l[3], h[3];
            bounds(order[i], l, h);

            for (int a = 0; a < 3; ++a) {
                clo[a] = std::min(clo[a], (l[a] + h[a]) * 0.5f);
                chi[a] = std::max(chi[a], (l[a] + h[a]) * 0.5f);
            }
        }

        int axis = 0;

        for (int a = 1; a < 3; ++a) {
            if ((chi[a] - clo[a]) > (chi[axis] - clo[axis])) axis = a;
        }

        const uint32_t m = (b + e) / 2;
        std::nth_element(order.begin() + b, order.begin() + m, order.begin() + e, [&](uint32_t x, uint32_t y) {
            float lx[3], hx[3], ly[3], hy[3];
            bounds(x, lx, hx);
            bounds(y, ly, hy);
            return (lx[axis] + hx[axis]) < (ly[axis] + hy[axis]);
        });

        const uint32_t self = uint32_t(nodes.size());
        nodes.push_back(Node{});
        float llo[3], lhi[3], rlo[3], rhi[3];
        const uint32_t left = build(b, m, llo, lhi);
        const uint32_t right = build(m, e, rlo, rhi);
        Node &n = nodes[self];

        for (int a = 0; a < 3; ++a) {
            n.lmin[a] = llo[a];
            n.lmax[a] = lhi[a];
            n.rmin[a] = rlo[a];
            n.rmax[a] = rhi[a];
            lo[a] = std::min(llo[a], rlo[a]);
            hi[a] = std::max(lhi[a], rhi[a]);
        }

        n.left = left;
        n.right = right;
        return self;
    }

    std::vector<uint8_t> pack(bool opaque)
    {

        const uint32_t n = uint32_t(idx.size() / 3);

        order.resize(n);

        for (uint32_t i = 0; i < n; ++i) order[i] = i;

        if (n > 0) {
            float lo[3], hi[3];
            build(0, n, lo, hi);
        }

        std::vector<uint8_t> out(blasSize(n), 0);
        BlasHeader h = { n, uint32_t(nodes.size()), opaque ? 1u : 0u, 0 };

        std::memcpy(out.data(), &h, sizeof(h));
        std::memcpy(out.data() + sizeof(BlasHeader), nodes.data(), nodes.size() * sizeof(Node));
        std::memcpy(out.data() + blasTriOffset(n), tris.data(), tris.size() * sizeof(Tri));
        return out;
    }
};

static void random_tris(std::mt19937 &rng, uint32_t n, std::vector<float> &pos, std::vector<uint32_t> &idx)
{

    std::uniform_real_distribution<float> c(-50.0f, 50.0f);
    std::uniform_real_distribution<float> d(-3.0f, 3.0f);

    pos.clear();
    idx.clear();

    for (uint32_t t = 0; t < n; ++t) {
        const float cx = c(rng), cy = c(rng), cz = c(rng);

        for (int k = 0; k < 3; ++k) {
            idx.push_back(uint32_t(pos.size() / 3));
            pos.push_back(cx + d(rng));
            pos.push_back(cy + d(rng));
            pos.push_back(cz + d(rng));
        }
    }
}

int main()
{

    // sizes
    CHECK(blasSize(0) == 256);
    CHECK(blasSize(1) == 256);
    CHECK((blasSize(2000) % 256) == 0);
    CHECK(blasSize(2000) >= 16 + 64ull * 1999 + 48ull * 2000);
    CHECK(tlasSize(9) == 768);
    CHECK(blasScratchSize(0) > 0);
    CHECK(blasScratchSize(5000) >= 2ull * 4 * 8192);

    // inverse_round_trip
    {
        const float c = std::cos(0.7f);
        const float s = std::sin(0.7f);
        const float m[3][4] = { { c, 0, s, 3 }, { 0, 1, 0, -2 }, { -s, 0, c, 5 } };
        float inv[3][4];

        CHECK(affineInverse(m, inv));

        for (int r = 0; r < 3; ++r) {
            for (int col = 0; col < 4; ++col) {
                float v = (col == 3) ? m[r][3] : 0.0f;

                for (int k = 0; k < 3; ++k) {
                    v += m[r][k] * ((col == 3) ? inv[k][3] : inv[k][col]);
                }

                CHECK(near(v, (col == r) ? 1.0f : 0.0f, 1e-5f));
            }
        }

        const float singular[3][4] = { { 1, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 1, 0 } };
        CHECK(!affineInverse(singular, inv));
    }

    // pack_instances
    {
        InstanceIn in = {};
        in.objectToWorld[0][0] = 1;
        in.objectToWorld[1][1] = 1;
        in.objectToWorld[2][2] = 1;
        in.objectToWorld[0][3] = 10;
        in.mask = 0x08;
        in.instanceID = 4;
        in.cullDisable = true;
        in.blasOffset = 4096;
        const std::vector<uint8_t> packed = packInstances(&in, 1);
        CHECK(packed.size() == sizeof(TlasHeader) + sizeof(Instance));
        TlasHeader h;
        Instance inst;
        std::memcpy(&h, packed.data(), sizeof(h));
        std::memcpy(&inst, packed.data() + sizeof(h), sizeof(inst));
        CHECK(h.instanceCount == 1);
        CHECK(near(inst.worldToObject[0][3], -10.0f, 1e-6f));
        CHECK(inst.mask == 0x08 && inst.instanceID == 4 && inst.flags == 1u && inst.blasOffset == 4096);
    }

    // reference_hits
    {
        const std::vector<float> pos = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
        const std::vector<uint32_t> idx = { 0, 1, 2 };
        const float o1[3] = { 0.25f, 0.25f, -1.0f };
        const float d1[3] = { 0, 0, 1 };
        const float o2[3] = { 0.25f, 0.25f, 1.0f };
        const float d2[3] = { 0, 0, -1 };
        RefHit a, b;
        CHECK(traceReference(pos, idx, o1, d1, 0.0f, 100.0f, false, &a));
        CHECK(near(a.t, 1.0f, 1e-6f) && near(a.u, 0.25f, 1e-6f) && near(a.v, 0.25f, 1e-6f) && a.prim == 0);
        CHECK(traceReference(pos, idx, o2, d2, 0.0f, 100.0f, false, &b));
        CHECK(a.frontFace != b.frontFace);
        const bool aCulled = !traceReference(pos, idx, o1, d1, 0.0f, 100.0f, true, &a);
        const bool bCulled = !traceReference(pos, idx, o2, d2, 0.0f, 100.0f, true, &b);
        CHECK(aCulled != bCulled);
    }

    // reference_bvh_matches_bruteforce
    {
        std::mt19937 rng(7);
        std::vector<float> pos;
        std::vector<uint32_t> idx;
        random_tris(rng, 500, pos, idx);
        CpuBuilder builder(pos, idx);
        const std::vector<uint8_t> blas = builder.pack(true);
        std::uniform_real_distribution<float> o(-60.0f, 60.0f);
        std::uniform_real_distribution<float> d(-1.0f, 1.0f);
        uint32_t hits = 0;

        for (int r = 0; r < 2000; ++r) {
            const float org[3] = { o(rng), o(rng), o(rng) };
            const float dir[3] = { d(rng), d(rng), d(rng) };
            RefHit want, got;
            const bool w = traceReference(pos, idx, org, dir, 0.0f, 1e30f, false, &want);
            const bool g = traceBvh(blas.data(), org, dir, 0.0f, 1e30f, false, &got);
            CHECK(w == g);

            if (w) {
                ++hits;
                CHECK(near(want.t, got.t, 1e-4f * std::max(1.0f, want.t)) && (want.prim == got.prim) && (want.frontFace == got.frontFace));
            }
        }

        CHECK(hits > 100);

        // An empty and a one-triangle BLAS.
        const std::vector<float> none;
        const std::vector<uint32_t> noIdx;
        CpuBuilder empty(none, noIdx);
        const std::vector<uint8_t> emptyBlas = empty.pack(true);
        const float org[3] = { 0, 0, -1 };
        const float dir[3] = { 0, 0, 1 };
        RefHit h;
        CHECK(!traceBvh(emptyBlas.data(), org, dir, 0.0f, 1e30f, false, &h));
        const std::vector<float> one = { -1, -1, 0, 1, -1, 0, 0, 1, 0 };
        const std::vector<uint32_t> oneIdx = { 0, 1, 2 };
        CpuBuilder single(one, oneIdx);
        const std::vector<uint8_t> singleBlas = single.pack(true);
        CHECK(traceBvh(singleBlas.data(), org, dir, 0.0f, 1e30f, false, &h) && near(h.t, 1.0f, 1e-6f) && h.prim == 0);
    }

    std::printf("webgpu_as_test OK\n");
    return 0;
}
