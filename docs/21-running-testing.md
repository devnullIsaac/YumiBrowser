# Running and Testing

## Running

```bash
# Normal launch (loads dashboard + webapps from XDG data dir)
./release/yumibrowser

# Dev mode: run a single WASM webapp
./release/yumibrowser --webapp path/to/app.wasm

# Custom data directory
./release/yumibrowser --data-dir /path/to/data
```

Data directory resolution order:
1. `$XDG_DATA_HOME/com.yumi.browser`
2. `~/.local/share/com.yumi.browser`
3. `release/` next to the binary (dev mode)

## Testing

```bash
cd build
meson test
```

Covers: buffer serialization, database schema, audit logging, cryptographic primitives (including the Threefish/Skein AEAD construction), join verification, delta sync, full registrar integration, GUI widgets (textbox, docbox, treeview, picturebox, nodegraph, unicode strings), UDP transport, secure UDP, high-level client, and dashboard integration.
