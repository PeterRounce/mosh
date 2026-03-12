# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Mosh (Mobile Shell) is a remote terminal application supporting intermittent connectivity, client roaming, and speculative local echo over UDP. It uses SSH for authentication/setup, then communicates via encrypted UDP (AES-128-OCB) on ports 60000-61000.

## Build Commands

Install dependencies (Debian/Ubuntu/WSL):
```bash
sudo apt install -y build-essential protobuf-compiler libprotobuf-dev pkg-config \
    libutempter-dev zlib1g-dev libncurses5-dev libssl-dev bash-completion tmux less
```

Build and test:
```bash
./autogen.sh                  # Generate configure script (first time or after configure.ac changes)
./configure                   # Configure build
make                          # Build
make check                    # Run all tests (best without -j on loaded machines)
```

Run a single test:
```bash
cd src/tests && ./emulation-scroll.test          # Run one e2e test
cd src/tests && ./ocb-aes                        # Run one unit test binary
```

Useful configure flags: `--enable-compile-warnings=error`, `--enable-examples`, `--enable-code-coverage`, `--with-crypto-library={openssl,nettle,apple-common-crypto}`.

Static analysis: `make cppcheck`, `make scan-build` (clang), `make cov-build` (Coverity).

Code coverage (requires `lcov`): `./configure --enable-code-coverage && make check-code-coverage`.

## Code Style

- **Enforced by `.clang-format`** (clang-format 14+, Mozilla-based): 116 column limit, spaces in parentheses `foo( x )`, braces after class/function but not control statements.
- **C++17** standard.
- **Naming:** `PascalCase` classes, `snake_case` functions, `UPPER_CASE` constants.
- **Namespaces:** `Crypto`, `Network`, `Terminal`, `Parser` — map directly to `src/` subdirectories.
- **Asserts:** `fatal_assert()` for production-safe assertions, `dos_assert()` for debug-only.
- CI runs `clang-format` lint on all PRs.

## Architecture

The system is a client-server model communicating over encrypted UDP with state synchronization:

```
mosh-client (local)                    mosh-server (remote)
┌─────────────────────┐                ┌─────────────────────┐
│ STMClient           │                │ mosh-server.cc      │
│  └─TerminalOverlay  │◄──UDP/OCB────►│  └─PTY management   │
│     (prediction UI) │  (Network::   │    └─Shell process   │
│                     │   Transport)  │                     │
└─────────────────────┘                └─────────────────────┘
```

**Key layers (bottom-up):**

1. **crypto/** — AES-128-OCB authenticated encryption with pluggable backends (OpenSSL, Nettle, Apple CommonCrypto). `Crypto::Session` manages nonces and ciphertexts.

2. **network/** — UDP transport with encryption. `Network::Connection` handles sockets. `Network::Transport<MyState, RemoteState>` is a templated state-synchronization transport. `TransportSender` manages outgoing reliability and flow control. `transportfragment` handles packet fragmentation.

3. **terminal/** — Full VT100/ANSI terminal emulator. `Parser::UTF8Parser` → `Terminal::Emulator` → `Terminal::Framebuffer` (2D cell grid). `Terminal::Display` formats output.

4. **statesync/** — Bridges terminal and network. `Terminal::Complete` wraps the full terminal state and implements `diff_from()`/`apply_string()` for minimal state diffs over the wire.

5. **frontend/** — Application layer. `STMClient` (client state machine with prediction/local echo) and `mosh-server.cc` (PTY management, adaptive frame rate). `TerminalOverlay` renders prediction UI.

6. **protobufs/** — Protocol Buffers (proto2, LITE_RUNTIME) define wire formats: `hostinput.proto`, `userinput.proto`, `transportinstruction.proto`.

## Testing

Tests use a bash/tmux-based e2e framework (`src/tests/e2e-test`). Tests compare terminal output between mosh sessions and direct execution:
- `verify` mode: baseline (mosh) vs direct (no mosh) — expect identical
- `same`/`different` modes: compare baseline vs variant
- `post` mode: custom verification script

Unit tests exist for crypto (`ocb-aes`, `encrypt-decrypt`, `base64`, `nonce-incr`).

Expected failures (XFAIL): `e2e-failure.test`, `emulation-attributes-256color8.test`.

E2e test logs go to `<testname>.test.d/` with `<action>.exitstatus`, `<action>.tmux.log`, and `<action>.capture` files. Use `printf` over `echo` in test scripts for portability.

## Security Considerations

Hardening is enabled by default (`--enable-hardening`): stack protector, FORTIFY_SOURCE, PIE, RELRO. No privileged code runs — utmp updates go through libutempter. Session keys must never be exposed via process environment.
