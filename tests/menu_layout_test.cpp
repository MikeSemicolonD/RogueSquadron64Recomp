#include "game/menu_layout.h"
#include "check.h"
#include <cstdio>
#include <vector>

int main() {

    // resolve_index_clamps_and_counts_from_end
    CHECK(rs64::menu::resolve_index(0, 5) == 0);
    CHECK(rs64::menu::resolve_index(-1, 5) == 4);
    CHECK(rs64::menu::resolve_index(9, 5) == 4);
    CHECK(rs64::menu::resolve_index(-20, 5) == 0);

    // move_to_index_reorders_in_place
    std::vector<int> v{10, 20, 30, 40};

    rs64::menu::move_to_index(v, 3, 1);
    CHECK((v == std::vector<int>{10, 40, 20, 30}));
    rs64::menu::move_to_index(v, 0, -1);
    CHECK((v == std::vector<int>{40, 20, 30, 10}));
    rs64::menu::move_to_index(v, 2, 99);
    CHECK((v == std::vector<int>{40, 20, 10, 30}));

    std::printf("menu_layout_test: ok\n");

    return 0;
}
