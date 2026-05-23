/**
 * @file test_dashboard.c
 * @brief Unit tests for DashboardRuntime — slot management, clipboard
 *        mediation, link buffer, recovery mode, friend system, export,
 *        file dialog context, reconnect filter.
 *
 * Mocks out SDL, GPU, WASM, and yumi_client so the tests are headless.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>

/* ── Test harness ──────────────────────────────────────────────── */

static int g_run  = 0;
static int g_fail = 0;

#define T(cond, msg) do { g_run++; if (!(cond)) { g_fail++; \
    fprintf(stderr, "  FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); } } while(0)
#define SEC(name) fprintf(stdout, "── %s\n", name)

/* ── Minimal stubs for types the dashboard header requires ── */

/* Forward-declare opaque types that dashboard_runtime.h needs */
struct GpuContext { int dummy; };
struct wasm_engine_t_struct { int dummy; };

/* Include the real header for struct definitions */
#include "dashboard_runtime.h"

/* ── Helper: create a zeroed dashboard ── */

static DashboardRuntime make_dashboard(void) {
    DashboardRuntime d;
    memset(&d, 0, sizeof(d));
    /* Reserve modest starter capacity so tests that poke array members
     * directly (e.g. d.groups[0]) have backing storage.  The dashboard
     * imposes no hard cap — these arrays grow on demand in production. */
    if (!dashboard_reserve_slots  (&d, 8) ||
        !dashboard_reserve_groups (&d, 8) ||
        !dashboard_reserve_friends(&d, 16)) {
        fprintf(stderr, "make_dashboard: reserve failed\n");
        abort();
    }
    return d;
}

static void free_dashboard(DashboardRuntime *d) {
    free(d->slots);
    free(d->groups);
    free(d->friends);
    memset(d, 0, sizeof(*d));
}

/* ══════════════════════════════════════════════════════════════════
 *  Section 1: Slot management (no GPU needed)
 * ══════════════════════════════════════════════════════════════════ */

static void test_slot_limits(void) {
    SEC("1. Slot limits");
    DashboardRuntime d = make_dashboard();

    T(d.slot_count == 0, "initial slot_count is 0");
    T(d.focused_slot == 0, "initial focused_slot is 0");
    T(d.group_count == 0, "initial group_count is 0");
}

/* ══════════════════════════════════════════════════════════════════
 *  Section 2: Group state transitions
 * ══════════════════════════════════════════════════════════════════ */

static void test_group_state(void) {
    SEC("2. Group state transitions");
    DashboardRuntime d = make_dashboard();

    /* Add a group manually */
    d.group_count = 1;
    DashboardGroup *g = &d.groups[0];
    g->state = GROUP_STATE_DISCONNECTED;

    T(g->state == GROUP_STATE_DISCONNECTED, "initial state is DISCONNECTED");

    g->state = GROUP_STATE_CONNECTING;
    T(g->state == GROUP_STATE_CONNECTING, "can set CONNECTING");

    g->state = GROUP_STATE_CONNECTED;
    T(g->state == GROUP_STATE_CONNECTED, "can set CONNECTED");

    g->state = GROUP_STATE_REMOVED_KICKED;
    T(g->state == GROUP_STATE_REMOVED_KICKED, "can set REMOVED_KICKED");

    g->state = GROUP_STATE_REMOVED_BANNED;
    T(g->state == GROUP_STATE_REMOVED_BANNED, "can set REMOVED_BANNED");
}

/* ══════════════════════════════════════════════════════════════════
 *  Section 3: Link buffer
 * ══════════════════════════════════════════════════════════════════ */

static void test_link_buffer(void) {
    SEC("3. Link buffer");
    DashboardRuntime d = make_dashboard();
    d.group_count = 2;

    /* Initially empty */
    const void *data = NULL;
    uint32_t len = 0;
    T(!dashboard_get_group_link(&d, 0, &data, &len), "link buf initially empty");

    /* Set link */
    const char *link = "yumi://group/test123";
    dashboard_set_group_link(&d, 0, link, (uint32_t)strlen(link));

    T(dashboard_get_group_link(&d, 0, &data, &len), "link buf has data after set");
    T(len == strlen(link), "link buf length matches");
    T(memcmp(data, link, len) == 0, "link buf data matches");

    /* Other group unaffected */
    T(!dashboard_get_group_link(&d, 1, &data, &len), "group 1 link still empty");

    /* Out-of-range group */
    T(!dashboard_get_group_link(&d, 99, &data, &len), "out-of-range returns false");
}

/* ══════════════════════════════════════════════════════════════════
 *  Section 4: Friend system (in-memory only, no DuckDB)
 * ══════════════════════════════════════════════════════════════════ */

static void test_friend_system(void) {
    SEC("4. Friend system");
    DashboardRuntime d = make_dashboard();

    uint8_t peer_a[GR_PEER_ID_LEN] = {0};
    uint8_t peer_b[GR_PEER_ID_LEN] = {0};
    peer_a[0] = 0xAA;
    peer_b[0] = 0xBB;

    T(!dashboard_is_friend(&d, peer_a), "peer_a not friend initially");
    T(d.friend_count == 0, "friend_count starts at 0");

    /* Add friend */
    T(dashboard_confirm_add_friend(&d, peer_a, "Alice"), "add Alice as friend");
    T(d.friend_count == 1, "friend_count is 1");
    T(dashboard_is_friend(&d, peer_a), "peer_a is now friend");
    T(!dashboard_is_friend(&d, peer_b), "peer_b still not friend");

    /* Add duplicate — should succeed (already a friend) */
    T(dashboard_confirm_add_friend(&d, peer_a, "Alice"), "re-add is ok");
    T(d.friend_count == 1, "friend_count still 1 after re-add");

    /* Add second */
    T(dashboard_confirm_add_friend(&d, peer_b, "Bob"), "add Bob");
    T(d.friend_count == 2, "friend_count is 2");

    /* Remove */
    dashboard_remove_friend(&d, peer_a);
    T(d.friend_count == 1, "friend_count is 1 after remove");
    T(!dashboard_is_friend(&d, peer_a), "peer_a no longer friend");
    T(dashboard_is_friend(&d, peer_b), "peer_b still friend");

    /* Remove non-existent — no crash */
    dashboard_remove_friend(&d, peer_a);
    T(d.friend_count == 1, "removing non-friend is no-op");
}

/* ══════════════════════════════════════════════════════════════════
 *  Section 5: Recovery mode toggle
 * ══════════════════════════════════════════════════════════════════ */

static void test_recovery_mode(void) {
    SEC("5. Recovery mode");
    DashboardRuntime d = make_dashboard();
    d.group_count = 2;
    d.groups[0].state = GROUP_STATE_CONNECTED;
    d.groups[1].state = GROUP_STATE_REMOVED_KICKED;

    T(!d.recovery_mode, "recovery mode off initially");
    T(d.view == DASHBOARD_VIEW_NORMAL, "view is NORMAL");

    dashboard_toggle_recovery_mode(&d);
    T(d.recovery_mode, "recovery mode ON after toggle");
    T(d.view == DASHBOARD_VIEW_RECOVERY, "view is RECOVERY");
    T(d.groups[1].recovery_readonly, "kicked group marked readonly");

    dashboard_toggle_recovery_mode(&d);
    T(!d.recovery_mode, "recovery mode OFF after second toggle");
    T(d.view == DASHBOARD_VIEW_NORMAL, "view back to NORMAL");
}

/* ══════════════════════════════════════════════════════════════════
 *  Section 6: Reconnect filter (friend-based)
 * ══════════════════════════════════════════════════════════════════ */

static void test_reconnect_filter(void) {
    SEC("6. Reconnect filter");
    DashboardRuntime d = make_dashboard();
    d.group_count = 1;
    d.groups[0].member_count = 200;  /* > DASHBOARD_RECONNECT_FILTER_THRESHOLD */

    uint8_t friend_id[GR_PEER_ID_LEN] = {0};
    uint8_t stranger_id[GR_PEER_ID_LEN] = {0};
    friend_id[0] = 0x01;
    stranger_id[0] = 0x02;

    dashboard_confirm_add_friend(&d, friend_id, "MyFriend");

    /* Friend: should set pending IPC */
    d.pending_ipc.type = IPC_NONE;
    dashboard_handle_reconnect_request(&d, friend_id, 0);
    T(d.pending_ipc.type != IPC_NONE, "friend reconnect creates notification");

    /* Stranger in large group: should be filtered out */
    d.pending_ipc.type = IPC_NONE;
    dashboard_handle_reconnect_request(&d, stranger_id, 0);
    T(d.pending_ipc.type == IPC_NONE, "stranger reconnect filtered in large group");

    /* Small group: stranger should pass */
    d.groups[0].member_count = 10;
    d.pending_ipc.type = IPC_NONE;
    dashboard_handle_reconnect_request(&d, stranger_id, 0);
    T(d.pending_ipc.type != IPC_NONE, "stranger reconnect allowed in small group");
}

/* ══════════════════════════════════════════════════════════════════
 *  Section 7: IPC request types
 * ══════════════════════════════════════════════════════════════════ */

static void test_ipc_types(void) {
    SEC("7. IPC request types");

    T(IPC_NONE == 0, "IPC_NONE is 0");
    T(IPC_PASTE_PENDING != IPC_COPY_PENDING, "PASTE != COPY");
    T(IPC_FILE_OPEN_PENDING != IPC_FILE_SAVE_PENDING, "FILE_OPEN != FILE_SAVE");
    T(IPC_FOLDER_OPEN_PENDING != IPC_FRIEND_ADD_PENDING, "FOLDER != FRIEND_ADD");

    IPCRequest req;
    memset(&req, 0, sizeof(req));
    T(req.type == IPC_NONE, "zeroed IPC is NONE");
    T(req.slot_index == 0, "zeroed slot_index is 0");
    T(req.text_len == 0, "zeroed text_len is 0");
}

/* ══════════════════════════════════════════════════════════════════
 *  Section 8: Dashboard view enum
 * ══════════════════════════════════════════════════════════════════ */

static void test_view_enum(void) {
    SEC("8. Dashboard view enum");

    T(DASHBOARD_VIEW_NORMAL == 0, "NORMAL is 0");
    T(DASHBOARD_VIEW_SETTINGS == 1, "SETTINGS is 1");
    T(DASHBOARD_VIEW_RECOVERY == 2, "RECOVERY is 2");
}

/* ══════════════════════════════════════════════════════════════════
 *  Section 9: GroupState enum
 * ══════════════════════════════════════════════════════════════════ */

static void test_group_state_enum(void) {
    SEC("9. GroupState enum");

    T(GROUP_STATE_DISCONNECTED == 0, "DISCONNECTED is 0");
    T(GROUP_STATE_CONNECTING == 1, "CONNECTING is 1");
    T(GROUP_STATE_CONNECTED == 2, "CONNECTED is 2");
    T(GROUP_STATE_FAILED == 3, "FAILED is 3");
    T(GROUP_STATE_REMOVED_KICKED == 4, "REMOVED_KICKED is 4");
    T(GROUP_STATE_REMOVED_BANNED == 5, "REMOVED_BANNED is 5");
}

/* ══════════════════════════════════════════════════════════════════
 *  Section 10: Settings view toggle
 * ══════════════════════════════════════════════════════════════════ */

static void test_settings_view(void) {
    SEC("10. Settings view");
    DashboardRuntime d = make_dashboard();

    T(d.view == DASHBOARD_VIEW_NORMAL, "initial view is NORMAL");

    /* dashboard_view_settings sets the view but returns false
       because settings_db_active is false (no DB for test) */
    bool ok = dashboard_view_settings(&d);
    T(!ok, "view_settings returns false without DB");
    T(d.view == DASHBOARD_VIEW_SETTINGS, "view changed to SETTINGS");
}

/* ══════════════════════════════════════════════════════════════════
 *  Section 11: Delete removed group guard
 * ══════════════════════════════════════════════════════════════════ */

static void test_delete_removed_guard(void) {
    SEC("11. Delete removed group guard");
    DashboardRuntime d = make_dashboard();
    d.group_count = 2;
    d.groups[0].state = GROUP_STATE_CONNECTED;
    d.groups[1].state = GROUP_STATE_REMOVED_KICKED;

    /* Should NOT delete a connected group */
    dashboard_delete_removed_group(&d, 0);
    T(d.groups[0].state == GROUP_STATE_CONNECTED, "connected group not deleted");

    /* Should delete a kicked group */
    dashboard_delete_removed_group(&d, 1);
    T(d.groups[1].state == GROUP_STATE_DISCONNECTED, "kicked group deleted");
}

/* ══════════════════════════════════════════════════════════════════
 *  Section 12: Reconnect magic constant
 * ══════════════════════════════════════════════════════════════════ */

static void test_reconnect_constants(void) {
    SEC("12. Reconnect constants");

    T(DASHBOARD_RECONNECT_MAGIC == 0x59524E43, "YRNC magic correct");
    T(DASHBOARD_RECONNECT_PACKET_LEN == 16, "packet len is 16");
    T(DASHBOARD_RECONNECT_FILTER_THRESHOLD == 100, "filter threshold is 100");
}

/* ══════════════════════════════════════════════════════════════════
 *  Section 13: Friend capacity (unlimited — grows on demand)
 * ══════════════════════════════════════════════════════════════════ */

static void test_friend_capacity(void) {
    SEC("13. Friend capacity (dynamic)");
    DashboardRuntime d = make_dashboard();

    /* Add a large number of friends and verify the array grows. */
    const uint32_t N = 2048;
    for (uint32_t i = 0; i < N; i++) {
        uint8_t id[GR_PEER_ID_LEN] = {0};
        id[0] = (uint8_t)(i >> 8);
        id[1] = (uint8_t)(i & 0xFF);
        bool ok = dashboard_confirm_add_friend(&d, id, "peer");
        if (!ok) {
            T(false, "dashboard grows friend capacity on demand");
            free_dashboard(&d);
            return;
        }
    }
    T(d.friend_count == N, "friend_count tracks every add");
    T(d.friend_cap   >= N, "friend_cap grew to accommodate all adds");

    free_dashboard(&d);
}

/* ══════════════════════════════════════════════════════════════════
 *  Section 14: Link buffer overflow
 * ══════════════════════════════════════════════════════════════════ */

static void test_link_buffer_overflow(void) {
    SEC("14. Link buffer overflow");
    DashboardRuntime d = make_dashboard();
    d.group_count = 1;

    /* Write more than WEBAPP_LINK_BUF_SIZE */
    uint8_t big[WEBAPP_LINK_BUF_SIZE + 100];
    memset(big, 0xAB, sizeof(big));
    dashboard_set_group_link(&d, 0, big, sizeof(big));

    const void *data = NULL;
    uint32_t len = 0;
    T(dashboard_get_group_link(&d, 0, &data, &len), "link available");
    T(len == WEBAPP_LINK_BUF_SIZE, "clamped to WEBAPP_LINK_BUF_SIZE");
}

/* ══════════════════════════════════════════════════════════════════
 *  Main
 * ══════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("test_dashboard: %d sections\n", 14);

    test_slot_limits();
    test_group_state();
    test_link_buffer();
    test_friend_system();
    test_recovery_mode();
    test_reconnect_filter();
    test_ipc_types();
    test_view_enum();
    test_group_state_enum();
    test_settings_view();
    test_delete_removed_guard();
    test_reconnect_constants();
    test_friend_capacity();
    test_link_buffer_overflow();

    printf("\n%d/%d passed\n", g_run - g_fail, g_run);
    return g_fail ? 1 : 0;
}
