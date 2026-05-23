/**
 * @file test_nodegraph.c
 * @brief Tests for nodegraph.h — pure logical node graph.
 */
#define NODEGRAPH_IMPLEMENTATION
#include "nodegraph.h"

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

#define TEST(name) static void name(void)
#define RUN(name) do { printf("  %-60s", #name); name(); printf("PASS\n"); } while(0)

/* ── Helpers ──────────────────────────────────────────────────── */

static void make_graph(ng_graph_t *g) {
  ng_graph_create(g);
}

/** Create a simple definition with N inputs and M outputs, all FLOAT. */
static uint32_t make_float_def(ng_graph_t *g, const char *name,
                               int n_in, int n_out) {
  uint32_t def = ng_def_create(g, name, NG_COLOR(100, 100, 100, 255));
  assert(def != 0);
  for (int i = 0; i < n_in; i++) {
    char buf[32];
    snprintf(buf, sizeof(buf), "in%d", i);
    int idx = ng_def_add_input(g, def, buf, NG_PIN_FLOAT, NG_COLOR_FLOAT);
    assert(idx == i);
  }
  for (int i = 0; i < n_out; i++) {
    char buf[32];
    snprintf(buf, sizeof(buf), "out%d", i);
    int idx = ng_def_add_output(g, def, buf, NG_PIN_FLOAT, NG_COLOR_FLOAT);
    assert(idx == i);
  }
  return def;
}

/** Create a node with N FLOAT inputs and M FLOAT outputs using ng_add_node. */
static uint32_t add_float_node(ng_graph_t *g, const char *name,
                               int n_in, int n_out) {
  ng_pin_desc_t *ins = NULL;
  ng_pin_desc_t *outs = NULL;
  if (n_in > 0) {
    ins = (ng_pin_desc_t *)malloc((size_t)n_in * sizeof(ng_pin_desc_t));
    for (int i = 0; i < n_in; i++) {
      ins[i].name = "in";
      ins[i].type = NG_PIN_FLOAT;
      ins[i].color = NG_COLOR_FLOAT;
    }
  }
  if (n_out > 0) {
    outs = (ng_pin_desc_t *)malloc((size_t)n_out * sizeof(ng_pin_desc_t));
    for (int i = 0; i < n_out; i++) {
      outs[i].name = "out";
      outs[i].type = NG_PIN_FLOAT;
      outs[i].color = NG_COLOR_FLOAT;
    }
  }
  uint32_t id = ng_add_node(g, name, NG_COLOR(100, 100, 100, 255),
                            ins, n_in, outs, n_out);
  free(ins);
  free(outs);
  return id;
}

/* Topo iteration collector */
typedef struct {
  uint32_t ids[256];
  int      count;
} topo_result_t;

static int topo_cb(const ng_node_view_t *view, int order, void *user) {
  (void)order;
  topo_result_t *r = (topo_result_t *)user;
  if (r->count < 256) r->ids[r->count++] = view->id;
  return 0;
}

/* Node iteration collector */
typedef struct {
  uint32_t ids[256];
  int      count;
} node_result_t;

static int node_cb(const ng_node_view_t *view, void *user) {
  node_result_t *r = (node_result_t *)user;
  if (r->count < 256) r->ids[r->count++] = view->id;
  return 0;
}

/* Connection iteration collector */
typedef struct {
  uint32_t ids[256];
  int      count;
} conn_result_t;

static int conn_cb(const ng_connection_view_t *view, void *user) {
  conn_result_t *r = (conn_result_t *)user;
  if (r->count < 256) r->ids[r->count++] = view->id;
  return 0;
}

/* Early-stop callbacks */
static int node_cb_stop_at_1(const ng_node_view_t *view, void *user) {
  (void)view;
  int *count = (int *)user;
  (*count)++;
  return 1; /* stop immediately */
}

static int conn_cb_stop_at_1(const ng_connection_view_t *view, void *user) {
  (void)view;
  int *count = (int *)user;
  (*count)++;
  return 1;
}

static int topo_cb_stop_at_1(const ng_node_view_t *view, int order,
                             void *user) {
  (void)view; (void)order;
  int *count = (int *)user;
  (*count)++;
  return 1;
}

/* ═══════════════════════════════════════════════════════════════
 *  Lifecycle
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_create_destroy) {
  ng_graph_t g;
  make_graph(&g);
  assert(g.def_count == 0);
  assert(g.node_count == 0);
  assert(g.conn_count == 0);
  ng_graph_destroy(&g);
}

TEST(test_create_initial_state) {
  ng_graph_t g;
  make_graph(&g);
  assert(ng_def_count(&g) == 0);
  assert(ng_node_count(&g) == 0);
  assert(ng_connection_count(&g) == 0);
  assert(g.next_def_id == 1);
  assert(g.next_node_id == 1);
  assert(g.next_conn_id == 1);
  ng_graph_destroy(&g);
}

TEST(test_destroy_empty) {
  ng_graph_t g;
  make_graph(&g);
  ng_graph_destroy(&g);
  /* No crash */
}

TEST(test_destroy_with_content) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "Test", 2, 1);
  ng_node_create(&g, d);
  ng_node_create(&g, d);
  ng_graph_destroy(&g);
  /* No crash, no leak */
}

TEST(test_double_destroy) {
  ng_graph_t g;
  make_graph(&g);
  ng_graph_destroy(&g);
  /* After destroy, memset to 0. Destroying zeroed struct should be safe. */
  ng_graph_destroy(&g);
}

