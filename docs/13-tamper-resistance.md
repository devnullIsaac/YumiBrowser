# Tamper Resistance and Recovery

## Signed Messages

Messages are required to be signed by the sending user with their ML-DSA-87 secret key. Messages are verified against the group registrar; the protocol is designed to reject messages that do not verify under the claimed author's key.

## Chat History Reconstruction

During reconstruction, messages from other users are intended to be relayed only if those users are listed in the group registrar. Messages from removed users are designed to be hidden or dropped. To repeat content from a removed user, it must be attributed to you, not to them.

## Registrar Integrity

Changes to the group registrar are required to carry the correct signature matching the policy for the given role or the owner signature key. Unauthorized changes are designed to be rejected and the previous registrar state is designed to be preserved.

## Audit Log

Every governance action (peer add, kick, ban, role change, epoch rotation, registrar sign, invite create, etc.) is recorded in a hash-chained audit log, with each entry signed by its actor. The chain is verifiable end-to-end, fork detection is built in, and retention is bounded by policy.

### Retention Policy

Retention is defined by the **Group Registrar**, not by the Yumi Browser application. The group owner sets the retention policy for the group, and that policy governs what rebroadcaster servers preserve on the group's behalf.

The defaults shipped by the project, where a group owner has not configured otherwise, are:

- **Chat messages**: 30 days
- **Shared files**: 180 days
- **Audit log entries**: retained for the lifetime of the group (governance continuity requires this)

These defaults are what rebroadcasters generally adhere to. Individual peers can configure different (typically longer) retention on their own installations, and because every peer is a full participant in the group's history, **peers may individually preserve the complete history of a group indefinitely** regardless of what the rebroadcaster retains. Users should therefore assume that content shared in a group may be preserved in full by one or more group members for as long as those members remain in the group (and, subject to local law and group norms, after they leave). The retention configuration on any one peer — including a rebroadcaster — is a floor on group memory, not a ceiling.

This is consistent with how ordinary human conversation has always worked: anyone present is free to remember what was said. Yumi Browser's retention defaults govern the *shared caching* of history for offline catch-up; they do not, and cannot, govern the memories (local databases) of individual peers.

## Recovery from Key Compromise

If a signature key is compromised, all registrar changes are tracked in DuckDB and the group can initiate a key-rotation and clean-up process from a known-good historical state.

