# Third-Party Code and Licensing

Yumi Browser as a whole is licensed under the **GNU Affero General Public
License, version 3** (see `LICENSE`). This document records third-party
source code that is **vendored directly into this repository's source
tree**, the licenses under which that code is redistributed here, and how
those licenses interact with the project's AGPL-3.0 license.

Third-party code that is pulled in as a Git submodule under `deps/` (SDL,
OpenSSL, oqs-provider, FFmpeg, FreeType, HarfBuzz, ICU, LibRaw, Wasmer,
Dawn, DuckDB, Slang, libjuice, bzip2, libjpeg-turbo, etc.) is **not**
covered by this file. Each of those projects ships its own license; please
consult the corresponding `deps/<name>/` directory for upstream license
text. A summary of those dependencies and their licenses will be added to
this document prior to the 1.0 release.

---

## 1. Vendored Source Files

### 1.1 Skein-1024 / Threefish-1024 reference implementation

**Location in this repository:** `src/crypto/`

| File                      | Origin                                  |
| ------------------------- | --------------------------------------- |
| `src/crypto/skein.h`      | Skein hash function reference (NIST SHA-3 submission) |
| `src/crypto/skein.c`      | Skein hash function reference           |
| `src/crypto/skein_block.c`| Threefish block function reference      |
| `src/crypto/skein_iv.h`   | Skein initial vectors                   |
| `src/crypto/skein_port.h` | Portable type / endian shims for Skein  |
| `src/crypto/skein_debug.h`| Optional debug instrumentation          |
| `src/crypto/skein_debug.c`| Optional debug instrumentation          |
| `src/crypto/SHA3api_ref.h`| NIST SHA-3 candidate API wrapper        |
| `src/crypto/SHA3api_ref.c`| NIST SHA-3 candidate API wrapper        |

**Authors:** Doug Whiting and the Skein team (Bruce Schneier, Niels
Ferguson, Stefan Lucks, Doug Whiting, Mihir Bellare, Tadayoshi Kohno, Jon
Callas, and Jesse Walker).

**License:** Public domain. Each file carries a notice reading, in part:

> This algorithm and source code is released to the public domain.

The Skein reference code was published by its authors as part of the
NIST SHA-3 competition and explicitly placed in the public domain.

**Modifications by Yumi Browser:** the files are used as-is, except for
trivial inclusion-path adjustments to fit this project's source layout.
The cryptographic algorithms have not been modified. Where Yumi calls
into these files, it does so through the wrappers in
`src/crypto/crypto.c` and `include/crypto.h`.

**Compatibility with AGPL-3.0:** Public-domain code is compatible with
AGPL-3.0 by definition — the AGPL only adds obligations to the
distribution of the combined work; it does not strip the public-domain
status of these specific files. Anyone redistributing only the contents
of `src/crypto/` listed above may continue to do so as public-domain
code.

---

### 1.2 Brian Gladman portable headers

**Location in this repository:**

- `src/crypto/brg_endian.h`
- `src/crypto/brg_types.h`

**Author:** Dr Brian Gladman (Worcester, UK).

**License:** Brian Gladman's standard "either-or" license, reproduced from
the source files:

> Copyright (c) 2003, Dr Brian Gladman, Worcester, UK. All rights reserved.
>
> LICENSE TERMS
>
> The free distribution and use of this software in both source and binary
> form is allowed (with or without changes) provided that:
>
>   1. distributions of this source code include the above copyright
>      notice, this list of conditions and the following disclaimer;
>
>   2. distributions in binary form include the above copyright
>      notice, this list of conditions and the following disclaimer
>      in the documentation and/or other associated materials;
>
>   3. the copyright holder's name is not used to endorse products
>      built using this software without specific written permission.
>
> ALTERNATIVELY, provided that this notice is retained in full, this
> product may be distributed under the terms of the GNU General Public
> License (GPL), in which case the provisions of the GPL apply INSTEAD
> OF those given above.
>
> DISCLAIMER
>
> This software is provided 'as is' with no explicit or implied warranties
> in respect of its properties, including, but not limited to, correctness
> and/or fitness for purpose.

This is a permissive BSD-style license with an OR-GPL relicensing option.
It is widely recognized as a "Free Software" license and is compatible
with the GPL family (including AGPL-3.0) via either the BSD-style branch
(BSD-3-clause-style permissions are GPL-compatible) or the explicit
GPL relicensing branch.