/* ═══════════════════════════════════════════════════════════════
 *  Node Definitions — Create
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_def_create) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = ng_def_create(&g, "MyDef", NG_COLOR(255, 0, 0, 255));
  assert(d == 1);
  assert(ng_def_count(&g) == 1);
  ng_graph_destroy(&g);
}

TEST(test_def_create_multiple) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d1 = ng_def_create(&g, "A", NG_COLOR(0,0,0,255));
  uint32_t d2 = ng_def_create(&g, "B", NG_COLOR(0,0,0,255));
  uint32_t d3 = ng_def_create(&g, "C", NG_COLOR(0,0,0,255));
  assert(d1 == 1 && d2 == 2 && d3 == 3);
  assert(ng_def_count(&g) == 3);
  ng_graph_destroy(&g);
}

TEST(test_def_ids_monotonic) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t prev = 0;
  for (int i = 0; i < 50; i++) {
    uint32_t d = ng_def_create(&g, "X", NG_COLOR(0,0,0,255));
    assert(d > prev);
    prev = d;
  }
  ng_graph_destroy(&g);
}

TEST(test_def_create_null_name) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = ng_def_create(&g, NULL, NG_COLOR(0,0,0,255));
  assert(d != 0);
  const ng_node_def_t *def = ng_def_get(&g, d);
  assert(def != NULL);
  assert(def->name == NULL);
  ng_graph_destroy(&g);
}

TEST(test_def_get) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = ng_def_create(&g, "GetMe", NG_COLOR(10, 20, 30, 255));
  const ng_node_def_t *def = ng_def_get(&g, d);
  assert(def != NULL);
  assert(def->id == d);
  assert(strcmp(def->name, "GetMe") == 0);
  assert(def->header_color.r == 10);
  assert(def->header_color.g == 20);
  assert(def->header_color.b == 30);
  assert(def->input_count == 0);
  assert(def->output_count == 0);
  ng_graph_destroy(&g);
}

TEST(test_def_get_invalid) {
  ng_graph_t g;
  make_graph(&g);
  assert(ng_def_get(&g, 0) == NULL);
  assert(ng_def_get(&g, 999) == NULL);
  ng_graph_destroy(&g);
}

/* ═══════════════════════════════════════════════════════════════
 *  Node Definitions — Pins
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_def_add_input) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = ng_def_create(&g, "N", NG_COLOR(0,0,0,255));
  int idx = ng_def_add_input(&g, d, "A", NG_PIN_FLOAT, NG_COLOR_FLOAT);
  assert(idx == 0);
  const ng_node_def_t *def = ng_def_get(&g, d);
  assert(def->input_count == 1);
  assert(strcmp(def->inputs[0].name, "A") == 0);
  assert(def->inputs[0].type == NG_PIN_FLOAT);
  ng_graph_destroy(&g);
}

TEST(test_def_add_output) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = ng_def_create(&g, "N", NG_COLOR(0,0,0,255));
  int idx = ng_def_add_output(&g, d, "Out", NG_PIN_COLOR, NG_COLOR_COLOR);
  assert(idx == 0);
  const ng_node_def_t *def = ng_def_get(&g, d);
  assert(def->output_count == 1);
  assert(def->outputs[0].type == NG_PIN_COLOR);
  ng_graph_destroy(&g);
}

TEST(test_def_add_multiple_pins) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = ng_def_create(&g, "Multi", NG_COLOR(0,0,0,255));
  for (int i = 0; i < 20; i++) {
    int idx = ng_def_add_input(&g, d, "pin", NG_PIN_FLOAT, NG_COLOR_FLOAT);
    assert(idx == i);
  }
  for (int i = 0; i < 15; i++) {
    int idx = ng_def_add_output(&g, d, "pin", NG_PIN_INT, NG_COLOR_INT);
    assert(idx == i);
  }
  const ng_node_def_t *def = ng_def_get(&g, d);
  assert(def->input_count == 20);
  assert(def->output_count == 15);
  ng_graph_destroy(&g);
}

TEST(test_def_add_pin_invalid_def) {
  ng_graph_t g;
  make_graph(&g);
  assert(ng_def_add_input(&g, 999, "X", NG_PIN_FLOAT, NG_COLOR_FLOAT) == -1);
  assert(ng_def_add_output(&g, 0, "X", NG_PIN_FLOAT, NG_COLOR_FLOAT) == -1);
  ng_graph_destroy(&g);
}

TEST(test_def_pin_types) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = ng_def_create(&g, "Types", NG_COLOR(0,0,0,255));
  ng_def_add_input(&g, d, "f", NG_PIN_FLOAT, NG_COLOR_FLOAT);
  ng_def_add_input(&g, d, "i", NG_PIN_INT, NG_COLOR_INT);
  ng_def_add_input(&g, d, "b", NG_PIN_BOOL, NG_COLOR_BOOL);
  ng_def_add_input(&g, d, "v2", NG_PIN_VEC2, NG_COLOR_VEC2);
  ng_def_add_input(&g, d, "v3", NG_PIN_VEC3, NG_COLOR_VEC3);
  ng_def_add_input(&g, d, "v4", NG_PIN_VEC4, NG_COLOR_VEC4);
  ng_def_add_input(&g, d, "c", NG_PIN_COLOR, NG_COLOR_COLOR);
  ng_def_add_input(&g, d, "s", NG_PIN_STRING, NG_COLOR_STRING);
  ng_def_add_input(&g, d, "t", NG_PIN_TEXTURE, NG_COLOR_TEXTURE);
  ng_def_add_input(&g, d, "a", NG_PIN_ANY, NG_COLOR_ANY);
  const ng_node_def_t *def = ng_def_get(&g, d);
  assert(def->input_count == 10);
  assert(def->inputs[0].type == NG_PIN_FLOAT);
  assert(def->inputs[9].type == NG_PIN_ANY);
  ng_graph_destroy(&g);
}

TEST(test_def_pin_colors) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = ng_def_create(&g, "C", NG_COLOR(0,0,0,255));
  ng_color_t c = NG_COLOR(11, 22, 33, 44);
  ng_def_add_input(&g, d, "p", NG_PIN_FLOAT, c);
  const ng_node_def_t *def = ng_def_get(&g, d);
  assert(def->inputs[0].color.r == 11);
  assert(def->inputs[0].color.g == 22);
  assert(def->inputs[0].color.b == 33);
  assert(def->inputs[0].color.a == 44);
  ng_graph_destroy(&g);
}

/* ═══════════════════════════════════════════════════════════════
 *  Node Instances — Create & Destroy
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_node_create) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "Def", 1, 1);
  uint32_t n = ng_node_create(&g, d);
  assert(n != 0);
  assert(ng_node_count(&g) == 1);
  ng_graph_destroy(&g);
}

TEST(test_node_create_invalid_def) {
  ng_graph_t g;
  make_graph(&g);
  assert(ng_node_create(&g, 0) == 0);
  assert(ng_node_create(&g, 999) == 0);
  assert(ng_node_count(&g) == 0);
  ng_graph_destroy(&g);
}

TEST(test_node_create_multiple_same_def) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "Shared", 2, 1);
  uint32_t n1 = ng_node_create(&g, d);
  uint32_t n2 = ng_node_create(&g, d);
  uint32_t n3 = ng_node_create(&g, d);
  assert(n1 != n2 && n2 != n3 && n1 != n3);
  assert(ng_node_count(&g) == 3);
  ng_graph_destroy(&g);
}

TEST(test_node_ids_monotonic) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 0, 0);
  uint32_t prev = 0;
  for (int i = 0; i < 50; i++) {
    uint32_t n = ng_node_create(&g, d);
    assert(n > prev);
    prev = n;
  }
  ng_graph_destroy(&g);
}

TEST(test_node_destroy) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t n = ng_node_create(&g, d);
  assert(ng_node_count(&g) == 1);
  ng_node_destroy(&g, n);
  assert(ng_node_count(&g) == 0);
  ng_graph_destroy(&g);
}

TEST(test_node_destroy_invalid) {
  ng_graph_t g;
  make_graph(&g);
  ng_node_destroy(&g, 0);
  ng_node_destroy(&g, 999);
  /* No crash */
  ng_graph_destroy(&g);
}

TEST(test_node_destroy_removes_connections) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  uint32_t c = ng_node_create(&g, d);
  ng_add_connection(&g, a, 0, b, 0);
  ng_add_connection(&g, b, 0, c, 0);
  assert(ng_connection_count(&g) == 2);
  ng_node_destroy(&g, b);
  assert(ng_connection_count(&g) == 0);
  assert(ng_node_count(&g) == 2);
  ng_graph_destroy(&g);
}

