#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace rs64::menu {

// A button's `index`: 0-based, negative counts from the end, clamped to the list.
inline int resolve_index(int index, int count)
{

    if (count <= 0) return 0;

    const int i = index < 0 ? count + index : index;

    return std::clamp(i, 0, count - 1);
}

template <class T>
void move_to_index(std::vector<T>& v, size_t from, int index)
{

    if (from >= v.size()) return;

    const int to = resolve_index(index, (int)v.size());
    T item = std::move(v[from]);

    v.erase(v.begin() + from);
    v.insert(v.begin() + to, std::move(item));
}

} // namespace rs64::menu
