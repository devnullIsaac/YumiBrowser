# The Group Model

Everything in Yumi Browser revolves around the **Group Registrar**, a portable document (sharable via link, QR code, git repo, or any other means) that defines a group.

## What the Group Registrar Contains

- Peer identities (peer ID hash, ML-DSA-87 signing public key, ML-KEM-1024 public key, network address)
- WebApp service hashes (all browsers reject non-matching services, preventing corruption or hostile injection)
- Group signature keys (owner signature plus up to 50 role-based signatures with write permission to the registrar, capped for performance)
- Policy per role (unauthorized changes to the registrar are rejected; the previous registrar is preserved)
- Date/time stamps
- List of rebroadcaster servers (address, ID hash, service hashes, signing public key, content encryption keypair)
- List of signaling servers (address, ID hash)
- Hash and signature of the registrar itself (validated against last known group signature keys; invite tickets include this information)

## Storage and Size Management

All Group Registrar data is stored in DuckDB on disk, not in RAM, reducing memory pressure. In user settings, the registrar size is capped at 200 MB by default (adjustable). When a user joins a group, they are informed of the registrar size. If it exceeds the configured limit, the user is prompted: "This group registrar exceeds 200 MB. Do you still want to download it to join?" The 200 MB cap accommodates roughly 2 million peers, which is well beyond typical group sizes.

### Typical Registrar Sizes

The 200 MB cap is an upper bound, not a representative size. In practice, most groups sit far below it. A private group of under 100 people — which covers the majority of real-world use cases, from small teams to close-circle friend groups to typical hobby communities — generally has a group registrar well under **5 MB**. Joining a group of that size is fast, cheap, and nearly invisible.

Even large communities rarely need more than a few thousand members. In practice, most groups — even public content hubs — do not often need to exceed **10,000 people**. The 200 MB cap and the multi-million-peer theoretical capacity exist as architectural headroom, not as an expected operating range. The day-to-day experience is small, fast, and lightweight.

## Group Types

Groups come in two flavors: **private/encrypted** (invite only) and **public/unencrypted** (open to anyone). In private groups, the group epoch key can be rotated to remove bad actors — the new key is distributed to everyone except the removed peer, effectively locking them out.

The group owner has authority over group configuration: authorizing which webapps are available, setting moderation policy, defining retention rules, and managing membership. All governance happens at the group level. There is no global authority, no platform-level moderation, no centralized control.

## Social Topology

Groups scale naturally from a private conversation (2 people) to a close circle (5–15), a team (30–50), a community (~150), or a public content hub (millions). Public and private groups coexist — content lives in public groups, conversation about it happens in private ones. A user can reference something from a public group in their private circle, and friends can follow the reference to join if they choose.

There is no algorithm, no feed, no engagement metric. A group of 3 friends sharing photos is just as valid as a public group of a million people.

## Group-to-Group Content Mirroring

A planned feature on the Yumi Browser roadmap is **group-to-group content mirroring**. A moderator or administrator of a group will be able to mirror content from another group that they have read-only access to, making that content available within their own group's context.

### What Mirroring Enables

This capability is designed for legitimate, transparent content sharing between groups:

- **News feeds** — a journalism group publishes stories; subscriber groups mirror them for their own members
- **Marketplace data** — a commerce group lists items and prices; regional groups mirror the listings
- **Price information** — a financial data group publishes rates; trading groups mirror them
- **Video and picture galleries** — a media group publishes creative work; fan communities mirror them
- **Any other content type** that a webapp chooses to support

### Trust and Responsibility

There is a real risk of data exfiltration between groups through mirroring. It is important to be honest about this: **Yumi Browser is open source, and a technically capable user can always modify the software to extract and redistribute content.** No technical measure can fully prevent this when the user controls the client. Mirroring does not create a new risk; it makes an existing one explicit and manageable.

By designing mirroring as a first-class, documented capability rather than pretending it cannot happen, Yumi Browser puts the decision where it belongs: with the group. It is up to each group to assess who they trust, who they admit, and who they grant moderator privileges to. A group that admits untrustworthy members or grants moderator rights to irresponsible actors will have problems regardless of whether mirroring exists. The feature does not create the trust problem; it surfaces it.

### Chat Is Deliberately Excluded

The default chat webapp distributed by Yumi Browser will **deliberately disallow** redistribution and mirroring of chat content, and will not accept mirrored chat from other groups. This is a design choice, not a security guarantee. It does not mean chat redistribution is impossible — a modified client could still attempt it — but it means that doing so requires more effort than simply clicking a mirror button. Chat is treated differently because the expectation of privacy in a conversation is different from the expectation of privacy in a published gallery or marketplace listing.

### The Foundation Is Trust

As with every other aspect of Yumi Browser, the foundation of a successful group is the ability to trust the people in it. Cryptography enforces boundaries against outsiders. Governance and transparency enforce boundaries against insiders. Mirroring is a tool that responsible groups can use to share content broadly; it is also a tool that irresponsible groups can misuse. The software provides the capability. The people in the group provide the judgment.
