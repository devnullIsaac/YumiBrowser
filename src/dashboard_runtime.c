/*
 * dashboard_runtime.c - Dashboard supervisor implementation: window ownership, webapp slot management, clipboard mediation, group lifecycle, recovery, export.
 * Copyright (C) 2026 DevNullIsaac
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file dashboard_runtime.c
 * @brief Dashboard supervisor runtime — owns the window, manages
 *        webapp slots, mediates clipboard, handles group lifecycle,
 *        recovery mode, friend list, settings, and export.
 */

#include "dashboard_runtime.h"
#include "webapp_runtime.h"
#include "static_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <dirent.h>
#include <lzma.h>

/* ------------------------------------------------------------------ */
/*  XDG path helpers                                                   */
/* ------------------------------------------------------------------ */

#define APP_ID "com.yumi.browser"

static bool mkdirs(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

static const char *get_data_dir(void) {
    static char buf[4096];
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0])
        snprintf(buf, sizeof(buf), "%s/" APP_ID, xdg);
    else {
        const char *home = getenv("HOME");
        if (!home) home = "/tmp";
        snprintf(buf, sizeof(buf), "%s/.local/share/" APP_ID, home);
    }
    mkdirs(buf);
    return buf;
}

/* ------------------------------------------------------------------ */
/*  Dynamic-storage reserve helpers                                    */
/* ------------------------------------------------------------------ */
/*
 *  Slots, groups and friends live in heap-allocated arrays that grow
 *  on demand.  The dashboard imposes no upper bound — capacity is
 *  bounded only by available memory.
 */

static bool reserve_generic(void **arr, uint32_t *cap, uint32_t want,
                            size_t elem_size, uint32_t initial) {
    if (want <= *cap) return true;
    uint32_t new_cap = *cap ? *cap : initial;
    while (new_cap < want) {
        uint32_t next = new_cap * 2;
        if (next <= new_cap) return false;  /* overflow */
        new_cap = next;
    }
    void *p = realloc(*arr, (size_t)new_cap * elem_size);
    if (!p) return false;
    /* Zero the newly-added tail so callers can rely on .rt == NULL etc. */
    memset((uint8_t *)p + (size_t)(*cap) * elem_size, 0,
           (size_t)(new_cap - *cap) * elem_size);
    *arr = p;
    *cap = new_cap;
    return true;
}

bool dashboard_reserve_slots(DashboardRuntime *d, uint32_t want) {
    return reserve_generic((void **)&d->slots, &d->slot_cap, want,
                           sizeof(WebAppSlot), DASHBOARD_INITIAL_SLOT_CAP);
}

bool dashboard_reserve_groups(DashboardRuntime *d, uint32_t want) {
    return reserve_generic((void **)&d->groups, &d->group_cap, want,
                           sizeof(DashboardGroup), DASHBOARD_INITIAL_GROUP_CAP);
}

bool dashboard_reserve_friends(DashboardRuntime *d, uint32_t want) {
    return reserve_generic((void **)&d->friends, &d->friend_cap, want,
                           sizeof(DashboardFriend), DASHBOARD_INITIAL_FRIEND_CAP);
}

/* ------------------------------------------------------------------ */
/*  Settings DuckDB (privileged)                                       */
/* ------------------------------------------------------------------ */

static bool settings_db_init(DashboardRuntime *d, const char *path) {
    duckdb_config config;
    if (duckdb_create_config(&config) != DuckDBSuccess) return false;
    /* Settings DB is privileged — external access allowed for export */
    duckdb_set_config(config, "threads", "2");
    duckdb_set_config(config, "memory_limit", "128MB");
    char *err = NULL;
    if (duckdb_open_ext(path, &d->settings_db, config, &err) != DuckDBSuccess) {
        fprintf(stderr, "[dashboard] Settings DB open failed: %s\n", err ? err : "?");
        duckdb_free(err); duckdb_destroy_config(&config); return false;
    }
    duckdb_destroy_config(&config);
    if (duckdb_connect(d->settings_db, &d->settings_conn) != DuckDBSuccess) {
        duckdb_close(&d->settings_db); return false;
    }

    /* Create tables if they don't exist */
    duckdb_result r;
    duckdb_query(d->settings_conn,
        "CREATE TABLE IF NOT EXISTS settings ("
        "  group_id BLOB, key TEXT, value TEXT,"
        "  PRIMARY KEY (group_id, key)"
        ");", &r);
    duckdb_destroy_result(&r);

    duckdb_query(d->settings_conn,
        "CREATE TABLE IF NOT EXISTS global_settings ("
        "  key TEXT PRIMARY KEY, value TEXT"
        ");", &r);
    duckdb_destroy_result(&r);

    duckdb_query(d->settings_conn,
        "CREATE TABLE IF NOT EXISTS friends ("
        "  peer_id BLOB PRIMARY KEY,"
        "  display_name TEXT,"
        "  added_at BIGINT"
        ");", &r);
    duckdb_destroy_result(&r);

    d->settings_db_active = true;
    printf("[dashboard] Settings DB ready: %s\n", path);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Friend list — load from DB                                         */
/* ------------------------------------------------------------------ */

static void load_friends(DashboardRuntime *d) {
    d->friend_count = 0;
    if (!d->settings_db_active) return;

    duckdb_result r;
    if (duckdb_query(d->settings_conn,
            "SELECT peer_id, display_name, added_at FROM friends "
            "ORDER BY added_at;", &r) != DuckDBSuccess) {
        duckdb_destroy_result(&r); return;
    }

    /* Stream chunks (non-deprecated API).  Capacity grows on demand —
     * the friend list has no fixed upper bound. */
    duckdb_data_chunk chunk;
    while ((chunk = duckdb_fetch_chunk(r)) != NULL) {
        idx_t rows = duckdb_data_chunk_get_size(chunk);
        duckdb_vector v_peer = duckdb_data_chunk_get_vector(chunk, 0);
        duckdb_vector v_name = duckdb_data_chunk_get_vector(chunk, 1);
        duckdb_vector v_added = duckdb_data_chunk_get_vector(chunk, 2);

        duckdb_string_t *peers = (duckdb_string_t *)duckdb_vector_get_data(v_peer);
        duckdb_string_t *names = (duckdb_string_t *)duckdb_vector_get_data(v_name);
        int64_t         *added = (int64_t *)       duckdb_vector_get_data(v_added);
        uint64_t *valid_peer = duckdb_vector_get_validity(v_peer);
        uint64_t *valid_name = duckdb_vector_get_validity(v_name);

        if (!dashboard_reserve_friends(d, d->friend_count + (uint32_t)rows)) {
            duckdb_destroy_data_chunk(&chunk);
            break;
        }

        for (idx_t i = 0; i < rows; i++) {
            DashboardFriend *f = &d->friends[d->friend_count];
            memset(f, 0, sizeof(*f));

            if (!valid_peer || duckdb_validity_row_is_valid(valid_peer, i)) {
                uint32_t blen = duckdb_string_t_length(peers[i]);
                const char *bdata = duckdb_string_t_data(&peers[i]);
                if (blen >= GR_PEER_ID_LEN)
                    memcpy(f->peer_id, bdata, GR_PEER_ID_LEN);
            }
            if (!valid_name || duckdb_validity_row_is_valid(valid_name, i)) {
                uint32_t nlen = duckdb_string_t_length(names[i]);
                const char *ndata = duckdb_string_t_data(&names[i]);
                size_t copy = nlen < sizeof(f->display_name) - 1
                                ? nlen : sizeof(f->display_name) - 1;
                memcpy(f->display_name, ndata, copy);
                f->display_name[copy] = '\0';
            } else {
                f->display_name[0] = '\0';
            }
            f->added_at = added[i];
            d->friend_count++;
        }
        duckdb_destroy_data_chunk(&chunk);
    }
    duckdb_destroy_result(&r);
    printf("[dashboard] Loaded %u friends\n", d->friend_count);
}

/* ------------------------------------------------------------------ */
/*  UTF-8 → codepoint (for text input dispatch)                        */
/* ------------------------------------------------------------------ */

static uint32_t utf8_next(const char **p) {
    const uint8_t *s = (const uint8_t *)*p;
    uint32_t cp; int extra;
    if      (s[0] < 0x80) { cp = s[0]; extra = 0; }
    else if (s[0] < 0xC0) { (*p)++; return 0xFFFD; }
    else if (s[0] < 0xE0) { cp = s[0] & 0x1F; extra = 1; }
    else if (s[0] < 0xF0) { cp = s[0] & 0x0F; extra = 2; }
    else if (s[0] < 0xF8) { cp = s[0] & 0x07; extra = 3; }
    else                   { (*p)++; return 0xFFFD; }
    for (int i = 0; i < extra; i++) {
        if ((s[1+i] & 0xC0) != 0x80) { *p = (const char *)(s+1+i); return 0xFFFD; }
        cp = (cp << 6) | (s[1+i] & 0x3F);
    }
    *p = (const char *)(s + 1 + extra);
    return cp;
}

/* ================================================================== */
/*  Lifecycle                                                          */
/* ================================================================== */

bool dashboard_init(DashboardRuntime *d,
                    GpuContext *gpu,
                    SDL_Window *window,
                    const char *settings_db_path) {
    memset(d, 0, sizeof(*d));
    d->gpu              = gpu;
    d->window           = window;
    d->focused_slot     = -1;
    d->view             = DASHBOARD_VIEW_NORMAL;
    d->foreground_group = UINT32_MAX;

    /* Pre-allocate starter capacity for slots / groups / friends.
     * All three arrays grow on demand thereafter — no hard cap. */
    if (!dashboard_reserve_slots  (d, DASHBOARD_INITIAL_SLOT_CAP)  ||
        !dashboard_reserve_groups (d, DASHBOARD_INITIAL_GROUP_CAP) ||
        !dashboard_reserve_friends(d, DASHBOARD_INITIAL_FRIEND_CAP)) {
        fprintf(stderr, "[dashboard] Initial capacity allocation failed\n");
        return false;
    }

    clipboard_bindings_init(&d->clipboard);

    /* Settings DB */
    char db_path[4096];
    if (settings_db_path)
        snprintf(db_path, sizeof(db_path), "%s", settings_db_path);
    else
        snprintf(db_path, sizeof(db_path), "%s/settings.db", get_data_dir());

    if (!settings_db_init(d, db_path)) {
        fprintf(stderr, "[dashboard] Warning: settings DB failed, continuing without\n");
    }

    load_friends(d);

    /* Compositing pipeline for offscreen → swapchain blitting */
    if (!gpu_compositor_init(&d->compositor, gpu)) {
        fprintf(stderr, "[dashboard] Warning: compositor init failed\n");
    }

    printf("[dashboard] Initialized\n");
    return true;
}

void dashboard_destroy(DashboardRuntime *d) {
    /* Destroy all webapp slots */
    for (uint32_t i = 0; i < d->slot_count; i++) {
        if (d->slots[i].rt) {
            webapp_runtime_destroy(d->slots[i].rt);
            free(d->slots[i].rt);
            d->slots[i].rt = NULL;
        }
        gpu_destroy_offscreen_texture(&d->slots[i].offscreen_tex,
                                      &d->slots[i].offscreen_view);
    }

    /* Disconnect all groups */
    for (uint32_t i = 0; i < d->group_count; i++) {
        if (d->groups[i].client) {
            yumi_client_destroy(d->groups[i].client);
            d->groups[i].client = NULL;
        }
    }

    clipboard_bindings_destroy(&d->clipboard);

    /* Compositing pipeline */
    gpu_compositor_destroy(&d->compositor);

    /* Settings DB */
    if (d->settings_db_active) {
        duckdb_disconnect(&d->settings_conn);
        duckdb_close(&d->settings_db);
        d->settings_db_active = false;
    }

    /* Free dynamic arrays. */
    free(d->slots);
    free(d->groups);
    free(d->friends);

    memset(d, 0, sizeof(*d));
    printf("[dashboard] Destroyed\n");
}

/* ================================================================== */
/*  Webapp slot management                                             */
/* ================================================================== */

int dashboard_add_slot(DashboardRuntime *d,
                       uint32_t group_index,
                       const char *wasm_path,
                       const WebAppViewport *vp,
                       bool is_dashboard) {
    if (!dashboard_reserve_slots(d, d->slot_count + 1)) {
        fprintf(stderr, "[dashboard] Slot allocation failed\n");
        return -1;
    }
    if (group_index >= d->group_count) {
        fprintf(stderr, "[dashboard] Invalid group_index %u\n", group_index);
        return -1;
    }

    uint32_t idx = d->slot_count;
    WebAppSlot *slot = &d->slots[idx];
    memset(slot, 0, sizeof(*slot));
    slot->group_index = group_index;
    slot->viewport    = *vp;

    /* Derive a stable 16-byte app_id from the wasm path (two-lane FNV-1a
     * + golden-ratio mixer).  This is not a cryptographic hash — it only
     * needs to be deterministic and collision-resistant enough for UI /
     * quota bookkeeping. */
    {
        uint64_t h1 = 0xcbf29ce484222325ull;
        uint64_t h2 = 0x9e3779b97f4a7c15ull;
        for (const unsigned char *p = (const unsigned char *)wasm_path;
             p && *p; p++) {
            h1 ^= *p; h1 *= 0x100000001b3ull;
            h2  = (h2 + *p) * 0x9e3779b97f4a7c15ull;
        }
        memcpy(slot->app_id,     &h1, 8);
        memcpy(slot->app_id + 8, &h2, 8);
    }
    /* Basename of wasm_path -> app_name. */
    {
        const char *base = wasm_path ? wasm_path : "";
        const char *sep  = strrchr(base, '/');
        if (sep) base = sep + 1;
        snprintf(slot->app_name, sizeof(slot->app_name), "%s", base);
    }

    /* Allocate offscreen texture */
    if (!gpu_create_offscreen_texture(d->gpu,
            (uint32_t)vp->w, (uint32_t)vp->h,
            &slot->offscreen_tex, &slot->offscreen_view)) {
        fprintf(stderr, "[dashboard] Offscreen texture creation failed\n");
        return -1;
    }

    /* Create and init the webapp runtime */
    slot->rt = calloc(1, sizeof(WebAppRuntime));
    if (!slot->rt) return -1;

    /* Build per-webapp DB path from group + slot */
    char db_path[4096];
    snprintf(db_path, sizeof(db_path), "%s/webapp_%u_%u.db",
             get_data_dir(), group_index, idx);

    if (!webapp_runtime_init(slot->rt, d, d->gpu, group_index, db_path, is_dashboard)) {
        free(slot->rt); slot->rt = NULL;
        gpu_destroy_offscreen_texture(&slot->offscreen_tex, &slot->offscreen_view);
        return -1;
    }

    if (!webapp_runtime_load(slot->rt, d->window, wasm_path)) {
        webapp_runtime_destroy(slot->rt);
        free(slot->rt); slot->rt = NULL;
        gpu_destroy_offscreen_texture(&slot->offscreen_tex, &slot->offscreen_view);
        return -1;
    }

    /* Assign the offscreen surface to the webapp */
    webapp_runtime_set_viewport(slot->rt, vp);

    webapp_runtime_call_init(slot->rt);

    d->slot_count++;
    printf("[dashboard] Added slot %u (group %u): %s\n", idx, group_index, wasm_path);
    return (int)idx;
}

void dashboard_remove_slot(DashboardRuntime *d, uint32_t index) {
    if (index >= d->slot_count) return;
    WebAppSlot *slot = &d->slots[index];

    if (slot->rt) {
        webapp_runtime_destroy(slot->rt);
        free(slot->rt);
        slot->rt = NULL;
    }
    gpu_destroy_offscreen_texture(&slot->offscreen_tex, &slot->offscreen_view);

    /* Shift remaining slots down */
    for (uint32_t i = index; i + 1 < d->slot_count; i++)
        d->slots[i] = d->slots[i + 1];
    d->slot_count--;
    memset(&d->slots[d->slot_count], 0, sizeof(WebAppSlot));

    if (d->focused_slot == (int)index) d->focused_slot = -1;
    else if (d->focused_slot > (int)index) d->focused_slot--;

    printf("[dashboard] Removed slot %u\n", index);
}

/* ================================================================== */
/*  Tiling layout                                                      */
/* ================================================================== */

void dashboard_layout_recalc(DashboardRuntime *d) {
    if (d->slot_count == 0) return;

    int ww, wh;
    SDL_GetWindowSize(d->window, &ww, &wh);

    /* Simple grid layout: as many columns as sqrt(count) */
    uint32_t cols = 1;
    while (cols * cols < d->slot_count) cols++;
    uint32_t rows = (d->slot_count + cols - 1) / cols;

    int cell_w = ww / (int)cols;
    int cell_h = wh / (int)rows;

    for (uint32_t i = 0; i < d->slot_count; i++) {
        uint32_t col = i % cols;
        uint32_t row = i / cols;
        WebAppViewport vp = {
            .x = (int)col * cell_w,
            .y = (int)row * cell_h,
            .w = cell_w,
            .h = cell_h,
        };
        d->slots[i].viewport = vp;
        if (d->slots[i].rt) {
            gpu_resize_offscreen_texture(d->gpu,
                (uint32_t)vp.w, (uint32_t)vp.h,
                &d->slots[i].offscreen_tex, &d->slots[i].offscreen_view);
            webapp_runtime_set_viewport(d->slots[i].rt, &vp);
        }
    }
}

void dashboard_layout_fullscreen(DashboardRuntime *d, uint32_t slot_index) {
    if (slot_index >= d->slot_count) return;
    int ww, wh;
    SDL_GetWindowSize(d->window, &ww, &wh);

    /* Hide all other slots, give full window to this one */
    for (uint32_t i = 0; i < d->slot_count; i++) {
        if (i == slot_index) {
            WebAppViewport vp = { .x = 0, .y = 0, .w = ww, .h = wh };
            d->slots[i].viewport = vp;
            if (d->slots[i].rt) {
                gpu_resize_offscreen_texture(d->gpu, (uint32_t)ww, (uint32_t)wh,
                    &d->slots[i].offscreen_tex, &d->slots[i].offscreen_view);
                webapp_runtime_set_viewport(d->slots[i].rt, &vp);
            }
        } else {
            d->slots[i].viewport = (WebAppViewport){ 0, 0, 0, 0 };
        }
    }
}

void dashboard_layout_split_h(DashboardRuntime *d,
                               uint32_t slot_a, uint32_t slot_b) {
    if (slot_a >= d->slot_count || slot_b >= d->slot_count) return;
    int ww, wh;
    SDL_GetWindowSize(d->window, &ww, &wh);
    int half = ww / 2;

    WebAppViewport vp_a = { .x = 0,    .y = 0, .w = half,      .h = wh };
    WebAppViewport vp_b = { .x = half,  .y = 0, .w = ww - half, .h = wh };

    d->slots[slot_a].viewport = vp_a;
    d->slots[slot_b].viewport = vp_b;

    if (d->slots[slot_a].rt) {
        gpu_resize_offscreen_texture(d->gpu, (uint32_t)vp_a.w, (uint32_t)vp_a.h,
            &d->slots[slot_a].offscreen_tex, &d->slots[slot_a].offscreen_view);
        webapp_runtime_set_viewport(d->slots[slot_a].rt, &vp_a);
    }
    if (d->slots[slot_b].rt) {
        gpu_resize_offscreen_texture(d->gpu, (uint32_t)vp_b.w, (uint32_t)vp_b.h,
            &d->slots[slot_b].offscreen_tex, &d->slots[slot_b].offscreen_view);
        webapp_runtime_set_viewport(d->slots[slot_b].rt, &vp_b);
    }
}

void dashboard_layout_split_v(DashboardRuntime *d,
                               uint32_t slot_a, uint32_t slot_b) {
    if (slot_a >= d->slot_count || slot_b >= d->slot_count) return;
    int ww, wh;
    SDL_GetWindowSize(d->window, &ww, &wh);
    int half = wh / 2;

    WebAppViewport vp_a = { .x = 0, .y = 0,    .w = ww, .h = half };
    WebAppViewport vp_b = { .x = 0, .y = half,  .w = ww, .h = wh - half };

    d->slots[slot_a].viewport = vp_a;
    d->slots[slot_b].viewport = vp_b;

    if (d->slots[slot_a].rt) {
        gpu_resize_offscreen_texture(d->gpu, (uint32_t)vp_a.w, (uint32_t)vp_a.h,
            &d->slots[slot_a].offscreen_tex, &d->slots[slot_a].offscreen_view);
        webapp_runtime_set_viewport(d->slots[slot_a].rt, &vp_a);
    }
    if (d->slots[slot_b].rt) {
        gpu_resize_offscreen_texture(d->gpu, (uint32_t)vp_b.w, (uint32_t)vp_b.h,
            &d->slots[slot_b].offscreen_tex, &d->slots[slot_b].offscreen_view);
        webapp_runtime_set_viewport(d->slots[slot_b].rt, &vp_b);
    }
}

/* ================================================================== */
/*  Frame rendering                                                    */
/* ================================================================== */

void dashboard_tick(DashboardRuntime *d) {
    /* Drive per-slot maintenance that must run faster than the render
     * frame — currently just topping up the SDL audio stream backing
     * each webapp's media playback.  Kept here (rather than in main)
     * so the host loop doesn't need to reach into WebAppRuntime. */
    for (uint32_t i = 0; i < d->slot_count; i++) {
        WebAppSlot *slot = &d->slots[i];
        if (slot->rt)
            webapp_runtime_pump_audio(slot->rt);
    }
}

void dashboard_frame(DashboardRuntime *d) {
    if (!gpu_frame_begin(d->gpu)) return;

    /* Render dashboard's own UI background */
    gpu_set_clear_color(d->gpu, 0.08f, 0.08f, 0.10f, 1.0f);

    /* Call each webapp's frame (they render to their offscreen textures) */
    for (uint32_t i = 0; i < d->slot_count; i++) {
        WebAppSlot *slot = &d->slots[i];
        if (!slot->rt || slot->viewport.w <= 0 || slot->viewport.h <= 0)
            continue;
        webapp_runtime_call_frame(slot->rt);
    }

    /* ── Composite offscreen textures onto the swapchain ── */
    WGPUSurfaceTexture st;
    wgpuSurfaceGetCurrentTexture(d->gpu->surface, &st);
    if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        return;
    }
    WGPUTextureView swapchain_view = wgpuTextureCreateView(st.texture, NULL);

    WGPURenderPassColorAttachment color_att = {
        .view       = swapchain_view,
        .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
        .loadOp     = WGPULoadOp_Clear,
        .storeOp    = WGPUStoreOp_Store,
        .clearValue = (WGPUColor){ d->gpu->clear_color[0], d->gpu->clear_color[1],
                                    d->gpu->clear_color[2], d->gpu->clear_color[3] },
    };
    WGPURenderPassDescriptor rp_desc = {
        .colorAttachmentCount = 1,
        .colorAttachments     = &color_att,
    };
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(d->gpu->device, NULL);
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp_desc);

    /* Blit each webapp's offscreen texture */
    float tw = (float)d->gpu->width;
    float th = (float)d->gpu->height;
    for (uint32_t i = 0; i < d->slot_count; i++) {
        WebAppSlot *slot = &d->slots[i];
        if (!slot->rt || slot->viewport.w <= 0 || slot->viewport.h <= 0)
            continue;
        WGPUTextureView src = slot->rt->offscreen_view;
        if (!src) continue;
        gpu_composite_quad(&d->compositor, d->gpu, pass, src,
                           (float)slot->viewport.x, (float)slot->viewport.y,
                           (float)slot->viewport.w, (float)slot->viewport.h,
                           tw, th);
    }

    /* Dashboard overlay UI (buttons, sidebars) would go here */
    /* Draw any queued gpu_push_rect geometry (dashboard UI) */
    uint32_t vb_bytes = d->gpu->vertex_count * GPU_FLOATS_PER_VTX * sizeof(float);
    if (vb_bytes > 0) {
        wgpuQueueWriteBuffer(d->gpu->queue, d->gpu->vertex_buffer, 0,
                             d->gpu->vertices, vb_bytes);
        wgpuRenderPassEncoderSetViewport(pass, 0, 0, tw, th, 0.0f, 1.0f);
        wgpuRenderPassEncoderSetPipeline(pass, d->gpu->pipeline);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, d->gpu->vertex_buffer, 0, vb_bytes);
        wgpuRenderPassEncoderDraw(pass, d->gpu->vertex_count, 1, 0, 0);
    }

    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(d->gpu->queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);
    wgpuSurfacePresent(d->gpu->surface);
    wgpuTextureViewRelease(swapchain_view);
    wgpuTextureRelease(st.texture);
}