**Modifications by Yumi Browser:** none. The files are used unmodified,
except possibly for path/include-guard hygiene.

**Endorsement:** in line with clause 3 of the license, Yumi Browser does
**not** use Dr Brian Gladman's name to endorse this product. His name
appears here solely to satisfy the attribution requirement of the license.

---

## 1.3 DiceBear avatar art (UI mockup only)

**Where it appears:** demonstration / mockup imagery used in design
materials and (where applicable) screenshots embedded in this
repository's documentation. **No DiceBear output is required at runtime
or compiled into the shipped binary.**

**Origin:** generated via the DiceBear avatar library
(https://www.dicebear.com), `avataaars` style.

**License:**

- The DiceBear library itself is distributed under the **MIT License**
  (Copyright © Florian Körner).
- The `avataaars` sprite collection is licensed under
  **CC BY 4.0** (Copyright © Pablo Stanley,
  https://www.avataaars.com/).

**Attribution:** "Avataaars" by Pablo Stanley — https://www.avataaars.com/
— used under CC BY 4.0. No modifications to the underlying sprite parts
have been made beyond the parameter-driven composition performed by the
DiceBear library.

This entry exists solely to satisfy CC BY 4.0's attribution clause for
the avatar imagery used in mockups. It does not extend to any other
component of Yumi Browser.

---

## 1.4 *Wing It!* video content (preview video and demo webapp)

**Where it appears:**

- `Preview.webm` at the root of this repository — a short functional
  preview of Yumi Browser that includes excerpts from the Blender
  Foundation's *Wing It!* open movie.
- The demo WebAssembly application staged at `release/demo/demo.wasm`
  (built from sources outside this repository and shipped only as a
  first-run demonstration), which uses *Wing It!* footage as sample
  media to exercise the video pipeline.

**Origin:** *Wing It!*, an open movie produced by the Blender Studio /
Blender Foundation (https://studio.blender.org/projects/wing-it/).

**License:** **Creative Commons Attribution 4.0 International
(CC BY 4.0)**, https://creativecommons.org/licenses/by/4.0/.

Per the Blender Studio licensing notice:

> The work of the *Wing It!* project is licensed under the Creative
> Commons Attribution 4.0 license. […] In short, this means you can
> freely reuse and distribute this content, also commercially, as long
> as you include proper attribution.
>
> The attribution is (if not specifically mentioned otherwise):
>
> (CC) Blender Foundation | studio.blender.org
>
> Excluded from the Creative Commons license is: all logos on this
> website (including the Blender logo, *Wing It!* logo, Creative Commons
> logo, sponsor logos) and associated trademarks.

**Attribution (as required by CC BY 4.0):**

> *Wing It!* — (CC) Blender Foundation | studio.blender.org —
> used under CC BY 4.0.

**Modifications by Yumi Browser:** the source video has been trimmed,
re-encoded, and (in the demo webapp case) framed inside the Yumi Browser
UI for demonstration purposes. No claim of authorship over the original
*Wing It!* footage is made; all creative credit belongs to the Blender
Foundation and the *Wing It!* production team.

**Trademarks:** the Blender logo, the *Wing It!* logo, and any
associated Blender Foundation trademarks are **not** licensed under
CC BY 4.0 and are **not** used by Yumi Browser to endorse this product.
The attribution above refers to the audiovisual content only.

**Compatibility with AGPL-3.0:** CC BY 4.0 is a content license, not a
software license; it applies to the *Wing It!* media bundled alongside
Yumi Browser (in `Preview.webm` and, when present, in the demo
webapp's media assets), not to the AGPL-3.0 source code of Yumi Browser
itself. The two licenses apply to disjoint sets of files within this
distribution and do not conflict.

---

## 2. Combined-Work Licensing

The combined work — i.e., the Yumi Browser executable produced by building
this repository — is distributed under the **GNU Affero General Public
License, version 3 or (at your option) any later version**. This applies
to the binaries published under `release/` and to redistributed source
trees of this repository as a whole.

The vendored third-party files listed above retain their original licenses
within the source tree. Removing them from this repository and
redistributing them on their own continues to be governed by their
original licenses, not the AGPL-3.0.

---

## 3. Reporting Licensing Issues

If you believe a file in this repository is missing attribution, has the
wrong license recorded, or is incompatible with AGPL-3.0, please open
an issue or contact the maintainer at the address listed in
`SECURITY.md`. Licensing problems are treated as priority bugs.