TEST(test_node_destroy_middle) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 0, 0);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  uint32_t c = ng_node_create(&g, d);
  ng_node_destroy(&g, b);
  assert(ng_node_count(&g) == 2);
  /* a and c should still be findable */
  ng_node_view_t v;
  assert(ng_get_node_view(&g, 0, &v));
  assert(v.id == a);
  assert(ng_get_node_view(&g, 1, &v));
  assert(v.id == c);
  ng_graph_destroy(&g);
}

/* ═══════════════════════════════════════════════════════════════
 *  Node Instances — user_data & widgets
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_node_user_data) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 0, 0);
  uint32_t n = ng_node_create(&g, d);
  int dummy = 42;
  ng_node_set_user_data(&g, n, &dummy);
  assert(ng_node_get_user_data(&g, n) == &dummy);
  ng_graph_destroy(&g);
}

TEST(test_node_user_data_default_null) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 0, 0);
  uint32_t n = ng_node_create(&g, d);
  assert(ng_node_get_user_data(&g, n) == NULL);
  ng_graph_destroy(&g);
}

TEST(test_node_user_data_invalid) {
  ng_graph_t g;
  make_graph(&g);
  assert(ng_node_get_user_data(&g, 999) == NULL);
  ng_node_set_user_data(&g, 999, (void *)0x1234); /* no crash */
  ng_graph_destroy(&g);
}

TEST(test_node_header_widget) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 0, 0);
  uint32_t n = ng_node_create(&g, d);
  int x = 1;
  ng_node_set_header_widget(&g, n, &x);
  assert(ng_node_get_header_widget(&g, n) == &x);
  ng_graph_destroy(&g);
}

TEST(test_node_content_widget) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 0, 0);
  uint32_t n = ng_node_create(&g, d);
  float y = 3.14f;
  ng_node_set_content_widget(&g, n, &y);
  assert(ng_node_get_content_widget(&g, n) == &y);
  ng_graph_destroy(&g);
}

TEST(test_node_widgets_default_null) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 0, 0);
  uint32_t n = ng_node_create(&g, d);
  assert(ng_node_get_header_widget(&g, n) == NULL);
  assert(ng_node_get_content_widget(&g, n) == NULL);
  ng_graph_destroy(&g);
}

TEST(test_node_widgets_invalid) {
  ng_graph_t g;
  make_graph(&g);
  assert(ng_node_get_header_widget(&g, 999) == NULL);
  assert(ng_node_get_content_widget(&g, 0) == NULL);
  ng_node_set_header_widget(&g, 999, (void *)1);
  ng_node_set_content_widget(&g, 0, (void *)1);
  ng_graph_destroy(&g);
}

/* ═══════════════════════════════════════════════════════════════
 *  Connections — Basic
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_connect_basic) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  ng_error_t err;
  uint32_t c = ng_connect(&g, a, 0, b, 0, &err);
  assert(c != 0);
  assert(err == NG_OK);
  assert(ng_connection_count(&g) == 1);
  ng_graph_destroy(&g);
}

TEST(test_connect_returns_id) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 2);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  uint32_t c = ng_node_create(&g, d);
  uint32_t c1 = ng_add_connection(&g, a, 0, b, 0);
  uint32_t c2 = ng_add_connection(&g, a, 0, c, 0);
  assert(c1 != 0 && c2 != 0);
  assert(c1 != c2);
  ng_graph_destroy(&g);
}

TEST(test_connect_null_err) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  uint32_t c = ng_connect(&g, a, 0, b, 0, NULL);
  assert(c != 0);
  ng_graph_destroy(&g);
}

/* ═══════════════════════════════════════════════════════════════
 *  Connections — Validation
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_validate_ok) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  assert(ng_validate_connection(&g, a, 0, b, 0) == NG_OK);
  ng_graph_destroy(&g);
}

TEST(test_validate_self_connection) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t a = ng_node_create(&g, d);
  assert(ng_validate_connection(&g, a, 0, a, 0) == NG_ERR_SELF_CONNECTION);
  ng_graph_destroy(&g);
}

TEST(test_validate_node_not_found) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t a = ng_node_create(&g, d);
  assert(ng_validate_connection(&g, a, 0, 999, 0) == NG_ERR_NODE_NOT_FOUND);
  assert(ng_validate_connection(&g, 999, 0, a, 0) == NG_ERR_NODE_NOT_FOUND);
  ng_graph_destroy(&g);
}

TEST(test_validate_pin_out_of_range) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  assert(ng_validate_connection(&g, a, 1, b, 0) == NG_ERR_PIN_OUT_OF_RANGE);
  assert(ng_validate_connection(&g, a, 0, b, 1) == NG_ERR_PIN_OUT_OF_RANGE);
  assert(ng_validate_connection(&g, a, -1, b, 0) == NG_ERR_PIN_OUT_OF_RANGE);
  assert(ng_validate_connection(&g, a, 0, b, -1) == NG_ERR_PIN_OUT_OF_RANGE);
  ng_graph_destroy(&g);
}

TEST(test_validate_type_mismatch) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d1 = ng_def_create(&g, "A", NG_COLOR(0,0,0,255));
  ng_def_add_output(&g, d1, "out", NG_PIN_FLOAT, NG_COLOR_FLOAT);
  uint32_t d2 = ng_def_create(&g, "B", NG_COLOR(0,0,0,255));
  ng_def_add_input(&g, d2, "in", NG_PIN_STRING, NG_COLOR_STRING);
  uint32_t a = ng_node_create(&g, d1);
  uint32_t b = ng_node_create(&g, d2);
  assert(ng_validate_connection(&g, a, 0, b, 0) == NG_ERR_TYPE_MISMATCH);
  ng_graph_destroy(&g);
}

TEST(test_validate_any_type_compatible) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d1 = ng_def_create(&g, "A", NG_COLOR(0,0,0,255));
  ng_def_add_output(&g, d1, "out", NG_PIN_FLOAT, NG_COLOR_FLOAT);
  uint32_t d2 = ng_def_create(&g, "B", NG_COLOR(0,0,0,255));
  ng_def_add_input(&g, d2, "in", NG_PIN_ANY, NG_COLOR_ANY);
  uint32_t a = ng_node_create(&g, d1);
  uint32_t b = ng_node_create(&g, d2);
  assert(ng_validate_connection(&g, a, 0, b, 0) == NG_OK);
  ng_graph_destroy(&g);
}

TEST(test_validate_any_output_to_typed_input) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d1 = ng_def_create(&g, "A", NG_COLOR(0,0,0,255));
  ng_def_add_output(&g, d1, "out", NG_PIN_ANY, NG_COLOR_ANY);
  uint32_t d2 = ng_def_create(&g, "B", NG_COLOR(0,0,0,255));
  ng_def_add_input(&g, d2, "in", NG_PIN_TEXTURE, NG_COLOR_TEXTURE);
  uint32_t a = ng_node_create(&g, d1);
  uint32_t b = ng_node_create(&g, d2);
  assert(ng_validate_connection(&g, a, 0, b, 0) == NG_OK);
  ng_graph_destroy(&g);
}

TEST(test_validate_duplicate) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  ng_add_connection(&g, a, 0, b, 0);
  assert(ng_validate_connection(&g, a, 0, b, 0) == NG_ERR_DUPLICATE);
  ng_graph_destroy(&g);
}

TEST(test_validate_input_occupied) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  uint32_t c = ng_node_create(&g, d);
  ng_add_connection(&g, a, 0, c, 0);
  assert(ng_validate_connection(&g, b, 0, c, 0) == NG_ERR_INPUT_OCCUPIED);
  ng_graph_destroy(&g);
}

TEST(test_connect_fails_on_validation) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t a = ng_node_create(&g, d);
  ng_error_t err;
  uint32_t c = ng_connect(&g, a, 0, a, 0, &err);
  assert(c == 0);
  assert(err == NG_ERR_SELF_CONNECTION);
  assert(ng_connection_count(&g) == 0);
  ng_graph_destroy(&g);
}

/* ═══════════════════════════════════════════════════════════════
 *  Connections — Type Compatibility Override
 * ═══════════════════════════════════════════════════════════════ */

