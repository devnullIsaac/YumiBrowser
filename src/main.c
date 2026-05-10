/*
    Yumi Browser — Main Entry Point
    Copyright (C) 2026  DevNullIsaac

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "deps.h"
#include "gpu.h"
#include "dashboard_runtime.h"
#include "crypto.h"
#include "ffmpeg_loader.h"

/* ------------------------------------------------------------------ */
/*  XDG data directory resolution                                      */
/* ------------------------------------------------------------------ */

#define APP_ID "com.yumi.browser"

/**
 * Resolve the XDG data directory for Yumi Browser.
 * Checks (in order): $XDG_DATA_HOME/com.yumi.browser,
 *                     ~/.local/share/com.yumi.browser,
 *                     release/ next to the binary (dev mode).
 */
static const char *resolve_data_dir(const char *argv0) {
    static char buf[4096];

    /* 1. XDG_DATA_HOME */
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0]) {
        snprintf(buf, sizeof(buf), "%s/" APP_ID, xdg);
        struct stat st;
        if (stat(buf, &st) == 0 && S_ISDIR(st.st_mode))
            return buf;
    }

    /* 2. ~/.local/share */
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(buf, sizeof(buf), "%s/.local/share/" APP_ID, home);
        struct stat st;
        if (stat(buf, &st) == 0 && S_ISDIR(st.st_mode))
            return buf;
    }

    /* 3. release/ next to the binary (dev / portable mode) */
    /* Try to find the directory containing argv0 */
    const char *slash = strrchr(argv0, '/');
    if (slash) {
        size_t dir_len = (size_t)(slash - argv0);
        snprintf(buf, sizeof(buf), "%.*s", (int)dir_len, argv0);
    } else {
        snprintf(buf, sizeof(buf), ".");
    }

    /* Check if buf itself has dashboard/ and webapps/ (i.e. we're in release/) */
    char check[4096];
    snprintf(check, sizeof(check), "%s/dashboard", buf);
    struct stat st;
    if (stat(check, &st) == 0 && S_ISDIR(st.st_mode))
        return buf;

    /* Try ../release/ relative to binary */
    char release_buf[4096];
    snprintf(release_buf, sizeof(release_buf), "%s/../release", buf);
    snprintf(check, sizeof(check), "%s/dashboard", release_buf);
    if (stat(check, &st) == 0 && S_ISDIR(st.st_mode)) {
        /* Canonicalize */
        if (realpath(release_buf, buf))
            return buf;
    }

    /* Fallback: current working directory */
    buf[0] = '.';
    buf[1] = '\0';
    return buf;
}

/**
 * Build a path to a specific WASM file within the data directory.
 * Returns static buffer — not reentrant.
 */
static const char *data_path(const char *data_dir, const char *subdir,
                             const char *filename) {
    static char path[4096];
    snprintf(path, sizeof(path), "%s/%s/%s", data_dir, subdir, filename);
    return path;
}

/**
 * Check if a file exists and is a regular file.
 */
static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    if (yumi_crypto_init() != YUMI_CRYPTO_OK) { fprintf(stderr, "crypto init failed\n"); return 1; }
    printf("[crypto] Post-quantum crypto initialized (OpenSSL + oqs-provider)\n");

    if (ffmpeg_loader_init() != 0) { fprintf(stderr, "ffmpeg loader init failed\n"); return 1; }
    printf("[ffmpeg] Dynamic loader initialized\n");

    /* Parse CLI args */
    const char *wasm_path = NULL;
    const char *data_dir_override = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--webapp") == 0 && i + 1 < argc) {
            wasm_path = argv[++i];
        } else if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
            data_dir_override = argv[++i];
        } else if (!wasm_path) {
            wasm_path = argv[i]; /* backward compat: bare arg = wasm path */
        }
    }

    /* Resolve data directory (XDG or relative to binary) */
    const char *data_dir = data_dir_override ? data_dir_override
                                             : resolve_data_dir(argv[0]);
    printf("[main] Data directory: %s\n", data_dir);

    SDL_SetAppMetadata("Yumi Browser", "0.1.0", "com.yumi.browser");
    SDL_SetHint(SDL_HINT_APP_ID, "yumibrowser");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS | SDL_WINDOW_TRANSPARENT;
#if defined(__APPLE__)
    flags |= SDL_WINDOW_METAL;
