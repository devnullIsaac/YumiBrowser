# Current Development Focus

- **MISRA-C compliance** for first-party Yumi Browser code — the C host, the in-house crypto abstraction layer, the group registrar, and the networking stack. Third-party dependencies (OpenSSL, FFmpeg, DuckDB, Dawn, ICU, HarfBuzz, liboqs, Wasmer, libjuice, etc.) are outside the compliance scope; they are used as shipped and their security postures are the responsibility of their upstreams. Where a dependency is small enough and the upstream is willing, there may be future attempts to contribute MISRA-C compliance work back — libjuice is a candidate. If an upstream is not willing to accept that work, a renamed fork under AGPL-3.0 remains an option. This is future work and not a commitment.
- **Frama-C annotations** on security-critical paths to formally prove memory safety and absence of undefined behavior in the crypto abstraction layer, the group registrar, and the secure UDP stack. Scope is first-party code, per the same boundary as the MISRA-C work above.
- **Test vectors and construction documentation** for the Threefish/Skein AEAD, intended to be published as a dedicated construction document to make external review possible.
- **A layman's auditing guide** (see Roadmap) to lower the barrier for independent review of the codebase.
- **External cryptographic review** of the AEAD composition, pursued through targeted funding channels such as NLnet NGI Zero.
- **Continued stabilization of the default webapp set** and the Dashboard.
- **Reproducible Flatpak builds** so users can verify that binary distributions match the source tree.
- **Data migration infrastructure** for preserving user data across Yumi Browser version upgrades, consistent with the Long-Term Stability Commitment.
