# Infrastructure

Three server types, each with exactly one job. All are operated by users or third parties chosen by users; the project does not operate any of them.

## Signaling Server

A lightweight rendezvous service, typically operated by the user or a third party of the user's choosing. It temporarily stores a 24-hour lease table mapping hashed peer identifiers to network addresses (IP/port) for NAT traversal. It does not store group membership lists, message content, encryption keys, or personally identifiable information. All entries expire automatically after 24 hours. The operator cannot read group communications or identify users by real-world identity.

## Rebroadcast Server

An optional content relay, typically operated by the group owner or a third party designated by the group owner. It stores encrypted data blobs and wrapped encryption keys. It does not possess the group epoch key and therefore cannot decrypt stored content. It stores no real names, email addresses, or other personally identifiable information. The operator can see metadata such as upload times, data sizes, and hashed peer IDs. The group owner controls retention policy and server selection.

See [12 — Rebroadcast Server](12-rebroadcast-server.md) for full technical details.

## Full Server

A 24/7 node acting as a full peer with encryption keys and group membership. For groups needing persistent availability. Generally not recommended due to being a high-value target, but the option exists.