static int allow_all(ng_pin_type_t from, ng_pin_type_t to, void *user) {
  (void)from; (void)to; (void)user;
  return 1;
}

static int deny_all(ng_pin_type_t from, ng_pin_type_t to, void *user) {
  (void)from; (void)to; (void)user;
  return 0;
}

TEST(test_type_compat_override_allow) {
  ng_graph_t g;
  make_graph(&g);
  ng_graph_set_type_compat(&g, allow_all, NULL);
  uint32_t d1 = ng_def_create(&g, "A", NG_COLOR(0,0,0,255));
  ng_def_add_output(&g, d1, "o", NG_PIN_FLOAT, NG_COLOR_FLOAT);
  uint32_t d2 = ng_def_create(&g, "B", NG_COLOR(0,0,0,255));
  ng_def_add_input(&g, d2, "i", NG_PIN_STRING, NG_COLOR_STRING);
  uint32_t a = ng_node_create(&g, d1);
  uint32_t b = ng_node_create(&g, d2);
  assert(ng_validate_connection(&g, a, 0, b, 0) == NG_OK);
  ng_graph_destroy(&g);
}

TEST(test_type_compat_override_deny) {
  ng_graph_t g;
  make_graph(&g);
  ng_graph_set_type_compat(&g, deny_all, NULL);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  assert(ng_validate_connection(&g, a, 0, b, 0) == NG_ERR_TYPE_MISMATCH);
  ng_graph_destroy(&g);
}

static int compat_with_user(ng_pin_type_t from, ng_pin_type_t to, void *user) {
  (void)from; (void)to;
  return *(int *)user;
}

TEST(test_type_compat_user_data) {
  ng_graph_t g;
  make_graph(&g);
  int flag = 0;
  ng_graph_set_type_compat(&g, compat_with_user, &flag);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  assert(ng_validate_connection(&g, a, 0, b, 0) == NG_ERR_TYPE_MISMATCH);
  flag = 1;
  assert(ng_validate_connection(&g, a, 0, b, 0) == NG_OK);
  ng_graph_destroy(&g);
}

/* ═══════════════════════════════════════════════════════════════
 *  Connections — Disconnect
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_disconnect_by_id) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  uint32_t c = ng_add_connection(&g, a, 0, b, 0);
  assert(ng_connection_count(&g) == 1);
  ng_disconnect(&g, c);
  assert(ng_connection_count(&g) == 0);
  ng_graph_destroy(&g);
}

TEST(test_disconnect_invalid_id) {
  ng_graph_t g;
  make_graph(&g);
  ng_disconnect(&g, 0);
  ng_disconnect(&g, 999);
  ng_graph_destroy(&g);
}

TEST(test_disconnect_pin_output) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 2, 1);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  uint32_t c = ng_node_create(&g, d);
  ng_add_connection(&g, a, 0, b, 0);
  ng_add_connection(&g, a, 0, c, 0);
  assert(ng_connection_count(&g) == 2);
  ng_disconnect_pin(&g, a, NG_PIN_OUTPUT, 0);
  assert(ng_connection_count(&g) == 0);
  ng_graph_destroy(&g);
}

TEST(test_disconnect_pin_input) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 2, 1);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  ng_add_connection(&g, a, 0, b, 0);
  ng_add_connection(&g, a, 0, b, 1);
  assert(ng_connection_count(&g) == 2);
  ng_disconnect_pin(&g, b, NG_PIN_INPUT, 0);
  assert(ng_connection_count(&g) == 1);
  ng_graph_destroy(&g);
}

TEST(test_disconnect_node) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  uint32_t c = ng_node_create(&g, d);
  ng_add_connection(&g, a, 0, b, 0);
  ng_add_connection(&g, b, 0, c, 0);
  assert(ng_connection_count(&g) == 2);
  ng_disconnect_node(&g, b);
  assert(ng_connection_count(&g) == 0);
  ng_graph_destroy(&g);
}

TEST(test_disconnect_preserves_others) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  uint32_t c = ng_node_create(&g, d);
  uint32_t x = ng_node_create(&g, d);
  ng_add_connection(&g, a, 0, b, 0);
  uint32_t keep = ng_add_connection(&g, c, 0, x, 0);
  assert(ng_connection_count(&g) == 2);
  ng_disconnect_node(&g, a);
  assert(ng_connection_count(&g) == 1);
  /* The remaining connection should be c->x */
  ng_connection_view_t v;
  assert(ng_get_connection_view(&g, 0, &v));
  assert(v.id == keep);
  ng_graph_destroy(&g);
}

/* ═══════════════════════════════════════════════════════════════
 *  Queries
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_is_input_connected) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 2, 1);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  assert(ng_is_input_connected(&g, b, 0) == 0);
  ng_add_connection(&g, a, 0, b, 0);
  assert(ng_is_input_connected(&g, b, 0) == 1);
  assert(ng_is_input_connected(&g, b, 1) == 0);
  ng_graph_destroy(&g);
}

TEST(test_is_output_connected) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 2);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  assert(ng_is_output_connected(&g, a, 0) == 0);
  ng_add_connection(&g, a, 0, b, 0);
  assert(ng_is_output_connected(&g, a, 0) == 1);
  assert(ng_is_output_connected(&g, a, 1) == 0);
  ng_graph_destroy(&g);
}

/* ═══════════════════════════════════════════════════════════════
 *  Cycle Detection
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_has_cycle_empty) {
  ng_graph_t g;
  make_graph(&g);
  assert(ng_has_cycle(&g) == 0);
  ng_graph_destroy(&g);
}

TEST(test_has_cycle_no_connections) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  ng_node_create(&g, d);
  ng_node_create(&g, d);
  assert(ng_has_cycle(&g) == 0);
  ng_graph_destroy(&g);
}

TEST(test_has_cycle_linear) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  uint32_t c = ng_node_create(&g, d);
  ng_add_connection(&g, a, 0, b, 0);
  ng_add_connection(&g, b, 0, c, 0);
  assert(ng_has_cycle(&g) == 0);
  ng_graph_destroy(&g);
}

TEST(test_has_cycle_diamond) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 2, 2);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  uint32_t c = ng_node_create(&g, d);
  uint32_t x = ng_node_create(&g, d);
  ng_add_connection(&g, a, 0, b, 0);
  ng_add_connection(&g, a, 1, c, 0);
  ng_add_connection(&g, b, 0, x, 0);
  ng_add_connection(&g, c, 0, x, 1);
  assert(ng_has_cycle(&g) == 0);
  ng_graph_destroy(&g);
}

TEST(test_has_cycle_two_node_cycle) {
  ng_graph_t g;
  make_graph(&g);
  /* Need to bypass validation (self-connection check doesn't apply,
     but input-occupied does). Use 2 pins each. */
  uint32_t d = make_float_def(&g, "D", 2, 2);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  ng_add_connection(&g, a, 0, b, 0);
  ng_add_connection(&g, b, 0, a, 0);
  assert(ng_has_cycle(&g) == 1);
  ng_graph_destroy(&g);
}

