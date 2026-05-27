# Long-Term Stability Commitment

Yumi Browser is designed around a stability commitment that most browser-class software does not attempt.

## Baseline: Five Years of Stability

A Yumi Browser installation should continue to function, join groups, and interoperate with other peers for at least five years from its release without forced migration. Groups that form on a given version should be able to keep running on that version without the platform pulling the rug out from under them.

## Target: Twenty Years of Stability

The aspirational goal is two decades. This is not a promise, but it is the design target that drives every other decision in the project — the cryptographic choices, the refusal to churn APIs, the deliberate avoidance of fashionable dependencies, the conservative wire format.

## What Changes and What Doesn’t

The C host layer is designed to change rarely. Most evolution happens in dependencies (security fixes pulled in from upstream projects) and in the WebAssembly API surface (new host functions added to enable new kinds of webapps). The core architecture — the sandbox model, the group registrar, the cryptographic construction, the wire protocol — is designed to stay put.

## WebAssembly API Backward Compatibility

**API compatibility for all WebAssembly-exposed host functions is a core design objective of Yumi Browser.** Once a host function is exposed to WebAssembly, the project's design intent is that it stays exposed and its contract does not change.

This is a strong design goal that drives how the project is built: the entire point of the runtime model is that webapps written today should continue to work in a Yumi Browser built ten years from now. Where the project can reasonably prevent a breaking change, it intends to do so.

When a dependency evolves in a way that would otherwise break the API Yumi exposes to WebAssembly — for example, if an upstream library changes its function signatures or removes functionality that a Yumi host function depends on — Yumi Browser intends to provide a translation layer that preserves the original WebAssembly-facing contract. The dependency is allowed to evolve underneath the translation layer; the contract with WebAssembly modules is preserved wherever feasible.

### Status of These Commitments

The statements in this document describe *design goals and engineering intent*. Yumi Browser is distributed under the AGPL-3.0 license, and the terms of that license control how the software is provided. Where this document and the license differ, the license is what governs.

The authoritative source for the API contracts is the set of WebAssembly header files shipped with the project in `sdk/`. Webapp developers should treat those headers as the contract. If it is documented there, it will keep working.

## Data Migration as a First-Class Concern

Yumi Browser is designed from the start to preserve user data across versions. The database schemas used by the Group Registrar and by webapps are versioned, migration paths are part of the release process, and the project's position is that data loss across a version upgrade is a bug, not a user problem.

This matters because the platform is designed to be used for years. A group that has accumulated years of chat history, shared files, and governance decisions should not lose any of that because of a version bump. The migration infrastructure exists specifically to prevent that class of failure.

## The Cryptographic Constraint

Long-term stability and cryptographic agility are in tension, and this document needs to be honest about that tension rather than pretend it doesn't exist.

The entire Group Registrar — peer identities, signatures, encrypted content, the chain of custody over governance actions — depends on specific cryptographic assumptions. The peer ID is derived from a specific hash of a specific signing key. The envelope encryption assumes specific primitives. The signatures on the audit log assume a specific signature scheme. If any of those assumptions fail, the dependent structures do not survive the failure intact.

If a primitive Yumi Browser uses becomes compromised or broken, Yumi Browser will have to be updated to replace it. When that update ships, users will need to upgrade to it — and the upgrade is not a trivial "install the new version and everything keeps working." **In the worst case, a cryptographic break could require groups to be rebuilt on the new version.** This is not a hypothetical. It is a structural consequence of building a peer-to-peer system where identity, membership, and content are all cryptographically bound together.

The migration path in that scenario would look roughly like this: admins re-invite members to a new group on the upgraded Yumi Browser, and a VPS server re-mirrors the historical data (messages, files, logs) so that the upgraded webapps can sync it into the new group. It is a significant undertaking, not an unorganized chaos — but it is significant.

**This is precisely why Yumi Browser uses the strongest cryptography available.** The whole point of choosing Threefish-1024 and Skein-1024 at the symmetric layer, ML-KEM-1024 and FrodoKEM-1344 and BrainPool-P512r1 at the asymmetric layer, and ML-DSA-87 for signatures is to push the probability of a forced cryptographic migration as far out into the future as possible. Every bit of security margin, every hybrid construction, every deliberately-oversized parameter choice is in service of the same goal: *not needing to do this migration.* The stronger the cryptography, the longer the baseline stability commitment holds, and the less likely it is that users will ever have to experience the worst-case migration path.

Users should be aware that this is the shape of the risk. It is disclosed here, up front, rather than buried.
