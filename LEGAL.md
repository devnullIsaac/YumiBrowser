# Project Notices

This file collects the project-level notices that apply to Yumi Browser. It is a convenience document for readers; the [LICENSE](LICENSE) file is what actually controls how the software is licensed.

---

## 1. License

Yumi Browser is distributed under the **GNU Affero General Public License, version 3 (AGPL-3.0)**. The full text is in [LICENSE](LICENSE).

Third-party dependencies vendored under `deps/` (SDL3, Dawn, Wasmer, DuckDB, OpenSSL, oqs-provider, FFmpeg, FreeType, HarfBuzz, ICU, libjuice, Clay, LibRaw, bzip2, libjpeg-turbo, and others) are distributed under their own respective licenses. Those licenses are preserved in the upstream source trees and are not superseded by Yumi Browser's AGPL-3.0 license. A per-dependency summary, with links to the upstream license file in each case, is in [THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md).

## 2. Project Status

Yumi Browser is pre-alpha software under active development by a solo maintainer. It is provided as-is on the terms set out in the AGPL-3.0. Statements in the project's documentation describing stability, backward compatibility, security properties, or development direction are *design goals and engineering intent*, not contractual commitments or service-level agreements.

The software has not undergone formal third-party security review as of the date of this notice. Users running Yumi Browser for sensitive activity should evaluate the [Threat Model](docs/11-threat-model.md) carefully and understand the residual risks described there.

## 3. Export Control

Yumi Browser contains cryptographic functionality. Download, use, export, re-export, or transfer of this software may be restricted in some jurisdictions. Compliance with applicable export-control, import-control, and sanctions rules is the responsibility of the individual user or redistributor.

## 4. Data and Privacy

Yumi Browser is peer-to-peer software. The project does not operate a centralized service and does not collect, store, or process user data. Each user holds the data on their own device and is the controller of their own data. Any data-protection requests should be directed to the individuals who hold the data, or to the operators of any third-party services (such as a rebroadcaster or signaling server) the user has chosen to use.

The project does not hold user communications, encryption keys, or metadata, and therefore cannot produce records that it does not possess.

## 5. Younger Users

Yumi Browser is intended for users aged 13 and older. It is not directed to younger children, and the project does not operate any data-collection infrastructure, user-account system, or telemetry that would gather information about any user. Parents and guardians who allow a younger user to install or use Yumi Browser remain responsible for that user's participation, including which groups they join and who they communicate with.

A planned join-lock feature aimed at supervising installations is described in [docs/24-roadmap.md#parental-control](docs/24-roadmap.md#parental-control).

## 6. User Responsibility

Use of Yumi Browser is the responsibility of the individual user. Users remain subject to the laws of their own jurisdictions. The project does not endorse or encourage unlawful conduct, and it does not have the technical position of an intermediary that could review, moderate, or police peer-to-peer communications between its users.

## 7. Naming (Nominative Use)

Yumi Browser does not claim trademark rights in the name "Yumi Browser" or in the project's visual identity. The project is distributed as free and open-source software and forks are permitted under the terms of AGPL-3.0.

As a matter of community practice, and to reduce user confusion:

- The name "Yumi Browser" is best used for builds distributed by the project itself, or for unmodified rebuilds of the project's source tree.
- Modified forks and derivative distributions are encouraged to use a different name, consistent with long-standing open-source convention (for example, the practice established around rebranding modified Firefox builds). This is a request, not a trademark claim.

Neutral, descriptive, and nominative references ("built for Yumi Browser," "compatible with Yumi Browser," "a Yumi Browser webapp," reviews, comparisons, journalistic or academic references) are expressly unobjectionable and require no permission.

## 8. Language

The English-language version of this file and of the project's documentation under `docs/` is authoritative. Translations are offered as a convenience; in case of discrepancy, the English original controls.

## 9. Incident Response and Disclosure

Yumi Browser is maintained by a solo developer. There is no 24/7 operations team and no on-call rotation, and response capacity is finite.

- Security reports received through [SECURITY.md](SECURITY.md) are triaged on a best-effort basis.
- When a vulnerability is confirmed and patched, the project intends to publish a security advisory through GitHub Security Advisories describing the issue, its severity, the affected versions, and the fix.
- Disclosure of an incident — its existence, nature, and remediation — happens after a patch is available, through the published advisory. Pre-patch disclosure is avoided to protect users.

This posture mirrors the coordinated-disclosure practice used by most open-source projects of comparable scale.

## 10. Contact

This is a solo-maintained open-source project without a dedicated compliance team. General correspondence should be addressed through the contact channels published in the project repository. Response capacity is best measured in weeks rather than days.

**Security vulnerabilities** should be reported privately through GitHub Security Advisories, not through general contact channels and not through public issues. See [SECURITY.md](SECURITY.md) for the full reporting process.

---

*This file is part of the Yumi Browser source tree and is updated as the project evolves. The authoritative version is the one present in the current source release.*
