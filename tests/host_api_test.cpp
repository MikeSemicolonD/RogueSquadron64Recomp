#include "host_api.h"
#include "check.h"
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace rs64::host;

// Frozen v1 table: fields are only appended, so these offsets must never change.
struct rs64_host_api_v1 {
    uint32_t version;
    uint32_t size;
    void (*log)(const char* line);
    int (*add_hook)(uint32_t hook, rs64_hook_fn fn, void* user);
    int (*add_text_source)(const char* name, rs64_text_fn fn, void* user);
    int (*add_input_filter)(rs64_input_fn fn, void* user);
    void (*set_flag)(const char* name, int value);
    int (*get_flag)(const char* name);
    uint32_t (*call)(uint8_t* rdram, void* ctx, uint32_t vram, const uint32_t* args, uint32_t nargs, const uint32_t* stack, uint32_t nstack, float f12, float f14, float* f0_out);
    uint32_t (*alloc)(uint8_t* rdram, uint32_t size);
    const char* (*state_id)(void);
    int (*in_cutscene)(void);
};
#define V1_SAME(f) static_assert(offsetof(rs64_host_api_v1, f) == offsetof(rs64_host_api, f), "v1 field moved: " #f)
V1_SAME(version);
V1_SAME(size);
V1_SAME(log);
V1_SAME(add_hook);
V1_SAME(add_text_source);
V1_SAME(add_input_filter);
V1_SAME(set_flag);
V1_SAME(get_flag);
V1_SAME(call);
V1_SAME(alloc);
V1_SAME(state_id);
V1_SAME(in_cutscene);
static_assert(offsetof(rs64_host_api, add_action) == sizeof(rs64_host_api_v1), "v2 fields must start right after the v1 table");
static_assert(offsetof(rs64_host_api, add_service) == offsetof(rs64_host_api, base_path) + sizeof(void*), "v3 fields must start right after the v2 table");

static std::vector<int>* g_order;
static int h_a(uint32_t, uint8_t*, void*, void*) { g_order->push_back(1); return RS64_HOOK_CONTINUE; }
static int h_b(uint32_t, uint8_t*, void*, void*) { g_order->push_back(2); return RS64_HOOK_RETURN; }
static int h_c(uint32_t, uint8_t*, void*, void*) { g_order->push_back(3); return RS64_HOOK_CONTINUE; }

static void test_dispatch_order_and_return() {
    reset_for_tests();
    std::vector<int> order; g_order = &order;
    CHECK(dispatch(5, nullptr, nullptr) == RS64_HOOK_CONTINUE);
    add_hook(5, h_a, nullptr);
    add_hook(5, h_b, nullptr);
    add_hook(5, h_c, nullptr);
    CHECK(dispatch(5, nullptr, nullptr) == RS64_HOOK_RETURN);
    CHECK((order == std::vector<int>{1, 2}));
    CHECK(add_hook(0, h_a, nullptr) != 0 && add_hook(5, nullptr, nullptr) != 0);
}

static const char* t_one(void*) { return "ONE"; }
static const char* t_two(void*) { return "TWO"; }
static void test_text_source_first_wins() {
    reset_for_tests();
    CHECK(text_source("x") == nullptr);
    CHECK(add_text_source("x", t_one, nullptr) == 0);
    CHECK(add_text_source("x", t_two, nullptr) != 0);
    CHECK(std::strcmp(text_source("x"), "ONE") == 0);
}

static void f_start(int, uint16_t* b, float*, float*, void*) { *b |= 0x1000; }
static void f_a(int, uint16_t* b, float* x, float*, void*) { if (*b & 0x1000) *x = 0.5f; }
static void test_input_filters_chain() {
    reset_for_tests();
    uint16_t b = 0; float x = 0, y = 0;
    add_input_filter(f_start, nullptr);
    add_input_filter(f_a, nullptr);
    run_input_filters(0, &b, &x, &y);
    CHECK(b == 0x1000 && x == 0.5f);
}

static void test_flags() {
    reset_for_tests();
    CHECK(flag("hangar_launch") == 0);
    set_flag("hangar_launch", 1);
    CHECK(flag("hangar_launch") == 1);
}

static int g_act = 0;
static void a_set(void* u) { g_act = (int)(intptr_t)u; }
static void test_actions_first_wins() {
    reset_for_tests();
    g_act = 0;
    CHECK(!run_action("mp_x"));
    CHECK(add_action("mp_x", a_set, (void*)1) == 0);
    CHECK(add_action("mp_x", a_set, (void*)2) != 0);
    CHECK(run_action("mp_x") && g_act == 1);
    CHECK(add_action(nullptr, a_set, nullptr) != 0 && add_action("y", nullptr, nullptr) != 0);
}

static int c_yes(void*) { return 1; }
static int c_no(void*) { return 0; }
static void test_conditions() {
    reset_for_tests();
    CHECK(condition("mp_ready") == -1);
    CHECK(add_condition("mp_ready", c_yes, nullptr) == 0);
    CHECK(add_condition("mp_ready", c_no, nullptr) != 0);
    CHECK(condition("mp_ready") == 1);
    CHECK(add_condition("off", c_no, nullptr) == 0 && condition("off") == 0);
}