/* ================================================================== */
/*  Event dispatch                                                     */
/* ================================================================== */

/** Hit-test mouse coords against slot viewports, return slot index or -1. */
static int hit_test_slot(const DashboardRuntime *d, float x, float y) {
    for (uint32_t i = 0; i < d->slot_count; i++) {
        const WebAppViewport *vp = &d->slots[i].viewport;
        if (vp->w <= 0 || vp->h <= 0) continue;
        if (x >= vp->x && x < vp->x + vp->w &&
            y >= vp->y && y < vp->y + vp->h)
            return (int)i;
    }
    return -1;
}

static void set_focus(DashboardRuntime *d, int slot_index) {
    if (d->focused_slot == slot_index) return;

    /* Unfocus previous */
    if (d->focused_slot >= 0 && d->focused_slot < (int)d->slot_count) {
        d->slots[d->focused_slot].focused = false;
        if (d->slots[d->focused_slot].rt)
            webapp_dispatch_focus(d->slots[d->focused_slot].rt, 0);
    }

    d->focused_slot = slot_index;

    /* Focus new */
    if (slot_index >= 0 && slot_index < (int)d->slot_count) {
        d->slots[slot_index].focused = true;
        if (d->slots[slot_index].rt)
            webapp_dispatch_focus(d->slots[slot_index].rt, 1);
    }
}

