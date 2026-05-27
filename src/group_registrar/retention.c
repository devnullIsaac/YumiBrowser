/*
 * retention.c - Group Registrar retention policy: signed retention rules with permission gating, audit-log emission, and fork-aware semantics.
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

#include "internal.h"

gr_error_t gr_retention_set(gr_registrar_t *reg,
                            const gr_retention_t *policy,
                            const gr_identity_t *signer) {
    if (!reg || !policy || !signer) return GR_ERR_INVALID_PARAM;

    /* Block mutations while registrar is unverified */
    if (gr_join_is_untrusted(reg))
        return GR_ERR_JOIN_UNVERIFIED;

    if (!gr_check_perm(reg, signer, GR_PERM_EDIT_RETENTION))
        return GR_ERR_UNAUTHORIZED;

    GR_LOCK(reg);

    if (!gr_txn_begin(reg)) { GR_UNLOCK(reg); return GR_ERR_DB; }

    reg->header.retention = *policy;
    gr_error_t err = gr_header_save(reg);
    if (err != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return err; }

    err = gr_audit_append(reg, GR_CHANGE_RETENTION_SET, signer, NULL,
                           "retention policy updated");
    if (err != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return err; }
    if (!gr_txn_commit(reg)) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
    GR_UNLOCK(reg);
    return GR_OK;
}

gr_error_t gr_retention_get(const gr_registrar_t *reg,
                            gr_retention_t *out) {
    if (!reg || !out) return GR_ERR_INVALID_PARAM;
    *out = reg->header.retention;
    return GR_OK;
}