static std::string g_typed;
static int k_pass(const char*, int, void*) { return 0; }
static int k_take(const char* t, int key, void*) {
    g_typed += t ? std::string(t) : std::string(1, (char)key);
    return 1;
}
static int k_unreached(const char*, int, void*) {
    CHECK(false);
    return 1;
}
static void test_key_handlers_stop_at_first_taker() {
    reset_for_tests();
    g_typed.clear();
    CHECK(run_key_handlers("a", 0) == 0);
    add_key_handler(k_pass, nullptr);
    add_key_handler(k_take, nullptr);
    add_key_handler(k_unreached, nullptr);
    CHECK(run_key_handlers("1", 0) == 1 && run_key_handlers(nullptr, '\b') == 1);
    CHECK(g_typed == "1\b");
}

static std::vector<int>* g_quits;
static void q_push(void* u) { g_quits->push_back((int)(intptr_t)u); }
static void test_quit_handlers_in_order() {
    reset_for_tests();
    std::vector<int> q;
    g_quits = &q;
    add_quit_handler(q_push, (void*)1);
    add_quit_handler(q_push, (void*)2);
    run_quit_handlers();
    CHECK((q == std::vector<int>{1, 2}));
}

static void test_api_table() {
    reset_for_tests();
    const rs64_host_api* a = api();
    CHECK(a->version == RS64_HOST_API_VERSION && a->size == sizeof(rs64_host_api));
    CHECK(a->add_hook == add_hook && a->add_text_source == add_text_source);
    CHECK(a->version == 3 && RS64_HOST_API_VERSION == 3);
    CHECK(a->add_service == add_service && a->get_service == get_service);
    CHECK(a->add_action == add_action && a->add_condition == add_condition && a->add_key_handler == add_key_handler && a->add_quit_handler == add_quit_handler);
    CHECK(a->cutscene_skips() == 0u && a->screen_keyboard_shown() == -1 && a->any_key_held() == 0 && std::strcmp(a->base_path(), "") == 0);
    a->yield(nullptr, nullptr);
    a->request_quit();
    a->text_input(1);
}

static void test_seal_refuses_registration() {
    reset_for_tests();
    CHECK(add_hook(5, h_a, nullptr) == 0);
    seal();
    CHECK(add_hook(5, h_a, nullptr) != 0);
    CHECK(add_text_source("x", t_one, nullptr) != 0);
    CHECK(add_input_filter(f_start, nullptr) != 0);
    CHECK(add_action("a", a_set, nullptr) != 0 && add_condition("c", c_yes, nullptr) != 0);
    CHECK(add_key_handler(k_take, nullptr) != 0 && add_quit_handler(q_push, nullptr) != 0);
    reset_for_tests();
    CHECK(add_hook(5, h_a, nullptr) == 0);
}

static int reentrant_hook(uint32_t hook, uint8_t*, void*, void*) {
    return add_hook(hook, h_c, nullptr) == 0 ? RS64_HOOK_CONTINUE : RS64_HOOK_RETURN;
}

static void test_seal_reentrancy_safe() {
    reset_for_tests();
    add_hook(5, reentrant_hook, nullptr);
    std::vector<int> order; g_order = &order;
    int res = dispatch(5, nullptr, nullptr);
    CHECK(res == RS64_HOOK_CONTINUE && order.size() == 0);
}

static void test_mod_init_version_gate() {
    CHECK(init_version() == RS64_HOST_API_VERSION);
    CHECK(accept_mod_init(0, 0) && accept_mod_init(0, RS64_HOST_API_VERSION));
    CHECK(accept_mod_init(0, 1) && accept_mod_init(0, 2) && accept_mod_init(0, 3) && !accept_mod_init(0, 4));
    CHECK(!accept_mod_init(0, RS64_HOST_API_VERSION + 1) && !accept_mod_init(0, 0xFFFFFFFFu));
    CHECK(!accept_mod_init(1, 1) && !accept_mod_init(0xFFFFFFFFu, 0));
}

static void test_rollback_drops_refused_registrations() {
    reset_for_tests();
    add_hook(5, h_a, nullptr);
    add_text_source("keep", t_one, nullptr);
    add_input_filter(f_start, nullptr);
    add_action("keep", a_set, (void*)1);
    const Mark m = mark();
    add_hook(5, h_c, nullptr);
    add_hook(7, h_c, nullptr);
    add_text_source("drop", t_two, nullptr);
    add_input_filter(f_a, nullptr);
    add_action("drop", a_set, nullptr);
    add_condition("drop", c_yes, nullptr);
    add_key_handler(k_take, nullptr);
    add_quit_handler(q_push, nullptr);
    static const int svc = 0;
    add_service("drop", &svc);
    rollback(m);
    CHECK(get_service("drop") == nullptr);
    CHECK(!run_action("drop") && run_action("keep") && condition("drop") == -1 && run_key_handlers("x", 0) == 0);
    std::vector<int> fresh;
    g_quits = &fresh;
    run_quit_handlers();
    CHECK(fresh.empty());
    std::vector<int> order; g_order = &order;
    dispatch(5, nullptr, nullptr);
    dispatch(7, nullptr, nullptr);
    CHECK((order == std::vector<int>{1}));
    CHECK(text_source("drop") == nullptr && std::strcmp(text_source("keep"), "ONE") == 0);
    uint16_t b = 0; float x = 0, y = 0;
    run_input_filters(0, &b, &x, &y);
    CHECK(b == 0x1000 && x == 0.0f);
}

