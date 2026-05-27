# Rebroadcast Server

Optional. Stores and forwards encrypted content that it cannot decrypt. Solves two distinct problems: the **offline problem** (peers coming back online can catch up on messages they missed) and the **large file problem** (it is impractical for every group member to store and serve large files like videos or archives).

## Envelope Encryption

Content is designed to use envelope encryption. Each file or blob is encrypted with a unique per-content key (CEK). That CEK is wrapped by the group's current epoch key (KEK), which only active group members hold. The rebroadcaster stores the encrypted blob and wrapped CEK bundle. The architecture does not provide the rebroadcaster with the epoch key, so under this design it is not able to decrypt what it stores.

## Efficient Membership Changes

When a member is removed and the epoch rotates, only the CEK bundles are re-wrapped under the new epoch key. The encrypted blobs themselves are never re-encrypted, making membership changes computationally cheap regardless of stored content volume.

## What a Rebroadcaster Operator Actually Sees

Yumi Browser is designed for confidentiality from the ground up. To the operator of a VPS cloud server running a Yumi rebroadcaster, the stored content is intended to appear as opaque ciphertext, and the surrounding metadata is designed to be reduced to non-standard hashed peer IDs with no correspondence to any external identity system. Under this design, the operator is not intended to be able to read the content, identify who the peers are outside the hash space, or ascertain what kind of content is being stored. Group identity, user identity, webapp semantics, message content, and file content are intended to sit outside what the operator can observe. The hash IDs and the traffic pattern are what remains. For these reasons, commodity cloud hosting is generally sufficient for the rebroadcaster role, at the operator's discretion — see below.

## Metadata Caveat

A rebroadcaster that is seized or compromised yields only opaque encrypted blobs, but it also yields the traffic pattern around those blobs — which hashed peer IDs uploaded what, when, how often, and in what sizes. This is a real surface that Yumi does not fully mitigate. The recommended deployment (rebroadcaster behind a VPN, restricted to known peers) narrows the threat but does not eliminate it. Groups with strong metadata-protection requirements should either avoid rebroadcasters entirely and rely on peer availability, or operate their own rebroadcaster under their own control.

## Why You Probably Want a Rebroadcaster

In theory, a group can operate without a rebroadcaster and rely on peer availability alone. In practice, most residential internet connections have very limited upload bandwidth — often under 100 megabits per second on the upload side, and sometimes much less. For a group that wants to share video, large files, or catch up on missed content, peer-only serving is often not enough, especially when active peers are on asymmetric consumer connections.

A cheap VPS solves this. **IONOS is a reasonable recommendation for most groups**: as of writing, they offer VPS instances with 1 Gbps unmetered connections starting around $3 per month, which is enough headroom for a small-to-medium group to run a rebroadcaster comfortably. Other providers with similar offerings work too — the recommendation is for unmetered or generously-metered bandwidth, not for IONOS specifically. The rebroadcaster role does not need a powerful VPS; it needs bandwidth.

Because of the confidentiality properties described above, the operator of that VPS — whether that's IONOS, Hetzner, OVH, or someone else — is a semi-trusted relay at worst. They can see encrypted blobs moving between hashed peer IDs. They are not issued the content, the real-world identities, the group identity, or the purpose; what remains visible to them is the traffic pattern described in the metadata caveat above. That is a very different trust relationship than running group communication through Discord or Slack, and it's what makes commodity cloud hosting a reasonable choice for this role.

## Deployment

Because the rebroadcaster holds persistent content and wrapped key bundles, operators are expected to run it behind a VPN, limiting access to known group peers. The VPN boundary is the operator's responsibility. Yumi Browser does not enforce this, but deployment documentation recommends it strongly.

## Retention Policy

Set by the group owner in the Group Registrar. File retention can be configured independently from message retention, allowing groups to expire chat history quickly while keeping shared files available longer.

## Peer Seeding

Peers who have already downloaded a file can serve it directly to other group members, reducing rebroadcaster load and making popular content resilient to server downtime.

The rebroadcast server is never required. Groups with sufficient peer availability and modest storage needs can operate without one entirely.