TEST(test_has_cycle_three_node_cycle) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 2, 2);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  uint32_t c = ng_node_create(&g, d);
  ng_add_connection(&g, a, 0, b, 0);
  ng_add_connection(&g, b, 0, c, 0);
  ng_add_connection(&g, c, 0, a, 0);
  assert(ng_has_cycle(&g) == 1);
  ng_graph_destroy(&g);
}

TEST(test_has_cycle_disconnected_components) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 2, 2);
  /* Component 1: linear */
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  ng_add_connection(&g, a, 0, b, 0);
  /* Component 2: cycle */
  uint32_t c = ng_node_create(&g, d);
  uint32_t x = ng_node_create(&g, d);
  ng_add_connection(&g, c, 0, x, 0);
  ng_add_connection(&g, x, 0, c, 0);
  assert(ng_has_cycle(&g) == 1);
  ng_graph_destroy(&g);
}

TEST(test_has_cycle_after_removing_cycle_edge) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 2, 2);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  ng_add_connection(&g, a, 0, b, 0);
  uint32_t back = ng_add_connection(&g, b, 0, a, 0);
  assert(ng_has_cycle(&g) == 1);
  ng_disconnect(&g, back);
  assert(ng_has_cycle(&g) == 0);
  ng_graph_destroy(&g);
}

/* ═══════════════════════════════════════════════════════════════
 *  Helper Functions
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_add_node_helper) {
  ng_graph_t g;
  make_graph(&g);
  ng_pin_desc_t ins[] = {
    {"A", NG_PIN_FLOAT, NG_COLOR_FLOAT},
    {"B", NG_PIN_INT,   NG_COLOR_INT},
  };
  ng_pin_desc_t outs[] = {
    {"Result", NG_PIN_FLOAT, NG_COLOR_FLOAT},
  };
  uint32_t n = ng_add_node(&g, "Mix", NG_COLOR(100,100,200,255), ins, 2, outs, 1);
  assert(n != 0);
  assert(ng_node_count(&g) == 1);
  assert(ng_def_count(&g) == 1);
  ng_node_view_t v;
  assert(ng_get_node_view(&g, 0, &v));
  assert(strcmp(v.name, "Mix") == 0);
  assert(v.input_count == 2);
  assert(v.output_count == 1);
  assert(v.inputs[0].type == NG_PIN_FLOAT);
  assert(v.inputs[1].type == NG_PIN_INT);
  assert(v.outputs[0].type == NG_PIN_FLOAT);
  ng_graph_destroy(&g);
}

TEST(test_add_node_no_pins) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t n = ng_add_node(&g, "Empty", NG_COLOR(0,0,0,255), NULL, 0, NULL, 0);
  assert(n != 0);
  ng_node_view_t v;
  assert(ng_get_node_view(&g, 0, &v));
  assert(v.input_count == 0);
  assert(v.output_count == 0);
  ng_graph_destroy(&g);
}

TEST(test_add_connection_helper) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t a = add_float_node(&g, "A", 0, 1);
  uint32_t b = add_float_node(&g, "B", 1, 0);
  uint32_t c = ng_add_connection(&g, a, 0, b, 0);
  assert(c != 0);
  assert(ng_connection_count(&g) == 1);
  ng_graph_destroy(&g);
}

TEST(test_add_connection_fails_validation) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t a = add_float_node(&g, "A", 1, 1);
  uint32_t c = ng_add_connection(&g, a, 0, a, 0);
  assert(c == 0);
  ng_graph_destroy(&g);
}

/* ═══════════════════════════════════════════════════════════════
 *  Node Views
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_node_view_basic) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "MyNode", 2, 3);
  uint32_t n = ng_node_create(&g, d);
  int x = 7;
  ng_node_set_user_data(&g, n, &x);
  ng_node_set_header_widget(&g, n, (void *)0xAA);
  ng_node_set_content_widget(&g, n, (void *)0xBB);

  ng_node_view_t v;
  assert(ng_get_node_view(&g, 0, &v));
  assert(v.id == n);
  assert(v.def_id == d);
  assert(strcmp(v.name, "MyNode") == 0);
  assert(v.input_count == 2);
  assert(v.output_count == 3);
  assert(v.user_data == &x);
  assert(v.header_widget == (void *)0xAA);
  assert(v.content_widget == (void *)0xBB);
  ng_graph_destroy(&g);
}

TEST(test_node_view_out_of_range) {
  ng_graph_t g;
  make_graph(&g);
  ng_node_view_t v;
  assert(ng_get_node_view(&g, 0, &v) == 0);
  assert(ng_get_node_view(&g, -1, &v) == 0);
  assert(ng_get_node_view(&g, 100, &v) == 0);
  ng_graph_destroy(&g);
}

/* ═══════════════════════════════════════════════════════════════
 *  Connection Views
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_connection_view) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d1 = ng_def_create(&g, "Src", NG_COLOR(0,0,0,255));
  ng_def_add_output(&g, d1, "Color", NG_PIN_COLOR, NG_COLOR_COLOR);
  uint32_t d2 = ng_def_create(&g, "Dst", NG_COLOR(0,0,0,255));
  ng_def_add_input(&g, d2, "Tint", NG_PIN_COLOR, NG_COLOR_COLOR);
  uint32_t a = ng_node_create(&g, d1);
  uint32_t b = ng_node_create(&g, d2);
  uint32_t c = ng_add_connection(&g, a, 0, b, 0);

  ng_connection_view_t v;
  assert(ng_get_connection_view(&g, 0, &v));
  assert(v.id == c);
  assert(v.from_node == a);
  assert(v.from_pin == 0);
  assert(strcmp(v.from_pin_name, "Color") == 0);
  assert(v.from_type == NG_PIN_COLOR);
  assert(v.to_node == b);
  assert(v.to_pin == 0);
  assert(strcmp(v.to_pin_name, "Tint") == 0);
  assert(v.to_type == NG_PIN_COLOR);
  ng_graph_destroy(&g);
}

TEST(test_connection_view_out_of_range) {
  ng_graph_t g;
  make_graph(&g);
  ng_connection_view_t v;
  assert(ng_get_connection_view(&g, 0, &v) == 0);
  assert(ng_get_connection_view(&g, -1, &v) == 0);
  ng_graph_destroy(&g);
}

/* ═══════════════════════════════════════════════════════════════
 *  Iteration — Nodes
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_iterate_nodes_empty) {
  ng_graph_t g;
  make_graph(&g);
  node_result_t r = {0};
  int visited = ng_iterate_nodes(&g, node_cb, &r);
  assert(visited == 0);
  assert(r.count == 0);
  ng_graph_destroy(&g);
}

TEST(test_iterate_nodes_all) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 0, 0);
  uint32_t n1 = ng_node_create(&g, d);
  uint32_t n2 = ng_node_create(&g, d);
  uint32_t n3 = ng_node_create(&g, d);
  node_result_t r = {0};
  int visited = ng_iterate_nodes(&g, node_cb, &r);
  assert(visited == 3);
  assert(r.ids[0] == n1);
  assert(r.ids[1] == n2);
  assert(r.ids[2] == n3);
  ng_graph_destroy(&g);
}

TEST(test_iterate_nodes_early_stop) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 0, 0);
  ng_node_create(&g, d);
  ng_node_create(&g, d);
  ng_node_create(&g, d);
  int count = 0;
  int visited = ng_iterate_nodes(&g, node_cb_stop_at_1, &count);
  assert(visited == 1);
  assert(count == 1);
  ng_graph_destroy(&g);
}

/* ═══════════════════════════════════════════════════════════════
 *  Iteration — Connections
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_iterate_connections_empty) {
  ng_graph_t g;
  make_graph(&g);
  conn_result_t r = {0};
  int visited = ng_iterate_connections(&g, conn_cb, &r);
  assert(visited == 0);
  ng_graph_destroy(&g);
}

TEST(test_iterate_connections_all) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 2, 2);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  uint32_t c = ng_node_create(&g, d);
  uint32_t c1 = ng_add_connection(&g, a, 0, b, 0);
  uint32_t c2 = ng_add_connection(&g, b, 0, c, 0);
  conn_result_t r = {0};
  int visited = ng_iterate_connections(&g, conn_cb, &r);
  assert(visited == 2);
  assert(r.ids[0] == c1);
  assert(r.ids[1] == c2);
  ng_graph_destroy(&g);
}

TEST(test_iterate_connections_early_stop) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 2, 2);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  uint32_t c = ng_node_create(&g, d);
  ng_add_connection(&g, a, 0, b, 0);
  ng_add_connection(&g, b, 0, c, 0);
  int count = 0;
  int visited = ng_iterate_connections(&g, conn_cb_stop_at_1, &count);
  assert(visited == 1);
  ng_graph_destroy(&g);
}

/* ═══════════════════════════════════════════════════════════════
 *  Iteration — Per-Node Connections
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_iterate_node_connections) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 2, 2);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  uint32_t c = ng_node_create(&g, d);
  uint32_t c1 = ng_add_connection(&g, a, 0, b, 0);
  uint32_t c2 = ng_add_connection(&g, b, 0, c, 0);
  uint32_t c3 = ng_add_connection(&g, a, 1, c, 1);

  /* b has two connections: c1 (incoming) and c2 (outgoing) */
  conn_result_t r = {0};
  int visited = ng_iterate_node_connections(&g, b, conn_cb, &r);
  assert(visited == 2);
  assert(r.ids[0] == c1);
  assert(r.ids[1] == c2);

  /* c has c2 (incoming) and c3 (incoming) */
  conn_result_t r2 = {0};
  visited = ng_iterate_node_connections(&g, c, conn_cb, &r2);
  assert(visited == 2);
  (void)c3;
  ng_graph_destroy(&g);
}

