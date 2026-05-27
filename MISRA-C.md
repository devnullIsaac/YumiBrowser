# MISRA-C Compliance and Deviation Documentation

If you do not know what a MISRA-C Standard is, then you may need to purchase your own copy of the book found at https://misra.org.uk and read to understand it's purposes.

Compliance applies ONLY to Yumi Browser’s code – not dependencies.

"MISRA-C compliance is claimed solely for Yumi Browser’s own source files."
"Dependencies (FFmpeg, SDL3, DuckDB, etc.) are explicitly excluded from compliance claims."

# MISRA-C:2025 Deviations for Yumi Browser

## Deviation 1: Directive 4.12 (Dynamic Memory Allocation)
**Violated in:** All dependencies of Yumi Browser.
**Justification:** These libraries rely on dynamic memory for core functionality (e.g., media decoding, query execution). Refactoring them is impractical for a sole developer. This is not a safety-critical project—crashes are acceptable in a general-purpose browser, MISRA-C Compliance is to strengthen overall code to improve resiliency.
**Mitigations:**
- Static analysis (Frama-C) scans the browser’s code and libraries code to address any bugs found.
**Alternative Approaches Considered:**
- Static allocation → Impossible for variable-sized data (require refactoring library codes.)
- Memory pools → Would require rewriting library internals.
- Replacing Malloc/Free with custom static allocation → Not practical due to driver implementation that is linked in with this project. This have been tried before.

## Deviation 2: Rule 11.3 (Cast Between Pointers to Different Object Types) — `static_heap(x, y)` macro
**Violated in:** `tests/test_static_heap.c` (and any future translation unit that instantiates the `static_heap(x, y)` macro family).
**Specific construct:** Inside `release_##x##_inline` and `release_##x##_ptr`, the user-facing `x*` pointer returned by `lease_*` is converted back to the enclosing intrusive-list node type `x##_linked_list_node` (a `struct x##_linked_list_node *`) via a direct cast:

```c
x##_linked_list_node item = (x##_linked_list_node)node;
```

**Why the cast is sound (despite the rule):**
- The static buffer is declared as `static struct x##_linked_list_node x##_buffer[y];` — every byte the user can ever hold a pointer into is, at the language-of-allocation level, a member of a `struct x##_linked_list_node` object.
- The user payload field `bindings` is the first non-static-storage member of the node struct, so a `_Static_assert(offsetof(struct x##_linked_list_node, bindings) == 0, ...)` holds and is enforced at compile time by the macro itself.
- C11 §6.7.2.1 ¶15 guarantees: *"A pointer to a structure object, suitably converted, points to its initial member ... and vice versa."* The cast performed in `release_*` is precisely the "vice versa" direction that the C standard explicitly blesses.
- Strict aliasing (C11 §6.5 ¶7) is not violated: the storage was *defined* as `struct x##_linked_list_node`, and both `x*` (accessing the `bindings` member) and `struct x##_linked_list_node *` (accessing the whole node) are valid lvalue types for that storage.

**Justification for deviating rather than refactoring:**
Three legitimate refactors were considered and rejected:
- **Parallel index array** (replace `next` pointers with `uint32_t` indices in a sibling array): rejected — changes the data layout and doubles the cold-line working set on free-list traversal.
- **`void*` intermediary** (`void* tmp = node; x##_linked_list_node item = tmp;`): rejected — trades a Rule 11.3 *required* violation for a Rule 11.5 *advisory* violation. Net win is purely cosmetic; the actual operation is unchanged.
- **`char*` byte arithmetic** (`(char*)node - (char*)x##_buffer` to derive an array index, then `&x##_buffer[idx]`): rejected — adds a division by `sizeof(node)` on every release for no semantic benefit when the existing cast is already C-standard-defined.

**Scope of the deviation:**
- Exactly **two** cast sites per macro instantiation: one in `release_##x##_inline`, one in `release_##x##_ptr`.
- No other `static_heap` operation performs a Rule 11.3 cast. `lease_*` returns `&item->bindings` (member-address operator — no cast). All internal list manipulation operates on the native `x##_linked_list_node` type.

**Mitigations:**
- `_Static_assert` enforces `offsetof(node, bindings) == 0` at every macro instantiation. If a future maintainer reorders the struct, the build fails immediately rather than silently breaking the deviation's soundness argument.
- The cast is wrapped in macro-generated code that no caller can subvert; user code never sees the node type.
- A comment block on each cast site cross-references this deviation document.

**Risk assessment:** **None.** The deviation rests on a guarantee provided by the C standard itself; an analyzer that flags this construct is reporting a *style* concern, not a *safety* concern.

