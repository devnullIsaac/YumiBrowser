# Anti-Sybil Measures

## Private Groups

The registrar's peer list is designed to block any peer not listed from communicating with the group. This boundary is intended to be enforced at the protocol level. Under this design, even if an attacker obtains the group epoch key from a compromised member, they are not authorized by the registrar as a peer and so participation is intended to be refused. The group key protects the content. The registrar protects the membership. These are two separate boundaries.

If a member's browser encounters an unknown peer attempting to communicate, it is designed not to admit that peer into the existing group. If the user wants to talk to the unknown peer, the browser creates a new group for that interaction. The protocol is intended to prevent contamination of the existing group through this path.

## Public Groups

Anyone can join by design. Behavioral profiling becomes relevant here. Each user's browser builds a local assessment of each peer based on observed behavior: timing patterns, social graph consistency, interaction quality, and longevity. This profiling happens entirely locally. There is no central authority deciding who is real. The browser provides the data; the user decides what to do with it.

Behavioral profiling for Sybil resistance is not a solved research problem and Yumi does not claim otherwise. The browser exposes the raw signals (action rates, destructive-action ratios, churn, peer staleness, epoch rotation patterns, delta-sync anomalies) as a passive, read-only analysis layer over the audit log. These signals are intended as inputs to local user judgment, not as an automated trust score.

## Vetting-Circle Pattern

Public groups are well-suited to serve as antechambers for private ones. A public group can operate as a vetting circle where trusted members identify legitimate participants and promote them into a private group where the hard registrar-level membership boundary then applies. This pattern reframes public groups as filters rather than destinations, and it is the recommended approach for communities that want openness at the edge and strong boundaries at the core.

## Out-of-Band Invitation as the Primary Path

Joining a private group is designed to happen through an out-of-band invitation — a ticket shared by link, QR code, or file through a channel the group already trusts. This is deliberate and is part of Yumi's broader security posture: no single communication medium is trusted alone, and the invitation path is held separate from the transport path so that a compromise of one does not compromise the other.

## Collaborative Surfaces as Anti-Sybil

Yumi Browser's collaborative nature is itself an anti-Sybil mechanism. Because the platform supports multiple concurrent webapps — voice chat, video chat, gaming, text chat, collaborative document editing, drawing — users have a granular, multi-dimensional view of each peer. A text-only bot is easy to automate. A peer who voice chats, plays games, co-edits a document, and draws with you over weeks is not. The more collaborative surfaces a group uses, the harder it becomes to maintain a convincing fake identity.

## Signed WebApps as an Attack-Surface Filter

Every WebApp distributed for Yumi Browser is signed by its developer and accompanied by identifying metadata. The Group Registrar pins the specific WebApp hash, version, and checksum that a given group uses, and browsers are designed to reject anything that doesn't match. In practice, most group activity happens on webapps that are already vetted, widely used, and observed by the community — the default set shipped with Yumi Browser, plus webapps that have accumulated reputation through the community distribution page. This is another architectural barrier intended to narrow the attack surface: a malicious webapp is designed to have to get past the signature check, past the group owner's explicit authorization, and past the collective observation of every other group using that webapp. The default path for a Yumi Browser user is not "run arbitrary code from strangers" — it is "run a webapp that the rest of the community has already looked at."

