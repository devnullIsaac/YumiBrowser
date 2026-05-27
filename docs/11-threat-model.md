# Threat Model

Yumi Browser is designed against a specific set of adversaries. This section states what Yumi defends against and what it does not. Every claim elsewhere in this document should be read against this threat model, not against an implicit or imagined one.

## In Scope — Yumi Is Intended to Defend Against

- **Passive network observers** along the path between peers. Wire data is designed to be encrypted at the datagram level; the transport header, session envelope, and payload are intended to sit inside the ciphertext.
- **Active network attackers** attempting to inject, modify, reorder, or replay messages. Data is intended to be protected by authenticated encryption (Threefish-1024-CTR + Skein-1024-MAC in Encrypt-then-MAC composition) and signed per-message by the authoring peer, and a per-session anti-replay window is maintained.
- **Unauthorized peers** attempting to join a private group. Membership is intended to be enforced by the registrar's peer list, not merely by possession of the epoch key.
- **Compromised signaling servers.** In the current protocol, signaling servers are designed to see only hashed peer IDs mapped to IP/port, with a 24-hour lease. They are not intended to see group identities, group contents, or peer relationships.
- **Compromised rebroadcasters** (for content). In the current design, rebroadcasters are intended to see only opaque encrypted blobs and wrapped CEK bundles. They are not issued the epoch key and are therefore not able to decrypt stored content under this design. See the metadata caveat under the Rebroadcast Server section for what they *can* see.
- **Single points of failure at the asymmetric layer.** The KEM layer pairs independent primitives in a hybrid construction so that the compromise of any single KEM does not compromise session establishment. See Cryptographic Construction below.
- **Quantum adversaries** (to the extent that current post-quantum KEMs and signatures are sound). Yumi uses post-quantum primitives for all asymmetric operations, combined with classical primitives in a hybrid construction.
- **Disgruntled moderators or administrators.** Governance actions are transparent through the signed audit chain, and recovery mode preserves social graph continuity so that alienating moderation leads to community migration rather than captive users. See Moderator Accountability.

## Out of Scope — Yumi Does Not Defend Against

- **Compromised endpoints.** If an attacker has code execution on the host running Yumi Browser, they can read the local identity keys, the current epoch key, and the plaintext of any message the user sends or receives. This is a limitation of all client-side cryptography. The Flatpak sandbox (see [Recommended Deployment](18-recommended-deployment.md)) mitigates the surface but does not eliminate it.
- **Social engineering.** If a user is persuaded to add a malicious peer to their group, share an invite with the wrong person, or grant moderator permissions to an untrustworthy member, Yumi's technical defenses do not prevent the resulting harm.
- **Traffic analysis against the rebroadcaster.** See the Rebroadcast Server section.
- **Denial of service at the network level.** An attacker who can flood a peer's connection or block their traffic can prevent Yumi from functioning. Yumi has no anti-DoS guarantees beyond what the underlying network stack provides.
- **Legal compulsion of group members.** If a group member is legally or physically compelled to reveal their identity keys or epoch keys, Yumi cannot protect the content that those keys can decrypt.
- **Device loss without full-disk encryption.** If an attacker gains physical access to an unlocked device running Yumi Browser, they can access that user's group memberships and keys. Device-level encryption (LUKS, FileVault, etc.) is the user's responsibility.
- **Side-channel attacks against the host.** Yumi uses constant-time comparisons for tag verification and takes standard precautions in the crypto code, but does not claim formal side-channel resistance at the hardware level.
- **Forced migration from a cryptographic break.** If a primitive used in Yumi Browser is broken or compromised in the future, users will need to upgrade to a version using replacement primitives, and in the worst case existing groups may need to be rebuilt on the new version. See Long-Term Stability Commitment for the shape of this risk and why the cryptographic choices are made to push it as far out as possible.

The goal of this document is to be honest about both halves. A security tool that overstates its defenses is worse than one that accurately describes its limits, because users make decisions based on what the tool claims.