## Deviation 3: Rule 17.7 (Unused Return Value) — `mtx_unlock` in `static_heap(x, y)` macro
**Violated in:** `tests/test_static_heap.c`, inside `static_heap(x, y)`-generated functions (`init_static_heap_##x`, `x##_refill_from_global`, `x##_spill_to_global`, `x##_tls_flush`).
**Specific construct:** `(void)mtx_unlock(&x##_global_mtx);`

**Why the return value is discarded:**
- Per C11 §7.26.4.6, `mtx_unlock` returns `thrd_error` only if (a) the mutex is invalid, or (b) the calling thread does not hold the lock.
- Both preconditions are made impossible by construction:
  - The mutex is initialised exactly once via `call_once`, and the success of `mtx_init` is captured in `x##_global_mtx_ok`. Every code path that calls `mtx_unlock` is reached only after a successful `mtx_lock` on that same mutex (via the `x##_lock_global()` helper, which checks `x##_global_mtx_ok` and the return value of `mtx_lock`).
  - The thread that calls `mtx_unlock` is the same thread that just successfully called `mtx_lock` — there is no path that unlocks a mutex held by another thread.
- Therefore the only documented failure modes of `mtx_unlock` are physically unreachable. A non-`thrd_success` return at this point would indicate corrupted runtime state for which there is no meaningful recovery action.

**Mitigations:**
- All other C11 threading primitives whose failures *are* reachable (`mtx_init`, `mtx_lock`, `tss_create`, `tss_set`) have their return values checked, captured in success-flags, and propagated through the new `YUMI_MEMORY_ALLOC_INTERNAL_ERROR` enum value.
- Each `(void)mtx_unlock` site is paired with an inline comment naming this deviation.

**Risk assessment:** **None.** Checking the return would require defining behaviour for an impossible state.

## Deviation 4: Directive 4.9 (Function-Like Macros) and Rule 20.10 (`#` and `##` Operators) — `static_heap(x, y)` macro
**Violated in:** `tests/test_static_heap.c`, the entire body of the `static_heap(x, y)` macro and its sibling `define_static_heap_init(x)` macro.

**Specific construct:** A multi-hundred-line function-like macro that uses the `##` token-pasting operator to generate per-type symbol names (`init_static_heap_##x`, `lease_##x##_ptr`, `release_##x##_inline`, etc.) and per-type data structures (`x##_buffer`, `x##_global_mtx`, `x##_tls`, ...).

**Why the macro form is necessary:**
- The macro generates a *family* of strongly-typed allocators, one per user struct, with no runtime type-erasure cost. The alternative — a single generic allocator parameterised by `void*` and `size_t` — would defeat the very purpose of the allocator (zero-cost type safety, inlinable hot path, per-type buffer sizing).
- C11 provides no other mechanism for generic programming. `_Generic` operates on expressions, not on type definitions, and cannot synthesise new symbol names. `<tgmath.h>`-style dispatch likewise cannot create per-type storage.
- C++ templates would solve this with type safety and without preprocessor metaprogramming, but the project is constrained to C11.

**Mitigations:**
- The macro is invoked at file scope only and is documented at its definition site with the full set of generated symbol names and their semantics.
- `_Static_assert` is used inside the macro body to validate layout invariants (`offsetof(node, bindings) == 0`).
- No nested macro instantiation: each `static_heap(x, y)` call expands once per translation unit per type.
- All generated functions have `static` linkage by default; the `_ptr` API has well-defined external linkage when explicitly requested.

