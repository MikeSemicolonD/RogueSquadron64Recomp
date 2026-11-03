#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

// The wire format shared by the session's messages (net/messages.cpp) and the relay server's protocol (relay-server/src/relay_protocol.cpp).
namespace rs64::net {

// Little-endian wire writer.
struct Writer {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { u8((uint8_t)v); u8((uint8_t)(v >> 8)); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) u8((uint8_t)(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) u8((uint8_t)(v >> (8 * i))); }
};

// Every read is bounds-checked; a short read clears ok and returns 0.
struct Reader {
    const uint8_t* p;
    size_t n;
    size_t at = 0;
    bool ok = true;

    bool need(size_t k)
    {

        if (!ok || n - at < k)
        {
            ok = false;
        }

        return ok;
    }

    uint8_t u8() { return need(1) ? p[at++] : 0; }

    uint16_t u16()
    {

        if (!need(2)) return 0;

        const uint16_t v = (uint16_t)(p[at] | (p[at + 1] << 8));

        at += 2;
        return v;
    }

    uint32_t u32()
    {

        if (!need(4)) return 0;

        uint32_t v = 0;

        for (int i = 3; i >= 0; --i) v = (v << 8) | p[at + i];

        at += 4;
        return v;
    }

    uint64_t u64()
    {

        if (!need(8)) return 0;

        uint64_t v = 0;

        for (int i = 7; i >= 0; --i) v = (v << 8) | p[at + i];

        at += 8;
        return v;
    }

    bool done() const { return ok && at == n; }
};

// Fields by type: integers as their bits, floats as their IEEE bits, fixed arrays element by element.
inline void put(Writer& w, uint8_t v) { w.u8(v); }
inline void put(Writer& w, uint16_t v) { w.u16(v); }
inline void put(Writer& w, uint32_t v) { w.u32(v); }
inline void put(Writer& w, int32_t v) { w.u32((uint32_t)v); }

inline void put(Writer& w, float v)
{

    uint32_t b;

    std::memcpy(&b, &v, 4);
    w.u32(b);
}

template <class T, size_t N>
void put(Writer& w, const T (&a)[N])
{
    for (const T& v : a) put(w, v);
}

inline void take(Reader& r, uint8_t* v) { *v = r.u8(); }
inline void take(Reader& r, uint16_t* v) { *v = r.u16(); }
inline void take(Reader& r, uint32_t* v) { *v = r.u32(); }
inline void take(Reader& r, int32_t* v) { *v = (int32_t)r.u32(); }

inline void take(Reader& r, float* v)
{

    const uint32_t b = r.u32();

    std::memcpy(v, &b, 4);
}

template <class T, size_t N>
void take(Reader& r, T (*a)[N])
{
    for (T& v : *a) take(r, &v);
}

template <class... F>
void put_all(Writer& w, const F&... fields)
{
    (put(w, fields), ...);
}

// The fields in order, straight into the outputs; the packet decoded when this returns true (the caller commits from temporaries).
template <class... F>
bool take_all(Reader& r, F*... out)
{

    (take(r, out), ...);
    return r.done();
}

// Scalar fields through temporaries: the outputs change only when the packet decoded to its last byte.
template <class... F>
bool take_all_committed(Reader& r, F*... out)
{

    std::tuple<F...> v;

    if (!std::apply([&r](F&... x) { return take_all(r, &x...); }, v))
    {
        return false;
    }

    std::apply([&](const F&... x) { ((*out = x), ...); }, v);
    return true;
}

// A string after its length (a Len), cut to max_len on the way out and refused past it on the way in.
template <class Len>
void put_string(Writer& w, const std::string& s, size_t max_len)
{

    const size_t len = s.size() < max_len ? s.size() : max_len;

    put(w, (Len)len);
    w.b.insert(w.b.end(), s.begin(), s.begin() + len);
}

template <class Len>
bool take_string(Reader& r, std::string* out, size_t max_len)
{

    Len len = 0;

    take(r, &len);

    if (!r.ok || len > max_len || !r.need(len))
    {
        return false;
    }

    out->assign((const char*)r.p + r.at, len);
    r.at += len;
    return true;
}

}
