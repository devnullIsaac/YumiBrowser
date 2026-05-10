# Contributing to Yumi Browser

**Yumi Browser is a deliberately solo-developer project.** Keeping the
codebase, the wire protocol, the WebAssembly API surface, and the long-term
vision coherent across a 20-year horizon requires a single hand on the
tiller. That is a feature, not an oversight.

This means, concretely:

- **External pull requests are not accepted at this time.** Drive-by
  patches, however well-intentioned, will be closed without merge. Please
  do not be offended — the policy applies to everyone equally.
- **Bug reports and well-formed feature suggestions are welcome.** They
  are the most useful way to contribute. See the sections below.
- **Forks are welcome under AGPL-3.0.** If you want to take the code in a
  different direction, the license fully permits it. That is what copyleft
  is for.

If at some future point the project formally opens to outside code
contributions, this document will be updated and announced in
[`CHANGELOG.md`](CHANGELOG.md). Until then, please respect the policy.

---

## 1. Before You File Anything

- Read [`README.md`](README.md) to understand the project's scope.
- Read [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md). It is short.
- For anything that smells security-relevant, read
  [`SECURITY.md`](SECURITY.md) **before** opening a public issue. Never
  file exploit details in a public tracker.
- For licensing of vendored files, read [`THIRD_PARTY.md`](THIRD_PARTY.md).

---

## 2. Reporting Bugs

Open an issue using the **Bug report** template. Useful reports include:

- Yumi Browser version (`git rev-parse HEAD` or release tag).
- Operating system and version.
- Build mode (`debug` vs. `--release`).
- Exact reproduction steps.
- Expected vs. actual behavior.
- Relevant log output. The host writes to stderr; please prefer text logs
  over screenshots.

If the bug involves a crash, an assertion, or memory corruption, please
include a backtrace from a debug build (`build.sh` without `--release`).

A good bug report is, in practice, the single highest-leverage way an
outside contributor can help this project.

---

## 3. Suggesting Features

Open an issue using the **Feature request** template. Yumi has a
deliberately narrow scope (see the README's "Scope: Browser vs. WebApp
Pipeline" section). Suggestions that belong on the webapp-pipeline side,
or that introduce ambient capabilities to webapps, are unlikely to be
accepted regardless of how compelling they are.

The maintainer reads every well-formed feature request, but reserves the
right to decline any of them without extended debate. The project's
stability and scope discipline come first.

---

## 4. Security-Sensitive Findings

Anything that affects:

- `src/crypto/`, `src/group_registrar/crypto.c`, `include/crypto.h`
- `src/group_registrar/identity.c`, `src/group_registrar/db.c`
- `src/network/` (peer-to-peer transport, packet parsing, framing)
- `src/webapp_runtime.c`, `src/dashboard_runtime.c`, or any `*_bindings.c`
- The WebAssembly import surface

should be reported through the channel described in
[`SECURITY.md`](SECURITY.md), **not** as a public issue. This applies
even though external code contributions are not accepted: a written
description of the bug, the threat model it breaks, and ideally a
reproduction is the contribution. The maintainer will write the fix.

---

## 5. Forking

Yumi Browser is licensed under **AGPL-3.0**. You are explicitly free to
fork it, study it, modify it, redistribute it, and run modified versions,
subject to the AGPL's terms (notably the network-use source-availability
clause). A fork that diverges from the upstream vision is a legitimate
outcome and the license exists to make it possible.

If your fork is intended to be friendly — i.e. you eventually want
upstream to consider your direction — the most useful thing is still to
file an issue describing the change *before* writing it, so that the
maintainer can flag scope or stability concerns early.

---

## 6. What to Expect

This is a single-maintainer project with a long horizon. Triage may take
time. Issues that ignore the templates, that bundle multiple unrelated
problems, or that re-litigate already-decided scope questions will likely
be closed. Issues that are well-scoped, reproducible, and respectful of
the project's stability goals will be prioritized.

Thank you for understanding the solo-developer model. It is what lets
this project credibly aim at a 20-year stability horizon.
