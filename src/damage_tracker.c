/*
 * damage_tracker.c - Implementation of the dashboard's per-slot dirty/damage tracker driving conditional guest frame export calls.
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
 * @file damage_tracker.c
 * @brief Implementation of the dashboard's per-slot dirty/damage tracker.
 *
*/

#include "damage_tracker.h"

#include <stdlib.h>
#include <string.h>

void damage_tracker_init(DamageTracker *t) {
    if (!t) return;
    t->slot_dirty  = NULL;
    t->capacity    = 0;
    t->ui_dirty    = true;
    t->full_redraw = true;
}

void damage_tracker_destroy(DamageTracker *t) {
    if (!t) return;
    free(t->slot_dirty);
    t->slot_dirty  = NULL;
    t->capacity    = 0;
    t->ui_dirty    = false;
    t->full_redraw = false;
}

bool damage_tracker_reserve(DamageTracker *t, uint32_t want) {
    if (!t) return false;
    if (want <= t->capacity) return true;

    uint32_t new_cap = t->capacity ? t->capacity : 8;
    while (new_cap < want) {
        uint32_t next = new_cap * 2;
        if (next <= new_cap) return false; /* overflow */
        new_cap = next;
    }

    bool *p = (bool *)realloc(t->slot_dirty, (size_t)new_cap * sizeof(bool));
    if (!p) return false;

    for (uint32_t i = t->capacity; i < new_cap; i++) p[i] = true;

    t->slot_dirty = p;
    t->capacity   = new_cap;
    return true;
}

void damage_tracker_mark_slot(DamageTracker *t, uint32_t slot_index) {
    if (!t || slot_index >= t->capacity) return;
    t->slot_dirty[slot_index] = true;
}

void damage_tracker_mark_ui(DamageTracker *t) {
    if (!t) return;
    t->ui_dirty = true;
}

void damage_tracker_mark_all(DamageTracker *t, uint32_t slot_count) {
    if (!t) return;
    t->full_redraw = true;
    t->ui_dirty    = true;

    uint32_t n = slot_count < t->capacity ? slot_count : t->capacity;
    for (uint32_t i = 0; i < n; i++) t->slot_dirty[i] = true;
}

bool damage_tracker_slot_dirty(const DamageTracker *t, uint32_t slot_index) {
    if (!t) return false;
    if (t->full_redraw) return true;
    if (slot_index >= t->capacity) return false;
    return t->slot_dirty[slot_index];
}

bool damage_tracker_should_render(const DamageTracker *t, uint32_t slot_count) {
    if (!t) return false;
    if (t->full_redraw || t->ui_dirty) return true;
    uint32_t n = slot_count < t->capacity ? slot_count : t->capacity;
    for (uint32_t i = 0; i < n; i++) {
        if (t->slot_dirty[i]) return true;
    }
    return false;
}

void damage_tracker_clear(DamageTracker *t, uint32_t slot_count) {
    if (!t) return;
    t->full_redraw = false;
    t->ui_dirty    = false;
    uint32_t n = slot_count < t->capacity ? slot_count : t->capacity;
    for (uint32_t i = 0; i < n; i++) t->slot_dirty[i] = false;
}

void damage_tracker_remove_slot(DamageTracker *t,
                                uint32_t removed_index,
                                uint32_t old_slot_count) {
    if (!t || !t->slot_dirty) return;
    if (removed_index >= old_slot_count) return;

    for (uint32_t i = removed_index; i + 1 < old_slot_count && i + 1 < t->capacity; i++)
        t->slot_dirty[i] = t->slot_dirty[i + 1];

    if (old_slot_count > 0 && (old_slot_count - 1) < t->capacity)
        t->slot_dirty[old_slot_count - 1] = false;
}