**Risk assessment:** **Low.** Macro expansion is mechanical and the generated names follow a strict, documented pattern. The trade — preprocessor metaprogramming in exchange for zero-overhead generics — is the standard C11 idiom for this problem class (cf. `sys/queue.h`'s `LIST_*`/`TAILQ_*` macros in BSD libc, which take the same deviation).

## Deviation 5: Rule 1.2 (Language Extensions) — Compiler Intrinsics in `tests/test_static_heap.c`
**Violated in:** `tests/test_static_heap.c`, macros `YUMI_LIKELY`, `YUMI_UNLIKELY`, `YUMI_COLD`, `YUMI_ALWAYS_INLINE`, `YUMI_DO_NOT_OPTIMIZE`.

**Specific constructs:**
- `__builtin_expect(x, n)` (GCC/Clang) — branch-prediction hint.
- `__attribute__((cold))`, `__attribute__((noinline))`, `__attribute__((always_inline))` — code-placement and inlining directives.
- `__asm__ __volatile__("" : : "g"(p) : "memory")` — Google-Benchmark-style escape barrier to defeat dead-store and dead-load elimination inside microbenchmarks.

**Why the extensions are used:**
- All five constructs are essential for *measuring* the allocator's performance honestly. Without the escape barrier, `-O3` correctly proves the entire benchmark loop is dead code and elides it, producing meaningless sub-nanosecond timings.
- The branch-prediction hints and `cold`/`always_inline` attributes are how the hot/cold path separation in the allocator is communicated to the optimiser. Without them, the cold global-refill path is inlined into the hot TLS-cache path and bloats the instruction cache.
- ISO C11 provides no portable equivalent. C23 introduces `[[likely]]` / `[[unlikely]]` and `[[noinline]]` but the project is C11.

**Why the deviation is acceptable:**
- All uses are wrapped in macros (`YUMI_*`) that have a defined fallback to empty / no-op when an unsupported compiler is detected. The code remains correct (only slower / harder to micro-benchmark) on any conforming C11 implementation.
- The constructs appear only in `tests/test_static_heap.c`, not in shipping browser code. They are *measurement* and *optimisation-hinting* tools, not load-bearing semantics.

**Mitigations:**
- A single point of definition (the `YUMI_LIKELY` / `YUMI_UNLIKELY` / etc. macro block at the top of the file) makes the extension surface auditable in one place.
- Each macro is documented with the GCC/Clang attribute it expands to and the no-op fallback for other compilers.

**Risk assessment:** **None.** The extensions are advisory-to-the-compiler; removing them changes performance characteristics but not behavioural correctness.

---

# Summary Table

| Deviation | Rule | Severity | Scope | Risk |
|-----------|------|----------|-------|------|
| 1 | Dir 4.12 (Dynamic Memory) | Required | Dependencies only | Accepted (general-purpose browser) |
| 2 | Rule 11.3 (Pointer Cast) | Required | `static_heap` macro, 2 sites per instantiation | None (C11 §6.7.2.1 explicitly blesses the cast) |
| 3 | Rule 17.7 (Unused Return) | Required | `mtx_unlock` calls in `static_heap` macro | None (failure modes physically unreachable) |
| 4 | Dir 4.9 + Rule 20.10 (Function-Like Macros + `##`) | Advisory | `static_heap(x, y)` macro definition | Low (idiomatic C11 generic-container pattern) |
| 5 | Rule 1.2 (Language Extensions) | Advisory | `YUMI_*` perf macros in test file only | None (macros have no-op fallback) |

---

# Performance Note: MISRA-C Compliance Is Not a Performance Tax

A common objection to MISRA-C Directive 4.12 (no dynamic memory) is that replacing `malloc`/`free`/`calloc` with bounded pools must cost performance. For Yumi Browser's own code, the opposite is measured.

The `static_heap(x, y)` allocator (`src/memory/static_heap.c`) is the mitigation strategy for Deviation 1 in browser-owned translation units. Its multithreaded benchmark in [`tests/test_static_heap.c`](tests/test_static_heap.c) (`test_10_second_mixed_sizes_static_heap` vs `test_10_second_mixed_sizes_malloc_baseline`) runs identical 8-thread workloads against three concurrent object pools of varied size (16 B, 128 B, 512 B), with the same touch pattern, the same `YUMI_DO_NOT_OPTIMIZE` escape barriers, and the same partial-failure handling. The result:

- **`static_heap` is ~2.24× faster than glibc `malloc`/`free`** on the varied-size workload.
- `malloc` is only competitive — and only sometimes wins — when the entire program allocates a *single* fixed size for its entire lifetime. That is not a realistic workload for any nontrivial program; a real browser has dozens of object sizes in flight concurrently.

**Why this is the expected result, not a surprise:**
- The hot path is a TLS-cached pop from a per-thread free list with no atomic operations and no global mutex — typically a handful of cycles.
- The global pool is touched only on TLS-cache refill (batched, 32 nodes at a time) or spill, amortising the lock-acquire cost over many user-facing operations.
- The buffer is contiguous static storage; cache and TLB behaviour is predictable and friendly. `malloc`'s arena structure is not, and its locking strategy must serve adversarial workloads it cannot predict at compile time.
- There is no metadata header per allocation, no coalescing pass, no size-class lookup at runtime — the type system pre-decides the size class.

**Implication for the compliance argument:**
The MISRA-C posture for Yumi Browser is not "accept a performance penalty in exchange for safety." It is "the discipline that satisfies the rule is also the discipline that runs faster, because static type-specialised pools dominate general-purpose heaps for realistic mixed-size workloads." More probably than not, browser-owned code that completes its migration to `static_heap` will run faster than equivalent code written against `malloc`/`free`, not slower.

Reproduce locally:

```sh
./build.sh
./build/test_static_heap            # runs the full suite, including the two mixed-size benchmarks
```

The two relevant tests print their ops/second; the ratio between them is the speedup on your hardware.

