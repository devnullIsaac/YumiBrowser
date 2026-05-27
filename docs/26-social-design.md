# Social Design — Why Yumi Is Built Around Groups

> **TL;DR.** Yumi treats a *group of people you know* as the primary unit, not a website or a global feed. The architecture, the cryptography, and the governance model all follow from that choice.

## 1. The Inversion

Most mainstream software starts with the product and asks the user to bring their relationships into it. Yumi starts with the relationship and gives it tools. The unit is the group, not the page.

Platforms that own the room and rent you a corner can change the terms of that rental at any time. Yumi gives the room to the group. The apps are furniture inside it.

## 2. Private-by-Default

For a tool whose primary use is talking with people you already know, private-by-default is the right baseline. Group conversation in Yumi is end-to-end encrypted across the group; one-to-one whispers add a pairwise layer on top.

The cryptography is not there to defeat any authority — it is there because peer-to-peer communication requires it. Without authenticated encryption, anyone on the network path can impersonate a peer, modify messages in transit, or inject false content into a group. Cryptography is what the protocol uses to establish that the person you are talking to is the person you invited, and that the words you receive are the words they sent. Stripping it out would not produce a more open Yumi; it would produce a broken one.

## 3. Groups as the Social Unit

People naturally organize into bounded groups — families, friends, hobbies, workplaces, congregations. A tool that treats every user as an isolated individual floating in a global feed is fighting that grain rather than serving it.

Making the group the atomic unit has practical consequences for the social dynamics inside the tool. Members of a group know they will continue to interact with one another, so reputation and accountability mean something. There is no global audience to perform for, and no algorithm rewarding the loudest content. The tool is shaped to feel more like a room and less like a stage.

This is a design choice, not a claim that groups are always right or that individual expression is bad. It is a claim that, for the kinds of conversation Yumi is built for, the bounded group is the better default.

## 4. Algorithm and Feed

Yumi does not ship with an algorithmic feed, infinite scroll, or engagement metrics. This is a default, not a prohibition.

If a group wants an algorithmic feed, a recommendation engine, or an engagement-optimized content stream, they can build or install a webapp that provides one. The platform is open at the WebAssembly layer. Nothing in the architecture prevents it. But the default experience is a group of people with tools that serve their relationship, not a content stream optimized for attention.

## 5. Architectural Consequences

The social model shapes the code in specific, concrete ways:

- **No global identity.** There is no Yumi account, no platform-wide username, and no social graph owned by a corporation. Identity is scoped to the group.
- **Registrar-enforced membership.** Private groups use the Group Registrar to define who is in and who is out. This is a technical boundary, not a social suggestion.
- **Forkability as a right.** Recovery mode makes it near-costless for a community to leave a bad moderator and reconstitute elsewhere. This disciplines moderation by making exile survivable.
- **Vetting circles.** Public groups can serve as antechambers for private ones, letting communities filter participants organically before granting them access to bounded spaces.
- **Collaborative surfaces.** Voice chat, video, co-editing, and gaming are not just features; they are also anti-Sybil signals. A peer who voice-chats, plays games, and draws with you over weeks is much harder to fake than a text-only bot.
- **WebApp sandboxing.** Regular webapps are not granted the capability to phone home, access raw networks, or observe other groups. The architecture is designed so the platform does not have a path to become a panopticon.

## 6. Scope of the Project

Yumi is a tool. Users remain subject to the rules of their own jurisdictions; the project does not change that, and it is not built to position itself above it. The architecture exists because peer-to-peer communication requires integrity and authenticity, not as a statement on any policy debate.

## 7. Data at Rest

Yumi encrypts data in transit and authenticates every message, but it does not perform full-disk encryption. Group databases, chat history, shared files, and identity keys are stored on the user's device in the form the user has configured.

Disk encryption is the responsibility of the operating system, which can provide it more efficiently than an application-layer reimplementation. Yumi Browser intends to assess its storage environment and warn the user if it detects that data is being stored on an unencrypted volume.

Because Yumi Browser does not operate a centralized service, the data resides with the user, or — for groups that use them — on rebroadcasters and signaling servers operated by third parties chosen by the group, never on infrastructure operated by the project. The centralized-platform model, where a corporation holds billions of conversations in a single database, is architecturally incompatible with Yumi Browser, and Yumi Browser does not implement it.