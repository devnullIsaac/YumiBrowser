# WebApp Distribution

Yumi Browser ships with a default set of webapps covering core collaborative functionality: chat, file sharing, video streaming, picture sharing, music sharing, animated emoji, and livestreaming.

## Signed WebApps with Developer Metadata

Every webapp distributed for Yumi Browser is signed by its developer and accompanied by identifying information. Group owners specify which webapps their group uses by listing the hash, version, and checksum in the Group Registrar, and browsers are designed to reject any webapp whose signature, hash, or metadata does not match. This is intended as an architectural barrier against supply-chain attacks: substituting a hostile webapp for a legitimate one is designed to require breaking the signature or compromising the registrar, both of which are protected by the cryptographic stack described earlier.

## Most Group Activity Happens on Vetted WebApps

The default webapps shipped with Yumi Browser are authored and signed by the project, widely used across the platform, and observed by the community. Additional webapps distributed through the community page accumulate reputation through use — by the time a webapp sees broad adoption, it has been looked at by many eyes. This is how the attack surface of the platform stays narrow in practice: users are not running arbitrary code from strangers; they are running webapps that the rest of the community has already exercised and watched.

## Community Page

A public group whose sole purpose is distributing additional webapps, with reviews and verification stamps. Trust is not centralized — anyone can create a competing distribution group.

## Installation and Group Binding

Group owners specify which webapps their group uses by listing the hash, version, and checksum in the Group Registrar. Members download matching webapps from the community page or directly from peers.

## Version Management

All installed versions are retained until explicitly purged. Groups set which version to use in the registrar. Multiple versions coexist locally for users in multiple groups.