bool dashboard_dispatch_event(DashboardRuntime *d, const SDL_Event *ev) {
    switch (ev->type) {

    /* ── Window ── */
    case SDL_EVENT_QUIT:
        return false;

    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        gpu_resize(d->gpu, (uint32_t)ev->window.data1, (uint32_t)ev->window.data2);
        dashboard_layout_recalc(d);
        dashboard_frame(d);
        break;

    case SDL_EVENT_WINDOW_RESIZED:
        dashboard_layout_recalc(d);
        dashboard_frame(d);
        break;

    case SDL_EVENT_WINDOW_EXPOSED:
        dashboard_frame(d);
        break;

    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        /* Window-level focus — keep current slot focus */
        break;

    case SDL_EVENT_WINDOW_FOCUS_LOST:
        /* Unfocus current slot when window loses focus */
        set_focus(d, -1);
        break;

    /* ── Keyboard — forward to webapp, then handle clipboard ── */
    case SDL_EVENT_KEY_DOWN: {
        bool ctrl = (ev->key.mod & SDL_KMOD_CTRL) != 0;

        /* Always forward key event to the webapp first */
        if (d->focused_slot >= 0 && d->slots[d->focused_slot].rt)
            webapp_dispatch_key(d->slots[d->focused_slot].rt,
                (int)ev->key.scancode, (int)ev->key.key,
                (int)ev->key.mod, 1);

        /* Dashboard-level clipboard mediation (text paste into text fields) */
        if (ctrl && ev->key.key == SDLK_V) {
            if (d->focused_slot >= 0)
                dashboard_handle_paste_request(d, (uint32_t)d->focused_slot);
        }
        break;
    }
    case SDL_EVENT_KEY_UP:
        if (d->focused_slot >= 0 && d->slots[d->focused_slot].rt)
            webapp_dispatch_key(d->slots[d->focused_slot].rt,
                (int)ev->key.scancode, (int)ev->key.key,
                (int)ev->key.mod, 0);
        break;

    /* ── Text input ── */
    case SDL_EVENT_TEXT_INPUT: {
        if (d->focused_slot < 0) break;
        WebAppRuntime *rt = d->slots[d->focused_slot].rt;
        if (!rt) break;
        const char *p = ev->text.text;
        while (*p) {
            uint32_t cp = utf8_next(&p);
            if (cp && cp != 0xFFFD)
                webapp_dispatch_char(rt, cp);
        }
        break;
    }

    /* ── Mouse — hit-test and translate to slot-local coords ── */
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        int slot = hit_test_slot(d, ev->button.x, ev->button.y);
        set_focus(d, slot);
        if (slot >= 0 && d->slots[slot].rt) {
            const WebAppViewport *vp = &d->slots[slot].viewport;
            webapp_dispatch_mouse_button(d->slots[slot].rt,
                (int)ev->button.button, 1,
                ev->button.x - vp->x, ev->button.y - vp->y);
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        int slot = hit_test_slot(d, ev->button.x, ev->button.y);
        if (slot >= 0 && d->slots[slot].rt) {
            const WebAppViewport *vp = &d->slots[slot].viewport;
            webapp_dispatch_mouse_button(d->slots[slot].rt,
                (int)ev->button.button, 0,
                ev->button.x - vp->x, ev->button.y - vp->y);
        }
        break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        int slot = hit_test_slot(d, ev->motion.x, ev->motion.y);
        if (slot >= 0 && d->slots[slot].rt) {
            const WebAppViewport *vp = &d->slots[slot].viewport;
            webapp_dispatch_mouse_motion(d->slots[slot].rt,
                ev->motion.x - vp->x, ev->motion.y - vp->y,
                ev->motion.xrel, ev->motion.yrel,
                (int)ev->motion.state);
        }
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (d->focused_slot >= 0 && d->slots[d->focused_slot].rt)
            webapp_dispatch_mouse_wheel(d->slots[d->focused_slot].rt,
                ev->wheel.x, ev->wheel.y);
        break;

    /* ── Touch ── */
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION: {
        int type = (ev->type == SDL_EVENT_FINGER_DOWN) ? 0 :
                   (ev->type == SDL_EVENT_FINGER_UP)   ? 1 : 2;
        /* Touch coords are normalized 0–1, broadcast to focused slot */
        if (d->focused_slot >= 0 && d->slots[d->focused_slot].rt)
            webapp_dispatch_touch(d->slots[d->focused_slot].rt,
                (int)(ev->tfinger.fingerID & 0x7FFFFFFF),
                type, ev->tfinger.x, ev->tfinger.y, ev->tfinger.pressure);
        break;
    }

    /* ── Joystick / gamepad — forward to focused slot ── */
    case SDL_EVENT_JOYSTICK_ADDED:
        if (d->focused_slot >= 0 && d->slots[d->focused_slot].rt)
            webapp_dispatch_joystick_added(d->slots[d->focused_slot].rt, (int)ev->jdevice.which);
        break;
    case SDL_EVENT_JOYSTICK_REMOVED:
        if (d->focused_slot >= 0 && d->slots[d->focused_slot].rt)
            webapp_dispatch_joystick_removed(d->slots[d->focused_slot].rt, (int)ev->jdevice.which);
        break;
    case SDL_EVENT_JOYSTICK_AXIS_MOTION:
        if (d->focused_slot >= 0 && d->slots[d->focused_slot].rt)
            webapp_dispatch_joystick_axis(d->slots[d->focused_slot].rt,
                (int)ev->jaxis.which, (int)ev->jaxis.axis, (int)ev->jaxis.value);
        break;
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
        if (d->focused_slot >= 0 && d->slots[d->focused_slot].rt)
            webapp_dispatch_joystick_button(d->slots[d->focused_slot].rt,
                (int)ev->jbutton.which, (int)ev->jbutton.button, 1);
        break;
    case SDL_EVENT_JOYSTICK_BUTTON_UP:
        if (d->focused_slot >= 0 && d->slots[d->focused_slot].rt)
            webapp_dispatch_joystick_button(d->slots[d->focused_slot].rt,
                (int)ev->jbutton.which, (int)ev->jbutton.button, 0);
        break;
    case SDL_EVENT_JOYSTICK_HAT_MOTION:
        if (d->focused_slot >= 0 && d->slots[d->focused_slot].rt)
            webapp_dispatch_joystick_hat(d->slots[d->focused_slot].rt,
                (int)ev->jhat.which, (int)ev->jhat.hat, (int)ev->jhat.value);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (d->focused_slot >= 0 && d->slots[d->focused_slot].rt)
            webapp_dispatch_gamepad_added(d->slots[d->focused_slot].rt, (int)ev->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (d->focused_slot >= 0 && d->slots[d->focused_slot].rt)
            webapp_dispatch_gamepad_removed(d->slots[d->focused_slot].rt, (int)ev->gdevice.which);
        break;

    default:
        break;
    }
    return true;
}

/* ================================================================== */
/*  Group lifecycle                                                    */
/* ================================================================== */

static void dashboard_group_event_cb(void *user,
                                      yumi_client_event_t event,
                                      const uint8_t *peer_id) {
    DashboardGroupCtx *ctx = (DashboardGroupCtx *)user;
    DashboardRuntime *d = ctx->d;
    uint32_t gi = ctx->group_index;

    if (gi >= d->group_count) return;
    DashboardGroup *g = &d->groups[gi];

    switch (event) {
    case YUMI_EVENT_SYNC_COMPLETE: {
        /* Update cached member count */
        gr_registrar_t *reg = yumi_client_get_registrar(g->client);
        if (reg) {
            uint32_t count = 0;
            gr_peer_count(reg, GR_PEER_ACTIVE, &count);
            g->member_count = count;
        }
        break;
    }
    case YUMI_EVENT_VERIFIED:
        g->state = GROUP_STATE_CONNECTED;
        printf("[dashboard] Group %u verified and connected\n", gi);
        break;

    case YUMI_EVENT_PEER_KICKED: {
        /* We were kicked or banned — check which */
        gr_registrar_t *reg = yumi_client_get_registrar(g->client);
        if (reg && peer_id) {
            gr_peer_t self_peer;
            if (gr_peer_get(reg, peer_id, &self_peer) == GR_OK) {
                if (self_peer.status == GR_PEER_BANNED)
                    g->state = GROUP_STATE_REMOVED_BANNED;
                else
                    g->state = GROUP_STATE_REMOVED_KICKED;
            } else {
                g->state = GROUP_STATE_REMOVED_KICKED;
            }
        } else {
            g->state = GROUP_STATE_REMOVED_KICKED;
        }

        /* Check if the kicker is a friend and include details */
        if (peer_id) {
            gr_registrar_t *reg2 = yumi_client_get_registrar(g->client);
            if (reg2) {
                gr_peer_t p;
                if (gr_peer_get(reg2, peer_id, &p) == GR_OK &&
                    p.removed_by[0] != 0) {
                    bool kicker_is_friend = dashboard_is_friend(d, p.removed_by);
                    printf("[dashboard] Kicked from group %u by %s (friend=%s): %s\n",
                           gi,
                           kicker_is_friend ? "friend" : "non-friend",
                           kicker_is_friend ? "yes" : "no",
                           p.removed_reason);
                }
            }
        }

        g->recovery_readonly = true;
        printf("[dashboard] Removed from group %u (%s)\n", gi,
               g->state == GROUP_STATE_REMOVED_BANNED ? "banned" : "kicked");
        break;
    }
    case YUMI_EVENT_PEER_CONNECTED:
    case YUMI_EVENT_PEER_DISCONNECTED:
    case YUMI_EVENT_VERIFICATION_FAILED:
    case YUMI_EVENT_EPOCH_ROTATED:
    case YUMI_EVENT_DUPLICATE_PEER:
    case YUMI_EVENT_MESH_ABUSE:
    case YUMI_EVENT_BOOT_BLOCKED:
    default:
        break;
    }
}

int dashboard_connect_group(DashboardRuntime *d, uint32_t group_index) {
    if (group_index >= d->group_count) return -1;
    DashboardGroup *g = &d->groups[group_index];
    if (g->state == GROUP_STATE_CONNECTED) return 0;

    g->state = GROUP_STATE_CONNECTING;

    /* Allocate persistent callback context */
    DashboardGroupCtx *ctx = NULL;
    yumi_memory_alloc_error_enum allocRes = lease_DashboardGroupCtx(&ctx);
    if (allocRes != YUMI_MEMORY_ALLOC_SUCCESS ||
        ctx == NULL)
    {
        return -1;
    }
    ctx->d = d;
    ctx->group_index = group_index;

    yumi_client_config_t cfg = {
        .db_path    = g->db_path,
        .on_event   = dashboard_group_event_cb,
        .user       = ctx,
    };

    int ret = yumi_client_open(&g->client, &cfg, g->group_id, NULL);
    if (ret != 0) {
        g->state = GROUP_STATE_FAILED;
        release_DashboardGroupCtx(ctx);
        return -1;
    }

    printf("[dashboard] Connecting group %u...\n", group_index);
    return 0;
}

void dashboard_disconnect_group(DashboardRuntime *d, uint32_t group_index) {
    if (group_index >= d->group_count) return;
    DashboardGroup *g = &d->groups[group_index];

    /* Close all webapp slots belonging to this group */
    for (uint32_t i = d->slot_count; i > 0; i--) {
        if (d->slots[i-1].group_index == group_index)
            dashboard_remove_slot(d, i-1);
    }

    if (g->client) {
        yumi_client_destroy(g->client);
        g->client = NULL;
    }
    g->state = GROUP_STATE_DISCONNECTED;
    printf("[dashboard] Disconnected group %u\n", group_index);
}

int dashboard_process_invite(DashboardRuntime *d,
                             const uint8_t *invite_blob,
                             size_t invite_len,
                             const char *db_path) {
    if (!dashboard_reserve_groups(d, d->group_count + 1)) return -1;

    uint32_t idx = d->group_count;
    DashboardGroup *g = &d->groups[idx];
    memset(g, 0, sizeof(*g));
    snprintf(g->db_path, sizeof(g->db_path), "%s", db_path);
    g->state = GROUP_STATE_CONNECTING;

    /* TODO: yumi_client_join() with callbacks for progress/status/success/fail */
    (void)invite_blob; (void)invite_len;
    printf("[dashboard] Processing invite into group %u\n", idx);

    d->group_count++;
    return (int)idx;
}

int dashboard_generate_invite(DashboardRuntime *d,
                              uint32_t group_index,
                              int64_t expiry_ms,
                              uint8_t **out_blob,
                              size_t *out_len) {
    if (group_index >= d->group_count) return -1;
    DashboardGroup *g = &d->groups[group_index];
    if (!g->client) return -1;
    return yumi_client_invite(g->client, expiry_ms, out_blob, out_len);
}

void dashboard_invalidate_invite(DashboardRuntime *d, uint32_t group_index) {
    (void)d; (void)group_index;
    /* TODO: call registrar invite invalidation */
}

void dashboard_clear_invite(DashboardRuntime *d, uint32_t group_index) {
    (void)d; (void)group_index;
    /* TODO: call registrar invite clear */
}

void dashboard_set_bandwidth_limit(DashboardRuntime *d,
                                   uint32_t group_index,
                                   uint32_t bytes_per_sec) {
    (void)d; (void)group_index; (void)bytes_per_sec;
    /* TODO: set rate limit on the SUDP layer */
}

/* ================================================================== */
/*  Clipboard mediation                                                */
/* ================================================================== */

void dashboard_handle_paste_request(DashboardRuntime *d, uint32_t slot_index) {
    if (slot_index >= d->slot_count) return;

    /* Stage the IPC request — dashboard UI will show confirmation */
    d->pending_ipc.type       = IPC_PASTE_PENDING;
    d->pending_ipc.slot_index = slot_index;

    /* Read system clipboard */
    char *text = SDL_GetClipboardText();
    if (text) {
        uint32_t len = (uint32_t)strlen(text);
        if (len >= sizeof(d->pending_ipc.text))
            len = sizeof(d->pending_ipc.text) - 1;
        memcpy(d->pending_ipc.text, text, len);
        d->pending_ipc.text[len] = '\0';
        d->pending_ipc.text_len  = len;
        SDL_free(text);
    } else {
        d->pending_ipc.text[0]  = '\0';
        d->pending_ipc.text_len = 0;
    }

    /* TODO: show confirmation dialog in dashboard UI.
       For now, auto-confirm (deliver immediately). */
    if (d->pending_ipc.text_len > 0) {
        WebAppRuntime *rt = d->slots[slot_index].rt;
        if (rt && rt->fn_on_paste_result && rt->memory) {
            /* Write text into the guest's linear memory (use a
               well-known scratch region or allocate via guest export).
               For now, we call the guest with ptr=0 len=0 to signal
               "paste available" — real delivery requires a guest-side
               allocator or a shared mailbox convention.
               
               Simple approach: write to end of first memory page and
               call with that pointer.  Guest MUST have reserved space. */
            /* Deliver text via on_paste_result(text_ptr, text_len) */
            uint32_t text_len = d->pending_ipc.text_len;
            size_t mem_size = wasm_memory_data_size(rt->memory);
            /* Use a scratch area at the end of memory if enough space */
            if (mem_size >= (size_t)(text_len + 16)) {
                uint32_t scratch_ptr = (uint32_t)(mem_size - text_len - 4);
                /* Align down */
                scratch_ptr &= ~3u;
                uint8_t *base = (uint8_t *)wasm_memory_data(rt->memory);
                memcpy(base + scratch_ptr, d->pending_ipc.text, text_len);
                wasm_val_t a[] = {
                    { .kind = WASM_I32, .of.i32 = (int32_t)scratch_ptr },
                    { .kind = WASM_I32, .of.i32 = (int32_t)text_len },
                };
                wasm_val_vec_t args = { .size = 2, .data = a };
                wasm_val_vec_t res = WASM_EMPTY_VEC;
                wasm_trap_t *t = wasm_func_call(rt->fn_on_paste_result, &args, &res);
                if (t) {
                    wasm_message_t m; wasm_trap_message(t, &m);
                    fprintf(stderr, "[dashboard] on_paste_result trap: %.*s\n",
                            (int)m.size, m.data);
                    wasm_byte_vec_delete(&m); wasm_trap_delete(t);
                }
            }
        }
    }
    d->pending_ipc.type = IPC_NONE;
}

void dashboard_handle_copy_request(DashboardRuntime *d,
                                   const char *text, uint32_t len) {
    if (!text || len == 0 || len >= CLIPBOARD_MAX_SIZE) return;
    /* Copy to system clipboard — no confirmation needed for copy */
    ClipboardBuffer* clipboardBuf = NULL;
    if (lease_ClipboardBuffer(&clipboardBuf) == YUMI_MEMORY_ALLOC_SUCCESS)
    {
        memcpy(clipboardBuf->buffer, text, len);
        clipboardBuf->buffer[len] = '\0';
        SDL_SetClipboardText((const char*)clipboardBuf->buffer);
        release_ClipboardBuffer(clipboardBuf);
    }
}

/* ================================================================== */
/*  Intra-group link buffer                                            */
/* ================================================================== */

void dashboard_set_group_link(DashboardRuntime *d,
                              uint32_t group_index,
                              const void *data, uint32_t len) {
    if (group_index >= d->group_count) return;
    DashboardGroup *g = &d->groups[group_index];
    if (len > WEBAPP_LINK_BUF_SIZE) len = WEBAPP_LINK_BUF_SIZE;
    memcpy(g->link_buf, data, len);
    g->link_buf_len = len;
}

bool dashboard_get_group_link(const DashboardRuntime *d,
                              uint32_t group_index,
                              const void **out_data,
                              uint32_t *out_len) {
    if (group_index >= d->group_count) return false;
    const DashboardGroup *g = &d->groups[group_index];
    if (g->link_buf_len == 0) return false;
    *out_data = g->link_buf;
    *out_len  = g->link_buf_len;
    return true;
}

/* ================================================================== */
/*  File dialog portal API                                             */
/* ================================================================== */

/* Deliver a file path result to the requesting WASM webapp */
static void file_dialog_deliver(DashboardRuntime *d, uint32_t slot_index,
                                const char *path) {
    if (slot_index >= d->slot_count) return;
    WebAppRuntime *rt = d->slots[slot_index].rt;
    if (!rt || !rt->fn_on_file_result || !rt->memory) return;

    uint32_t path_len = path ? (uint32_t)strlen(path) : 0;
    if (path_len == 0) {
        /* Cancelled — notify with ptr=0, len=0 */
        wasm_val_t a[] = {
            { .kind = WASM_I32, .of.i32 = 0 },
            { .kind = WASM_I32, .of.i32 = 0 },
            { .kind = WASM_I32, .of.i32 = 0 },
        };
        wasm_val_vec_t args = { .size = 3, .data = a };
        wasm_val_vec_t res = WASM_EMPTY_VEC;
        wasm_trap_t *t = wasm_func_call(rt->fn_on_file_result, &args, &res);
        if (t) { wasm_trap_delete(t); }
        return;
    }

    size_t mem_size = wasm_memory_data_size(rt->memory);
    if (mem_size < (size_t)(path_len + 16)) return;

    uint32_t scratch_ptr = (uint32_t)(mem_size - path_len - 4);
    scratch_ptr &= ~3u;
    uint8_t *base = (uint8_t *)wasm_memory_data(rt->memory);
    memcpy(base + scratch_ptr, path, path_len);

    wasm_val_t a[] = {
        { .kind = WASM_I32, .of.i32 = (int32_t)slot_index },
        { .kind = WASM_I32, .of.i32 = (int32_t)scratch_ptr },
        { .kind = WASM_I32, .of.i32 = (int32_t)path_len },
    };
    wasm_val_vec_t args = { .size = 3, .data = a };
    wasm_val_vec_t res = WASM_EMPTY_VEC;
    wasm_trap_t *t = wasm_func_call(rt->fn_on_file_result, &args, &res);
    if (t) {
        wasm_message_t m; wasm_trap_message(t, &m);
        fprintf(stderr, "[dashboard] on_file_result trap: %.*s\n",
                (int)m.size, m.data);
        wasm_byte_vec_delete(&m); wasm_trap_delete(t);
    }
}

static void SDLCALL file_dialog_callback(void *userdata,
                                          const char * const *filelist,
                                          int filter) {
    (void)filter;
    FileDialogCtx *ctx = (FileDialogCtx *)userdata;
    const char *path = (filelist && filelist[0]) ? filelist[0] : NULL;
    file_dialog_deliver(ctx->d, ctx->slot_index, path);
    ctx->d->pending_ipc.type = IPC_NONE;
    free(ctx);
}

void dashboard_open_file_dialog(DashboardRuntime *d, uint32_t slot_index) {
    if (slot_index >= d->slot_count) return;

    FileDialogCtx *ctx = NULL;
    if (lease_FileDialogCtx(&ctx) == YUMI_MEMORY_ALLOC_SUCCESS)
    {
        ctx->d = d;
        ctx->slot_index = slot_index;
        ctx->handle = IPC_FILE_OPEN_PENDING;

        d->pending_ipc.type       = IPC_FILE_OPEN_PENDING;
        d->pending_ipc.slot_index = slot_index;

        SDL_ShowOpenFileDialog(file_dialog_callback, ctx, d->window,
                            NULL, 0, NULL, false);
    }
}

void dashboard_save_file_dialog(DashboardRuntime *d, uint32_t slot_index) {
    if (slot_index >= d->slot_count) return;

    FileDialogCtx *ctx = NULL;
    if (lease_FileDialogCtx(&ctx) == YUMI_MEMORY_ALLOC_SUCCESS)
    {
        ctx->d = d;
        ctx->slot_index = slot_index;
        ctx->handle = IPC_FILE_SAVE_PENDING;

        d->pending_ipc.type       = IPC_FILE_SAVE_PENDING;
        d->pending_ipc.slot_index = slot_index;

        SDL_ShowSaveFileDialog(file_dialog_callback, ctx, d->window,
                                NULL, 0, NULL);
    }
}

void dashboard_open_folder_dialog(DashboardRuntime *d, uint32_t slot_index) {
    if (slot_index >= d->slot_count) return;

    FileDialogCtx *ctx = NULL;
    if (lease_FileDialogCtx(&ctx) == YUMI_MEMORY_ALLOC_SUCCESS)
    {
        ctx->d = d;
        ctx->slot_index = slot_index;
        ctx->handle = IPC_FOLDER_OPEN_PENDING;

        d->pending_ipc.type       = IPC_FOLDER_OPEN_PENDING;
        d->pending_ipc.slot_index = slot_index;

        SDL_ShowOpenFolderDialog(file_dialog_callback, ctx, d->window,
                                NULL, false);
    }
}

/* ================================================================== */
/*  Settings & export                                                  */
/* ================================================================== */

bool dashboard_view_settings(DashboardRuntime *d) {
    d->view = DASHBOARD_VIEW_SETTINGS;
    /* TODO: query settings table and populate UI */
    return d->settings_db_active;
}

bool dashboard_change_setting(DashboardRuntime *d,
                              const uint8_t *group_id,
                              const char *key,
                              const char *value) {
    if (!d->settings_db_active || !key || !value) return false;

    duckdb_prepared_statement stmt = NULL;
    bool ok = false;
    if (group_id) {
        /* Per-group setting */
        if (duckdb_prepare(d->settings_conn,
                "INSERT INTO settings (group_id, key, value) VALUES ($1, $2, $3) "
                "ON CONFLICT (group_id, key) DO UPDATE SET value = $3;",
                &stmt) == DuckDBSuccess) {
            duckdb_bind_blob(stmt, 1, group_id, GR_HASH_LEN);
            duckdb_bind_varchar(stmt, 2, key);
            duckdb_bind_varchar(stmt, 3, value);
            duckdb_result res;
            ok = (duckdb_execute_prepared(stmt, &res) == DuckDBSuccess);
            duckdb_destroy_result(&res);
            duckdb_destroy_prepare(&stmt);
        }
    } else {
        /* Global setting */
        if (duckdb_prepare(d->settings_conn,
                "INSERT INTO global_settings (key, value) VALUES ($1, $2) "
                "ON CONFLICT (key) DO UPDATE SET value = $2;",
                &stmt) == DuckDBSuccess) {
            duckdb_bind_varchar(stmt, 1, key);
            duckdb_bind_varchar(stmt, 2, value);
            duckdb_result res;
            ok = (duckdb_execute_prepared(stmt, &res) == DuckDBSuccess);
            duckdb_destroy_result(&res);
            duckdb_destroy_prepare(&stmt);
        }
    }
    return ok;
}

bool dashboard_reset_settings(DashboardRuntime *d) {
    if (!d->settings_db_active) return false;
    duckdb_result r;
    duckdb_state st = duckdb_query(d->settings_conn,
        "DELETE FROM settings; DELETE FROM global_settings;", &r);
    duckdb_destroy_result(&r);
    return st == DuckDBSuccess;
}

bool dashboard_export_group(DashboardRuntime *d, uint32_t group_index) {
    if (group_index >= d->group_count) return false;
    DashboardGroup *g = &d->groups[group_index];

    if (g->db_path[0] == '\0') {
        fprintf(stderr, "[dashboard] No db_path for group %u\n", group_index);
        return false;
    }

    /* Read the group's DuckDB file */
    FILE *db_file = fopen(g->db_path, "rb");
    if (db_file == NULL) {
        fprintf(stderr, "[dashboard] Cannot open %s for export\n", g->db_path);
        return false;
    }

    fseek(db_file, 0, SEEK_END);
    long file_size = ftell(db_file);
    fseek(db_file, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(db_file);
        return false;
    }

    char export_path[4192];
    snprintf(export_path, sizeof(export_path), "%s.xz", g->db_path);

    FILE *out = fopen(export_path, "wb");
    if (!out) {
        fprintf(stderr, "[dashboard] Cannot write export to %s\n", export_path);
        fclose(db_file);
        return false;
    }

    uint8_t in_buf[4096];
    uint8_t out_buf[4096];

    /* LZMA compress */
    lzma_stream strm = LZMA_STREAM_INIT;
    lzma_ret ret = lzma_easy_encoder(&strm, 6, LZMA_CHECK_CRC64);
    if (ret != LZMA_OK) {
        fprintf(stderr, "[dashboard] LZMA encoder init failed\n");
        fclose(db_file);
        fclose(out);
        return false;
    }

    strm.next_in = NULL;
    strm.avail_in = 0;
    strm.next_out = out_buf;
    strm.avail_out = sizeof(out_buf);

    lzma_action action = LZMA_RUN;
    bool success = true;

    while (success) {
        // Read more input if buffer is empty
        if (strm.avail_in == 0 && action == LZMA_RUN) {
            size_t read_size = fread(in_buf, 1, sizeof(in_buf), db_file);
            strm.next_in = in_buf;
            strm.avail_in = read_size;

            if (read_size < sizeof(in_buf)) {
                if (feof(db_file)) {
                    action = LZMA_FINISH;
                } else {
                    fprintf(stderr, "[dashboard] Read error\n");
                    success = false;
                    break;
                }
            }
        }

        // Compress
        ret = lzma_code(&strm, action);

        // Write output if buffer is full or we're done
        if (strm.avail_out == 0 || ret == LZMA_STREAM_END) {
            size_t write_size = sizeof(out_buf) - strm.avail_out;
            if (write_size > 0) {
                if (fwrite(out_buf, 1, write_size, out) != write_size) {
                    fprintf(stderr, "[dashboard] Write error\n");
                    success = false;
                    break;
                }
                strm.next_out = out_buf;
                strm.avail_out = sizeof(out_buf);
            }
        }

        // Check for completion
        if (ret == LZMA_STREAM_END) {
            break;
        }

        if (ret != LZMA_OK) {
            fprintf(stderr, "[dashboard] LZMA compression failed: %d\n", ret);
            success = false;
            break;
        }
    }

    lzma_end(&strm);
    fclose(db_file);
    fclose(out);

    if (success) {
        printf("[dashboard] Exported group %u to %s\n", group_index, export_path);
    } else {
        remove(export_path);
    }

    return success;
}


/* ================================================================== */
/*  Recovery mode                                                      */
/* ================================================================== */

void dashboard_toggle_recovery_mode(DashboardRuntime *d) {
    d->recovery_mode = !d->recovery_mode;
    d->view = d->recovery_mode ? DASHBOARD_VIEW_RECOVERY : DASHBOARD_VIEW_NORMAL;

    if (d->recovery_mode) {
        /* Mark all kicked/banned groups as recovery_readonly */
        for (uint32_t i = 0; i < d->group_count; i++) {
            DashboardGroup *g = &d->groups[i];
            if (g->state == GROUP_STATE_REMOVED_KICKED ||
                g->state == GROUP_STATE_REMOVED_BANNED) {
                g->recovery_readonly = true;
            }
        }
    }

    printf("[dashboard] Recovery mode %s\n", d->recovery_mode ? "ON" : "OFF");
}

int dashboard_send_reconnect(DashboardRuntime *d,
                             uint32_t group_index,
                             const uint8_t peer_id[GR_PEER_ID_LEN]) {
    if (group_index >= d->group_count) return -1;
    DashboardGroup *g = &d->groups[group_index];
    if (!g->client) return -1;

    /* Look up peer's last-known endpoint from the registrar */
    gr_peer_t peer;
    gr_registrar_t *reg = yumi_client_get_registrar(g->client);
    if (!reg || gr_peer_get(reg, peer_id, &peer) != GR_OK)
        return -1;

    if (peer.ip[0] == '\0' || peer.port == 0) return -1;

    /* Build the reconnect packet: [4-byte magic][12-byte peer_id prefix] */
    uint8_t pkt[DASHBOARD_RECONNECT_PACKET_LEN];
    uint32_t magic = DASHBOARD_RECONNECT_MAGIC;
    memcpy(pkt, &magic, 4);
    memcpy(pkt + 4, peer_id, 12);

    /* Send via raw UDP */
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    int off = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

    struct sockaddr_in6 addr = {0};
    addr.sin6_family = AF_INET6;
    addr.sin6_port   = htons(peer.port);
    inet_pton(AF_INET6, peer.ip, &addr.sin6_addr);

    ssize_t sent = sendto(fd, pkt, sizeof(pkt), 0,
                          (struct sockaddr *)&addr, sizeof(addr));
    close(fd);

    if (sent == sizeof(pkt)) {
        printf("[dashboard] Reconnect signal sent to peer in group %u\n",
               group_index);
        return 0;
    }
    return -1;
}

void dashboard_handle_reconnect_request(DashboardRuntime *d,
                                        const uint8_t peer_id[GR_PEER_ID_LEN],
                                        uint32_t group_index) {
    if (group_index >= d->group_count) return;
    DashboardGroup *g = &d->groups[group_index];

    /* Filter: if group had >= 100 members AND peer is not a friend, drop */
    if (g->member_count >= DASHBOARD_RECONNECT_FILTER_THRESHOLD &&
        !dashboard_is_friend(d, peer_id)) {
        printf("[dashboard] Reconnect from non-friend in large group filtered out\n");
        return;
    }

    /* Store the reconnect notification in a pending IPC dialog so the
       dashboard frame renderer can show "Peer X wishes to reconnect" */
    d->pending_ipc.type       = IPC_FRIEND_ADD_PENDING;  /* reuse for notification */
    d->pending_ipc.slot_index = group_index;
    memcpy(d->pending_ipc.peer_id, peer_id, GR_PEER_ID_LEN);

    /* Check if peer is a friend and include display name */
    for (uint32_t i = 0; i < d->friend_count; i++) {
        if (memcmp(d->friends[i].peer_id, peer_id, GR_PEER_ID_LEN) == 0) {
            snprintf(d->pending_ipc.text, sizeof(d->pending_ipc.text),
                     "Friend \"%s\" wishes to reconnect to group %u",
                     d->friends[i].display_name, group_index);
            printf("[dashboard] %s\n", d->pending_ipc.text);
            return;
        }
    }

    snprintf(d->pending_ipc.text, sizeof(d->pending_ipc.text),
             "A peer wishes to reconnect to group %u", group_index);
    printf("[dashboard] %s\n", d->pending_ipc.text);
}

void dashboard_delete_removed_group(DashboardRuntime *d, uint32_t group_index) {
    if (group_index >= d->group_count) return;
    DashboardGroup *g = &d->groups[group_index];

    /* Only allow deletion of removed groups */
    if (g->state != GROUP_STATE_REMOVED_KICKED &&
        g->state != GROUP_STATE_REMOVED_BANNED) {
        fprintf(stderr, "[dashboard] Can only delete removed groups\n");
        return;
    }

    /* Close all slots for this group */
    for (uint32_t i = d->slot_count; i > 0; i--) {
        if (d->slots[i-1].group_index == group_index)
            dashboard_remove_slot(d, i-1);
    }

    if (g->client) {
        yumi_client_destroy(g->client);
        g->client = NULL;
    }

    /* TODO: delete DuckDB files from disk */
    printf("[dashboard] Deleted removed group %u\n", group_index);
    g->state = GROUP_STATE_DISCONNECTED;
}

/* ================================================================== */
/*  Friend system                                                      */
/* ================================================================== */

bool dashboard_is_friend(const DashboardRuntime *d,
                         const uint8_t peer_id[GR_PEER_ID_LEN]) {
    for (uint32_t i = 0; i < d->friend_count; i++) {
        if (memcmp(d->friends[i].peer_id, peer_id, GR_PEER_ID_LEN) == 0)
            return true;
    }
    return false;
}

void dashboard_request_add_friend(DashboardRuntime *d,
                                  uint32_t slot_index,
                                  const uint8_t peer_id[GR_PEER_ID_LEN]) {
    /* Stage the friend-add dialog */
    d->pending_ipc.type       = IPC_FRIEND_ADD_PENDING;
    d->pending_ipc.slot_index = slot_index;
    memcpy(d->pending_ipc.peer_id, peer_id, GR_PEER_ID_LEN);
    /* TODO: show confirmation dialog "Are you sure you want to add X as friend?" */
    printf("[dashboard] Friend-add requested by slot %u\n", slot_index);
}

bool dashboard_confirm_add_friend(DashboardRuntime *d,
                                  const uint8_t peer_id[GR_PEER_ID_LEN],
                                  const char *display_name) {
    if (dashboard_is_friend(d, peer_id)) return true; /* already a friend */
    if (!dashboard_reserve_friends(d, d->friend_count + 1)) return false;

    DashboardFriend *f = &d->friends[d->friend_count];
    memcpy(f->peer_id, peer_id, GR_PEER_ID_LEN);
    if (display_name)
        snprintf(f->display_name, sizeof(f->display_name), "%s", display_name);
    else
        f->display_name[0] = '\0';
    f->added_at = (int64_t)time(NULL);
    d->friend_count++;

    /* Persist to DB */
    if (d->settings_db_active) {
        duckdb_prepared_statement stmt = NULL;
        if (duckdb_prepare(d->settings_conn,
                "INSERT INTO friends (peer_id, display_name, added_at) "
                "VALUES ($1, $2, $3) ON CONFLICT DO NOTHING;",
                &stmt) == DuckDBSuccess) {
            duckdb_bind_blob(stmt, 1, f->peer_id, GR_PEER_ID_LEN);
            if (display_name)
                duckdb_bind_varchar(stmt, 2, display_name);
            else
                duckdb_bind_null(stmt, 2);
            duckdb_bind_int64(stmt, 3, f->added_at);
            duckdb_result res;
            duckdb_execute_prepared(stmt, &res);
            duckdb_destroy_result(&res);
            duckdb_destroy_prepare(&stmt);
        }
    }

    printf("[dashboard] Added friend (count=%u)\n", d->friend_count);
    return true;
}

void dashboard_remove_friend(DashboardRuntime *d,
                             const uint8_t peer_id[GR_PEER_ID_LEN]) {
    for (uint32_t i = 0; i < d->friend_count; i++) {
        if (memcmp(d->friends[i].peer_id, peer_id, GR_PEER_ID_LEN) == 0) {
            /* Shift remaining */
            for (uint32_t j = i; j + 1 < d->friend_count; j++)
                d->friends[j] = d->friends[j + 1];
            d->friend_count--;

            /* Remove from DB */
            if (d->settings_db_active) {
                duckdb_prepared_statement stmt = NULL;
                if (duckdb_prepare(d->settings_conn,
                        "DELETE FROM friends WHERE peer_id = $1;",
                        &stmt) == DuckDBSuccess) {
                    duckdb_bind_blob(stmt, 1, peer_id, GR_PEER_ID_LEN);
                    duckdb_result res;
                    duckdb_execute_prepared(stmt, &res);
                    duckdb_destroy_result(&res);
                    duckdb_destroy_prepare(&stmt);
                }
            }
            printf("[dashboard] Removed friend (count=%u)\n", d->friend_count);
            return;
        }
    }
}

void dashboard_request_remove_friend(DashboardRuntime *d,
                                     uint32_t slot_index,
                                     const uint8_t peer_id[GR_PEER_ID_LEN]) {
    (void)slot_index;
    /* TODO: show confirmation dialog before removing */
    dashboard_remove_friend(d, peer_id);
}

void dashboard_send_friend_list(DashboardRuntime *d, uint32_t slot_index) {
    (void)d; (void)slot_index;
    /* TODO: call guest's on_friend_list_result export with peer ID array */
}

/* ================================================================== */
/*  Group tab notifications                                            */
/* ================================================================== */

void dashboard_group_notify(DashboardRuntime *d,
                            uint32_t group_index,
                            const char *text, uint32_t text_len,
                            const void *img,  uint32_t img_len,
                            uint32_t flags) {
    (void)d; (void)group_index;
    (void)text; (void)text_len;
    (void)img;  (void)img_len;
    (void)flags;
    /* TODO: display notification on the group's tab */
}

/* ================================================================== */
/*  Folder scanning                                                    */
/* ================================================================== */

int dashboard_folder_scan_next(DashboardRuntime *d,
                               uint32_t scan_id,
                               uint32_t slot_index,
                               uint32_t out_wasm_ptr) {
    (void)d; (void)scan_id; (void)slot_index; (void)out_wasm_ptr;
    /* TODO: read next directory entry into WASM linear memory */
    return 0; /* 0 = done */
}

void dashboard_folder_scan_close(DashboardRuntime *d, uint32_t scan_id) {
    if (scan_id >= DASHBOARD_MAX_FOLDER_SCANS) return;
    DashboardFolderScan *s = &d->folder_scans[scan_id];
    if (!s->active) return;
    if (s->dir_handle) {
        closedir((DIR *)s->dir_handle);
        s->dir_handle = NULL;
    }
    s->active = false;
}

/* ================================================================== */
/*  Profile / social updates                                           */
/* ================================================================== */

void dashboard_update_profile_picture(DashboardRuntime *d,
                                      uint32_t group_index,
                                      const void *data, uint32_t len) {
    (void)d; (void)group_index; (void)data; (void)len;
    /* TODO: write to group registrar */
}

void dashboard_update_status(DashboardRuntime *d,
                             uint32_t group_index,
                             const char *status) {
    (void)d; (void)group_index; (void)status;
}

void dashboard_update_bios(DashboardRuntime *d,
                           uint32_t group_index,
                           const char *bios) {
    (void)d; (void)group_index; (void)bios;
}

void dashboard_update_quotes(DashboardRuntime *d,
                             uint32_t group_index,
                             const char *quotes) {
    (void)d; (void)group_index; (void)quotes;
}

void dashboard_update_activity(DashboardRuntime *d,
                               uint32_t group_index,
                               const char *activity) {
    (void)d; (void)group_index; (void)activity;
}

void dashboard_add_emoji(DashboardRuntime *d,
                         uint32_t group_index,
                         const char *emoji) {
    (void)d; (void)group_index; (void)emoji;
}

void dashboard_remove_emoji(DashboardRuntime *d,
                            uint32_t group_index,
                            const char *emoji) {
    (void)d; (void)group_index; (void)emoji;
}

/* ================================================================== */
/*  Group / webapp management (see sdk/wasm_dashboard.h)               */
/* ================================================================== */

/* YumiGroupInfo field offsets within a YUMI_GROUP_INFO_SIZE record. */
#define GI_OFF_ID            0
#define GI_OFF_STATE         32
#define GI_OFF_FLAGS         36
#define GI_OFF_MEMBER_COUNT  40
#define GI_OFF_WEBAPP_COUNT  44
#define GI_OFF_NAME          48

/* YumiWebAppInfo field offsets within a YUMI_WEBAPP_INFO_SIZE record. */
#define WI_OFF_ID            0
#define WI_OFF_FLAGS         16
#define WI_OFF_SLOT_INDEX    20
#define WI_OFF_DB_SIZE       24
#define WI_OFF_DB_QUOTA      32
#define WI_OFF_NAME          40

/* Flags (must match sdk/wasm_dashboard.h). */
#define YUMI_GROUP_FLAG_CONNECTED   (1u << 0)
#define YUMI_GROUP_FLAG_BACKGROUND  (1u << 1)
#define YUMI_GROUP_FLAG_RECOVERY    (1u << 2)
#define YUMI_GROUP_FLAG_DB_OPEN     (1u << 3)
#define YUMI_WEBAPP_FLAG_ALLOWED    (1u << 0)
#define YUMI_WEBAPP_FLAG_RUNNING    (1u << 1)
#define YUMI_WEBAPP_FLAG_BACKGROUND (1u << 2)
#define YUMI_WEBAPP_FLAG_SELF       (1u << 3)

int32_t dashboard_set_slot_db_quota(DashboardRuntime *d,
                                    uint32_t slot_index,
                                    uint64_t quota_bytes) {
    if (slot_index >= d->slot_count) return -1;
    WebAppRuntime *rt = d->slots[slot_index].rt;
    if (!rt) return -1;

    if (quota_bytes == 0 /* unlimited */ ||
        quota_bytes > DASHBOARD_DB_QUOTA_HARD_MAX) {
        quota_bytes = DASHBOARD_DB_QUOTA_HARD_MAX;
    }

    webapp_runtime_set_db_quota(rt, quota_bytes);
    webapp_runtime_notify_db_quota_changed(rt, quota_bytes);
    printf("[dashboard] Slot %u DuckDB quota -> %llu bytes\n",
           slot_index, (unsigned long long)quota_bytes);
    return 0;
}

/* Access a WASM linear-memory byte pointer through a WebAppRuntime.
 * Uses the same access pattern as webapp_runtime.c's wasm_mem / wasm_mem_check;
 * we duplicate it locally to avoid having to expose those internal helpers. */
static uint8_t *rt_mem_ptr(WebAppRuntime *rt, uint32_t ptr, uint32_t len) {
    if (!rt || !rt->memory) return NULL;
    uint8_t *base = (uint8_t *)wasm_memory_data(rt->memory);
    size_t   sz   = wasm_memory_data_size(rt->memory);
    if ((size_t)ptr + (size_t)len > sz) return NULL;
    return base + ptr;
}

static uint32_t dashboard_group_flags(const DashboardGroup *g) {
    uint32_t f = 0;
    if (g->state == GROUP_STATE_CONNECTED) f |= YUMI_GROUP_FLAG_CONNECTED;
    if (g->background)                     f |= YUMI_GROUP_FLAG_BACKGROUND;
    if (g->recovery_readonly)              f |= YUMI_GROUP_FLAG_RECOVERY;
    if (g->client)                         f |= YUMI_GROUP_FLAG_DB_OPEN;
    return f;
}

static uint32_t dashboard_count_webapps_in_group(const DashboardRuntime *d,
                                                 uint32_t group_index) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->slot_count; i++) {
        if (d->slots[i].group_index == group_index) n++;
    }
    return n;
}