#endif
    SDL_Window *window = SDL_CreateWindow("Yumi Browser", 1280, 844, flags);
    if (!window) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }

    GpuContext gpu;
    if (!gpu_init(&gpu, window)) { fprintf(stderr, "gpu_init failed\n"); return 1; }

    DashboardRuntime dashboard;
    if (!dashboard_init(&dashboard, &gpu, window, NULL)) {
        fprintf(stderr, "dashboard_init failed\n"); return 1;
    }

    /* If --webapp was given, add a single full-screen slot for dev/testing */
    if (wasm_path) {
        /* Need at least one group entry for the slot */
        if (dashboard.group_count == 0) {
            DashboardGroup *g = &dashboard.groups[0];
            memset(g, 0, sizeof(*g));
            snprintf(g->db_path, sizeof(g->db_path), "/tmp/yumi_dev_group.db");
            g->state = GROUP_STATE_CONNECTED;
            dashboard.group_count = 1;
        }

        int ww, wh;
        SDL_GetWindowSize(window, &ww, &wh);
        WebAppViewport vp = { .x = 0, .y = 0, .w = ww, .h = wh };
        int slot = dashboard_add_slot(&dashboard, 0, wasm_path, &vp, false);
        if (slot < 0) {
            fprintf(stderr, "Failed to load webapp '%s'\n", wasm_path);
            dashboard_destroy(&dashboard);
            gpu_destroy(&gpu);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
        dashboard.focused_slot = slot;
    } else {
        /* ---- Auto-load dashboard + webapps from XDG data dir ---- */

        /* Ensure at least one group for the dashboard slot */
        if (dashboard.group_count == 0) {
            DashboardGroup *g = &dashboard.groups[0];
            memset(g, 0, sizeof(*g));
            snprintf(g->db_path, sizeof(g->db_path), "/tmp/yumi_default_group.db");
            g->state = GROUP_STATE_CONNECTED;
            dashboard.group_count = 1;
        }

        int ww, wh;
        SDL_GetWindowSize(window, &ww, &wh);

        /* 1. Load dashboard.wasm if present */
        const char *dash_wasm = data_path(data_dir, "dashboard", "mato_dashboard.wasm");
        if (file_exists(dash_wasm)) {
            WebAppViewport vp = { .x = 0, .y = 0, .w = ww, .h = wh };
            int slot = dashboard_add_slot(&dashboard, 0, dash_wasm, &vp, true);
            if (slot >= 0) {
                dashboard.focused_slot = slot;
                printf("[main] Loaded dashboard: %s\n", dash_wasm);
            } else {
                fprintf(stderr, "[main] Failed to load dashboard: %s\n", dash_wasm);
            }
        } else {
            printf("[main] No dashboard.wasm found at %s\n", dash_wasm);
        }

        /* 2. Scan webapps/ directory and load each .wasm file */
        char webapps_dir[4096];
        snprintf(webapps_dir, sizeof(webapps_dir), "%s/webapps", data_dir);
        DIR *dp = opendir(webapps_dir);
        if (dp) {
            struct dirent *ent;
            while ((ent = readdir(dp)) != NULL) {
                size_t nlen = strlen(ent->d_name);
                if (nlen < 6) continue; /* need at least "x.wasm" */
                if (strcmp(ent->d_name + nlen - 5, ".wasm") != 0) continue;

                char webapp_path[4096];
                snprintf(webapp_path, sizeof(webapp_path), "%s/%s",
                         webapps_dir, ent->d_name);

                if (!file_exists(webapp_path)) continue;

                printf("[main] Found webapp: %s\n", ent->d_name);
                /* Webapps are loaded on demand via the dashboard UI,
                   but we register them as available. For now, log discovery. */
            }
            closedir(dp);
        }
    }

    /* ---- Main loop ---- */
    bool running = true;
    bool focused = true;

    while (running) {
        SDL_Event ev;

        if (focused) {
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_WINDOW_FOCUS_GAINED) focused = true;
                if (ev.type == SDL_EVENT_WINDOW_FOCUS_LOST)   focused = false;
                if (!dashboard_dispatch_event(&dashboard, &ev)) {
                    running = false;
                    break;
                }
            }
        } else {
            if (SDL_WaitEventTimeout(&ev, 50)) {
                do {
                    if (ev.type == SDL_EVENT_WINDOW_FOCUS_GAINED) focused = true;
                    if (ev.type == SDL_EVENT_WINDOW_FOCUS_LOST)   focused = false;
                    if (!dashboard_dispatch_event(&dashboard, &ev)) {
                        running = false;
                        break;
                    }
                } while (SDL_PollEvent(&ev));
            }
            /* Pump audio for all webapp slots */
            for (uint32_t i = 0; i < dashboard.slot_count; i++) {
                if (dashboard.slots[i].rt)
                    webapp_runtime_pump_audio(dashboard.slots[i].rt);
            }
        }

        if (!running) break;

        static Uint64 last_frame_tick = 0;
        Uint64 now = SDL_GetTicks();
        if (focused || now - last_frame_tick >= 500) {
            last_frame_tick = now;
            dashboard_frame(&dashboard);
        }
    }

    dashboard_destroy(&dashboard);
    gpu_destroy(&gpu);
    SDL_DestroyWindow(window);
    SDL_Quit();
    ffmpeg_loader_deinit();
    printf("Clean shutdown.\n");
    return 0;
}