TEST(test_iterate_node_connections_empty) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t a = ng_node_create(&g, d);
  conn_result_t r = {0};
  int visited = ng_iterate_node_connections(&g, a, conn_cb, &r);
  assert(visited == 0);
  ng_graph_destroy(&g);
}

/* ═══════════════════════════════════════════════════════════════
 *  Topological Iteration
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_topo_empty) {
  ng_graph_t g;
  make_graph(&g);
  topo_result_t r = {0};
  int visited = ng_iterate_topo(&g, topo_cb, &r);
  assert(visited == 0);
  ng_graph_destroy(&g);
}

TEST(test_topo_single_node) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 0, 0);
  uint32_t n = ng_node_create(&g, d);
  topo_result_t r = {0};
  int visited = ng_iterate_topo(&g, topo_cb, &r);
  assert(visited == 1);
  assert(r.ids[0] == n);
  ng_graph_destroy(&g);
}

TEST(test_topo_linear_chain) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  uint32_t c = ng_node_create(&g, d);
  ng_add_connection(&g, a, 0, b, 0);
  ng_add_connection(&g, b, 0, c, 0);
  topo_result_t r = {0};
  int visited = ng_iterate_topo(&g, topo_cb, &r);
  assert(visited == 3);
  assert(r.ids[0] == a);
  assert(r.ids[1] == b);
  assert(r.ids[2] == c);
  ng_graph_destroy(&g);
}

TEST(test_topo_diamond) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 2, 2);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  uint32_t c = ng_node_create(&g, d);
  uint32_t x = ng_node_create(&g, d);
  ng_add_connection(&g, a, 0, b, 0);
  ng_add_connection(&g, a, 1, c, 0);
  ng_add_connection(&g, b, 0, x, 0);
  ng_add_connection(&g, c, 0, x, 1);
  topo_result_t r = {0};
  int visited = ng_iterate_topo(&g, topo_cb, &r);
  assert(visited == 4);
  /* a must come first, x must come last */
  assert(r.ids[0] == a);
  assert(r.ids[3] == x);
  ng_graph_destroy(&g);
}

TEST(test_topo_skips_cycle) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 2, 2);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  ng_add_connection(&g, a, 0, b, 0);
  ng_add_connection(&g, b, 0, a, 0);
  topo_result_t r = {0};
  int visited = ng_iterate_topo(&g, topo_cb, &r);
  assert(visited == 0); /* Both have in-degree > 0, neither enqueued */
  ng_graph_destroy(&g);
}

TEST(test_topo_partial_cycle) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 2, 2);
  /* source -> (cycle: a <-> b) */
  uint32_t src = ng_node_create(&g, d);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  ng_add_connection(&g, src, 0, a, 0);
  ng_add_connection(&g, a, 0, b, 0);
  ng_add_connection(&g, b, 0, a, 1);
  topo_result_t r = {0};
  int visited = ng_iterate_topo(&g, topo_cb, &r);
  assert(visited == 1); /* Only src is visited */
  assert(r.ids[0] == src);
  ng_graph_destroy(&g);
}

TEST(test_topo_early_stop) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  ng_node_create(&g, d);
  ng_node_create(&g, d);
  ng_node_create(&g, d);
  int count = 0;
  int visited = ng_iterate_topo(&g, topo_cb_stop_at_1, &count);
  assert(visited == 1);
  ng_graph_destroy(&g);
}

TEST(test_topo_disconnected) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  ng_node_create(&g, d);
  ng_node_create(&g, d);
  ng_node_create(&g, d);
  topo_result_t r = {0};
  int visited = ng_iterate_topo(&g, topo_cb, &r);
  assert(visited == 3); /* All sources, no connections */
  ng_graph_destroy(&g);
}

