![Yumi Browser](Banner.png)

# Yumi Browser

> **Preview:** see [Preview.webm](Preview.webm) for a short video preview of Yumi Browser in action.

> **⚠ Pre-Alpha Software** — Yumi is in active early development. The architecture, protocols, and host runtime are taking shape; the user interface and webapp surface are not yet stable enough for general use. See [Project Status](#project-status).

**Yumi is a browser for your friends.**

Not for websites. Not for corporations. For **people you actually know**.

Open it, connect to your group, and you're in a shared space built around you. Chat, share files, watch videos together, edit documents, plan events — whatever your group needs. The apps live inside your relationship. Your relationship doesn't live inside someone else's app.

No accounts. No sign-ups. No platform reading your mail.

**Why "Browser"?** Yumi runs sandboxed WebAssembly webapps — you browse and launch apps inside your groups. Some webapps even let you build new webapps from within the browser. It is a browser, just not for the traditional web.

---

## The Internet Got Relationships Backwards

You don't visit a website and then try to bolt your friends onto it. You start with the people, and the tools serve the group.

Discord, Slack, Google Workspace, Teams — they all make you rent a room from a middleman. That middleman can surveil you, monetize you, censor you, or pull the plug tomorrow. You don't own the space. You're a guest in someone else's business model.

**Yumi flips the script.** The group comes first. The data is yours. The experience is yours. No intermediary. No fine print.

---

## Built to Last

Yumi is pre-alpha today because we are building the foundation to last decades.

- **5-year baseline stability (design goal)**: A Yumi installation is designed to keep working, joining groups, and interoperating with peers for at least five years from release without forced migration.
- **20-year aspirational target**: The architecture — sandbox model, group registrar, cryptographic construction, wire protocol — is designed to stay put.
- **Strict WASM API backward compatibility (design objective)**: Once a host function is exposed to WebAssembly, the project's intent is that it stays exposed, so webapps written today should run in a Yumi built years from now. See [docs/08-stability-commitment.md](docs/08-stability-commitment.md) for the full commitment — which is a design objective, not a warranty. Yumi Browser is provided as-is under AGPL-3.0.

---

## Yumi in Plain English

### What Yumi actually is

Yumi is a program you install on your computer. When you open it, you see a Dashboard — your personal control panel. From there, you join or create **groups**: private spaces for you and people you know. Inside each group, you run **apps** — chat, file sharing, video calls, picture albums, and more. These apps are not websites you visit in Chrome or Firefox. They are small programs that run inside Yumi itself, sandboxed so that they are not granted access to your files, your network, or other groups without permission.

That is why it is called a **browser**: you browse and launch apps inside it, just as you browse websites in a web browser. Some apps are even tools for building new apps. But instead of browsing the public internet, you are browsing the apps available inside your group.

### How it connects people

When you send a message or share a file in Yumi, it does not go to a central server owned by a company. It travels directly between the computers of the people in your group, protected by **group cryptography**: authorized members of the group hold the keys, and the design is intended so that parties outside the group — including the Yumi project itself — are not in a position to read the content. For one-to-one side conversations within a group, Yumi also supports **private "whisper" cryptography** layered on top of the group, so that a pairwise message between two members is intended to be readable only by those two members, not by the rest of the group. If a friend is offline, an optional **rebroadcast server** (a cheap cloud computer your group chooses) can hold encrypted copies until they come back online. That server is not issued the group's keys — under the current design it holds opaque ciphertext it is not equipped to decrypt.

Yumi's cryptography is built on **OpenSSL** (the same library used by Chrome, Firefox, and most commercial software) together with the **Open Quantum Safe `oqs-provider`** for post-quantum algorithms. The post-quantum primitives used (ML-KEM, ML-DSA) are NIST-standardized; the fallback and hybrid primitives (FrodoKEM, BrainPool-P512r1) are recognized in European and international standards. These are mainstream, standards-based building blocks, not bespoke crypto. See [docs/04-cryptography.md](docs/04-cryptography.md) for the full list and citations.

To find each other across home networks and Wi-Fi, Yumi uses small **signaling servers** that briefly remember where each peer is located. These servers see only scrambled identifiers, not group names, not content, not who you are talking to.

### The sociology: groups, not feeds

Yumi is built on a simple observation: for most of human history, conversation was private by default. Two people talking did not route their words through a distant intermediary that could record, filter, or monetize them. Modern platforms reversed this default. They own the room and rent you a corner.

Yumi restores the older pattern. The **group** is the atomic unit. There is no global feed, no algorithm deciding what you see, no engagement score, no corporate account system. Your identity exists only within the groups you join. A group of three friends sharing photos is just as valid as a public group of thousands.

This design is intentional because human beings naturally sort into groups of trust and mutual obligation. When people know they are in a bounded space with others they will continue to interact with, reputation matters and social accountability returns. The platform does not claim to fix human nature — it claims to give human nature a better container.

### Governance and accountability

Every group has a **registrar**: a signed document that says who is in the group, what roles they have, and which apps are allowed. Moderators and administrators are peers designated by the group's registrar — they are regular users the group has granted permissions to, not Yumi Browser staff. The Yumi Browser application itself does not moderate any group, and the project operates no service that could. When a registrar-designated moderator removes someone, that action is logged and signed on every member's machine. The group owner cannot hide it. If enough members disagree with how a group is run, they can collectively form a new group and take their social graph with them. The power to fork is built in, and its presence disciplines moderation.

### Security: honest limits

Yumi uses strong cryptography to protect messages in transit and at rest, including post-quantum algorithms designed to resist future advances in computing. However, no software can protect you if your own computer is compromised by malware, if you are tricked into inviting a malicious peer, or if someone with physical access to your unlocked device opens the app. See [docs/11-threat-model.md](docs/11-threat-model.md) for a complete, honest description of what Yumi defends against and what it does not.

---

## How It Works (The Short Version)

| What | How |
|---|---|
| **Window & Graphics** | SDL3 + WebGPU |
| **Apps** | Sandboxed WebAssembly |
| **Crypto** | Post-quantum primitives (see [docs/04-cryptography.md](docs/04-cryptography.md)) |
| **Networking** | Encrypted peer-to-peer (see [docs/06-networking.md](docs/06-networking.md)) |
| **Shaders** | Slang pipeline (see [docs/07-shaders.md](docs/07-shaders.md)) |

Want the full architecture deep-dive? Start with [00 — Architectural Overview](docs/00-overview.md).

---

## Younger Users

Yumi Browser is intended for users aged 13 and older. There is no global feed, no account system, and no telemetry — a user's data lives on their own device and on the devices of the peers in groups they have joined. Group membership is the unit of access, and parents or guardians who want to supervise a younger user's participation should do so by deciding which groups that user is in.

A dedicated join-lock feature for supervising installations is sketched in the future plans — see [docs/24-roadmap.md#parental-control](docs/24-roadmap.md#parental-control). Until it ships, operating-system-level controls (for example, GNOME Parental Controls / malcontent on Linux) are the recommended way to gate installation and use of the application.

---

## Get Yumi

### The Easy Way (Recommended)

**Flatpak** is the simplest and safest way to run Yumi on Linux:

```bash
flatpak-builder flatpak-build com.yumi.browser.yml --force-clean
```

### The Hard Way (Build from Source)

You need Linux (x86_64), GCC or Clang, CMake, Meson, Ninja, and patience — this compiles *everything* from scratch.

```bash
git clone --recursive <your repository URL here>
cd YumiBrowser
build.sh          # Debug
build.sh --release # Optimized
```

Then run it:

```bash
./release/yumibrowser                    # Normal launch
./release/yumibrowser --webapp app.wasm  # Dev mode: single app
./release/yumibrowser --data-dir /path   # Custom data directory
```

> **Platform Roadmap:** Linux (x86_64) is the first supported platform. Windows, macOS, Android, and iOS are intended for future support. Yumi's C core and WebAssembly sandboxing model are designed to be portable across operating systems.

---

## Documentation

| Doc | What's Inside |
|---|---|
| [00 — Overview](docs/00-overview.md) | The three pillars |
| [01 — Host Runtime](docs/01-host-runtime.md) | SDL3, WebGPU, WASM |
| [02 — Binding Modules](docs/02-binding-modules.md) | What the host exposes to apps |
| [03 — SDK](docs/03-sdk.md) | Building WASM apps for Yumi |
| [04 — Cryptography](docs/04-cryptography.md) | Keys, primitives, threat model |
| [05 — Group Registrar](docs/05-group-registrar.md) | Governance API |
| [06 — Networking](docs/06-networking.md) | P2P transport |
| [07 — Shaders](docs/07-shaders.md) | Shader pipeline |
| [08 — Stability Commitment](docs/08-stability-commitment.md) | 5-year baseline, 20-year target |
| [09 — Application Model](docs/09-application-model.md) | Dashboard vs. regular apps |
| [10 — Group Model](docs/10-group-model.md) | Storage, types, registrar |
| [11 — Threat Model](docs/11-threat-model.md) | What we defend against |
| [12 — Rebroadcast Server](docs/12-rebroadcast-server.md) | Offline catch-up & big files |
| [13 — Tamper Resistance](docs/13-tamper-resistance.md) | Signed messages, integrity |
| [14 — Anti-Sybil](docs/14-anti-sybil.md) | Private groups, vetting |
| [15 — Moderator Accountability](docs/15-moderator-accountability.md) | Transparent audit logs |
| [16 — WebApp Distribution](docs/16-webapp-distribution.md) | Signed apps, community page |
| [17 — Infrastructure](docs/17-infrastructure.md) | Signaling, rebroadcast, full servers |
| [18 — Deployment](docs/18-recommended-deployment.md) | Flatpak, sandboxing, VMs |
| [19 — FFmpeg Policy](docs/19-ffmpeg.md) | Codecs, patents, transcoding |
| [20 — Building](docs/20-building.md) | Full build instructions |
| [21 — Running & Testing](docs/21-running-testing.md) | Launch options, test suite |
| [22 — Project Structure](docs/22-project-structure.md) | Source tree |
| [23 — Development Focus](docs/23-development-focus.md) | MISRA-C, Frama-C, auditing |
| [24 — Roadmap](docs/24-roadmap.md) | What's next (not promises) |
| [25 — FAQ](docs/25-faq.md) | Common questions |
| [26 — Social Design](docs/26-social-design.md) | Why Yumi is built around groups |

---

## License

AGPL-3.0. See [LICENSE](LICENSE).

Third-party dependencies bundled under `deps/` are distributed under their own licenses — see [THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md) for the per-dependency summary.

---

## Project Status

Yumi Browser is **pre-alpha** software under active development by a solo maintainer. The host runtime, group registrar, networking stack, and cryptographic abstraction are progressing; the Dashboard UI, webapp surface, and user-facing flows are still in design. Until the UI/UX has been finalized (a working Figma design feeding into the Dashboard is the gating milestone), the project is not yet at the point where general users should be installing it.

There has been no independent third-party security review.

The cryptographic construction (Threefish-1024-CTR with Skein-1024-MAC under Encrypt-then-MAC) is implemented carefully against published primitives, but the composition as shipped has not been independently reviewed. If you need a formally reviewed stack today, Yumi is not the right fit yet.

MISRA-C compliance and Frama-C annotations are an ongoing objective, not a completed state. See [docs/23-development-focus.md](docs/23-development-focus.md) for details.

### AI-Assisted Development

Yumi Browser is a human-developed project. The maintainer uses AI coding assistants (including Anthropic's Claude) as a tool during development — the same way a developer might use a compiler, a linter, or a static analyzer. AI is not the author of the project, does not make design decisions, and does not commit code on its own. Every AI-suggested change is read, evaluated, edited where necessary, and accepted or rejected by the human maintainer before it lands.

This matters because AI assistants can produce code that looks correct but is subtly wrong. Yumi Browser treats AI output as a starting draft, not as ground truth. The project's reliance on **MISRA-C** as a coding discipline is part of how that draft is hardened: MISRA-C is a widely used set of rules for safety and security-relevant C code, originally developed for the automotive industry, and it constrains the language to a subset that is easier to reason about and harder to misuse. Combined with Frama-C static analysis (see [docs/23-development-focus.md](docs/23-development-focus.md)), it gives the project a structural check on what gets accepted into the tree — whether the initial draft came from a human or from an AI assistant. Any deviation from MISRA-C in Yumi Browser's own code is documented in [MISRA-C.md](MISRA-C.md) with the rationale.

In short: AI accelerates the work; the human is responsible for the result; MISRA-C and Frama-C are how the result is held to account.