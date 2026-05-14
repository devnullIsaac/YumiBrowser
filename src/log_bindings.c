#include "log_bindings.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ================================================================== */
/*  Memory helpers                                                     */
/* ================================================================== */

#define B    ((LogBindings *)env)

static uint8_t *wasm_mem_base(LogBindings *b) {
    return b->memory ? (uint8_t *)wasm_memory_data(b->memory) : NULL;
}
static size_t wasm_mem_size(LogBindings *b) {
    return b->memory ? wasm_memory_data_size(b->memory) : 0;
}
static const char *mem_read_str(LogBindings *b, uint32_t ptr, uint32_t len) {
    if (len == 0) return "";
    if ((size_t)ptr + len > wasm_mem_size(b)) return "";
    return (const char *)(wasm_mem_base(b) + ptr);
}

#define ARG_I32(n) (args->data[(n)].of.i32)
#define ARG_F32(n) (args->data[(n)].of.f32)
#define RET_I32(v) do { res->data[0] = (wasm_val_t){.kind=WASM_I32,.of.i32=(v)}; } while(0)

/* ================================================================== */
/*  Log level prefix                                                   */
/* ================================================================== */

static const char *level_tag(int level) {
    switch (level) {
        case 0: return "DEBUG";
        case 1: return "INFO";
        case 2: return "WARN";
        case 3: return "ERROR";
        case 4: return "FATAL";
        default: return "LOG";
    }
}

static FILE *level_stream(int level) {
    return (level >= 2) ? stderr : stdout;
}

/* ================================================================== */
/*  Host functions                                                     */
/* ================================================================== */

/*
 * log_write(ptr, len)
 *   Generic log — prints to stdout with [wasm] prefix.
 */
static wasm_trap_t *fn_log_write(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    uint32_t ptr = (uint32_t)ARG_I32(0);
    uint32_t len = (uint32_t)ARG_I32(1);
    const char *str = mem_read_str(B, ptr, len);
    fprintf(stdout, "[wasm] %.*s\n", (int)len, str);
    fflush(stdout);
    return NULL;
}

/*
 * log_write_level(level, ptr, len)
 *   level: 0=DEBUG 1=INFO 2=WARN 3=ERROR 4=FATAL
 */
static wasm_trap_t *fn_log_write_level(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    int level       = ARG_I32(0);
    uint32_t ptr    = (uint32_t)ARG_I32(1);
    uint32_t len    = (uint32_t)ARG_I32(2);
    const char *str = mem_read_str(B, ptr, len);
    FILE *out       = level_stream(level);
    fprintf(out, "[wasm:%s] %.*s\n", level_tag(level), (int)len, str);
    fflush(out);
    return NULL;
}

/*
 * log_int(value)
 *   Quick debug helper — prints a single i32.
 */
static wasm_trap_t *fn_log_int(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    int32_t val = ARG_I32(0);
    fprintf(stdout, "[wasm:int] %d (0x%08x)\n", val, (uint32_t)val);
    fflush(stdout);
    return NULL;
}

/*
 * log_float(value)
 *   Quick debug helper — prints a single f32.
 */
static wasm_trap_t *fn_log_float(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    float val = ARG_F32(0);
    fprintf(stdout, "[wasm:float] %f\n", val);
    fflush(stdout);
    return NULL;
}

/*
 * log_int2(a, b)
 *   Debug helper — prints two i32 values.
 */
static wasm_trap_t *fn_log_int2(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    int32_t a = ARG_I32(0);
    int32_t b = ARG_I32(1);
    fprintf(stdout, "[wasm:int2] %d, %d\n", a, b);
    fflush(stdout);
    return NULL;
}

/*
 * log_fmt_int(ptr, len, value)
 *   Prints a string label followed by an i32 value.
 *   e.g. log_fmt_int("pipeline handle", 15, h) → [wasm] pipeline handle: 42
 */
static wasm_trap_t *fn_log_fmt_int(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    uint32_t ptr    = (uint32_t)ARG_I32(0);
    uint32_t len    = (uint32_t)ARG_I32(1);
    int32_t  val    = ARG_I32(2);
    const char *str = mem_read_str(B, ptr, len);
    fprintf(stdout, "[wasm] %.*s: %d\n", (int)len, str, val);
    fflush(stdout);
    return NULL;
}

/*
 * log_fmt_float(ptr, len, value)
 *   Prints a string label followed by an f32 value.
 */
