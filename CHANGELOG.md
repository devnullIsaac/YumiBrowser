# Changelog

All notable changes to Yumi Browser will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project will adhere to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
once a 1.0 release is tagged. Until then, version numbers are pre-release
identifiers (`0.x`) and breaking changes are expected between any two
revisions.

> **Wire-protocol stability:** until 1.0, the wire protocol, the WebAssembly
> API surface, and the group-registrar contract may change without notice.
> The five-year stability commitment described in `README.md` applies from
> 1.0 onward.

---

## [Unreleased]

### Added
- `SECURITY.md` documenting the security policy, scope, reporting channel,
  and disclosure expectations.
- `THIRD_PARTY.md` documenting vendored third-party source code (Skein /
  Threefish reference implementation, Brian Gladman portable headers) and
  their licenses.
- `CONTRIBUTING.md` describing the contribution process, including extra
  scrutiny for security-sensitive areas of the codebase.
- `CODE_OF_CONDUCT.md` (short, project-specific).
- `AUTHORS` listing the maintainer and acknowledging third-party code
  authors.
- `.gitea/` issue and pull-request templates for Codeberg.
- Build / Install section in `README.md`.

### Changed
- (none yet)

### Fixed
- (none yet)

### Security
- (none yet)

---

## Release Process (for maintainers)

When tagging a release:

1. Move everything under `[Unreleased]` into a new section headed
   `## [X.Y.Z] - YYYY-MM-DD`.
2. Add an empty `[Unreleased]` section above it.
3. Update the comparison links at the bottom of this file.
4. Tag the commit (`git tag -a vX.Y.Z -m "Yumi Browser X.Y.Z"`).
5. Push the tag and create a release on Codeberg with the matching notes.

Security-relevant fixes are recorded under a `### Security` heading and
cross-referenced with any advisory ID issued under the policy in
`SECURITY.md`.

---

[Unreleased]: https://codeberg.org/DevNullIsaac/YumiBrowser/compare/HEAD...main