int32_t dashboard_fill_group_info(DashboardRuntime *d,
                                  WebAppRuntime *rt,
                                  uint32_t out_ptr,
                                  uint32_t out_cap) {
    uint32_t total   = d->group_count;
    uint32_t to_copy = (out_cap < total) ? out_cap : total;

    if (out_ptr == 0 || to_copy == 0) return (int32_t)total;

    uint8_t *base = rt_mem_ptr(rt, out_ptr,
                               (uint32_t)(to_copy * YUMI_GROUP_INFO_SIZE));
    if (!base) return -1;

    memset(base, 0, (size_t)to_copy * YUMI_GROUP_INFO_SIZE);
    for (uint32_t i = 0; i < to_copy; i++) {
        const DashboardGroup *g = &d->groups[i];
        uint8_t *rec = base + (size_t)i * YUMI_GROUP_INFO_SIZE;

        memcpy(rec + GI_OFF_ID, g->group_id, YUMI_GROUP_ID_LEN);

        uint32_t state    = (uint32_t)g->state;
        uint32_t flags    = dashboard_group_flags(g);
        uint32_t members  = g->member_count;
        uint32_t webapps  = dashboard_count_webapps_in_group(d, i);

        memcpy(rec + GI_OFF_STATE,        &state,   4);
        memcpy(rec + GI_OFF_FLAGS,        &flags,   4);
        memcpy(rec + GI_OFF_MEMBER_COUNT, &members, 4);
        memcpy(rec + GI_OFF_WEBAPP_COUNT, &webapps, 4);

        /* name: 64-byte NUL-padded */
        size_t nlen = strnlen(g->name, sizeof(g->name));
        if (nlen > 64) nlen = 64;
        memcpy(rec + GI_OFF_NAME, g->name, nlen);
    }
    return (int32_t)total;
}

int32_t dashboard_fill_webapp_info(DashboardRuntime *d,
                                   WebAppRuntime *rt,
                                   uint32_t group_index,
                                   uint32_t out_ptr,
                                   uint32_t out_cap) {
    if (group_index >= d->group_count) return -1;

    /* Count first. */
    uint32_t total = dashboard_count_webapps_in_group(d, group_index);
    uint32_t to_copy = (out_cap < total) ? out_cap : total;

    if (out_ptr == 0 || to_copy == 0) return (int32_t)total;

    uint8_t *base = rt_mem_ptr(rt, out_ptr,
                               (uint32_t)(to_copy * YUMI_WEBAPP_INFO_SIZE));
    if (!base) return -1;

    memset(base, 0, (size_t)to_copy * YUMI_WEBAPP_INFO_SIZE);

    uint32_t written = 0;
    for (uint32_t i = 0; i < d->slot_count && written < to_copy; i++) {
        const WebAppSlot *s = &d->slots[i];
        if (s->group_index != group_index) continue;

        uint8_t *rec = base + (size_t)written * YUMI_WEBAPP_INFO_SIZE;
        memcpy(rec + WI_OFF_ID, s->app_id, YUMI_WEBAPP_ID_LEN);

        uint32_t flags = YUMI_WEBAPP_FLAG_ALLOWED;
        if (s->rt)          flags |= YUMI_WEBAPP_FLAG_RUNNING;
        if (d->groups[group_index].background)
                            flags |= YUMI_WEBAPP_FLAG_BACKGROUND;
        if (s->rt == rt)    flags |= YUMI_WEBAPP_FLAG_SELF;

        uint32_t slot_idx = i;
        uint64_t db_size  = s->rt ? webapp_runtime_get_db_size(s->rt) : 0;
        uint64_t db_quota = s->rt ? webapp_runtime_get_db_quota(s->rt) : 0;

        memcpy(rec + WI_OFF_FLAGS,      &flags,    4);
        memcpy(rec + WI_OFF_SLOT_INDEX, &slot_idx, 4);
        memcpy(rec + WI_OFF_DB_SIZE,    &db_size,  8);
        memcpy(rec + WI_OFF_DB_QUOTA,   &db_quota, 8);

        size_t nlen = strnlen(s->app_name, sizeof(s->app_name));
        if (nlen > 64) nlen = 64;
        memcpy(rec + WI_OFF_NAME, s->app_name, nlen);

        written++;
    }
    return (int32_t)total;
}

int32_t dashboard_switch_group(DashboardRuntime *d, uint32_t group_index) {
    if (group_index >= d->group_count) return -1;
    if (d->foreground_group == group_index) return 0;

    /* Mark previous foreground as background. */
    if (d->foreground_group < d->group_count) {
        dashboard_group_set_background(d, d->foreground_group, true);
    }

    d->foreground_group = group_index;
    d->groups[group_index].background = false;

    /* Notify every running webapp in the newly-foregrounded group. */
    const uint8_t *gid = d->groups[group_index].group_id;
    for (uint32_t i = 0; i < d->slot_count; i++) {
        if (d->slots[i].group_index == group_index && d->slots[i].rt) {
            webapp_runtime_notify_group_switched(d->slots[i].rt, gid);
            webapp_runtime_notify_group_background(d->slots[i].rt, gid, false);
        }
    }
    printf("[dashboard] Foreground group -> %u\n", group_index);
    return 0;
}

int32_t dashboard_group_set_background(DashboardRuntime *d,
                                       uint32_t group_index,
                                       bool background) {
    if (group_index >= d->group_count) return -1;
    if (d->groups[group_index].background == background) return 0;

    d->groups[group_index].background = background;
    if (background && d->foreground_group == group_index) {
        d->foreground_group = UINT32_MAX;
    }

    const uint8_t *gid = d->groups[group_index].group_id;
    for (uint32_t i = 0; i < d->slot_count; i++) {
        if (d->slots[i].group_index == group_index && d->slots[i].rt) {
            webapp_runtime_notify_group_background(
                d->slots[i].rt, gid, background);
        }
    }
    printf("[dashboard] Group %u background = %d\n",
           group_index, (int)background);
    return 0;
}

/* ================================================================== */
/*  Group Registrar proxy (surface-level)                              */
/* ================================================================== */

/* Resolve (registrar, signer) for a group.  Returns true on success. */
static bool gr_resolve(DashboardRuntime *d, uint32_t gi,
                       gr_registrar_t **reg_out,
                       const gr_identity_t **signer_out) {
    if (!d || gi >= d->group_count) return false;
    yumi_client_t *c = d->groups[gi].client;
    if (!c) return false;
    gr_registrar_t *reg = yumi_client_get_registrar(c);
    if (!reg) return false;
    *reg_out = reg;
    if (signer_out) {
        const gr_identity_t *id = yumi_client_get_identity(c);
        if (!id) return false;
        *signer_out = id;
    }
    return true;
}

/* Helper: copy a NUL-bounded string into a fixed-size byte buffer,
 * NUL-padded.  out_len is the total record field length. */
static void str_copy_fixed(uint8_t *out, size_t out_len,
                           const char *src, size_t src_max) {
    memset(out, 0, out_len);
    if (!src) return;
    size_t n = strnlen(src, src_max);
    if (n > out_len) n = out_len;
    memcpy(out, src, n);
}

/* ── Peer marshalling ──────────────────────────────────────────── */

static void peer_marshal(const gr_peer_t *p, uint8_t *rec /* 280 */) {
    memset(rec, 0, 280);
    memcpy(rec +   0, p->peer_id,    GR_PEER_ID_LEN);
    uint32_t status = (uint32_t)p->status;
    memcpy(rec +  32, &status,       4);
    memcpy(rec +  36, &p->role_id,   4);
    memcpy(rec +  40, &p->joined_at, 8);
    memcpy(rec +  48, &p->removed_at,8);
    memcpy(rec +  56, &p->last_seen, 8);
    memcpy(rec +  64, p->removed_by, GR_PEER_ID_LEN);
    str_copy_fixed(rec + 96, 48, p->ip, GR_MAX_IP_LEN);
    memcpy(rec + 144, &p->port, 2);
    /* rec+146..151 = pad */
    str_copy_fixed(rec + 152, 128, p->removed_reason, GR_MAX_NAME_LEN);
}

int32_t dashboard_gr_peer_kick(DashboardRuntime *d, uint32_t gi,
                               const uint8_t peer_id[GR_PEER_ID_LEN],
                               const char *reason) {
    gr_registrar_t *reg; const gr_identity_t *me;
    if (!gr_resolve(d, gi, &reg, &me)) return -1;
    gr_error_t e = gr_peer_kick(reg, peer_id, reason, me);
    if (e == GR_OK)
        dashboard_gr_emit_change(d, gi, GR_CHANGE_PEER_KICKED,
                                 me->peer_id, peer_id, reason);
    return (int32_t)e;
}

int32_t dashboard_gr_peer_ban(DashboardRuntime *d, uint32_t gi,
                              const uint8_t peer_id[GR_PEER_ID_LEN],
                              const char *reason) {
    gr_registrar_t *reg; const gr_identity_t *me;
    if (!gr_resolve(d, gi, &reg, &me)) return -1;
    gr_error_t e = gr_peer_ban(reg, peer_id, reason, me);
    if (e == GR_OK)
        dashboard_gr_emit_change(d, gi, GR_CHANGE_PEER_BANNED,
                                 me->peer_id, peer_id, reason);
    return (int32_t)e;
}

int32_t dashboard_gr_peer_touch(DashboardRuntime *d, uint32_t gi,
                                const uint8_t peer_id[GR_PEER_ID_LEN]) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    /* Touch is a last-seen update — not broadcast as a GR change. */
    return (int32_t)gr_peer_touch(reg, peer_id);
}

int32_t dashboard_gr_peer_set_role(DashboardRuntime *d, uint32_t gi,
                                   const uint8_t peer_id[GR_PEER_ID_LEN],
                                   uint32_t role_id) {
    gr_registrar_t *reg; const gr_identity_t *me;
    if (!gr_resolve(d, gi, &reg, &me)) return -1;
    gr_error_t e = gr_peer_set_role(reg, peer_id, role_id, me);
    if (e == GR_OK)
        dashboard_gr_emit_change(d, gi, GR_CHANGE_PEER_ROLE_CHANGED,
                                 me->peer_id, peer_id, NULL);
    return (int32_t)e;
}

int32_t dashboard_gr_peer_get(DashboardRuntime *d, uint32_t gi,
                              const uint8_t peer_id[GR_PEER_ID_LEN],
                              uint8_t out_record[280]) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    gr_peer_t p;
    gr_error_t e = gr_peer_get(reg, peer_id, &p);
    if (e != GR_OK) return (int32_t)e;
    peer_marshal(&p, out_record);
    return 0;
}

int32_t dashboard_gr_peer_count(DashboardRuntime *d, uint32_t gi,
                                int32_t status_filter) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    uint32_t n = 0;
    if (gr_peer_count(reg, (gr_peer_status_t)status_filter, &n) != GR_OK)
        return -1;
    return (int32_t)n;
}

int32_t dashboard_gr_peer_list(DashboardRuntime *d, uint32_t gi,
                               int32_t status_filter,
                               uint8_t *out_records, uint32_t out_cap) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;

    /* Always return the true total. */
    uint32_t total = 0;
    if (gr_peer_count(reg, (gr_peer_status_t)status_filter, &total) != GR_OK)
        return -1;
    if (!out_records || out_cap == 0) return (int32_t)total;

    uint32_t to_copy = (out_cap < total) ? out_cap : total;
    gr_peer_t *buf = calloc(to_copy, sizeof(gr_peer_t));
    if (!buf) return -1;

    uint32_t got = 0;
    gr_error_t e = gr_peer_list(reg, buf, to_copy, &got,
                                (gr_peer_status_t)status_filter);
    if (e != GR_OK) { free(buf); return (int32_t)e; }

    for (uint32_t i = 0; i < got; i++)
        peer_marshal(&buf[i], out_records + (size_t)i * 280);
    free(buf);
    return (int32_t)total;
}

int32_t dashboard_gr_peer_is_authorized(DashboardRuntime *d, uint32_t gi,
                                        const uint8_t peer_id[GR_PEER_ID_LEN]) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    return gr_peer_is_authorized(reg, peer_id) ? 1 : 0;
}

/* ── Role marshalling ──────────────────────────────────────────── */

static void role_marshal(const gr_role_t *r, uint8_t *rec /* 152 */) {
    memset(rec, 0, 152);
    memcpy(rec +  0, &r->role_id,     4);
    memcpy(rec +  4, &r->permissions, 4);
    memcpy(rec +  8, &r->created_at,  8);
    memcpy(rec + 16, &r->modified_at, 8);
    str_copy_fixed(rec + 24, 128, r->name, GR_MAX_NAME_LEN);
}

int32_t dashboard_gr_role_add(DashboardRuntime *d, uint32_t gi,
                              const char *name, uint32_t permissions,
                              uint32_t *role_id_out) {
    gr_registrar_t *reg; const gr_identity_t *me;
    if (!gr_resolve(d, gi, &reg, &me)) return -1;
    gr_error_t e = gr_role_add(reg, name, permissions, me, role_id_out);
    if (e == GR_OK)
        dashboard_gr_emit_change(d, gi, GR_CHANGE_ROLE_ADDED,
                                 me->peer_id, NULL, name);
    return (int32_t)e;
}

int32_t dashboard_gr_role_remove(DashboardRuntime *d, uint32_t gi,
                                 uint32_t role_id) {
    gr_registrar_t *reg; const gr_identity_t *me;
    if (!gr_resolve(d, gi, &reg, &me)) return -1;
    gr_error_t e = gr_role_remove(reg, role_id, me);
    if (e == GR_OK)
        dashboard_gr_emit_change(d, gi, GR_CHANGE_ROLE_REMOVED,
                                 me->peer_id, NULL, NULL);
    return (int32_t)e;
}

int32_t dashboard_gr_role_set_permissions(DashboardRuntime *d, uint32_t gi,
                                          uint32_t role_id,
                                          uint32_t permissions) {
    gr_registrar_t *reg; const gr_identity_t *me;
    if (!gr_resolve(d, gi, &reg, &me)) return -1;
    gr_error_t e = gr_role_set_permissions(reg, role_id, permissions, me);
    if (e == GR_OK)
        dashboard_gr_emit_change(d, gi, GR_CHANGE_ROLE_MODIFIED,
                                 me->peer_id, NULL, NULL);
    return (int32_t)e;
}

int32_t dashboard_gr_role_get(DashboardRuntime *d, uint32_t gi,
                              uint32_t role_id, uint8_t out_record[152]) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    gr_role_t r;
    gr_error_t e = gr_role_get(reg, role_id, &r);
    if (e != GR_OK) return (int32_t)e;
    role_marshal(&r, out_record);
    return 0;
}

int32_t dashboard_gr_role_list(DashboardRuntime *d, uint32_t gi,
                               uint8_t *out_records, uint32_t out_cap) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    uint32_t total = 0;
    if (gr_role_count(reg, &total) != GR_OK) return -1;
    if (!out_records || out_cap == 0) return (int32_t)total;

    uint32_t to_copy = (out_cap < total) ? out_cap : total;
    gr_role_t *buf = calloc(to_copy, sizeof(gr_role_t));
    if (!buf) return -1;
    uint32_t got = 0;
    gr_error_t e = gr_role_list(reg, buf, to_copy, &got);
    if (e != GR_OK) { free(buf); return (int32_t)e; }
    for (uint32_t i = 0; i < got; i++)
        role_marshal(&buf[i], out_records + (size_t)i * 152);
    free(buf);
    return (int32_t)total;
}

int32_t dashboard_gr_role_count(DashboardRuntime *d, uint32_t gi) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    uint32_t n = 0;
    if (gr_role_count(reg, &n) != GR_OK) return -1;
    return (int32_t)n;
}

int32_t dashboard_gr_has_permission(DashboardRuntime *d, uint32_t gi,
                                    uint32_t permission) {
    gr_registrar_t *reg; const gr_identity_t *me;
    if (!gr_resolve(d, gi, &reg, &me)) return -1;
    return gr_has_permission(reg, me, (gr_permission_t)permission) ? 1 : 0;
}

/* ── Group-registrar webapp marshalling ────────────────────────── */

static void gr_webapp_marshal(const gr_webapp_t *w, uint8_t *rec /* 304 */) {
    memset(rec, 0, 304);
    memcpy(rec +   0, w->hash,     GR_SERVICE_HASH_LEN);
    memcpy(rec + 128, &w->version, 4);
    memcpy(rec + 136, &w->added_at,8);
    memcpy(rec + 144, w->added_by, GR_PEER_ID_LEN);
    str_copy_fixed(rec + 176, 128, w->name, GR_MAX_NAME_LEN);
}

static void gr_webapp_unmarshal(const uint8_t *rec /* 304 */, gr_webapp_t *w) {
    memset(w, 0, sizeof(*w));
    memcpy(w->hash,     rec +   0, GR_SERVICE_HASH_LEN);
    memcpy(&w->version, rec + 128, 4);
    memcpy(&w->added_at,rec + 136, 8);
    memcpy(w->added_by, rec + 144, GR_PEER_ID_LEN);
    memcpy(w->name,     rec + 176, 128);
    w->name[GR_MAX_NAME_LEN - 1] = '\0';
}

int32_t dashboard_gr_webapp_add(DashboardRuntime *d, uint32_t gi,
                                const uint8_t *record) {
    gr_registrar_t *reg; const gr_identity_t *me;
    if (!gr_resolve(d, gi, &reg, &me)) return -1;
    gr_webapp_t w;
    gr_webapp_unmarshal(record, &w);
    gr_error_t e = gr_webapp_add(reg, &w, me);
    if (e == GR_OK)
        dashboard_gr_emit_change(d, gi, GR_CHANGE_WEBAPP_ADDED,
                                 me->peer_id, NULL, w.name);
    return (int32_t)e;
}

int32_t dashboard_gr_webapp_remove(DashboardRuntime *d, uint32_t gi,
                                   const uint8_t hash[128]) {
    gr_registrar_t *reg; const gr_identity_t *me;
    if (!gr_resolve(d, gi, &reg, &me)) return -1;
    gr_error_t e = gr_webapp_remove(reg, hash, me);
    if (e == GR_OK)
        dashboard_gr_emit_change(d, gi, GR_CHANGE_WEBAPP_REMOVED,
                                 me->peer_id, NULL, NULL);
    return (int32_t)e;
}

int32_t dashboard_gr_webapp_is_authorized(DashboardRuntime *d, uint32_t gi,
                                          const uint8_t hash[128]) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    return gr_webapp_is_authorized(reg, hash) ? 1 : 0;
}

int32_t dashboard_gr_webapp_list(DashboardRuntime *d, uint32_t gi,
                                 uint8_t *out_records, uint32_t out_cap) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    uint32_t total = 0;
    if (gr_webapp_count(reg, &total) != GR_OK) return -1;
    if (!out_records || out_cap == 0) return (int32_t)total;

    uint32_t to_copy = (out_cap < total) ? out_cap : total;
    gr_webapp_t *buf = calloc(to_copy, sizeof(gr_webapp_t));
    if (!buf) return -1;
    uint32_t got = 0;
    gr_error_t e = gr_webapp_list(reg, buf, to_copy, &got);
    if (e != GR_OK) { free(buf); return (int32_t)e; }
    for (uint32_t i = 0; i < got; i++)
        gr_webapp_marshal(&buf[i], out_records + (size_t)i * 304);
    for (uint32_t i = 0; i < got; i++) { gr_free(buf[i].perm_data); gr_free(buf[i].role_mask); }
    free(buf);
    return (int32_t)total;
}

int32_t dashboard_gr_webapp_count(DashboardRuntime *d, uint32_t gi) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    uint32_t n = 0;
    if (gr_webapp_count(reg, &n) != GR_OK) return -1;
    return (int32_t)n;
}

/* ── Server marshalling (keeps public keys, drops secret kem_sk) ── */

static void server_marshal(const gr_server_t *s, uint8_t *rec /* 4480 */) {
    memset(rec, 0, 4480);
    uint32_t type = (uint32_t)s->type;
    memcpy(rec +    0, &type, 4);
    str_copy_fixed(rec + 8, 48, s->ip, GR_MAX_IP_LEN);
    memcpy(rec +   56, &s->port, 2);
    memcpy(rec +   64, s->id_hash,        GR_HASH_LEN);
    memcpy(rec +  192, s->sign_key,       GR_PUBLIC_KEY_LEN);
    memcpy(rec + 2784, s->service_hash,   GR_SERVICE_HASH_LEN);
    memcpy(rec + 2912, s->content_kem_pk, GR_KEM_PUBLIC_KEY_LEN);
    /* content_kem_sk is intentionally NOT exposed. */
}