/* ═══════════════════════════════════════════════════════════════
 *  Reconnection After Disconnect
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_reconnect_after_disconnect) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  uint32_t c1 = ng_add_connection(&g, a, 0, b, 0);
  ng_disconnect(&g, c1);
  assert(ng_connection_count(&g) == 0);
  uint32_t c2 = ng_add_connection(&g, a, 0, b, 0);
  assert(c2 != 0);
  assert(c2 != c1); /* IDs are never reused */
  assert(ng_connection_count(&g) == 1);
  ng_graph_destroy(&g);
}

TEST(test_reconnect_input_after_previous_removed) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  uint32_t c = ng_node_create(&g, d);
  uint32_t c1 = ng_add_connection(&g, a, 0, c, 0);
  /* c's input 0 is occupied */
  assert(ng_validate_connection(&g, b, 0, c, 0) == NG_ERR_INPUT_OCCUPIED);
  ng_disconnect(&g, c1);
  /* Now it's free */
  assert(ng_validate_connection(&g, b, 0, c, 0) == NG_OK);
  ng_graph_destroy(&g);
}

/* ═══════════════════════════════════════════════════════════════
 *  Edge Cases
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_output_fan_out) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  uint32_t c = ng_node_create(&g, d);
  uint32_t x = ng_node_create(&g, d);
  ng_add_connection(&g, a, 0, b, 0);
  ng_add_connection(&g, a, 0, c, 0);
  ng_add_connection(&g, a, 0, x, 0);
  assert(ng_connection_count(&g) == 3);
  assert(ng_is_output_connected(&g, a, 0) == 1);
  ng_graph_destroy(&g);
}

TEST(test_many_pins) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "Big", 100, 100);
  const ng_node_def_t *def = ng_def_get(&g, d);
  assert(def->input_count == 100);
  assert(def->output_count == 100);
  uint32_t n = ng_node_create(&g, d);
  assert(n != 0);
  ng_graph_destroy(&g);
}

TEST(test_name_is_copied) {
  ng_graph_t g;
  make_graph(&g);
  char buf[32];
  strcpy(buf, "Original");
  uint32_t d = ng_def_create(&g, buf, NG_COLOR(0,0,0,255));
  strcpy(buf, "Modified"); /* Modify the source */
  const ng_node_def_t *def = ng_def_get(&g, d);
  assert(strcmp(def->name, "Original") == 0); /* Must be a copy */
  ng_graph_destroy(&g);
}

TEST(test_pin_name_is_copied) {
  ng_graph_t g;
  make_graph(&g);
  char buf[32];
  strcpy(buf, "MyPin");
  uint32_t d = ng_def_create(&g, "D", NG_COLOR(0,0,0,255));
  ng_def_add_input(&g, d, buf, NG_PIN_FLOAT, NG_COLOR_FLOAT);
  strcpy(buf, "CHANGED");
  const ng_node_def_t *def = ng_def_get(&g, d);
  assert(strcmp(def->inputs[0].name, "MyPin") == 0);
  ng_graph_destroy(&g);
}

/* ═══════════════════════════════════════════════════════════════
 *  Stress Tests
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_stress_many_nodes) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t ids[500];
  for (int i = 0; i < 500; i++) {
    ids[i] = ng_node_create(&g, d);
    assert(ids[i] != 0);
  }
  assert(ng_node_count(&g) == 500);
  /* Verify all accessible */
  for (int i = 0; i < 500; i++) {
    ng_node_view_t v;
    assert(ng_get_node_view(&g, i, &v));
    assert(v.id == ids[i]);
  }
  ng_graph_destroy(&g);
}

TEST(test_stress_many_connections) {
  ng_graph_t g;
  make_graph(&g);
  /* Create a long chain: n0 -> n1 -> n2 -> ... -> n199 */
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t ids[200];
  for (int i = 0; i < 200; i++)
    ids[i] = ng_node_create(&g, d);
  for (int i = 0; i < 199; i++) {
    uint32_t c = ng_add_connection(&g, ids[i], 0, ids[i + 1], 0);
    assert(c != 0);
  }
  assert(ng_connection_count(&g) == 199);
  assert(ng_has_cycle(&g) == 0);

  topo_result_t r = {0};
  int visited = ng_iterate_topo(&g, topo_cb, &r);
  assert(visited == 200);
  assert(r.ids[0] == ids[0]);
  assert(r.ids[199] == ids[199]);
  ng_graph_destroy(&g);
}

TEST(test_stress_many_defs) {
  ng_graph_t g;
  make_graph(&g);
  for (int i = 0; i < 300; i++) {
    uint32_t d = ng_def_create(&g, "D", NG_COLOR(0,0,0,255));
    assert(d != 0);
    ng_def_add_input(&g, d, "in", NG_PIN_FLOAT, NG_COLOR_FLOAT);
    ng_def_add_output(&g, d, "out", NG_PIN_FLOAT, NG_COLOR_FLOAT);
  }
  assert(ng_def_count(&g) == 300);
  ng_graph_destroy(&g);
}

TEST(test_stress_create_destroy_cycle) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  for (int round = 0; round < 50; round++) {
    uint32_t ids[20];
    for (int i = 0; i < 20; i++)
      ids[i] = ng_node_create(&g, d);
    for (int i = 0; i < 19; i++)
      ng_add_connection(&g, ids[i], 0, ids[i + 1], 0);
    for (int i = 19; i >= 0; i--)
      ng_node_destroy(&g, ids[i]);
    assert(ng_node_count(&g) == 0);
    assert(ng_connection_count(&g) == 0);
  }
  ng_graph_destroy(&g);
}

TEST(test_stress_disconnect_reconnect) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t a = ng_node_create(&g, d);
  uint32_t b = ng_node_create(&g, d);
  for (int i = 0; i < 200; i++) {
    uint32_t c = ng_add_connection(&g, a, 0, b, 0);
    assert(c != 0);
    ng_disconnect(&g, c);
    assert(ng_connection_count(&g) == 0);
  }
  ng_graph_destroy(&g);
}

TEST(test_stress_add_node_helper) {
  ng_graph_t g;
  make_graph(&g);
  for (int i = 0; i < 200; i++) {
    uint32_t n = add_float_node(&g, "N", 3, 2);
    assert(n != 0);
  }
  assert(ng_node_count(&g) == 200);
  assert(ng_def_count(&g) == 200);
  ng_graph_destroy(&g);
}

TEST(test_stress_topo_wide) {
  /* Star topology: 1 source -> 100 sinks */
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 1, 1);
  uint32_t src = ng_node_create(&g, d);
  for (int i = 0; i < 100; i++) {
    uint32_t sink = ng_node_create(&g, d);
    ng_add_connection(&g, src, 0, sink, 0);
  }
  assert(ng_connection_count(&g) == 100);
  topo_result_t r = {0};
  int visited = ng_iterate_topo(&g, topo_cb, &r);
  assert(visited == 101);
  assert(r.ids[0] == src); /* source first */
  ng_graph_destroy(&g);
}

