Mosh: the mobile shell
======================

Mosh is a remote terminal application that supports intermittent
connectivity, allows roaming, and provides speculative local echo
and line editing of user keystrokes.

Unlike SSH, Mosh keeps sessions alive across network changes, sleep/wake
cycles, and connection drops. This is a fork with protocol v3 changes.
See [docs/developers.md](docs/developers.md) for full feature details,
usage, and distributor guidance.

Protocol v3
-----------

This version introduces wire protocol v3 with improved performance and
stability: XChaCha20-Poly1305 encryption (libsodium), zstd compression,
adaptive send timing, and faster reconnection after network loss. See
[docs/protocol-v3-changes.md](docs/protocol-v3-changes.md) for details.

Protocol v3 is not compatible with earlier versions. Both client and server
must be upgraded together.

Installing
----------

Download the `.deb` package from the
[latest release](https://github.com/PeterRounce/mosh/releases/latest)
and install on both client and server:

```
$ sudo apt install ./mosh_1.5.0-1_amd64.deb
```

This automatically installs all runtime dependencies (libsodium, libzstd,
libxxhash, etc.). To build from source instead, see
[docs/developers.md](docs/developers.md#building-from-source).

The upstream Mosh project is at <https://github.com/mobile-shell/mosh>.
This fork is licensed under GPLv3, same as upstream.