static void server_unmarshal(const uint8_t *rec, gr_server_t *s) {
    memset(s, 0, sizeof(*s));
    uint32_t type = 0;
    memcpy(&type, rec + 0, 4);
    s->type = (gr_server_type_t)type;
    memcpy(s->ip, rec + 8, 48);
    s->ip[GR_MAX_IP_LEN - 1] = '\0';
    memcpy(&s->port, rec + 56, 2);
    memcpy(s->id_hash,        rec +   64, GR_HASH_LEN);
    memcpy(s->sign_key,       rec +  192, GR_PUBLIC_KEY_LEN);
    memcpy(s->service_hash,   rec + 2784, GR_SERVICE_HASH_LEN);
    memcpy(s->content_kem_pk, rec + 2912, GR_KEM_PUBLIC_KEY_LEN);
    /* content_kem_sk left zero — webapps may not install keys. */
}

int32_t dashboard_gr_server_add(DashboardRuntime *d, uint32_t gi,
                                const uint8_t *record) {
    gr_registrar_t *reg; const gr_identity_t *me;
    if (!gr_resolve(d, gi, &reg, &me)) return -1;
    gr_server_t s;
    server_unmarshal(record, &s);
    gr_error_t e = gr_server_add(reg, &s, me);
    if (e == GR_OK)
        dashboard_gr_emit_change(d, gi, GR_CHANGE_SERVER_ADDED,
                                 me->peer_id, NULL, s.ip);
    return (int32_t)e;
}

int32_t dashboard_gr_server_remove(DashboardRuntime *d, uint32_t gi,
                                   const uint8_t id_hash[128]) {
    gr_registrar_t *reg; const gr_identity_t *me;
    if (!gr_resolve(d, gi, &reg, &me)) return -1;
    gr_error_t e = gr_server_remove(reg, id_hash, me);
    if (e == GR_OK)
        dashboard_gr_emit_change(d, gi, GR_CHANGE_SERVER_REMOVED,
                                 me->peer_id, NULL, NULL);
    return (int32_t)e;
}

int32_t dashboard_gr_server_list(DashboardRuntime *d, uint32_t gi,
                                 uint32_t server_type,
                                 uint8_t *out_records, uint32_t out_cap) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    uint32_t total = 0;
    if (gr_server_count(reg, (gr_server_type_t)server_type, &total) != GR_OK)
        return -1;
    if (!out_records || out_cap == 0) return (int32_t)total;

    uint32_t to_copy = (out_cap < total) ? out_cap : total;
    gr_server_t *buf = calloc(to_copy, sizeof(gr_server_t));
    if (!buf) return -1;
    uint32_t got = 0;
    gr_error_t e = gr_server_list(reg, (gr_server_type_t)server_type,
                                  buf, to_copy, &got);
    if (e != GR_OK) { free(buf); return (int32_t)e; }
    for (uint32_t i = 0; i < got; i++)
        server_marshal(&buf[i], out_records + (size_t)i * 4480);
    /* Wipe any residual secret bytes that gr_server_list may have
     * populated before we return to untrusted code. */
    memset(buf, 0, (size_t)to_copy * sizeof(gr_server_t));
    free(buf);
    return (int32_t)total;
}

int32_t dashboard_gr_server_count(DashboardRuntime *d, uint32_t gi,
                                  uint32_t server_type) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    uint32_t n = 0;
    if (gr_server_count(reg, (gr_server_type_t)server_type, &n) != GR_OK)
        return -1;
    return (int32_t)n;
}

/* ── Epoch rotate (no reads of epoch material allowed) ─────────── */

int32_t dashboard_gr_epoch_rotate(DashboardRuntime *d, uint32_t gi) {
    gr_registrar_t *reg; const gr_identity_t *me;
    if (!gr_resolve(d, gi, &reg, &me)) return -1;
    gr_error_t e = gr_epoch_rotate(reg, me);
    if (e == GR_OK)
        dashboard_gr_emit_change(d, gi, GR_CHANGE_EPOCH_ROTATED,
                                 me->peer_id, NULL, NULL);
    return (int32_t)e;
}

/* ── Retention ────────────────────────────────────────────────── */

int32_t dashboard_gr_retention_get(DashboardRuntime *d, uint32_t gi,
                                   int64_t out[3]) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    gr_retention_t r;
    gr_error_t e = gr_retention_get(reg, &r);
    if (e != GR_OK) return (int32_t)e;
    out[0] = r.message_retention_ms;
    out[1] = r.file_retention_ms;
    out[2] = r.registrar_max_bytes;
    return 0;
}

int32_t dashboard_gr_retention_set(DashboardRuntime *d, uint32_t gi,
                                   const int64_t policy[3]) {
    gr_registrar_t *reg; const gr_identity_t *me;
    if (!gr_resolve(d, gi, &reg, &me)) return -1;
    gr_retention_t r = {
        .message_retention_ms = policy[0],
        .file_retention_ms    = policy[1],
        .registrar_max_bytes  = policy[2],
    };
    gr_error_t e = gr_retention_set(reg, &r, me);
    if (e == GR_OK)
        dashboard_gr_emit_change(d, gi, GR_CHANGE_RETENTION_SET,
                                 me->peer_id, NULL, NULL);
    return (int32_t)e;
}

/* ── Group icon ────────────────────────────────────────────────── */

int32_t dashboard_gr_icon_set(DashboardRuntime *d, uint32_t gi,
                              const uint8_t *data, uint32_t data_len,
                              const char *mime,
                              uint16_t width, uint16_t height,
                              bool is_video,
                              const uint8_t *static_frame,
                              uint32_t static_frame_len) {
    gr_registrar_t *reg; const gr_identity_t *me;
    if (!gr_resolve(d, gi, &reg, &me)) return -1;
    gr_error_t e = gr_group_icon_set(reg, data, data_len, mime,
                                     width, height, is_video,
                                     static_frame, static_frame_len, me);
    if (e == GR_OK)
        dashboard_gr_emit_change(d, gi, GR_CHANGE_GROUP_ICON_SET,
                                 me->peer_id, NULL, mime);
    return (int32_t)e;
}

int32_t dashboard_gr_icon_get(DashboardRuntime *d, uint32_t gi,
                              uint8_t out_info[248],
                              uint8_t *data_out, uint32_t data_cap,
                              uint8_t *sf_out, uint32_t sf_cap) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    gr_group_icon_t icon = {0};
    gr_error_t e = gr_group_icon_get(reg, &icon);
    if (e != GR_OK) return (int32_t)e;

    memset(out_info, 0, 248);
    uint32_t dl  = (uint32_t)icon.data_len;
    uint32_t sfl = (uint32_t)icon.static_frame_len;
    uint32_t iv  = icon.is_video ? 1u : 0u;
    memcpy(out_info +   0, &dl,        4);
    memcpy(out_info +   4, &sfl,       4);
    memcpy(out_info +   8, &icon.width,  2);
    memcpy(out_info +  10, &icon.height, 2);
    memcpy(out_info +  12, &iv,        4);
    memcpy(out_info +  16, &icon.updated_at, 8);
    memcpy(out_info +  24, icon.content_hash, GR_HASH_LEN);
    memcpy(out_info + 152, icon.updated_by,   GR_PEER_ID_LEN);
    str_copy_fixed(out_info + 184, 64, icon.mime_type, sizeof(icon.mime_type));

    if (data_out && data_cap && icon.data) {
        size_t n = (icon.data_len < data_cap) ? icon.data_len : data_cap;
        memcpy(data_out, icon.data, n);
    }
    if (sf_out && sf_cap && icon.static_frame) {
        size_t n = (icon.static_frame_len < sf_cap)
                    ? icon.static_frame_len : sf_cap;
        memcpy(sf_out, icon.static_frame, n);
    }
    gr_group_icon_free(&icon);
    return 0;
}

int32_t dashboard_gr_icon_remove(DashboardRuntime *d, uint32_t gi) {
    gr_registrar_t *reg; const gr_identity_t *me;
    if (!gr_resolve(d, gi, &reg, &me)) return -1;
    gr_error_t e = gr_group_icon_remove(reg, me);
    if (e == GR_OK)
        dashboard_gr_emit_change(d, gi, GR_CHANGE_GROUP_ICON_REMOVED,
                                 me->peer_id, NULL, NULL);
    return (int32_t)e;
}

int32_t dashboard_gr_icon_hash(DashboardRuntime *d, uint32_t gi,
                               uint8_t hash_out[128]) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    return (int32_t)gr_group_icon_hash(reg, hash_out);
}

/* ── Invites ───────────────────────────────────────────────────── */

static void invite_marshal(const gr_invite_info_t *inv, uint8_t *rec /* 216 */) {
    memset(rec, 0, 216);
    memcpy(rec +   0, inv->verification_token, GR_HASH_LEN);
    memcpy(rec + 128, &inv->created_at, 8);
    memcpy(rec + 136, &inv->expires_at, 8);
    memcpy(rec + 144, inv->created_by, GR_PEER_ID_LEN);
    memcpy(rec + 176, inv->used_by,    GR_PEER_ID_LEN);
    uint32_t inval = inv->invalidated ? 1u : 0u;
    uint32_t used  = inv->used        ? 1u : 0u;
    memcpy(rec + 208, &inval, 4);
    memcpy(rec + 212, &used,  4);
}

int32_t dashboard_gr_invite_create(DashboardRuntime *d, uint32_t gi,
                                   int64_t expiry_ms,
                                   uint8_t *blob_out, uint32_t blob_cap,
                                   uint8_t verification_token_out[128]) {
    gr_registrar_t *reg; const gr_identity_t *me;
    if (!gr_resolve(d, gi, &reg, &me)) return -1;

    uint8_t *blob = NULL; size_t len = 0;
    uint8_t tok[GR_HASH_LEN];
    gr_error_t e = gr_invite_create(reg, me, expiry_ms, &blob, &len, tok);
    if (e != GR_OK) return (int32_t)e;

    memcpy(verification_token_out, tok, GR_HASH_LEN);
    if (blob_out && blob_cap) {
        size_t n = (len < blob_cap) ? len : blob_cap;
        memcpy(blob_out, blob, n);
    }
    int32_t actual = (int32_t)len;
    gr_free(blob);
    dashboard_gr_emit_change(d, gi, GR_CHANGE_INVITE_CREATED,
                             me->peer_id, NULL, NULL);
    return actual;
}

int32_t dashboard_gr_invite_invalidate(DashboardRuntime *d, uint32_t gi,
                                       const uint8_t token[128]) {
    gr_registrar_t *reg; const gr_identity_t *me;
    if (!gr_resolve(d, gi, &reg, &me)) return -1;
    gr_error_t e = gr_invite_invalidate(reg, token, me);
    if (e == GR_OK)
        dashboard_gr_emit_change(d, gi, GR_CHANGE_INVITE_INVALIDATED,
                                 me->peer_id, NULL, NULL);
    return (int32_t)e;
}

int32_t dashboard_gr_invite_check(DashboardRuntime *d, uint32_t gi,
                                  const uint8_t token[128]) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    bool valid = false;
    gr_error_t e = gr_invite_check(reg, token, &valid);
    if (e != GR_OK) return (int32_t)e;
    return valid ? 1 : 0;
}

int32_t dashboard_gr_invite_list(DashboardRuntime *d, uint32_t gi,
                                 uint8_t *out_records, uint32_t out_cap) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    uint32_t total = 0;
    if (gr_invite_count(reg, &total) != GR_OK) return -1;
    if (!out_records || out_cap == 0) return (int32_t)total;

    uint32_t to_copy = (out_cap < total) ? out_cap : total;
    gr_invite_info_t *buf = calloc(to_copy, sizeof(gr_invite_info_t));
    if (!buf) return -1;
    uint32_t got = 0;
    gr_error_t e = gr_invite_list(reg, buf, to_copy, &got);
    if (e != GR_OK) { free(buf); return (int32_t)e; }
    for (uint32_t i = 0; i < got; i++)
        invite_marshal(&buf[i], out_records + (size_t)i * 216);
    free(buf);
    return (int32_t)total;
}

int32_t dashboard_gr_invite_count(DashboardRuntime *d, uint32_t gi) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    uint32_t n = 0;
    if (gr_invite_count(reg, &n) != GR_OK) return -1;
    return (int32_t)n;
}

int32_t dashboard_gr_invite_parse(const uint8_t *blob, uint32_t blob_len,
                                  uint8_t out_ticket[312]) {
    gr_invite_ticket_t t;
    gr_error_t e = gr_invite_parse(blob, blob_len, &t);
    if (e != GR_OK) return (int32_t)e;

    memset(out_ticket, 0, 312);
    memcpy(out_ticket +   0, t.group_id, GR_HASH_LEN > 32 ? 32 : GR_HASH_LEN);
    /* Note: YumiGroupId is 32 bytes while GR_HASH_LEN is 128.
     * The first 32 bytes of the Skein-1024 group hash are used as the
     * stable handle throughout the webapp SDK. */
    uint32_t gt = (uint32_t)t.group_type;
    memcpy(out_ticket +  32, &gt,                 4);
    memcpy(out_ticket +  36, &t.bootstrap_count,  4);
    memcpy(out_ticket +  40, &t.signaling_count,  4);
    memcpy(out_ticket +  48, &t.expires_at,       8);
    memcpy(out_ticket +  56, t.verification_token, GR_HASH_LEN);
    str_copy_fixed(out_ticket + 184, 128, t.group_name, GR_MAX_NAME_LEN);
    return 0;
}

/* ═════════════════════════════════════════════════════════════════════
 *  Audit log proxy
 *
 *  Audit entries are delivered in a 600-byte wire layout matching
 *  @c YumiAuditEntry.  The ML-DSA signature is intentionally stripped
 *  — chain verification is performed host-side via
 *  @ref dashboard_gr_audit_verify_chain.
 * ═════════════════════════════════════════════════════════════════════ */

static void audit_entry_marshal(const gr_audit_entry_t *src,
                                uint8_t *rec /* 600 */) {
    memset(rec, 0, 600);
    memcpy(rec +   0, src->entry_hash, GR_HASH_LEN);
    memcpy(rec + 128, src->prev_hash,  GR_HASH_LEN);
    memcpy(rec + 256, &src->timestamp,    8);
    memcpy(rec + 264, &src->timestamp_ns, 8);
    uint32_t ct = (uint32_t)src->change_type;
    memcpy(rec + 272, &ct, 4);
    memcpy(rec + 276, &src->registrar_version, 4);
    memcpy(rec + 280, src->actor_id,  GR_PEER_ID_LEN);
    memcpy(rec + 312, src->target_id, GR_PEER_ID_LEN);
    memcpy(rec + 344, src->detail, 256);
    /* Signature deliberately NOT copied. */
}

int32_t dashboard_gr_audit_list(DashboardRuntime *d, uint32_t gi,
                                int64_t since_ms,
                                uint8_t *out_records, uint32_t out_cap) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    uint32_t total = 0;
    if (gr_audit_count(reg, &total) != GR_OK) return -1;
    if (!out_records || out_cap == 0) return (int32_t)total;

    uint32_t to_copy = (out_cap < total) ? out_cap : total;
    gr_audit_entry_t *buf = calloc(to_copy, sizeof(gr_audit_entry_t));
    if (!buf) return -1;
    uint32_t got = 0;
    gr_error_t e = gr_audit_list(reg, since_ms, buf, to_copy, &got);
    if (e != GR_OK) { free(buf); return (int32_t)e; }
    for (uint32_t i = 0; i < got; i++)
        audit_entry_marshal(&buf[i], out_records + (size_t)i * 600);
    /* Wipe residual signature bytes before returning to untrusted code. */
    memset(buf, 0, (size_t)to_copy * sizeof(gr_audit_entry_t));
    free(buf);
    return (int32_t)got;
}

int32_t dashboard_gr_audit_count(DashboardRuntime *d, uint32_t gi) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    uint32_t n = 0;
    if (gr_audit_count(reg, &n) != GR_OK) return -1;
    return (int32_t)n;
}

int32_t dashboard_gr_audit_verify_chain(DashboardRuntime *d, uint32_t gi,
                                        uint8_t out_record[32]) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    gr_audit_chain_result_t r = {0};
    if (gr_audit_verify_chain(reg, &r) != GR_OK) return -1;
    memset(out_record, 0, 32);
    memcpy(out_record +  0, &r.total_entries,     4);
    memcpy(out_record +  4, &r.verified_entries,  4);
    memcpy(out_record +  8, &r.invalid_hash,      4);
    memcpy(out_record + 12, &r.invalid_signature, 4);
    memcpy(out_record + 16, &r.unknown_actor,     4);
    memcpy(out_record + 20, &r.forks_detected,    4);
    memcpy(out_record + 24, &r.forks_resolved,    4);
    uint32_t hg = r.has_genesis ? 1u : 0u;
    memcpy(out_record + 28, &hg, 4);
    return 0;
}

static void fork_branch_marshal(const gr_audit_entry_t *src,
                                uint8_t *rec /* 176 */) {
    memset(rec, 0, 176);
    memcpy(rec +   0, src->entry_hash, GR_HASH_LEN);
    memcpy(rec + 128, &src->timestamp, 8);
    uint32_t ct = (uint32_t)src->change_type;
    memcpy(rec + 136, &ct, 4);
    memcpy(rec + 144, src->actor_id, GR_PEER_ID_LEN);
}

int32_t dashboard_gr_audit_list_forks(DashboardRuntime *d, uint32_t gi,
                                      uint8_t *out_records, uint32_t out_cap) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    if (!out_records || out_cap == 0) {
        uint32_t got = 0;
        gr_error_t e = gr_audit_list_forks(reg, NULL, 0, &got);
        if (e != GR_OK) return -1;
        return (int32_t)got;
    }
    gr_audit_fork_t *buf = calloc(out_cap, sizeof(gr_audit_fork_t));
    if (!buf) return -1;
    uint32_t got = 0;
    gr_error_t e = gr_audit_list_forks(reg, buf, out_cap, &got);
    if (e != GR_OK) { free(buf); return (int32_t)e; }
    for (uint32_t i = 0; i < got; i++) {
        uint8_t *dst = out_records + (size_t)i * 1544;
        memset(dst, 0, 1544);
        memcpy(dst +   0, buf[i].prev_hash, GR_HASH_LEN);
        memcpy(dst + 128, &buf[i].branch_count, 4);
        uint32_t bc = buf[i].branch_count;
        if (bc > GR_FORK_MAX_BRANCHES) bc = GR_FORK_MAX_BRANCHES;
        for (uint32_t j = 0; j < bc; j++)
            fork_branch_marshal(&buf[i].branches[j], dst + 136 + (size_t)j * 176);
    }
    memset(buf, 0, (size_t)out_cap * sizeof(gr_audit_fork_t));
    free(buf);
    return (int32_t)got;
}

/* ═════════════════════════════════════════════════════════════════════
 *  Behavioral analysis proxy
 * ═════════════════════════════════════════════════════════════════════ */

static void actor_burst_marshal(const gr_actor_burst_t *s, uint8_t *rec /* 32 */) {
    memset(rec, 0, 32);
    memcpy(rec +  0, &s->actions_in_window,   4);
    memcpy(rec +  4, &s->destructive_actions, 4);
    memcpy(rec +  8, &s->role_changes,        4);
    memcpy(rec + 12, &s->invites_created,     4);
    memcpy(rec + 16, &s->epoch_rotations,     4);
    memcpy(rec + 20, &s->actions_per_minute,  4);
    uint32_t bd = s->burst_detected ? 1u : 0u;
    memcpy(rec + 24, &bd, 4);
}

static void mutation_rate_marshal(const gr_mutation_rate_t *s, uint8_t *rec /* 24 */) {
    memset(rec, 0, 24);
    memcpy(rec +  0, &s->total_mutations,      4);
    memcpy(rec +  4, &s->distinct_actors,      4);
    memcpy(rec +  8, &s->mutations_per_minute, 4);
    memcpy(rec + 12, &s->mutations_per_actor,  4);
    uint32_t sd = s->swarm_detected ? 1u : 0u;
    memcpy(rec + 16, &sd, 4);
}

static void admin_score_marshal(const gr_admin_score_t *s, uint8_t *rec /* 32 */) {
    memset(rec, 0, 32);
    memcpy(rec +  0, &s->total_admin_actions,    4);
    memcpy(rec +  4, &s->kicks,                  4);
    memcpy(rec +  8, &s->bans,                   4);
    memcpy(rec + 12, &s->removes,                4);
    memcpy(rec + 16, &s->role_modifications,     4);
    memcpy(rec + 20, &s->permission_escalations, 4);
    memcpy(rec + 24, &s->destructive_ratio,      4);
    uint32_t ab = s->abuse_suspected ? 1u : 0u;
    memcpy(rec + 28, &ab, 4);
}

static void peer_churn_marshal(const gr_peer_churn_t *s, uint8_t *rec /* 32 */) {
    memset(rec, 0, 32);
    memcpy(rec +  0, &s->active_peers,      4);
    memcpy(rec +  4, &s->kicked_peers,      4);
    memcpy(rec +  8, &s->banned_peers,      4);
    memcpy(rec + 12, &s->left_peers,        4);
    memcpy(rec + 16, &s->joined_in_window,  4);
    memcpy(rec + 20, &s->removed_in_window, 4);
    memcpy(rec + 24, &s->churn_rate,        4);
    memcpy(rec + 28, &s->stale_count,       4);
}

static void delta_score_marshal(const gr_delta_score_t *s, uint8_t *rec /* 40 */) {
    memset(rec, 0, 40);
    uint64_t db = (uint64_t)s->delta_bytes;
    memcpy(rec +  0, &db, 8);
    memcpy(rec +  8, &s->entry_count,             4);
    memcpy(rec + 16, &s->offline_duration_ms,     8);
    memcpy(rec + 24, &s->bytes_per_offline_day,   4);
    memcpy(rec + 28, &s->entries_per_offline_day, 4);
    uint32_t an = s->anomalous ? 1u : 0u;
    memcpy(rec + 32, &an, 4);
}

static void epoch_pattern_marshal(const gr_epoch_pattern_t *s, uint8_t *rec /* 24 */) {
    memset(rec, 0, 24);
    memcpy(rec +  0, &s->rotations_in_window,   4);
    memcpy(rec +  8, &s->avg_epoch_lifetime_ms, 8);
    uint32_t er = s->excessive_rotation ? 1u : 0u;
    memcpy(rec + 16, &er, 4);
}

static void network_score_marshal(const gr_network_score_t *s, uint8_t *rec /* 32 */) {
    memset(rec, 0, 32);
    memcpy(rec +  0, &s->last_update_ms,        8);
    memcpy(rec +  8, &s->time_since_update_ms,  8);
    uint64_t erb = (uint64_t)s->estimated_registrar_bytes;
    memcpy(rec + 16, &erb, 8);
    memcpy(rec + 24, &s->total_audit_entries,   4);
    memcpy(rec + 28, &s->avg_entry_interval_ms, 4);
}

