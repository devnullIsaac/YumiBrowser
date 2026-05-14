/**
 * @file damage_tracker.h
 * @brief Per-slot dirty/damage tracking for the dashboard compositor.
 *
 * The dashboard composites N webapp slots (each rendered to an
 * offscreen texture) onto the swapchain every frame. Without any
 * invalidation signal, it would have to call every guest's `frame`
 * export and re-present the swapchain at the full host tick rate
 * (60 Hz focused / 30 Hz unfocused), even when nothing on screen
 * has changed. That wastes GPU work for a typically-static UI.
 *
 * `DamageTracker` is the abstraction that lets the dashboard answer
 * three questions every loop iteration:
 *
 *   1. Does any slot need its guest `frame` export called?
 *      (e.g. because an input event was just dispatched into it,
 *       its viewport was resized, or it called request_redraw())
 *
 *   2. Does the dashboard's own overlay/background need redrawing?
 *
 *   3. Do we need to present a new swapchain frame at all? If every
 *      slot's offscreen texture is byte-identical to last frame
 *      and the overlay hasn't moved, we can skip
 *      wgpuSurfaceGetCurrentTexture + the compositor pass + Present
 *      entirely and let the existing frontbuffer keep displaying.
 *
 * # Lifecycle
 *
 *   damage_tracker_init(&t);
 *   damage_tracker_reserve(&t, slot_capacity);    // grow as slots are added
 *
 *   // on input / event / resize:
 *   damage_tracker_mark_slot(&t, slot_index);     // route-target dirty
 *   damage_tracker_mark_ui(&t);                   // overlay dirty
 *   damage_tracker_mark_all(&t, slot_count);      // full redraw (resize, expose)
 *
 *   // each frame:
 *   if (damage_tracker_should_render(&t, slot_count)) {
 *       for (i in slots) if (damage_tracker_slot_dirty(&t,i))
 *           webapp_runtime_call_frame(slot[i].rt);
 *       composite_and_present();
 *       damage_tracker_clear(&t, slot_count);
 *   }
 *
 *   // slot index churn:
 *   damage_tracker_remove_slot(&t, idx, old_slot_count);
 *
 *   damage_tracker_destroy(&t);
 *
 * # Concurrency
 *
 * Not thread-safe; intended to be touched only from the dashboard's
 * main loop thread (the same thread that dispatches SDL events and
 * calls `dashboard_frame`).
 */

#ifndef DAMAGE_TRACKER_H
#define DAMAGE_TRACKER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Per-slot dirty/damage state.
 *
 * The `slot_dirty` array is indexed by slot index and grown via
 * @ref damage_tracker_reserve. `ui_dirty` covers any non-slot
 * pixels (background clear color, overlay UI, dashboard chrome).
 * `full_redraw` is set when we can't prove which slots changed
 * (resize, expose, focus change, first frame) and acts as an
 * "everything is dirty" override.
 */
typedef struct DamageTracker {
    bool     *slot_dirty;     /**< Length = @ref capacity. */
    uint32_t  capacity;       /**< Allocated length of @ref slot_dirty. */
    bool      ui_dirty;       /**< Dashboard overlay/background dirty. */
    bool      full_redraw;    /**< Force re-render of every slot + UI. */
} DamageTracker;

/** Zero-initialize. Safe to call on a fresh struct. */
void damage_tracker_init(DamageTracker *t);

/** Release the slot_dirty array and zero the struct. */
void damage_tracker_destroy(DamageTracker *t);

/**
 * Ensure @ref capacity is at least @p want.
 * Newly grown tail entries are initialized to dirty so that any
 * slot freshly added after a grow is rendered on its first frame.
 * @return false on allocation failure.
 */
bool damage_tracker_reserve(DamageTracker *t, uint32_t want);

/** Mark a single slot dirty. Out-of-range indices are ignored. */
void damage_tracker_mark_slot(DamageTracker *t, uint32_t slot_index);

/** Mark the dashboard's own overlay/background dirty. */
void damage_tracker_mark_ui(DamageTracker *t);

/**
 * Force a full redraw on the next frame (sets every slot dirty and
 * the UI dirty, and raises the `full_redraw` override so newly added
 * slots within the same frame are also rendered).
 *
 * @param slot_count  Current number of live slots.
 */
void damage_tracker_mark_all(DamageTracker *t, uint32_t slot_count);

/** Inspect whether a specific slot is dirty (or full_redraw is set). */
bool damage_tracker_slot_dirty(const DamageTracker *t, uint32_t slot_index);

/**
 * Quick "is there anything to render?" check.
 *
 * Returns true if @ref ui_dirty, @ref full_redraw, or any
 * `slot_dirty[i]` for i < @p slot_count is set.
 */
bool damage_tracker_should_render(const DamageTracker *t, uint32_t slot_count);

/** Clear every dirty bit. Call after a successful present. */
void damage_tracker_clear(DamageTracker *t, uint32_t slot_count);

/**
 * Handle slot removal: shift dirty bits at indices > @p removed_index
 * down by one and clear the now-vacant tail entry. @p old_slot_count
 * is the slot count BEFORE removal.
 */
void damage_tracker_remove_slot(DamageTracker *t,
                                uint32_t removed_index,
                                uint32_t old_slot_count);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DAMAGE_TRACKER_H */
