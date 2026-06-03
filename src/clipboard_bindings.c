/*
 * clipboard_bindings.c - Implementation of the clipboard WASM bindings (available/get/set) routed through the dashboard for user-mediated access.
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

#include "clipboard_bindings.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "static_memory.h"

#define B ((ClipboardBindings *)env)

/* ================================================================== */
/*  Memory helpers                                                     */
/* ================================================================== */

static uint8_t *wasm_mem_base(ClipboardBindings *b) {
    return b->memory ? (uint8_t *)wasm_memory_data(b->memory) : NULL;
}
static size_t wasm_mem_size(ClipboardBindings *b) {
    return b->memory ? wasm_memory_data_size(b->memory) : 0;
}

#define ARG_I32(n) (args->data[(n)].of.i32)
#define RET_I32(v) do { res->data[0] = \
    (wasm_val_t){.kind = WASM_I32, .of.i32 = (v)}; } while (0)

/* ================================================================== */
/*  Host functions                                                     */
/* ================================================================== */

/**
 * clipboard_available() -> i32
 * Returns 1 if the system clipboard contains text.
 */
static wasm_trap_t *fn_clipboard_available(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    (void)B; (void)args;
    RET_I32(SDL_HasClipboardText() ? 1 : 0);
    return NULL;
}

/**
 * clipboard_get(out_ptr: i32, max_len: i32) -> i32
 * Reads system clipboard text (UTF-8) into guest memory.
 * Always null-terminates if max_len > 0.
 * Returns bytes written (excluding null), 0 on empty/failure.
 */
static wasm_trap_t *fn_clipboard_get(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    uint32_t out_ptr = (uint32_t)ARG_I32(0);
    int32_t  max_len = ARG_I32(1);

    uint8_t *mem = wasm_mem_base(B);
    size_t   msz = wasm_mem_size(B);

    if (!mem || max_len <= 0 ||
        max_len > CLIPBOARD_MAX_SIZE ||
        (size_t)out_ptr + (size_t)max_len > msz) {
        RET_I32(0); return NULL;
    }

    char *text = SDL_GetClipboardText();
    if (!text || text[0] == '\0') {
        SDL_free(text);
        mem[out_ptr] = '\0';
        RET_I32(0); return NULL;
    }

    int len = (int)strlen(text);
    if (len >= max_len) len = max_len - 1;
    memcpy(mem + out_ptr, text, (size_t)len);
    mem[out_ptr + len] = '\0';
    SDL_free(text);

    RET_I32(len);
    return NULL;
}

/**
 * clipboard_set(ptr: i32, len: i32) -> i32
 * Writes UTF-8 text from guest memory to the system clipboard.
 * Returns 1 on success, 0 on failure.
 */
static wasm_trap_t *fn_clipboard_set(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    uint32_t ptr = (uint32_t)ARG_I32(0);
    int32_t  len = ARG_I32(1);

    uint8_t *mem = wasm_mem_base(B);
    size_t   msz = wasm_mem_size(B);

    if (!mem || len <= 0 ||
        len > CLIPBOARD_MAX_SIZE ||
        (size_t)ptr + (size_t)len > msz) {
        RET_I32(0); return NULL;
    }

    ClipboardBuffer* tmp = NULL;
    if (lease_ClipboardBuffer(&tmp) != YUMI_MEMORY_ALLOC_SUCCESS)
    {
        fprintf(stderr, "Failed to lease clipboard buffer for temporary copying.");
        RET_I32(0); // Fail to lease clipboard!
    }
    else
    {
        if (tmp != NULL)
        {
            memcpy(tmp->buffer, mem + ptr, (size_t)len);
            tmp->buffer[len] = '\0';

            bool ok = SDL_SetClipboardText(tmp->buffer);
            release_ClipboardBuffer(tmp);
            RET_I32(ok ? 1 : 0);
        }
        else
        {
            RET_I32(0);
        }
    }
    return NULL;
}

/* ================================================================== */
/*  Binding table                                                      */
/* ================================================================== */

typedef struct {
    const char                    *name;
    wasm_func_callback_with_env_t  cb;
    uint32_t np;  wasm_valkind_t   params[4];
    uint32_t nr;  wasm_valkind_t   results[1];
} ClipboardBindingEntry;

#define I WASM_I32

static const ClipboardBindingEntry CLIPBOARD_BINDINGS[] = {
    {"clipboard_available", fn_clipboard_available, 0, {0},   1, {I}},
    {"clipboard_get",       fn_clipboard_get,       2, {I,I}, 1, {I}},
    {"clipboard_set",       fn_clipboard_set,       2, {I,I}, 1, {I}},
};

#undef I

#define NUM_CLIPBOARD_BINDINGS \
    (sizeof(CLIPBOARD_BINDINGS) / sizeof(CLIPBOARD_BINDINGS[0]))

static wasm_functype_t *make_ft(uint32_t np, const wasm_valkind_t p[],
                                uint32_t nr, const wasm_valkind_t r[])
{
    wasm_valtype_vec_t params, results;
    if (np > 0) {
        wasm_valtype_t *pt[4];
        for (uint32_t i = 0; i < np; i++) pt[i] = wasm_valtype_new(p[i]);
        wasm_valtype_vec_new(&params, np, pt);
    } else { wasm_valtype_vec_new_empty(&params); }
    if (nr > 0) {
        wasm_valtype_t *rt[1];
        rt[0] = wasm_valtype_new(r[0]);
        wasm_valtype_vec_new(&results, nr, rt);
    } else { wasm_valtype_vec_new_empty(&results); }
    return wasm_functype_new(&params, &results);
}

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

void clipboard_bindings_init(ClipboardBindings *b) {
    memset(b, 0, sizeof(*b));
    printf("[clipboard] Clipboard bindings ready (%zu imports)\n",
           NUM_CLIPBOARD_BINDINGS);
}

void clipboard_bindings_set_memory(ClipboardBindings *b, wasm_memory_t *mem) {
    b->memory = mem;
}

size_t clipboard_bindings_get_imports(ClipboardBindings *b, wasm_store_t *store,
                                      const char ***out_names,
                                      wasm_func_t ***out_funcs)
{
    static const char  *names[NUM_CLIPBOARD_BINDINGS];
    static wasm_func_t *funcs[NUM_CLIPBOARD_BINDINGS];
    for (size_t i = 0; i < NUM_CLIPBOARD_BINDINGS; i++) {
        names[i] = CLIPBOARD_BINDINGS[i].name;
        wasm_functype_t *ft = make_ft(
            CLIPBOARD_BINDINGS[i].np, CLIPBOARD_BINDINGS[i].params,
            CLIPBOARD_BINDINGS[i].nr, CLIPBOARD_BINDINGS[i].results);
        funcs[i] = wasm_func_new_with_env(store, ft, CLIPBOARD_BINDINGS[i].cb, b, NULL);
        wasm_functype_delete(ft);
    }
    *out_names = names;
    *out_funcs = funcs;
    return NUM_CLIPBOARD_BINDINGS;
}

void clipboard_bindings_destroy(ClipboardBindings *b) {
    memset(b, 0, sizeof(*b));
}