static void behavior_config_marshal(const gr_behavior_config_t *s,
                                    uint8_t *rec /* 40 */) {
    memset(rec, 0, 40);
    memcpy(rec +  0, &s->burst_actions_per_min,         4);
    memcpy(rec +  4, &s->swarm_mutations_per_min,       4);
    memcpy(rec +  8, &s->abuse_destructive_ratio,       4);
    memcpy(rec + 12, &s->abuse_min_actions,             4);
    memcpy(rec + 16, &s->epoch_max_per_hour,            4);
    memcpy(rec + 20, &s->delta_anomaly_entries_per_day, 4);
    memcpy(rec + 24, &s->churn_attention_threshold,     4);
    uint32_t sc = s->scale_by_group_size ? 1u : 0u;
    memcpy(rec + 28, &sc, 4);
    memcpy(rec + 32, &s->alert_window_ms,               8);
}

static void behavior_config_unmarshal(const uint8_t *rec /* 40 */,
                                      gr_behavior_config_t *out) {
    memset(out, 0, sizeof *out);
    memcpy(&out->burst_actions_per_min,         rec +  0, 4);
    memcpy(&out->swarm_mutations_per_min,       rec +  4, 4);
    memcpy(&out->abuse_destructive_ratio,       rec +  8, 4);
    memcpy(&out->abuse_min_actions,             rec + 12, 4);
    memcpy(&out->epoch_max_per_hour,            rec + 16, 4);
    memcpy(&out->delta_anomaly_entries_per_day, rec + 20, 4);
    memcpy(&out->churn_attention_threshold,     rec + 24, 4);
    uint32_t sc = 0;
    memcpy(&sc,                                 rec + 28, 4);
    out->scale_by_group_size = (sc != 0);
    memcpy(&out->alert_window_ms,               rec + 32, 8);
}

int32_t dashboard_gr_behavior_actor_burst(DashboardRuntime *d, uint32_t gi,
                                          const uint8_t actor[GR_PEER_ID_LEN],
                                          int64_t window_ms,
                                          uint8_t out[32]) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    gr_actor_burst_t b = {0};
    if (gr_behavior_actor_burst(reg, actor, window_ms, &b) != GR_OK) return -1;
    actor_burst_marshal(&b, out);
    return 0;
}

int32_t dashboard_gr_behavior_mutation_rate(DashboardRuntime *d, uint32_t gi,
                                            int64_t window_ms,
                                            uint8_t out[24]) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    gr_mutation_rate_t m = {0};
    if (gr_behavior_mutation_rate(reg, window_ms, &m) != GR_OK) return -1;
    mutation_rate_marshal(&m, out);
    return 0;
}

int32_t dashboard_gr_behavior_admin_score(DashboardRuntime *d, uint32_t gi,
                                          const uint8_t admin[GR_PEER_ID_LEN],
                                          int64_t window_ms,
                                          uint8_t out[32]) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    gr_admin_score_t a = {0};
    if (gr_behavior_admin_score(reg, admin, window_ms, &a) != GR_OK) return -1;
    admin_score_marshal(&a, out);
    return 0;
}

int32_t dashboard_gr_behavior_peer_churn(DashboardRuntime *d, uint32_t gi,
                                         int64_t window_ms,
                                         uint8_t out[32]) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    gr_peer_churn_t c = {0};
    if (gr_behavior_peer_churn(reg, window_ms, &c) != GR_OK) return -1;
    peer_churn_marshal(&c, out);
    return 0;
}

int32_t dashboard_gr_behavior_delta_score(DashboardRuntime *d, uint32_t gi,
                                          uint64_t delta_bytes,
                                          uint32_t entry_count,
                                          uint8_t out[40]) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    gr_delta_score_t s = {0};
    if (gr_behavior_delta_score(reg, (size_t)delta_bytes, entry_count, &s)
        != GR_OK) return -1;
    delta_score_marshal(&s, out);
    return 0;
}

int32_t dashboard_gr_behavior_stale_peers(DashboardRuntime *d, uint32_t gi,
                                          int64_t stale_threshold_ms,
                                          uint8_t *out_ids, uint32_t max_count) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    uint32_t got = 0;
    if (gr_behavior_stale_peers(reg, stale_threshold_ms,
                                out_ids, max_count, &got) != GR_OK)
        return -1;
    return (int32_t)got;
}

int32_t dashboard_gr_behavior_epoch_pattern(DashboardRuntime *d, uint32_t gi,
                                            int64_t window_ms,
                                            uint8_t out[24]) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    gr_epoch_pattern_t p = {0};
    if (gr_behavior_epoch_pattern(reg, window_ms, &p) != GR_OK) return -1;
    epoch_pattern_marshal(&p, out);
    return 0;
}

int32_t dashboard_gr_behavior_network_score(DashboardRuntime *d, uint32_t gi,
                                            uint8_t out[32]) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    gr_network_score_t n = {0};
    if (gr_behavior_network_score(reg, &n) != GR_OK) return -1;
    network_score_marshal(&n, out);
    return 0;
}

int32_t dashboard_gr_behavior_snapshot(DashboardRuntime *d, uint32_t gi,
                                       int64_t window_ms,
                                       uint8_t out[200]) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    gr_behavior_snapshot_t s = {0};
    if (gr_behavior_snapshot(reg, window_ms, &s) != GR_OK) return -1;
    memset(out, 0, 200);
    peer_churn_marshal   (&s.churn,         out +   0);
    mutation_rate_marshal(&s.mutation_rate, out +  32);
    epoch_pattern_marshal(&s.epoch_pattern, out +  56);
    network_score_marshal(&s.network,       out +  80);
    admin_score_marshal  (&s.worst_admin,   out + 112);
    memcpy(out + 144, s.worst_admin_id, GR_PEER_ID_LEN);
    uint32_t hw = s.has_worst_admin ? 1u : 0u;
    memcpy(out + 176, &hw, 4);
    uint64_t es = (uint64_t)s.estimated_size;
    memcpy(out + 184, &es, 8);
    uint32_t na = s.needs_attention ? 1u : 0u;
    memcpy(out + 192, &na, 4);
    return 0;
}

int32_t dashboard_gr_behavior_get_config(DashboardRuntime *d, uint32_t gi,
                                         uint8_t out[40]) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    gr_behavior_config_t c = {0};
    if (gr_behavior_get_config(reg, &c) != GR_OK) return -1;
    behavior_config_marshal(&c, out);
    return 0;
}

int32_t dashboard_gr_behavior_set_config(DashboardRuntime *d, uint32_t gi,
                                         const uint8_t in[40]) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    gr_behavior_config_t c;
    behavior_config_unmarshal(in, &c);
    return (int32_t)gr_behavior_set_config(reg, &c);
}

/* ═════════════════════════════════════════════════════════════════════
 *  IP blocklist proxy
 * ═════════════════════════════════════════════════════════════════════ */

int32_t dashboard_gr_blocklist_check(DashboardRuntime *d, uint32_t gi,
                                     const char *ip,
                                     int64_t block_duration_ms) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    bool blocked = false;
    if (gr_blocklist_check(reg, ip, block_duration_ms, &blocked) != GR_OK)
        return -1;
    return blocked ? 1 : 0;
}

int32_t dashboard_gr_blocklist_record_fail(DashboardRuntime *d, uint32_t gi,
                                           const char *ip,
                                           int32_t max_fails) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    bool just = false;
    if (gr_blocklist_record_fail(reg, ip, (int)max_fails, &just) != GR_OK)
        return -1;
    return just ? 1 : 0;
}

int32_t dashboard_gr_blocklist_reset(DashboardRuntime *d, uint32_t gi,
                                     const char *ip) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    return (int32_t)gr_blocklist_reset(reg, ip);
}

int32_t dashboard_gr_blocklist_cleanup(DashboardRuntime *d, uint32_t gi,
                                       int64_t block_duration_ms) {
    gr_registrar_t *reg;
    if (!gr_resolve(d, gi, &reg, NULL)) return -1;
    return (int32_t)gr_blocklist_cleanup(reg, block_duration_ms);
}

/* ═════════════════════════════════════════════════════════════════════
 *  Group-Registrar change broadcast
 *
 *  Every successful mutation issued through the dashboard_gr_* proxy
 *  fans the change out to all webapps running in the affected group.
 * ═════════════════════════════════════════════════════════════════════ */

static uint32_t gr_change_category(uint32_t change_type) {
    switch (change_type) {
    case GR_CHANGE_PEER_ADDED:
    case GR_CHANGE_PEER_REMOVED:
    case GR_CHANGE_PEER_KICKED:
    case GR_CHANGE_PEER_BANNED:
    case GR_CHANGE_PEER_ADDRESS:
    case GR_CHANGE_PEER_ROLE_CHANGED:   return 1;  /* PEER      */
    case GR_CHANGE_ROLE_ADDED:
    case GR_CHANGE_ROLE_REMOVED:
    case GR_CHANGE_ROLE_MODIFIED:       return 2;  /* ROLE      */
    case GR_CHANGE_WEBAPP_ADDED:
    case GR_CHANGE_WEBAPP_REMOVED:      return 3;  /* WEBAPP    */
    case GR_CHANGE_SERVER_ADDED:
    case GR_CHANGE_SERVER_REMOVED:      return 4;  /* SERVER    */
    case GR_CHANGE_EPOCH_ROTATED:       return 5;  /* EPOCH     */
    case GR_CHANGE_RETENTION_SET:       return 6;  /* RETENTION */
    case GR_CHANGE_REGISTRAR_SIGNED:    return 7;  /* REGISTRAR */
    case GR_CHANGE_INVITE_CREATED:
    case GR_CHANGE_INVITE_USED:
    case GR_CHANGE_INVITE_INVALIDATED:  return 8;  /* INVITE    */
    case GR_CHANGE_GROUP_ICON_SET:
    case GR_CHANGE_GROUP_ICON_REMOVED:  return 9;  /* ICON      */
    default:                            return 0;  /* UNKNOWN   */
    }
    /* Note: blocklist mutations are not recorded in the GR audit log,
     * so category 10 (BLOCKLIST) is currently reserved and only used by
     * host-originated alerts, not by dashboard_gr_emit_change. */
}

void dashboard_gr_emit_change(DashboardRuntime *d, uint32_t gi,
                              uint32_t change_type,
                              const uint8_t actor_id[GR_PEER_ID_LEN],
                              const uint8_t target_id[GR_PEER_ID_LEN],
                              const char *detail) {
    if (!d || gi >= d->group_count) return;

    /* Build a 600-byte YumiAuditEntry-shaped notification.  The
     * hash-chained authoritative entry (with entry_hash / prev_hash /
     * signature) is reachable via dashboard_gr_audit_list immediately
     * after this fires — callers that need the canonical record should
     * read it back rather than trust the live notification payload. */
    uint8_t entry[600];
    memset(entry, 0, sizeof entry);
    int64_t ts  = gr_timestamp_ms();
    int64_t tns = gr_timestamp_ns();
    memcpy(entry + 256, &ts,          8);
    memcpy(entry + 264, &tns,         8);
    memcpy(entry + 272, &change_type, 4);
    if (actor_id)  memcpy(entry + 280, actor_id,  GR_PEER_ID_LEN);
    if (target_id) memcpy(entry + 312, target_id, GR_PEER_ID_LEN);
    if (detail) {
        size_t n = strnlen(detail, 255);
        memcpy(entry + 344, detail, n);
    }

    uint32_t category = gr_change_category(change_type);
    const uint8_t *gid = d->groups[gi].group_id; /* 32-byte truncated handle */

    /* Notifying individual webapps of GR change events is the
     * responsibility of the group-registrar / networking subsystem,
     * not the webapp runtime.  The webapp runtime deliberately does
     * not carry an on_gr_change export any more. */
    (void)category;
    (void)gid;
    (void)entry;
}

/* ═════════════════════════════════════════════════════════════════════
 *  Restricted IPC import surface
 * ─────────────────────────────────────────────────────────────────────
 *  All WASM-visible host callbacks below form the two tiered import
 *  sets.  The WebApp runtime MUST NOT reference any of these symbols
 *  directly — it consumes them via @ref webapp_ipc_table() (regular
 *  webapp slots) or @ref dashboard_only_ipc_table() (the privileged
 *  dashboard slot).
 *
 *  Every callback takes the caller's WebAppRuntime as `env`.  Pointer
 *  arguments are WASM linear-memory offsets (u32); strings and
 *  structs are validated against the current memory size before any
 *  dereference.
 * ═════════════════════════════════════════════════════════════════════ */

static inline uint8_t *ipc_mem(WebAppRuntime *rt) {
    return rt && rt->memory ? (uint8_t *)wasm_memory_data(rt->memory) : NULL;
}
static inline size_t ipc_mem_size(WebAppRuntime *rt) {
    return rt && rt->memory ? wasm_memory_data_size(rt->memory) : 0;
}
static inline bool ipc_mem_check(WebAppRuntime *rt, uint32_t ptr, uint32_t len) {
    if (!rt || !rt->memory) return false;
    size_t sz = wasm_memory_data_size(rt->memory);
    return (size_t)ptr + (size_t)len <= sz;
}

/* Slot-resolution helpers. */
static uint32_t rt_self_slot(WebAppRuntime *rt) {
    if (!rt->dashboard) return UINT32_MAX;
    for (uint32_t i = 0; i < rt->dashboard->slot_count; i++) {
        if (rt->dashboard->slots[i].rt == rt) return i;
    }
    return UINT32_MAX;
}

/* Resolve a (possibly NULL) 32-byte group-id pointer to a host group
 * index.  A NULL pointer (0) means "caller's group".  The guest handle
 * is the truncated first 32 bytes of the 128-byte Skein-1024 hash. */
static uint32_t resolve_group_arg(WebAppRuntime *rt, uint32_t gid_ptr) {
    if (gid_ptr == 0) return rt->group_index;
    if (!rt->dashboard) return UINT32_MAX;
    if (!ipc_mem_check(rt, gid_ptr, YUMI_GROUP_ID_LEN)) return UINT32_MAX;
    const uint8_t *gid = ipc_mem(rt) + gid_ptr;
    for (uint32_t i = 0; i < rt->dashboard->group_count; i++) {
        if (memcmp(rt->dashboard->groups[i].group_id, gid,
                   YUMI_GROUP_ID_LEN) == 0)
            return i;
    }
    return UINT32_MAX;
}

static uint32_t resolve_slot_arg(WebAppRuntime *rt,
                                 uint32_t gid_ptr, uint32_t app_ptr) {
    uint32_t gi = resolve_group_arg(rt, gid_ptr);
    if (gi == UINT32_MAX || !rt->dashboard) return UINT32_MAX;

    const uint8_t *app_id;
    uint8_t self_id[YUMI_WEBAPP_ID_LEN];
    if (app_ptr == 0) {
        uint32_t slot = rt_self_slot(rt);
        if (slot == UINT32_MAX) return UINT32_MAX;
        memcpy(self_id, rt->dashboard->slots[slot].app_id, YUMI_WEBAPP_ID_LEN);
        app_id = self_id;
    } else {
        if (!ipc_mem_check(rt, app_ptr, YUMI_WEBAPP_ID_LEN)) return UINT32_MAX;
        app_id = ipc_mem(rt) + app_ptr;
    }

    for (uint32_t i = 0; i < rt->dashboard->slot_count; i++) {
        if (rt->dashboard->slots[i].group_index == gi &&
            memcmp(rt->dashboard->slots[i].app_id, app_id,
                   YUMI_WEBAPP_ID_LEN) == 0)
            return i;
    }
    return UINT32_MAX;
}

/* ── Clipboard / link / dialogs / friends / notify ────────────── */

static wasm_trap_t *host_dashboard_request_paste(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args; (void)res;
    WebAppRuntime *rt = env;
    uint32_t slot = rt_self_slot(rt);
    if (slot != UINT32_MAX) dashboard_handle_paste_request(rt->dashboard, slot);
    return NULL;
}

static wasm_trap_t *host_dashboard_request_copy(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WebAppRuntime *rt = env;
    if (!rt->dashboard) return NULL;
    uint32_t ptr = (uint32_t)args->data[0].of.i32;
    uint32_t len = (uint32_t)args->data[1].of.i32;
    if (len > 4095) len = 4095;
    if (!ipc_mem_check(rt, ptr, len)) return NULL;
    dashboard_handle_copy_request(rt->dashboard,
                                  (const char *)(ipc_mem(rt) + ptr), len);
    return NULL;
}

static wasm_trap_t *host_webapp_copy_link(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WebAppRuntime *rt = env;
    if (!rt->dashboard) return NULL;
    uint32_t ptr = (uint32_t)args->data[0].of.i32;
    uint32_t len = (uint32_t)args->data[1].of.i32;
    if (len > WEBAPP_LINK_BUF_SIZE) len = WEBAPP_LINK_BUF_SIZE;
    if (!ipc_mem_check(rt, ptr, len)) return NULL;
    dashboard_set_group_link(rt->dashboard, rt->group_index,
                             ipc_mem(rt) + ptr, len);
    return NULL;
}

static wasm_trap_t *host_webapp_paste_link(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    if (!rt->dashboard) {
        res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = 0 };
        return NULL;
    }
    uint32_t buf_ptr = (uint32_t)args->data[0].of.i32;
    uint32_t buf_len = (uint32_t)args->data[1].of.i32;
    const void *data; uint32_t data_len;
    if (dashboard_get_group_link(rt->dashboard, rt->group_index,
                                 &data, &data_len)) {
        uint32_t copy_len = data_len < buf_len ? data_len : buf_len;
        if (ipc_mem_check(rt, buf_ptr, copy_len))
            memcpy(ipc_mem(rt) + buf_ptr, data, copy_len);
        res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = (int32_t)data_len };
    } else {
        res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = 0 };
    }
    return NULL;
}

static wasm_trap_t *host_dashboard_open_file(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args; (void)res;
    WebAppRuntime *rt = env;
    uint32_t slot = rt_self_slot(rt);
    if (slot != UINT32_MAX) dashboard_open_file_dialog(rt->dashboard, slot);
    return NULL;
}

static wasm_trap_t *host_dashboard_save_file(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args; (void)res;
    WebAppRuntime *rt = env;
    uint32_t slot = rt_self_slot(rt);
    if (slot != UINT32_MAX) dashboard_save_file_dialog(rt->dashboard, slot);
    return NULL;
}

static wasm_trap_t *host_dashboard_open_folder(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args; (void)res;
    WebAppRuntime *rt = env;
    uint32_t slot = rt_self_slot(rt);
    if (slot != UINT32_MAX) dashboard_open_folder_dialog(rt->dashboard, slot);
    return NULL;
}

static wasm_trap_t *host_dashboard_add_friend(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WebAppRuntime *rt = env;
    if (!rt->dashboard) return NULL;
    uint32_t peer_id_ptr = (uint32_t)args->data[0].of.i32;
    if (!ipc_mem_check(rt, peer_id_ptr, GR_PEER_ID_LEN)) return NULL;
    uint32_t slot = rt_self_slot(rt);
    if (slot != UINT32_MAX)
        dashboard_request_add_friend(rt->dashboard, slot,
                                     ipc_mem(rt) + peer_id_ptr);
    return NULL;
}

static wasm_trap_t *host_dashboard_get_friend_list(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args; (void)res;
    WebAppRuntime *rt = env;
    uint32_t slot = rt_self_slot(rt);
    if (slot != UINT32_MAX) dashboard_send_friend_list(rt->dashboard, slot);
    return NULL;
}

static wasm_trap_t *host_dashboard_remove_friend(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WebAppRuntime *rt = env;
    if (!rt->dashboard) return NULL;
    uint32_t peer_id_ptr = (uint32_t)args->data[0].of.i32;
    if (!ipc_mem_check(rt, peer_id_ptr, GR_PEER_ID_LEN)) return NULL;
    uint32_t slot = rt_self_slot(rt);
    if (slot != UINT32_MAX)
        dashboard_request_remove_friend(rt->dashboard, slot,
                                        ipc_mem(rt) + peer_id_ptr);
    return NULL;
}

static wasm_trap_t *host_dashboard_group_notify_cb(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WebAppRuntime *rt = env;
    if (!rt->dashboard) return NULL;

    uint32_t text_ptr = (uint32_t)args->data[0].of.i32;
    uint32_t text_len = (uint32_t)args->data[1].of.i32;
    uint32_t img_ptr  = (uint32_t)args->data[2].of.i32;
    uint32_t img_len  = (uint32_t)args->data[3].of.i32;
    uint32_t flags    = (uint32_t)args->data[4].of.i32;

    if (text_len > DASHBOARD_NOTIFY_TEXT_MAX) text_len = DASHBOARD_NOTIFY_TEXT_MAX;
    if (!ipc_mem_check(rt, text_ptr, text_len)) return NULL;
    if (img_ptr && img_len) {
        if (!ipc_mem_check(rt, img_ptr, img_len)) return NULL;
    } else {
        img_ptr = 0;
        img_len = 0;
    }

    dashboard_group_notify(rt->dashboard,
                           rt->group_index,
                           (const char *)(ipc_mem(rt) + text_ptr), text_len,
                           img_ptr ? (ipc_mem(rt) + img_ptr) : NULL, img_len,
                           flags);
    return NULL;
}

static wasm_trap_t *host_dashboard_folder_next(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    if (!rt->dashboard) {
        res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = -1 };
        return NULL;
    }
    uint32_t scan_id = (uint32_t)args->data[0].of.i32;
    uint32_t out_ptr = (uint32_t)args->data[1].of.i32;
    if (!ipc_mem_check(rt, out_ptr, DASHBOARD_FOLDER_ENTRY_SIZE)) {
        res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = -1 };
        return NULL;
    }
    uint32_t slot = rt_self_slot(rt);
    int rc = dashboard_folder_scan_next(rt->dashboard, scan_id,
                                        slot == UINT32_MAX ? 0 : slot,
                                        out_ptr);
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_folder_close(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WebAppRuntime *rt = env;
    if (!rt->dashboard) return NULL;
    uint32_t scan_id = (uint32_t)args->data[0].of.i32;
    dashboard_folder_scan_close(rt->dashboard, scan_id);
    return NULL;
}

/* ── Group / webapp management ────────────────────────────────── */

static wasm_trap_t *host_dashboard_self_webapp_id(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WebAppRuntime *rt = env;
    uint32_t out_ptr = (uint32_t)args->data[0].of.i32;
    if (!ipc_mem_check(rt, out_ptr, YUMI_WEBAPP_ID_LEN)) return NULL;
    uint32_t slot = rt_self_slot(rt);
    const uint8_t *id = (slot != UINT32_MAX)
                     ? rt->dashboard->slots[slot].app_id : NULL;
    if (id) memcpy(ipc_mem(rt) + out_ptr, id, YUMI_WEBAPP_ID_LEN);
    else    memset(ipc_mem(rt) + out_ptr, 0, YUMI_WEBAPP_ID_LEN);
    return NULL;
}

