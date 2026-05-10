# Security Policy

Yumi Browser is a cryptographic, peer-to-peer application platform. Because the
project's value proposition rests on the integrity of its sandbox, its wire
protocol, and its post-quantum crypto stack, we take security reports
seriously and ask reporters to treat them with care as well.

This document describes how to report a vulnerability, what to expect in
response, and the project's current scope and limits.

---

## 1. Scope

Reports are in scope if they affect any of the following components shipped
from this repository:

- The host runtime (`src/`, `include/`)
- The cryptographic layer (`src/crypto/`, `src/group_registrar/crypto.c`)
- The group registrar and identity / attestation logic
  (`src/group_registrar/`)
- The peer-to-peer network stack (`src/network/`)
- The WebAssembly sandbox host and capability boundary
  (`src/webapp_runtime.c`, `src/dashboard_runtime.c`, `*_bindings.c`)
- The build/release scripts in `scripts/` insofar as they produce the
  shipped binary in `release/`

Reports are **out of scope** for:

- Vulnerabilities in third-party dependencies that are already tracked
  upstream (please report those upstream first; see `THIRD_PARTY.md`).
  We are happy to receive a notification that an upstream advisory affects
  Yumi, but the primary fix path is the upstream project.
- Issues that require an attacker who is already an admitted, trusted member
  of a group to misuse data they were authorized to see. As stated in the
  README, this is a social-engineering risk, not a software vulnerability.
- Issues that require physical access to an unlocked device.
- Theoretical attacks against the underlying standardized primitives
  (ML-DSA-87, ML-KEM-1024, Skein-1024, Threefish-1024) where the report does
  not demonstrate a concrete exploitable issue in Yumi's use of them.
- Findings in pre-alpha or unfinished subsystems that are clearly marked as
  such in the README or in code comments. We still want to hear about them,
  but they will not be treated as embargoed vulnerabilities.

---

## 2. Reporting a Vulnerability

**Please report security issues privately.** Do not open a public issue, pull
request, or forum post.

- **Email:** `devnullisaac@gmail.com`
- **Subject prefix:** `[YUMI-SEC]`
- **Encryption:** PGP/GPG-encrypted mail is welcome. If you wish to use an
  encrypted channel and no key is published yet, send a short unencrypted
  message asking for a key fingerprint and we will reply with one.

Please include, at minimum:

1. A description of the issue and the component affected.
2. The commit hash or release tag the report applies to.
3. Reproduction steps, a proof-of-concept, or a crash trace if available.
4. Your assessment of impact (confidentiality / integrity / availability,
   local vs. remote, authenticated vs. unauthenticated).
5. Whether you wish to be credited, and under what name.

If you cannot reach the maintainer by email within a reasonable time, you
may instead open a *minimal* private issue on Codeberg using the
"Security report" template, **without exploit details**, asking for a
secure contact channel.

---

## 3. What to Expect

This is a single-maintainer project. The following are intentions, not SLAs:

- **Acknowledgement** of the report: within 7 days.
- **Initial triage** (in scope / out of scope, severity estimate): within
  14 days.
- **Coordinated disclosure window:** typically up to 90 days from
  acknowledgement, extendable by mutual agreement if the fix is complex
  or coordinated with upstream dependencies.
- **Fix release:** the fix will be released as a tagged version with notes
  in `CHANGELOG.md`. Reporters who wish to be credited will be listed in
  the changelog entry and (optionally) in a published advisory.

If a report is determined to be out of scope, you will be told so and given
the reasoning.

---

## 4. Supported Versions

Yumi Browser is currently **pre-alpha**. Until a 1.0 release is tagged, only
the `main` branch is supported and only the latest tagged pre-release (if
any) will receive security fixes. Once 1.0 ships, this section will be
updated with a concrete support matrix in line with the README's stated
five-year stability goal.

| Version | Supported          |
| ------- | ------------------ |
| `main`  | :white_check_mark: |
| < 1.0   | best effort, latest pre-release only |

---

## 5. Cryptographic Notes

Yumi's security claims rest on the following primitives, used as documented
in `include/crypto.h` and `src/crypto/`:

- **ML-DSA-87** (post-quantum signatures, via OpenSSL + oqs-provider)
- **ML-KEM-1024** (post-quantum KEM, via OpenSSL + oqs-provider)
- **Hybrid classical + post-quantum** construction
- **Skein-1024** (hashing, public-domain reference implementation)
- **Threefish-1024** (block cipher / AEAD, public-domain reference
  implementation)

Reports that demonstrate a *misuse* of these primitives by Yumi (wrong
mode, missing authentication, predictable nonce, key-reuse, malleable
encoding, side-channel in Yumi-specific glue code, etc.) are explicitly
in scope and welcomed.

The cryptographic stack and wire protocol have **not yet undergone a formal
third-party audit.** Closing that gap is a stated funding priority.
Independent review reports — even informal ones — are extremely valuable
and will be treated with the same disclosure process described above.

---

## 6. Safe Harbor

We will not pursue legal action against researchers who:

- Make a good-faith effort to comply with this policy,
- Avoid privacy violations, destruction of data, and degradation of service
  for users other than themselves,
- Give the project a reasonable opportunity to fix the issue before public
  disclosure,
- Do not exploit the issue beyond what is necessary to demonstrate it.

If in doubt, ask first.

---

Thank you for helping keep Yumi Browser and its users safe.
