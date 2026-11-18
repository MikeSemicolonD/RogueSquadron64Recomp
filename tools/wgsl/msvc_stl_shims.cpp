// shortcut: the Dawn release libraries come from a newer MSVC STL that calls these vectorized helpers, which the 14.44 runtime lacks; drop this file once Tint is built from source with the project's toolset.
// Each forwards to the standard algorithm, which the 14.44 headers implement without the helpers.

#include <algorithm>
#include <cstdint>

extern "C" {

    const void* __stdcall __std_min_element_4u(const void* first, const void* last) noexcept
    {
        return std::min_element(static_cast<const uint32_t*>(first), static_cast<const uint32_t*>(last));
    }

    const void* __stdcall __std_max_element_4u(const void* first, const void* last) noexcept
    {
        return std::max_element(static_cast<const uint32_t*>(first), static_cast<const uint32_t*>(last));
    }

    const void* __stdcall __std_min_element_8u(const void* first, const void* last) noexcept
    {
        return std::min_element(static_cast<const uint64_t*>(first), static_cast<const uint64_t*>(last));
    }

    void* __stdcall __std_unique_4(void* first, void* last) noexcept
    {
        return std::unique(static_cast<uint32_t*>(first), static_cast<uint32_t*>(last));
    }

    void* __stdcall __std_unique_8(void* first, void* last) noexcept
    {
        return std::unique(static_cast<uint64_t*>(first), static_cast<uint64_t*>(last));
    }

    void __stdcall __std_rotate(void* first, void* mid, void* last) noexcept
    {
        std::rotate(static_cast<unsigned char*>(first), static_cast<unsigned char*>(mid), static_cast<unsigned char*>(last));
    }

}