// Every `rs64_hook(N, ...)` in rogue_squadron.toml is preceded by a `# hook: NAME` comment that must equal hook_name(N).
static void test_toml_hook_ids_match_names() {
    std::ifstream f(RS64_TOML_PATH);
    CHECK(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string t = ss.str();
    std::regex re(R"(# hook: (RS64_HOOK_\w+)[\s\S]*?rs64_hook\((\d+)u?,)");
    int n = 0;
    std::set<uint32_t> seen;
    for (auto it = std::sregex_iterator(t.begin(), t.end(), re); it != std::sregex_iterator(); ++it, ++n) {
        const uint32_t id = (uint32_t)std::stoul((*it)[2]);
        seen.insert(id);
        if ((*it)[1].str() != hook_name(id)) {
            printf("toml hook %u is named %s, expected %s\n", id, (*it)[1].str().c_str(), hook_name(id));
            CHECK(false);
        }
    }
    std::set<uint32_t> expected;
    for (uint32_t i = 1; i < RS64_HOOK_COUNT; ++i) {
        if (hook_has_toml_site(i)) expected.insert(i);
    }
    CHECK(n == (int)expected.size());
    CHECK(seen == expected);
    int sites = 0;
    std::regex site(R"(rs64_hook\((\d+))");
    for (auto it = std::sregex_iterator(t.begin(), t.end(), site); it != std::sregex_iterator(); ++it, ++sites) {
        const uint32_t id = (uint32_t)std::stoul((*it)[1]);
        CHECK(hook_has_toml_site(id));
    }
    CHECK(sites == (int)expected.size());
}

struct CallRec {
    uint32_t vram = 0, nargs = 0, nstack = 0, args[4] = {}, stack[4] = {};
    float f12 = 0, f14 = 0;
};
static CallRec g_rec;
static uint32_t fake_call(uint8_t*, void*, uint32_t vram, const uint32_t* a, uint32_t n, const uint32_t* st, uint32_t ns, float f12, float f14, float* f0) {
    g_rec = CallRec{};
    g_rec.vram = vram; g_rec.nargs = n; g_rec.nstack = ns; g_rec.f12 = f12; g_rec.f14 = f14;
    for (uint32_t i = 0; i < n && i < 4; ++i) g_rec.args[i] = a[i];
    for (uint32_t i = 0; i < ns && i < 4; ++i) g_rec.stack[i] = st[i];
    if (f0) *f0 = 2.5f;
    return 0xBEEF;
}

static void test_call_marshals_stack_and_f14() {
    reset_for_tests();
    CHECK(api()->call(nullptr, nullptr, 0x80000000u, nullptr, 0, nullptr, 0, 0, 0, nullptr) == 0u);
    Services s;
    s.call = fake_call;
    bind_services(s);
    const uint32_t a[2] = {7, 8}, st[3] = {11, 12, 13};
    float f0 = 0;
    CHECK(api()->call(nullptr, nullptr, 0x800B4588u, a, 2, st, 3, 1.5f, -2.0f, &f0) == 0xBEEFu);
    CHECK(g_rec.vram == 0x800B4588u && g_rec.nargs == 2 && g_rec.nstack == 3);
    CHECK(g_rec.args[1] == 8 && g_rec.stack[2] == 13 && g_rec.f12 == 1.5f && g_rec.f14 == -2.0f && f0 == 2.5f);
    bind_services(Services{});
}

static void test_services() {
    reset_for_tests();
    static const int first = 1, second = 2;
    CHECK(get_service("mp.transport") == nullptr);
    CHECK(add_service("mp.transport", &first) == 0);
    CHECK(add_service("mp.transport", &second) != 0);
    CHECK(add_service(nullptr, &first) != 0 && add_service("x", nullptr) != 0);
    seal();
    CHECK(add_service("late", &second) != 0);
    CHECK(get_service("mp.transport") == &first && get_service("late") == nullptr && get_service(nullptr) == nullptr);
    CHECK(api()->get_service("mp.transport") == &first);
    reset_for_tests();
    CHECK(get_service("mp.transport") == nullptr);
}

int main() {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    test_dispatch_order_and_return();
    test_text_source_first_wins();
    test_actions_first_wins();
    test_conditions();
    test_key_handlers_stop_at_first_taker();
    test_quit_handlers_in_order();
    test_input_filters_chain();
    test_flags();
    test_api_table();
    test_seal_refuses_registration();
    test_seal_reentrancy_safe();
    test_toml_hook_ids_match_names();
    test_mod_init_version_gate();
    test_call_marshals_stack_and_f14();
    test_rollback_drops_refused_registrations();
    test_services();
    printf("host_api_test OK\n");
    return 0;
}