static wasm_trap_t *host_dashboard_self_group_id(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WebAppRuntime *rt = env;
    uint32_t out_ptr = (uint32_t)args->data[0].of.i32;
    if (!ipc_mem_check(rt, out_ptr, YUMI_GROUP_ID_LEN)) return NULL;
    if (rt->dashboard && rt->group_index < rt->dashboard->group_count) {
        memcpy(ipc_mem(rt) + out_ptr,
               rt->dashboard->groups[rt->group_index].group_id,
               YUMI_GROUP_ID_LEN);
    } else {
        memset(ipc_mem(rt) + out_ptr, 0, YUMI_GROUP_ID_LEN);
    }
    return NULL;
}

static wasm_trap_t *host_dashboard_list_groups(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t out_ptr = (uint32_t)args->data[0].of.i32;
    uint32_t out_cap = (uint32_t)args->data[1].of.i32;
    int32_t total = rt->dashboard
        ? dashboard_fill_group_info(rt->dashboard, rt, out_ptr, out_cap) : 0;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = total };
    return NULL;
}

static wasm_trap_t *host_dashboard_current_group(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WebAppRuntime *rt = env;
    uint32_t out_ptr = (uint32_t)args->data[0].of.i32;
    if (!ipc_mem_check(rt, out_ptr, YUMI_GROUP_ID_LEN)) return NULL;
    uint32_t g = rt->dashboard ? rt->dashboard->foreground_group : UINT32_MAX;
    if (rt->dashboard && g < rt->dashboard->group_count) {
        memcpy(ipc_mem(rt) + out_ptr,
               rt->dashboard->groups[g].group_id, YUMI_GROUP_ID_LEN);
    } else {
        memset(ipc_mem(rt) + out_ptr, 0, YUMI_GROUP_ID_LEN);
    }
    return NULL;
}

