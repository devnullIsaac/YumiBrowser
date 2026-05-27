# Application Model

Yumi Browser recognizes two distinct types of WebApp, each with different privilege levels, API surfaces, and responsibilities.

## Dashboard WebApp

The Dashboard is a special, confined WebApp that serves as the control plane for the entire browser. It is the only WebApp with access to browser-level configuration APIs. Through the Dashboard, users manage their groups, arrange their UI layout, configure networking settings, manage installed webapps, handle peer identities and keypairs, and control which groups are active.

The Dashboard governs the visual presentation layer — how windows are arranged, how groups are visually organized, and how the overall experience looks and feels. It supports deep customization of layout, animations, themes, and transitions.

### Dashboard API Surface

- Browser settings management (identity, keypairs, preferences)
- Group management (joining, leaving, creating, configuring groups)
- Network configuration (signaling server addresses, connection management)
- Installed webapp management (install, uninstall, version purging)
- UI layout and visual customization

### Dashboard Security Model

The Dashboard is not granted the capability to communicate with groups directly. This is an intentional architectural decision — the Dashboard is not issued network access. It manages the infrastructure that makes group communication possible, but it does not participate in group conversations or access group-encrypted content. It is designed as a local-only management interface. Even in the unlikely event that the Dashboard WebApp is compromised, the intended blast radius is limited: under this design it has no channel to exfiltrate data over the network. Its clipboard mediation (copy/paste) is observable by the webapp on the other end — the receiving webapp sees what was actually pasted.

The Dashboard is replaceable. Because it is itself a WebApp running in Wasmer, users can swap it for a different implementation. A power user might prefer a minimal, keyboard-driven Dashboard. A casual user might prefer something visually rich. The browser doesn't care — it just needs a WebAssembly module that speaks the Dashboard API.

## Regular WebApp

A Regular WebApp is any application that runs within the context of a group. It serves a dual role: a **user-facing UI** that the group member interacts with, and a **mini-server** running in the background that handles data exchange with other peers in the group.

On the UI side, the WebApp renders its interface through WebGPU. On the server side, it processes incoming data from group peers, maintains local state, and sends outgoing data — all through a networking API strictly scoped to the group the app is running in.

### Regular WebApp API Surface

- Sending and receiving messages to/from group peers only
- Accessing group-scoped shared state
- Rendering UI through WebGPU
- Local storage scoped to the group context

### Regular WebApp Restrictions

A Regular WebApp is not granted the capability to:

- Access browser-level settings
- Communicate outside its assigned group
- Make arbitrary network connections or phone home
- Interact with other groups or other webapps running in different group contexts
- Access the signaling layer, peer discovery, or any networking primitive directly

This hard separation is designed so that even a malicious Regular WebApp is confined to the group it runs in. Under this design it is not in a position to exfiltrate data to external servers, observe other groups, or modify the browser itself.

### Dashboard ↔ Regular WebApp Relationship

The relationship between Dashboard and Regular WebApps is one-directional. The Dashboard configures the environment in which Regular WebApps run, but Regular WebApps have no visibility into or control over the Dashboard.