TEST(test_stress_cycle_detection_large) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 2, 2);
  uint32_t ids[100];
  for (int i = 0; i < 100; i++)
    ids[i] = ng_node_create(&g, d);
  /* Long chain */
  for (int i = 0; i < 99; i++)
    ng_add_connection(&g, ids[i], 0, ids[i + 1], 0);
  assert(ng_has_cycle(&g) == 0);
  /* Close the loop */
  ng_add_connection(&g, ids[99], 0, ids[0], 1);
  assert(ng_has_cycle(&g) == 1);
  ng_graph_destroy(&g);
}

TEST(test_stress_mixed_operations) {
  ng_graph_t g;
  make_graph(&g);
  uint32_t d = make_float_def(&g, "D", 3, 3);
  for (int round = 0; round < 30; round++) {
    uint32_t n1 = ng_node_create(&g, d);
    uint32_t n2 = ng_node_create(&g, d);
    uint32_t n3 = ng_node_create(&g, d);
    ng_add_connection(&g, n1, 0, n2, 0);
    ng_add_connection(&g, n2, 0, n3, 0);
    ng_add_connection(&g, n1, 1, n3, 1);
    assert(ng_has_cycle(&g) == 0);
    ng_node_set_header_widget(&g, n1, (void *)(uintptr_t)round);
    ng_node_set_content_widget(&g, n2, (void *)(uintptr_t)(round + 1));
    ng_node_destroy(&g, n2);
  }
  ng_graph_destroy(&g);
}

/* ═══════════════════════════════════════════════════════════════
 *  main
 * ═══════════════════════════════════════════════════════════════ */

int main(void) {
  printf("test_nodegraph\n");

  printf(" Lifecycle:\n");
  RUN(test_create_destroy);
  RUN(test_create_initial_state);
  RUN(test_destroy_empty);
  RUN(test_destroy_with_content);
  RUN(test_double_destroy);

  printf(" Node Definitions — Create:\n");
  RUN(test_def_create);
  RUN(test_def_create_multiple);
  RUN(test_def_ids_monotonic);
  RUN(test_def_create_null_name);
  RUN(test_def_get);
  RUN(test_def_get_invalid);

  printf(" Node Definitions — Pins:\n");
  RUN(test_def_add_input);
  RUN(test_def_add_output);
  RUN(test_def_add_multiple_pins);
  RUN(test_def_add_pin_invalid_def);
  RUN(test_def_pin_types);
  RUN(test_def_pin_colors);

  printf(" Node Instances — Create & Destroy:\n");
  RUN(test_node_create);
  RUN(test_node_create_invalid_def);
  RUN(test_node_create_multiple_same_def);
  RUN(test_node_ids_monotonic);
  RUN(test_node_destroy);
  RUN(test_node_destroy_invalid);
  RUN(test_node_destroy_removes_connections);
  RUN(test_node_destroy_middle);

  printf(" Node Instances — user_data & widgets:\n");
  RUN(test_node_user_data);
  RUN(test_node_user_data_default_null);
  RUN(test_node_user_data_invalid);
  RUN(test_node_header_widget);
  RUN(test_node_content_widget);
  RUN(test_node_widgets_default_null);
  RUN(test_node_widgets_invalid);

  printf(" Connections — Basic:\n");
  RUN(test_connect_basic);
  RUN(test_connect_returns_id);
  RUN(test_connect_null_err);

  printf(" Connections — Validation:\n");
  RUN(test_validate_ok);
  RUN(test_validate_self_connection);
  RUN(test_validate_node_not_found);
  RUN(test_validate_pin_out_of_range);
  RUN(test_validate_type_mismatch);
  RUN(test_validate_any_type_compatible);
  RUN(test_validate_any_output_to_typed_input);
  RUN(test_validate_duplicate);
  RUN(test_validate_input_occupied);
  RUN(test_connect_fails_on_validation);

  printf(" Connections — Type Compatibility Override:\n");
  RUN(test_type_compat_override_allow);
  RUN(test_type_compat_override_deny);
  RUN(test_type_compat_user_data);

  printf(" Connections — Disconnect:\n");
  RUN(test_disconnect_by_id);
  RUN(test_disconnect_invalid_id);
  RUN(test_disconnect_pin_output);
  RUN(test_disconnect_pin_input);
  RUN(test_disconnect_node);
  RUN(test_disconnect_preserves_others);

  printf(" Queries:\n");
  RUN(test_is_input_connected);
  RUN(test_is_output_connected);

  printf(" Cycle Detection:\n");
  RUN(test_has_cycle_empty);
  RUN(test_has_cycle_no_connections);
  RUN(test_has_cycle_linear);
  RUN(test_has_cycle_diamond);
  RUN(test_has_cycle_two_node_cycle);
  RUN(test_has_cycle_three_node_cycle);
  RUN(test_has_cycle_disconnected_components);
  RUN(test_has_cycle_after_removing_cycle_edge);

  printf(" Helper Functions:\n");
  RUN(test_add_node_helper);
  RUN(test_add_node_no_pins);
  RUN(test_add_connection_helper);
  RUN(test_add_connection_fails_validation);

  printf(" Node Views:\n");
  RUN(test_node_view_basic);
  RUN(test_node_view_out_of_range);

  printf(" Connection Views:\n");
  RUN(test_connection_view);
  RUN(test_connection_view_out_of_range);

  printf(" Iteration — Nodes:\n");
  RUN(test_iterate_nodes_empty);
  RUN(test_iterate_nodes_all);
  RUN(test_iterate_nodes_early_stop);

  printf(" Iteration — Connections:\n");
  RUN(test_iterate_connections_empty);
  RUN(test_iterate_connections_all);
  RUN(test_iterate_connections_early_stop);

  printf(" Iteration — Per-Node Connections:\n");
  RUN(test_iterate_node_connections);
  RUN(test_iterate_node_connections_empty);

  printf(" Topological Iteration:\n");
  RUN(test_topo_empty);
  RUN(test_topo_single_node);
  RUN(test_topo_linear_chain);
  RUN(test_topo_diamond);
  RUN(test_topo_skips_cycle);
  RUN(test_topo_partial_cycle);
  RUN(test_topo_early_stop);
  RUN(test_topo_disconnected);

  printf(" Reconnection:\n");
  RUN(test_reconnect_after_disconnect);
  RUN(test_reconnect_input_after_previous_removed);

  printf(" Edge Cases:\n");
  RUN(test_output_fan_out);
  RUN(test_many_pins);
  RUN(test_name_is_copied);
  RUN(test_pin_name_is_copied);

  printf(" Stress Tests:\n");
  RUN(test_stress_many_nodes);
  RUN(test_stress_many_connections);
  RUN(test_stress_many_defs);
  RUN(test_stress_create_destroy_cycle);
  RUN(test_stress_disconnect_reconnect);
  RUN(test_stress_add_node_helper);
  RUN(test_stress_topo_wide);
  RUN(test_stress_cycle_detection_large);
  RUN(test_stress_mixed_operations);

  printf("\nAll nodegraph tests passed.\n");
  return 0;
}