static wasm_trap_t *fn_log_fmt_float(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    uint32_t ptr    = (uint32_t)ARG_I32(0);
    uint32_t len    = (uint32_t)ARG_I32(1);
    float    val    = ARG_F32(2);
    const char *str = mem_read_str(B, ptr, len);
    fprintf(stdout, "[wasm] %.*s: %f\n", (int)len, str, val);
    fflush(stdout);
    return NULL;
}

/*
 * log_assert(condition, ptr, len) -> i32
 *   If condition == 0, prints the assertion message to stderr.
 *   Returns the condition value (so caller can chain).
 */
static wasm_trap_t *fn_log_assert(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    int32_t  cond   = ARG_I32(0);
    uint32_t ptr    = (uint32_t)ARG_I32(1);
    uint32_t len    = (uint32_t)ARG_I32(2);
    if (!cond) {
        const char *str = mem_read_str(B, ptr, len);
        fprintf(stderr, "[wasm:ASSERT] %.*s\n", (int)len, str);
        fflush(stderr);
    }
    RET_I32(cond);
    return NULL;
}

/* ================================================================== */
/*  Binding table                                                      */
/* ================================================================== */

typedef struct {
    const char *name;
    wasm_func_callback_with_env_t cb;
    uint32_t np; wasm_valkind_t params[4];
    uint32_t nr; wasm_valkind_t results[1];
} LogBindingEntry;

#define I WASM_I32
#define F WASM_F32

static const LogBindingEntry LOG_BINDINGS[] = {
    /* name                 callback             np  params         nr  results */
    {"log_write",           fn_log_write,         2, {I,I},         0, {0}},
    {"log_write_level",     fn_log_write_level,   3, {I,I,I},       0, {0}},
    {"log_int",             fn_log_int,           1, {I},            0, {0}},
    {"log_float",           fn_log_float,         1, {F},            0, {0}},
    {"log_int2",            fn_log_int2,          2, {I,I},          0, {0}},
    {"log_fmt_int",         fn_log_fmt_int,       3, {I,I,I},        0, {0}},
    {"log_fmt_float",       fn_log_fmt_float,     3, {I,I,F},        0, {0}},
    {"log_assert",          fn_log_assert,        3, {I,I,I},        1, {I}},
};

#undef I
#undef F

#define NUM_LOG_BINDINGS (sizeof(LOG_BINDINGS)/sizeof(LOG_BINDINGS[0]))

/* ================================================================== */
/*  functype builder                                                   */
/* ================================================================== */

static wasm_functype_t *make_ft(uint32_t np, const wasm_valkind_t p[],
                                uint32_t nr, const wasm_valkind_t r[]) {
    wasm_valtype_vec_t params, results;
    if (np > 0) {
        wasm_valtype_t *pt[4];
        for (uint32_t i = 0; i < np; i++) pt[i] = wasm_valtype_new(p[i]);
        wasm_valtype_vec_new(&params, np, pt);
    } else {
        wasm_valtype_vec_new_empty(&params);
    }
    if (nr > 0) {
        wasm_valtype_t *rt[1];
        rt[0] = wasm_valtype_new(r[0]);
        wasm_valtype_vec_new(&results, nr, rt);
    } else {
        wasm_valtype_vec_new_empty(&results);
    }
    return wasm_functype_new(&params, &results);
}

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

void log_bindings_init(LogBindings *b) {
    memset(b, 0, sizeof(*b));
    printf("[log] Initialized log bindings (%zu imports)\n", NUM_LOG_BINDINGS);
}

void log_bindings_set_memory(LogBindings *b, wasm_memory_t *mem) {
    b->memory = mem;
}

size_t log_bindings_get_imports(LogBindings *b, wasm_store_t *store,
                                const char ***out_names,
                                wasm_func_t ***out_funcs) {
    static const char *names[NUM_LOG_BINDINGS];
    static wasm_func_t *funcs[NUM_LOG_BINDINGS];
    for (size_t i = 0; i < NUM_LOG_BINDINGS; i++) {
        names[i] = LOG_BINDINGS[i].name;
        wasm_functype_t *ft = make_ft(
            LOG_BINDINGS[i].np, LOG_BINDINGS[i].params,
            LOG_BINDINGS[i].nr, LOG_BINDINGS[i].results);
        funcs[i] = wasm_func_new_with_env(store, ft, LOG_BINDINGS[i].cb, b, NULL);
        wasm_functype_delete(ft);
    }
    *out_names = names;
    *out_funcs = funcs;
    return NUM_LOG_BINDINGS;
}

void log_bindings_destroy(LogBindings *b) {
    (void)b;
}
