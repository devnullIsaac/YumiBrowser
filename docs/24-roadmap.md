# Roadmap

The items in this section are directions the project *may* grow in. They are not commitments. Most of them will probably never ship on any particular timeline, and some will probably never ship at all. They are listed here so readers understand the platform's intended shape, not as promises. A solo developer has a finite budget of hours, and this list exceeds that budget deliberately — the purpose is to communicate intent, not delivery dates.

If you are evaluating Yumi for a current need, evaluate it on what is described in [What You Can Do Right Now](../README.md#what-you-can-do-right-now) and [Current Development Focus](23-development-focus.md). Everything below is the long view.

## Auditing Guide for the Layman

A structured walkthrough designed to lower the barrier of entry for reviewing the codebase, even for readers who are not professional security researchers. Intended to cover: a map of the codebase, the trust boundaries and where each one is enforced in code, a "follow the data" walkthrough tracing a single message end-to-end, red flags to look for in C and crypto code, how to verify that the named primitives are actually wired up, how to run the test suite and interpret results, and how to reproduce a build and verify the binary against the source.

The underlying principle: security through obscurity is not security, and security through authority is not security either. A project that asks users to trust its cryptographic claims owes those users a path to verifying those claims independently.

## Parental Control

A passphrase-gated lock on the *join* operation, enforced locally by the Dashboard. A guardian sets a passphrase on a younger user's installation; after that, joining a new group requires the passphrase. Groups already approved continue to work normally.

The lock is intended to be stored as a root-owned, immutable file (for example, via `chattr +i` on Linux filesystems that support it), so removing or altering it requires both the guardian's passphrase **and** system-administrator privilege on the device.

The lock governs **joining decisions only**. It does not read messages, scan files, filter content, monitor activity, or perform age verification — Yumi Browser is not in a position to do any of those things, since conversation inside a group is protected by group cryptography. Content-level supervision is the guardian's responsibility, exercised by deciding which groups the younger user is in.

Until this ships, operating-system-level controls (for example, GNOME Parental Controls / malcontent on Linux) are the recommended way to gate installation and use of the application.

## Expanded WebApp Library

Additional first-party webapps that the platform *could* grow in the direction of. Each is a meaningful undertaking and none are scheduled:

- **Forum** — Threaded, long-form discussion scoped to a group, signed and synchronized peer-to-peer.
- **Event Bulletins** — Shared event board for announcements, scheduling, and RSVPs.
- **File Share v2** — Folder structures, versioning, and search on top of the existing file-sharing infrastructure.
- **Git Project Sharing** — A collaborative code-hosting webapp along the lines of Forgejo, owned by the group rather than an external service.
- **Map Data and Analysis** — Geospatial webapp for sharing map annotations, routes, and spatial datasets.
- **Marketplace** — Peer-to-peer marketplace aimed at the space currently occupied by eBay, Craigslist, and Amazon Marketplace.

These are directions. They are not commitments.