static wasm_trap_t *host_dashboard_switch_group(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    int32_t rc = (gi != UINT32_MAX)
        ? dashboard_switch_group(rt->dashboard, gi) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_group_open(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    int32_t rc = (gi != UINT32_MAX)
        ? dashboard_connect_group(rt->dashboard, gi) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_group_close(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    if (gi != UINT32_MAX) dashboard_disconnect_group(rt->dashboard, gi);
    res->data[0] = (wasm_val_t){
        .kind = WASM_I32,
        .of.i32 = (gi != UINT32_MAX) ? 0 : -1,
    };
    return NULL;
}

static wasm_trap_t *host_dashboard_group_set_background(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t bg = (uint32_t)args->data[1].of.i32;
    int32_t rc = (gi != UINT32_MAX)
        ? dashboard_group_set_background(rt->dashboard, gi, bg != 0) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_list_webapps(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t out_ptr = (uint32_t)args->data[1].of.i32;
    uint32_t out_cap = (uint32_t)args->data[2].of.i32;
    int32_t total = (gi != UINT32_MAX)
        ? dashboard_fill_webapp_info(rt->dashboard, rt, gi, out_ptr, out_cap)
        : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = total };
    return NULL;
}

static wasm_trap_t *host_dashboard_webapp_db_size(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t slot = resolve_slot_arg(rt,
                                     (uint32_t)args->data[0].of.i32,
                                     (uint32_t)args->data[1].of.i32);
    int64_t v = -1;
    if (slot != UINT32_MAX && rt->dashboard->slots[slot].rt)
        v = (int64_t)webapp_runtime_get_db_size(rt->dashboard->slots[slot].rt);
    res->data[0] = (wasm_val_t){ .kind = WASM_I64, .of.i64 = v };
    return NULL;
}

static wasm_trap_t *host_dashboard_webapp_db_quota(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t slot = resolve_slot_arg(rt,
                                     (uint32_t)args->data[0].of.i32,
                                     (uint32_t)args->data[1].of.i32);
    int64_t v = -1;
    if (slot != UINT32_MAX && rt->dashboard->slots[slot].rt)
        v = (int64_t)webapp_runtime_get_db_quota(rt->dashboard->slots[slot].rt);
    res->data[0] = (wasm_val_t){ .kind = WASM_I64, .of.i64 = v };
    return NULL;
}

static wasm_trap_t *host_dashboard_webapp_set_db_quota(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t slot = resolve_slot_arg(rt,
                                     (uint32_t)args->data[0].of.i32,
                                     (uint32_t)args->data[1].of.i32);
    uint64_t quota = (uint64_t)args->data[2].of.i64;
    int32_t rc = (slot != UINT32_MAX)
        ? dashboard_set_slot_db_quota(rt->dashboard, slot, quota) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

/* ── GR proxy: peers ──────────────────────────────────────────── */

static wasm_trap_t *host_dashboard_peer_kick(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gid = (uint32_t)args->data[0].of.i32;
    uint32_t pid = (uint32_t)args->data[1].of.i32;
    uint32_t rp  = (uint32_t)args->data[2].of.i32;
    uint32_t rl  = (uint32_t)args->data[3].of.i32;
    uint32_t gi  = resolve_group_arg(rt, gid);
    int32_t rc = -1;
    if (gi != UINT32_MAX && ipc_mem_check(rt, pid, GR_PEER_ID_LEN) &&
        (rl == 0 || ipc_mem_check(rt, rp, rl))) {
        char rbuf[256]; rbuf[0] = 0;
        if (rl && rp) {
            uint32_t n = rl < sizeof(rbuf)-1 ? rl : sizeof(rbuf)-1;
            memcpy(rbuf, ipc_mem(rt) + rp, n); rbuf[n] = 0;
        }
        rc = dashboard_gr_peer_kick(rt->dashboard, gi,
                                    ipc_mem(rt) + pid,
                                    rl ? rbuf : NULL);
    }
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_peer_ban(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gid = (uint32_t)args->data[0].of.i32;
    uint32_t pid = (uint32_t)args->data[1].of.i32;
    uint32_t rp  = (uint32_t)args->data[2].of.i32;
    uint32_t rl  = (uint32_t)args->data[3].of.i32;
    uint32_t gi  = resolve_group_arg(rt, gid);
    int32_t rc = -1;
    if (gi != UINT32_MAX && ipc_mem_check(rt, pid, GR_PEER_ID_LEN) &&
        (rl == 0 || ipc_mem_check(rt, rp, rl))) {
        char rbuf[256]; rbuf[0] = 0;
        if (rl && rp) {
            uint32_t n = rl < sizeof(rbuf)-1 ? rl : sizeof(rbuf)-1;
            memcpy(rbuf, ipc_mem(rt) + rp, n); rbuf[n] = 0;
        }
        rc = dashboard_gr_peer_ban(rt->dashboard, gi,
                                   ipc_mem(rt) + pid,
                                   rl ? rbuf : NULL);
    }
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_peer_touch(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t pid = (uint32_t)args->data[1].of.i32;
    int32_t rc = (gi != UINT32_MAX && ipc_mem_check(rt, pid, GR_PEER_ID_LEN))
        ? dashboard_gr_peer_touch(rt->dashboard, gi, ipc_mem(rt) + pid) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_peer_set_role(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi  = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t pid = (uint32_t)args->data[1].of.i32;
    uint32_t rid = (uint32_t)args->data[2].of.i32;
    int32_t rc = (gi != UINT32_MAX && ipc_mem_check(rt, pid, GR_PEER_ID_LEN))
        ? dashboard_gr_peer_set_role(rt->dashboard, gi,
                                     ipc_mem(rt) + pid, rid) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_peer_get(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi  = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t pid = (uint32_t)args->data[1].of.i32;
    uint32_t out = (uint32_t)args->data[2].of.i32;
    int32_t rc = -1;
    if (gi != UINT32_MAX && ipc_mem_check(rt, pid, GR_PEER_ID_LEN) &&
        ipc_mem_check(rt, out, 280))
        rc = dashboard_gr_peer_get(rt->dashboard, gi,
                                   ipc_mem(rt) + pid, ipc_mem(rt) + out);
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_peer_count(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    int32_t sf = args->data[1].of.i32;
    res->data[0] = (wasm_val_t){
        .kind = WASM_I32,
        .of.i32 = (gi != UINT32_MAX)
            ? dashboard_gr_peer_count(rt->dashboard, gi, sf) : -1,
    };
    return NULL;
}

static wasm_trap_t *host_dashboard_peer_list(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi  = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    int32_t  sf  = args->data[1].of.i32;
    uint32_t out = (uint32_t)args->data[2].of.i32;
    uint32_t cap = (uint32_t)args->data[3].of.i32;
    int32_t total = -1;
    if (gi != UINT32_MAX &&
        (out == 0 || cap == 0 || ipc_mem_check(rt, out, cap * 280U)))
        total = dashboard_gr_peer_list(rt->dashboard, gi, sf,
                                       out ? ipc_mem(rt) + out : NULL, cap);
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = total };
    return NULL;
}

static wasm_trap_t *host_dashboard_peer_is_authorized(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t pid = (uint32_t)args->data[1].of.i32;
    int32_t rc = (gi != UINT32_MAX && ipc_mem_check(rt, pid, GR_PEER_ID_LEN))
        ? dashboard_gr_peer_is_authorized(rt->dashboard, gi,
                                          ipc_mem(rt) + pid) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

/* ── GR proxy: roles ──────────────────────────────────────────── */

static wasm_trap_t *host_dashboard_role_add(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi   = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t np   = (uint32_t)args->data[1].of.i32;
    uint32_t nl   = (uint32_t)args->data[2].of.i32;
    uint32_t perm = (uint32_t)args->data[3].of.i32;
    uint32_t ro   = (uint32_t)args->data[4].of.i32;
    int32_t rc = -1;
    if (gi != UINT32_MAX && ipc_mem_check(rt, np, nl) &&
        (ro == 0 || ipc_mem_check(rt, ro, 4))) {
        char nbuf[128]; nbuf[0] = 0;
        uint32_t n = nl < sizeof(nbuf)-1 ? nl : sizeof(nbuf)-1;
        if (n && np) memcpy(nbuf, ipc_mem(rt) + np, n);
        nbuf[n] = 0;
        uint32_t rid = 0;
        rc = dashboard_gr_role_add(rt->dashboard, gi, nbuf, perm, &rid);
        if (ro) memcpy(ipc_mem(rt) + ro, &rid, 4);
    }
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_role_remove(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t rid = (uint32_t)args->data[1].of.i32;
    int32_t rc = (gi != UINT32_MAX)
        ? dashboard_gr_role_remove(rt->dashboard, gi, rid) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_role_set_permissions(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t rid = (uint32_t)args->data[1].of.i32;
    uint32_t perm = (uint32_t)args->data[2].of.i32;
    int32_t rc = (gi != UINT32_MAX)
        ? dashboard_gr_role_set_permissions(rt->dashboard, gi, rid, perm) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_role_get(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t rid = (uint32_t)args->data[1].of.i32;
    uint32_t out = (uint32_t)args->data[2].of.i32;
    int32_t rc = (gi != UINT32_MAX && ipc_mem_check(rt, out, 152))
        ? dashboard_gr_role_get(rt->dashboard, gi, rid, ipc_mem(rt) + out) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_role_list(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi  = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t out = (uint32_t)args->data[1].of.i32;
    uint32_t cap = (uint32_t)args->data[2].of.i32;
    int32_t rc = -1;
    if (gi != UINT32_MAX &&
        (out == 0 || cap == 0 || ipc_mem_check(rt, out, cap * 152U)))
        rc = dashboard_gr_role_list(rt->dashboard, gi,
                                    out ? ipc_mem(rt) + out : NULL, cap);
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_role_count(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    res->data[0] = (wasm_val_t){
        .kind = WASM_I32,
        .of.i32 = (gi != UINT32_MAX)
            ? dashboard_gr_role_count(rt->dashboard, gi) : -1,
    };
    return NULL;
}

static wasm_trap_t *host_dashboard_has_permission(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t perm = (uint32_t)args->data[1].of.i32;
    res->data[0] = (wasm_val_t){
        .kind = WASM_I32,
        .of.i32 = (gi != UINT32_MAX)
            ? dashboard_gr_has_permission(rt->dashboard, gi, perm) : -1,
    };
    return NULL;
}

/* ── GR proxy: webapp manifest ─────────────────────────────────── */

static wasm_trap_t *host_dashboard_group_webapp_add(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi  = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t rec = (uint32_t)args->data[1].of.i32;
    int32_t rc = (gi != UINT32_MAX && ipc_mem_check(rt, rec, 304))
        ? dashboard_gr_webapp_add(rt->dashboard, gi, ipc_mem(rt) + rec) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_group_webapp_remove(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t h  = (uint32_t)args->data[1].of.i32;
    int32_t rc = (gi != UINT32_MAX && ipc_mem_check(rt, h, 128))
        ? dashboard_gr_webapp_remove(rt->dashboard, gi, ipc_mem(rt) + h) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_group_webapp_is_authorized(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t h  = (uint32_t)args->data[1].of.i32;
    int32_t rc = (gi != UINT32_MAX && ipc_mem_check(rt, h, 128))
        ? dashboard_gr_webapp_is_authorized(rt->dashboard, gi,
                                            ipc_mem(rt) + h) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_group_webapp_list(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi  = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t out = (uint32_t)args->data[1].of.i32;
    uint32_t cap = (uint32_t)args->data[2].of.i32;
    int32_t rc = -1;
    if (gi != UINT32_MAX &&
        (out == 0 || cap == 0 || ipc_mem_check(rt, out, cap * 304U)))
        rc = dashboard_gr_webapp_list(rt->dashboard, gi,
                                      out ? ipc_mem(rt) + out : NULL, cap);
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_group_webapp_count(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    res->data[0] = (wasm_val_t){
        .kind = WASM_I32,
        .of.i32 = (gi != UINT32_MAX)
            ? dashboard_gr_webapp_count(rt->dashboard, gi) : -1,
    };
    return NULL;
}

/* ── GR proxy: servers ─────────────────────────────────────────── */

static wasm_trap_t *host_dashboard_server_add(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi  = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t rec = (uint32_t)args->data[1].of.i32;
    int32_t rc = (gi != UINT32_MAX && ipc_mem_check(rt, rec, 4480))
        ? dashboard_gr_server_add(rt->dashboard, gi, ipc_mem(rt) + rec) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_server_remove(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t h  = (uint32_t)args->data[1].of.i32;
    int32_t rc = (gi != UINT32_MAX && ipc_mem_check(rt, h, 128))
        ? dashboard_gr_server_remove(rt->dashboard, gi, ipc_mem(rt) + h) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_server_list(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi  = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t typ = (uint32_t)args->data[1].of.i32;
    uint32_t out = (uint32_t)args->data[2].of.i32;
    uint32_t cap = (uint32_t)args->data[3].of.i32;
    int32_t rc = -1;
    if (gi != UINT32_MAX &&
        (out == 0 || cap == 0 || ipc_mem_check(rt, out, cap * 4480U)))
        rc = dashboard_gr_server_list(rt->dashboard, gi, typ,
                                      out ? ipc_mem(rt) + out : NULL, cap);
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_server_count(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi  = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t typ = (uint32_t)args->data[1].of.i32;
    res->data[0] = (wasm_val_t){
        .kind = WASM_I32,
        .of.i32 = (gi != UINT32_MAX)
            ? dashboard_gr_server_count(rt->dashboard, gi, typ) : -1,
    };
    return NULL;
}

/* ── GR proxy: epoch / retention ──────────────────────────────── */

static wasm_trap_t *host_dashboard_epoch_rotate(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    res->data[0] = (wasm_val_t){
        .kind = WASM_I32,
        .of.i32 = (gi != UINT32_MAX)
            ? dashboard_gr_epoch_rotate(rt->dashboard, gi) : -1,
    };
    return NULL;
}

static wasm_trap_t *host_dashboard_retention_get(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi  = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t out = (uint32_t)args->data[1].of.i32;
    int32_t rc = -1;
    if (gi != UINT32_MAX && ipc_mem_check(rt, out, 24)) {
        int64_t v[3] = {0,0,0};
        rc = dashboard_gr_retention_get(rt->dashboard, gi, v);
        if (rc == 0) memcpy(ipc_mem(rt) + out, v, 24);
    }
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_retention_set(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi  = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t inp = (uint32_t)args->data[1].of.i32;
    int32_t rc = -1;
    if (gi != UINT32_MAX && ipc_mem_check(rt, inp, 24)) {
        int64_t v[3];
        memcpy(v, ipc_mem(rt) + inp, 24);
        rc = dashboard_gr_retention_set(rt->dashboard, gi, v);
    }
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

/* ── GR proxy: group icon ─────────────────────────────────────── */

static wasm_trap_t *host_dashboard_group_icon_set(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi   = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t dp   = (uint32_t)args->data[1].of.i32;
    uint32_t dl   = (uint32_t)args->data[2].of.i32;
    uint32_t mp   = (uint32_t)args->data[3].of.i32;
    uint32_t ml   = (uint32_t)args->data[4].of.i32;
    uint32_t w    = (uint32_t)args->data[5].of.i32;
    uint32_t h    = (uint32_t)args->data[6].of.i32;
    uint32_t isv  = (uint32_t)args->data[7].of.i32;
    uint32_t sfp  = (uint32_t)args->data[8].of.i32;
    uint32_t sfl  = (uint32_t)args->data[9].of.i32;
    int32_t rc = -1;
    if (gi != UINT32_MAX &&
        ipc_mem_check(rt, dp, dl) &&
        (ml == 0 || ipc_mem_check(rt, mp, ml)) &&
        (sfl == 0 || ipc_mem_check(rt, sfp, sfl))) {
        char mbuf[64]; mbuf[0] = 0;
        if (ml && mp) {
            uint32_t n = ml < sizeof(mbuf)-1 ? ml : sizeof(mbuf)-1;
            memcpy(mbuf, ipc_mem(rt) + mp, n); mbuf[n] = 0;
        }
        rc = dashboard_gr_icon_set(rt->dashboard, gi,
                                   dl ? ipc_mem(rt) + dp : NULL, dl,
                                   ml ? mbuf : NULL,
                                   (uint16_t)w, (uint16_t)h,
                                   isv != 0,
                                   sfl ? ipc_mem(rt) + sfp : NULL, sfl);
    }
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_group_icon_get(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi   = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t info = (uint32_t)args->data[1].of.i32;
    uint32_t dp   = (uint32_t)args->data[2].of.i32;
    uint32_t dc   = (uint32_t)args->data[3].of.i32;
    uint32_t sfp  = (uint32_t)args->data[4].of.i32;
    uint32_t sfc  = (uint32_t)args->data[5].of.i32;
    int32_t rc = -1;
    if (gi != UINT32_MAX &&
        ipc_mem_check(rt, info, 248) &&
        (dc == 0 || ipc_mem_check(rt, dp, dc)) &&
        (sfc == 0 || ipc_mem_check(rt, sfp, sfc)))
        rc = dashboard_gr_icon_get(rt->dashboard, gi,
                                   ipc_mem(rt) + info,
                                   dc ? ipc_mem(rt) + dp : NULL, dc,
                                   sfc ? ipc_mem(rt) + sfp : NULL, sfc);
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_group_icon_remove(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    res->data[0] = (wasm_val_t){
        .kind = WASM_I32,
        .of.i32 = (gi != UINT32_MAX)
            ? dashboard_gr_icon_remove(rt->dashboard, gi) : -1,
    };
    return NULL;
}

static wasm_trap_t *host_dashboard_group_icon_hash(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi  = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t out = (uint32_t)args->data[1].of.i32;
    int32_t rc = (gi != UINT32_MAX && ipc_mem_check(rt, out, 128))
        ? dashboard_gr_icon_hash(rt->dashboard, gi, ipc_mem(rt) + out) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

/* ── GR proxy: invites ────────────────────────────────────────── */

static wasm_trap_t *host_dashboard_invite_create(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi  = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    int64_t exp  = args->data[1].of.i64;
    uint32_t bp  = (uint32_t)args->data[2].of.i32;
    uint32_t bc  = (uint32_t)args->data[3].of.i32;
    uint32_t tok = (uint32_t)args->data[4].of.i32;
    int32_t rc = -1;
    if (gi != UINT32_MAX &&
        (bc == 0 || ipc_mem_check(rt, bp, bc)) &&
        ipc_mem_check(rt, tok, 128))
        rc = dashboard_gr_invite_create(rt->dashboard, gi, exp,
                                        bc ? ipc_mem(rt) + bp : NULL, bc,
                                        ipc_mem(rt) + tok);
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_invite_invalidate(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi  = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t tok = (uint32_t)args->data[1].of.i32;
    int32_t rc = (gi != UINT32_MAX && ipc_mem_check(rt, tok, 128))
        ? dashboard_gr_invite_invalidate(rt->dashboard, gi,
                                         ipc_mem(rt) + tok) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_invite_check(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi  = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t tok = (uint32_t)args->data[1].of.i32;
    int32_t rc = (gi != UINT32_MAX && ipc_mem_check(rt, tok, 128))
        ? dashboard_gr_invite_check(rt->dashboard, gi,
                                    ipc_mem(rt) + tok) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_invite_list(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi  = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t out = (uint32_t)args->data[1].of.i32;
    uint32_t cap = (uint32_t)args->data[2].of.i32;
    int32_t rc = -1;
    if (gi != UINT32_MAX &&
        (out == 0 || cap == 0 || ipc_mem_check(rt, out, cap * 216U)))
        rc = dashboard_gr_invite_list(rt->dashboard, gi,
                                      out ? ipc_mem(rt) + out : NULL, cap);
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_invite_count(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    res->data[0] = (wasm_val_t){
        .kind = WASM_I32,
        .of.i32 = (gi != UINT32_MAX)
            ? dashboard_gr_invite_count(rt->dashboard, gi) : -1,
    };
    return NULL;
}

static wasm_trap_t *host_dashboard_invite_parse(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t bp  = (uint32_t)args->data[0].of.i32;
    uint32_t bl  = (uint32_t)args->data[1].of.i32;
    uint32_t out = (uint32_t)args->data[2].of.i32;
    int32_t rc = -1;
    if (ipc_mem_check(rt, bp, bl) && ipc_mem_check(rt, out, 312))
        rc = dashboard_gr_invite_parse(ipc_mem(rt) + bp, bl, ipc_mem(rt) + out);
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

/* ── GR proxy: audit log ──────────────────────────────────────── */

static wasm_trap_t *host_dashboard_audit_list(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi  = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    int64_t  sm  = args->data[1].of.i64;
    uint32_t out = (uint32_t)args->data[2].of.i32;
    uint32_t cap = (uint32_t)args->data[3].of.i32;
    int32_t rc = -1;
    if (gi != UINT32_MAX &&
        (out == 0 || cap == 0 || ipc_mem_check(rt, out, cap * 600U)))
        rc = dashboard_gr_audit_list(rt->dashboard, gi, sm,
                                     out ? ipc_mem(rt) + out : NULL, cap);
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_audit_count(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    res->data[0] = (wasm_val_t){
        .kind = WASM_I32,
        .of.i32 = (gi != UINT32_MAX)
            ? dashboard_gr_audit_count(rt->dashboard, gi) : -1,
    };
    return NULL;
}

static wasm_trap_t *host_dashboard_audit_verify_chain(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi  = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t out = (uint32_t)args->data[1].of.i32;
    int32_t rc = (gi != UINT32_MAX && ipc_mem_check(rt, out, 32))
        ? dashboard_gr_audit_verify_chain(rt->dashboard, gi, ipc_mem(rt) + out)
        : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_audit_list_forks(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi  = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t out = (uint32_t)args->data[1].of.i32;
    uint32_t cap = (uint32_t)args->data[2].of.i32;
    int32_t rc = -1;
    if (gi != UINT32_MAX &&
        (out == 0 || cap == 0 || ipc_mem_check(rt, out, cap * 1544U)))
        rc = dashboard_gr_audit_list_forks(rt->dashboard, gi,
                                           out ? ipc_mem(rt) + out : NULL, cap);
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

/* ── GR proxy: behavior analysis ──────────────────────────────── */

static wasm_trap_t *host_dashboard_behavior_actor_burst(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t ap = (uint32_t)args->data[1].of.i32;
    int64_t  wm = args->data[2].of.i64;
    uint32_t op = (uint32_t)args->data[3].of.i32;
    int32_t rc = -1;
    if (gi != UINT32_MAX &&
        ipc_mem_check(rt, ap, GR_PEER_ID_LEN) &&
        ipc_mem_check(rt, op, 32))
        rc = dashboard_gr_behavior_actor_burst(rt->dashboard, gi,
                                               ipc_mem(rt) + ap, wm,
                                               ipc_mem(rt) + op);
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_behavior_mutation_rate(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    int64_t  wm = args->data[1].of.i64;
    uint32_t op = (uint32_t)args->data[2].of.i32;
    int32_t rc = (gi != UINT32_MAX && ipc_mem_check(rt, op, 24))
        ? dashboard_gr_behavior_mutation_rate(rt->dashboard, gi, wm,
                                              ipc_mem(rt) + op) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_behavior_admin_score(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t ap = (uint32_t)args->data[1].of.i32;
    int64_t  wm = args->data[2].of.i64;
    uint32_t op = (uint32_t)args->data[3].of.i32;
    int32_t rc = -1;
    if (gi != UINT32_MAX &&
        ipc_mem_check(rt, ap, GR_PEER_ID_LEN) &&
        ipc_mem_check(rt, op, 32))
        rc = dashboard_gr_behavior_admin_score(rt->dashboard, gi,
                                               ipc_mem(rt) + ap, wm,
                                               ipc_mem(rt) + op);
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_behavior_peer_churn(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    int64_t  wm = args->data[1].of.i64;
    uint32_t op = (uint32_t)args->data[2].of.i32;
    int32_t rc = (gi != UINT32_MAX && ipc_mem_check(rt, op, 32))
        ? dashboard_gr_behavior_peer_churn(rt->dashboard, gi, wm,
                                           ipc_mem(rt) + op) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_behavior_delta_score(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint64_t db = (uint64_t)args->data[1].of.i64;
    uint32_t ec = (uint32_t)args->data[2].of.i32;
    uint32_t op = (uint32_t)args->data[3].of.i32;
    int32_t rc = (gi != UINT32_MAX && ipc_mem_check(rt, op, 40))
        ? dashboard_gr_behavior_delta_score(rt->dashboard, gi, db, ec,
                                            ipc_mem(rt) + op) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_behavior_stale_peers(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi  = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    int64_t  st  = args->data[1].of.i64;
    uint32_t op  = (uint32_t)args->data[2].of.i32;
    uint32_t cap = (uint32_t)args->data[3].of.i32;
    int32_t rc = -1;
    if (gi != UINT32_MAX &&
        (op == 0 || cap == 0 ||
         ipc_mem_check(rt, op, cap * (uint32_t)GR_PEER_ID_LEN)))
        rc = dashboard_gr_behavior_stale_peers(rt->dashboard, gi, st,
                                               op ? ipc_mem(rt) + op : NULL, cap);
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_behavior_epoch_pattern(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    int64_t  wm = args->data[1].of.i64;
    uint32_t op = (uint32_t)args->data[2].of.i32;
    int32_t rc = (gi != UINT32_MAX && ipc_mem_check(rt, op, 24))
        ? dashboard_gr_behavior_epoch_pattern(rt->dashboard, gi, wm,
                                              ipc_mem(rt) + op) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_behavior_network_score(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t op = (uint32_t)args->data[1].of.i32;
    int32_t rc = (gi != UINT32_MAX && ipc_mem_check(rt, op, 32))
        ? dashboard_gr_behavior_network_score(rt->dashboard, gi,
                                              ipc_mem(rt) + op) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_behavior_snapshot(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    int64_t  wm = args->data[1].of.i64;
    uint32_t op = (uint32_t)args->data[2].of.i32;
    int32_t rc = (gi != UINT32_MAX && ipc_mem_check(rt, op, 200))
        ? dashboard_gr_behavior_snapshot(rt->dashboard, gi, wm,
                                         ipc_mem(rt) + op) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_behavior_get_config(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t op = (uint32_t)args->data[1].of.i32;
    int32_t rc = (gi != UINT32_MAX && ipc_mem_check(rt, op, 40))
        ? dashboard_gr_behavior_get_config(rt->dashboard, gi,
                                           ipc_mem(rt) + op) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_behavior_set_config(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t ip = (uint32_t)args->data[1].of.i32;
    int32_t rc = (gi != UINT32_MAX && ipc_mem_check(rt, ip, 40))
        ? dashboard_gr_behavior_set_config(rt->dashboard, gi,
                                           ipc_mem(rt) + ip) : -1;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

/* ── GR proxy: IP blocklist ──────────────────────────────────── */

static bool blocklist_copy_ip(WebAppRuntime *rt,
                              uint32_t ptr, uint32_t len,
                              char buf[64]) {
    if (len >= 64 || !ipc_mem_check(rt, ptr, len)) return false;
    memcpy(buf, ipc_mem(rt) + ptr, len);
    buf[len] = '\0';
    return true;
}

static wasm_trap_t *host_dashboard_blocklist_check(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi  = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t ip  = (uint32_t)args->data[1].of.i32;
    uint32_t il  = (uint32_t)args->data[2].of.i32;
    int64_t  dur = args->data[3].of.i64;
    int32_t rc = -1;
    char buf[64];
    if (gi != UINT32_MAX && blocklist_copy_ip(rt, ip, il, buf))
        rc = dashboard_gr_blocklist_check(rt->dashboard, gi, buf, dur);
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_blocklist_record_fail(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t ip = (uint32_t)args->data[1].of.i32;
    uint32_t il = (uint32_t)args->data[2].of.i32;
    int32_t  mf = args->data[3].of.i32;
    int32_t rc = -1;
    char buf[64];
    if (gi != UINT32_MAX && blocklist_copy_ip(rt, ip, il, buf))
        rc = dashboard_gr_blocklist_record_fail(rt->dashboard, gi, buf, mf);
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_blocklist_reset(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    uint32_t ip = (uint32_t)args->data[1].of.i32;
    uint32_t il = (uint32_t)args->data[2].of.i32;
    int32_t rc = -1;
    char buf[64];
    if (gi != UINT32_MAX && blocklist_copy_ip(rt, ip, il, buf))
        rc = dashboard_gr_blocklist_reset(rt->dashboard, gi, buf);
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = rc };
    return NULL;
}

static wasm_trap_t *host_dashboard_blocklist_cleanup(
        void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WebAppRuntime *rt = env;
    uint32_t gi  = resolve_group_arg(rt, (uint32_t)args->data[0].of.i32);
    int64_t  dur = args->data[1].of.i64;
    res->data[0] = (wasm_val_t){
        .kind = WASM_I32,
        .of.i32 = (gi != UINT32_MAX)
            ? dashboard_gr_blocklist_cleanup(rt->dashboard, gi, dur) : -1,
    };
    return NULL;
}

/* ── IPC table ────────────────────────────────────────────────── */

#define IPC(fn_name, cb_name, np_val, nr_val, ...) \
    { .name = fn_name, .cb = cb_name, .np = np_val, \
      .params = __VA_ARGS__, .nr = nr_val, .results = {0} }

/* Webapp-tier IPC: bound into every *regular* webapp slot.  These are
 * the user-mediated service requests a normal webapp may issue
 * (mirrors sdk/wasm_webapp.h).  The dashboard webapp does not receive
 * these bindings. */
static const DashboardIPCEntry WEBAPP_IPC_FNS[] = {
    /* Clipboard / link / dialogs / friends / notify */
    { "dashboard_request_paste",      host_dashboard_request_paste,      0, {0},                                           0, {0} },
    { "dashboard_request_copy",       host_dashboard_request_copy,       2, {WASM_I32, WASM_I32},                          0, {0} },
    { "webapp_copy_link",             host_webapp_copy_link,             2, {WASM_I32, WASM_I32},                          0, {0} },
    { "webapp_paste_link",            host_webapp_paste_link,            2, {WASM_I32, WASM_I32},                          1, {WASM_I32} },
    { "dashboard_open_file_dialog",   host_dashboard_open_file,          0, {0},                                           0, {0} },
    { "dashboard_save_file_dialog",   host_dashboard_save_file,          0, {0},                                           0, {0} },
    { "dashboard_open_folder_dialog", host_dashboard_open_folder,        0, {0},                                           0, {0} },
    { "dashboard_add_friend",         host_dashboard_add_friend,         1, {WASM_I32},                                    0, {0} },
    { "dashboard_get_friend_list",    host_dashboard_get_friend_list,    0, {0},                                           0, {0} },
    { "dashboard_remove_friend",      host_dashboard_remove_friend,      1, {WASM_I32},                                    0, {0} },
    { "dashboard_group_notify",       host_dashboard_group_notify_cb,    5, {WASM_I32,WASM_I32,WASM_I32,WASM_I32,WASM_I32},0, {0} },
    { "dashboard_folder_next",        host_dashboard_folder_next,        2, {WASM_I32, WASM_I32},                          1, {WASM_I32} },
    { "dashboard_folder_close",       host_dashboard_folder_close,       1, {WASM_I32},                                    0, {0} },

    /* Self identity */
    { "dashboard_self_webapp_id",     host_dashboard_self_webapp_id,     1, {WASM_I32},                                    0, {0} },
    { "dashboard_self_group_id",      host_dashboard_self_group_id,      1, {WASM_I32},                                    0, {0} },
};

/* Dashboard-only IPC: bound only into the dashboard webapp slot
 * (mirrors sdk/wasm_dashboard.h).  Regular webapps do not receive
 * these bindings. */
static const DashboardIPCEntry DASHBOARD_IPC_FNS[] = {
    /* Group / webapp management */
    { "dashboard_list_groups",          host_dashboard_list_groups,          2, {WASM_I32, WASM_I32},                          1, {WASM_I32} },
    { "dashboard_current_group",        host_dashboard_current_group,        1, {WASM_I32},                                    0, {0} },
    { "dashboard_switch_group",         host_dashboard_switch_group,         1, {WASM_I32},                                    1, {WASM_I32} },
    { "dashboard_group_open",           host_dashboard_group_open,           1, {WASM_I32},                                    1, {WASM_I32} },
    { "dashboard_group_close",          host_dashboard_group_close,          1, {WASM_I32},                                    1, {WASM_I32} },
    { "dashboard_group_set_background", host_dashboard_group_set_background, 2, {WASM_I32, WASM_I32},                          1, {WASM_I32} },
    { "dashboard_list_webapps",         host_dashboard_list_webapps,         3, {WASM_I32, WASM_I32, WASM_I32},                1, {WASM_I32} },
    { "dashboard_webapp_db_size",       host_dashboard_webapp_db_size,       2, {WASM_I32, WASM_I32},                          1, {WASM_I64} },
    { "dashboard_webapp_db_quota",      host_dashboard_webapp_db_quota,      2, {WASM_I32, WASM_I32},                          1, {WASM_I64} },
    { "dashboard_webapp_set_db_quota",  host_dashboard_webapp_set_db_quota,  3, {WASM_I32, WASM_I32, WASM_I64},                1, {WASM_I32} },

    /* GR proxy: peers */
    { "dashboard_peer_kick",            host_dashboard_peer_kick,           4, {WASM_I32,WASM_I32,WASM_I32,WASM_I32,0},       1, {WASM_I32} },
    { "dashboard_peer_ban",             host_dashboard_peer_ban,            4, {WASM_I32,WASM_I32,WASM_I32,WASM_I32,0},       1, {WASM_I32} },
    { "dashboard_peer_touch",           host_dashboard_peer_touch,          2, {WASM_I32, WASM_I32},                          1, {WASM_I32} },
    { "dashboard_peer_set_role",        host_dashboard_peer_set_role,       3, {WASM_I32, WASM_I32, WASM_I32},                1, {WASM_I32} },
    { "dashboard_peer_get",             host_dashboard_peer_get,            3, {WASM_I32, WASM_I32, WASM_I32},                1, {WASM_I32} },
    { "dashboard_peer_count",           host_dashboard_peer_count,          2, {WASM_I32, WASM_I32},                          1, {WASM_I32} },
    { "dashboard_peer_list",            host_dashboard_peer_list,           4, {WASM_I32,WASM_I32,WASM_I32,WASM_I32,0},       1, {WASM_I32} },
    { "dashboard_peer_is_authorized",   host_dashboard_peer_is_authorized,  2, {WASM_I32, WASM_I32},                          1, {WASM_I32} },

    /* GR proxy: roles */
    { "dashboard_role_add",             host_dashboard_role_add,            5, {WASM_I32,WASM_I32,WASM_I32,WASM_I32,WASM_I32},1, {WASM_I32} },
    { "dashboard_role_remove",          host_dashboard_role_remove,         2, {WASM_I32, WASM_I32},                          1, {WASM_I32} },
    { "dashboard_role_set_permissions", host_dashboard_role_set_permissions,3, {WASM_I32, WASM_I32, WASM_I32},                1, {WASM_I32} },
    { "dashboard_role_get",             host_dashboard_role_get,            3, {WASM_I32, WASM_I32, WASM_I32},                1, {WASM_I32} },
    { "dashboard_role_list",            host_dashboard_role_list,           3, {WASM_I32, WASM_I32, WASM_I32},                1, {WASM_I32} },
    { "dashboard_role_count",           host_dashboard_role_count,          1, {WASM_I32},                                    1, {WASM_I32} },
    { "dashboard_has_permission",       host_dashboard_has_permission,      2, {WASM_I32, WASM_I32},                          1, {WASM_I32} },

    /* GR proxy: webapp manifest */
    { "dashboard_group_webapp_add",           host_dashboard_group_webapp_add,           2, {WASM_I32, WASM_I32},                          1, {WASM_I32} },
    { "dashboard_group_webapp_remove",        host_dashboard_group_webapp_remove,        2, {WASM_I32, WASM_I32},                          1, {WASM_I32} },
    { "dashboard_group_webapp_is_authorized", host_dashboard_group_webapp_is_authorized, 2, {WASM_I32, WASM_I32},                          1, {WASM_I32} },
    { "dashboard_group_webapp_list",          host_dashboard_group_webapp_list,          3, {WASM_I32, WASM_I32, WASM_I32},                1, {WASM_I32} },
    { "dashboard_group_webapp_count",         host_dashboard_group_webapp_count,         1, {WASM_I32},                                    1, {WASM_I32} },

    /* GR proxy: servers */
    { "dashboard_server_add",    host_dashboard_server_add,    2, {WASM_I32, WASM_I32},                          1, {WASM_I32} },
    { "dashboard_server_remove", host_dashboard_server_remove, 2, {WASM_I32, WASM_I32},                          1, {WASM_I32} },
    { "dashboard_server_list",   host_dashboard_server_list,   4, {WASM_I32,WASM_I32,WASM_I32,WASM_I32,0},       1, {WASM_I32} },
    { "dashboard_server_count",  host_dashboard_server_count,  2, {WASM_I32, WASM_I32},                          1, {WASM_I32} },

    /* GR proxy: epoch / retention */
    { "dashboard_epoch_rotate",   host_dashboard_epoch_rotate,   1, {WASM_I32},            1, {WASM_I32} },
    { "dashboard_retention_get",  host_dashboard_retention_get,  2, {WASM_I32, WASM_I32},  1, {WASM_I32} },
    { "dashboard_retention_set",  host_dashboard_retention_set,  2, {WASM_I32, WASM_I32},  1, {WASM_I32} },

    /* GR proxy: group icon */
    { "dashboard_group_icon_set",    host_dashboard_group_icon_set,    10, {WASM_I32,WASM_I32,WASM_I32,WASM_I32,WASM_I32}, 1, {WASM_I32} },
    { "dashboard_group_icon_get",    host_dashboard_group_icon_get,     6, {WASM_I32,WASM_I32,WASM_I32,WASM_I32,WASM_I32}, 1, {WASM_I32} },
    { "dashboard_group_icon_remove", host_dashboard_group_icon_remove,  1, {WASM_I32},                                    1, {WASM_I32} },
    { "dashboard_group_icon_hash",   host_dashboard_group_icon_hash,    2, {WASM_I32, WASM_I32},                          1, {WASM_I32} },

    /* GR proxy: invites */
    { "dashboard_invite_create",     host_dashboard_invite_create,     5, {WASM_I32,WASM_I64,WASM_I32,WASM_I32,WASM_I32}, 1, {WASM_I32} },
    { "dashboard_invite_invalidate", host_dashboard_invite_invalidate, 2, {WASM_I32, WASM_I32},                          1, {WASM_I32} },
    { "dashboard_invite_check",      host_dashboard_invite_check,      2, {WASM_I32, WASM_I32},                          1, {WASM_I32} },
    { "dashboard_invite_list",       host_dashboard_invite_list,       3, {WASM_I32, WASM_I32, WASM_I32},                1, {WASM_I32} },
    { "dashboard_invite_count",      host_dashboard_invite_count,      1, {WASM_I32},                                    1, {WASM_I32} },
    { "dashboard_invite_parse",      host_dashboard_invite_parse,      3, {WASM_I32, WASM_I32, WASM_I32},                1, {WASM_I32} },

    /* GR proxy: audit log */
    { "dashboard_audit_list",         host_dashboard_audit_list,         4, {WASM_I32,WASM_I64,WASM_I32,WASM_I32,0},       1, {WASM_I32} },
    { "dashboard_audit_count",        host_dashboard_audit_count,        1, {WASM_I32},                                    1, {WASM_I32} },
    { "dashboard_audit_verify_chain", host_dashboard_audit_verify_chain, 2, {WASM_I32, WASM_I32},                          1, {WASM_I32} },
    { "dashboard_audit_list_forks",   host_dashboard_audit_list_forks,   3, {WASM_I32, WASM_I32, WASM_I32},                1, {WASM_I32} },

    /* GR proxy: behavior analysis */
    { "dashboard_behavior_actor_burst",   host_dashboard_behavior_actor_burst,   4, {WASM_I32,WASM_I32,WASM_I64,WASM_I32,0}, 1, {WASM_I32} },
    { "dashboard_behavior_mutation_rate", host_dashboard_behavior_mutation_rate, 3, {WASM_I32,WASM_I64,WASM_I32},            1, {WASM_I32} },
    { "dashboard_behavior_admin_score",   host_dashboard_behavior_admin_score,   4, {WASM_I32,WASM_I32,WASM_I64,WASM_I32,0}, 1, {WASM_I32} },
    { "dashboard_behavior_peer_churn",    host_dashboard_behavior_peer_churn,    3, {WASM_I32,WASM_I64,WASM_I32},            1, {WASM_I32} },
    { "dashboard_behavior_delta_score",   host_dashboard_behavior_delta_score,   4, {WASM_I32,WASM_I64,WASM_I32,WASM_I32,0}, 1, {WASM_I32} },
    { "dashboard_behavior_stale_peers",   host_dashboard_behavior_stale_peers,   4, {WASM_I32,WASM_I64,WASM_I32,WASM_I32,0}, 1, {WASM_I32} },
    { "dashboard_behavior_epoch_pattern", host_dashboard_behavior_epoch_pattern, 3, {WASM_I32,WASM_I64,WASM_I32},            1, {WASM_I32} },
    { "dashboard_behavior_network_score", host_dashboard_behavior_network_score, 2, {WASM_I32,WASM_I32},                     1, {WASM_I32} },
    { "dashboard_behavior_snapshot",      host_dashboard_behavior_snapshot,      3, {WASM_I32,WASM_I64,WASM_I32},            1, {WASM_I32} },
    { "dashboard_behavior_get_config",    host_dashboard_behavior_get_config,    2, {WASM_I32,WASM_I32},                     1, {WASM_I32} },
    { "dashboard_behavior_set_config",    host_dashboard_behavior_set_config,    2, {WASM_I32,WASM_I32},                     1, {WASM_I32} },

    /* GR proxy: IP blocklist */
    { "dashboard_blocklist_check",       host_dashboard_blocklist_check,       4, {WASM_I32,WASM_I32,WASM_I32,WASM_I64,0}, 1, {WASM_I32} },
    { "dashboard_blocklist_record_fail", host_dashboard_blocklist_record_fail, 4, {WASM_I32,WASM_I32,WASM_I32,WASM_I32,0}, 1, {WASM_I32} },
    { "dashboard_blocklist_reset",       host_dashboard_blocklist_reset,       3, {WASM_I32,WASM_I32,WASM_I32},            1, {WASM_I32} },
    { "dashboard_blocklist_cleanup",     host_dashboard_blocklist_cleanup,     2, {WASM_I32,WASM_I64},                     1, {WASM_I32} },
};

const DashboardIPCEntry *webapp_ipc_table(size_t *count_out) {
    if (count_out) *count_out = sizeof(WEBAPP_IPC_FNS) / sizeof(WEBAPP_IPC_FNS[0]);
    return WEBAPP_IPC_FNS;
}

const DashboardIPCEntry *dashboard_only_ipc_table(size_t *count_out) {
    if (count_out) *count_out = sizeof(DASHBOARD_IPC_FNS) / sizeof(DASHBOARD_IPC_FNS[0]);
    return DASHBOARD_IPC_FNS;
}
