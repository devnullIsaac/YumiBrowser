# Security Policy

## Reporting a Vulnerability

If you believe you have found a security vulnerability in Yumi Browser, please report it **privately** through GitHub's private vulnerability reporting mechanism:

1. Go to the project's repository on GitHub.
2. Click the **Security** tab.
3. Click **Report a vulnerability** (this opens a private **Security Advisory** draft visible only to you and the maintainer).
4. Fill in the advisory with as much detail as you can reasonably provide.

GitHub Security Advisories provide a private channel for coordinated disclosure, a space for patch development, and a mechanism for publishing the advisory once a fix is available. This is the preferred reporting channel because it keeps the discussion private until users have an opportunity to update.

**Please do not file public GitHub issues for suspected security vulnerabilities.** Public issues are visible to everyone the moment they are filed and can expose users to risk before a fix is available.

## What to Include

A useful report generally includes:

- The Yumi Browser version, commit hash, or Flatpak build number where the issue was observed.
- The operating system and environment (e.g. Linux distribution, Flatpak vs. source build).
- A description of the vulnerability and the conditions required to reproduce it.
- A proof-of-concept or minimal reproducer, if one is available.
- Your assessment of impact (confidentiality, integrity, availability, sandbox escape, etc.).
- Whether the issue has been disclosed elsewhere, and if so, where.

If any of this information is unavailable, report what you have. Partial reports are still useful.

## Scope

In scope:

- The Yumi Browser host runtime (`src/`, `include/`) — C code maintained by this project.
- The in-house cryptographic abstraction layer (`src/crypto/`).
- The Group Registrar (`src/group_registrar/`).
- The networking stack (`src/network/`).
- The SDK headers (`sdk/`) that define the WebAssembly import surface.
- The default WebApps shipped with Yumi Browser (`webapps/`).

Out of scope (report upstream):

- Vulnerabilities in third-party dependencies (OpenSSL, oqs-provider, FFmpeg, DuckDB, Dawn, ICU, HarfBuzz, Wasmer, libjuice, SDL3, and others vendored under `deps/`). Please report those to their respective upstream projects. If an upstream fix requires a response in Yumi Browser (e.g. a version bump or translation-layer change), Yumi Browser will address that separately once the upstream advisory is public.
- Issues in third-party WebApps distributed through the community page. Those are the responsibility of their developers.
- Issues in third-party forks or repackaged distributions of Yumi Browser that are not rebuilds of the published source tree.

## Response Expectations

Yumi Browser is maintained by a solo developer. Response capacity is finite, and response time depends on the severity and complexity of the report. In general:

- Acknowledgement of receipt: best-effort, typically within a few days.
- Triage and initial assessment: depends on complexity.
- Fix, advisory publication, and coordinated disclosure: handled through the GitHub Security Advisory the reporter opened.

There is no bug bounty program. The project is grateful for responsible disclosure and will credit reporters in the published advisory if the reporter wishes.

## Safe Harbor

Security research conducted in good faith, consistent with this policy, is welcomed. The project does not intend to pursue legal action against researchers who:

- Make a good-faith effort to avoid privacy violations, service disruption, and destruction of data.
- Report vulnerabilities through the private channel described above rather than publicly.
- Do not access, modify, or exfiltrate data belonging to other users beyond what is necessary to demonstrate the vulnerability.
- Give the project a reasonable opportunity to address the issue before public disclosure.

This safe harbor is a statement of project policy. It does not bind third parties (cloud providers, other users, etc.). See also [LEGAL.md](LEGAL.md).

## Related Documents

- [LEGAL.md](LEGAL.md) — Project notices.
- [docs/11-threat-model.md](docs/11-threat-model.md) — What Yumi Browser is designed to defend against, and what it is not.
- [docs/23-development-focus.md](docs/23-development-focus.md) — Current security-related development work (MISRA-C, Frama-C, external review).
