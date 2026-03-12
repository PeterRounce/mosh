# Mosh Protocol v3: Performance and Stability Changes

This document describes the differences between mosh wire protocol v2 (mosh 1.3.x / 1.4.0) and protocol v3, introduced in the performance and stability overhaul.

Protocol v3 is a breaking change. v2 clients cannot connect to v3 servers and vice versa. A clear error message is shown on version mismatch.

## Cryptography

**v2:** AES-128-OCB with pluggable backends (OpenSSL, Nettle, Apple CommonCrypto). 128-bit key, 12-byte nonce (8-byte counter + 4 bytes fixed).

**v3:** XChaCha20-Poly1305 via libsodium. 256-bit key, 24-byte nonce (8-byte counter, 16 bytes zero-padded). Keys are locked in memory with `sodium_mlock()` and zeroed on destruction with `sodium_memzero()`.

The bundled OCB implementation (`ae.h`, `ocb_internal.cc`, `ocb_openssl.cc`) is removed. libsodium is the sole crypto provider.

## Compression

**v2:** zlib with default settings. Compression context re-initialized per call.

**v3:** zstd at compression level 1 with persistent `ZSTD_CCtx` and `ZSTD_DCtx` contexts. Persistent contexts avoid re-initialization overhead. Decompression is capped at 16 MiB to prevent memory exhaustion from malformed frames.

Expected improvement: ~2-3x faster compression at similar compression ratios.

## Event Loop (Linux)

**v2:** `pselect()` for I/O multiplexing on all platforms.

**v3:** `epoll_pwait()` on Linux (with `EPOLL_CLOEXEC`), falling back to `pselect()` on other platforms. `epoll_pwait()` provides lower per-call overhead and atomic signal unmasking equivalent to `pselect()`.

## Adaptive Send Timing

**v2:** Fixed `SEND_INTERVAL_MIN` of 20ms between frames.

**v3:** Adaptive minimum based on smoothed round-trip time (SRTT):

| SRTT       | Minimum Send Interval |
|------------|-----------------------|
| < 20ms     | 8ms (LAN)             |
| 20-50ms    | 15ms (regional)       |
| >= 50ms    | 20ms (WAN, same as v2)|

ACK delay reduced from 100ms to 30ms.

On low-latency links, v3 can achieve up to 125 frames/sec vs v2's 50 frames/sec ceiling.

## Terminal State Synchronization

### Generation Counter

The `Framebuffer` maintains a generation counter that increments on any mutation (cell edit, scroll, resize, reset). `TransportSender` tracks the generation to skip redundant `diff_from()` calls when the terminal state has not changed since the last diff was computed.

### Diff Caching

`TransportSender` caches the most recent diff output and reuses it when both the local framebuffer generation and the assumed receiver state number are unchanged. A separate cache exists for the resend optimization path. Both caches are invalidated when the assumed receiver state advances.

### Pre-allocated Buffers

`diff_from()` uses an output parameter (`std::string*`) instead of returning by value, allowing buffer reuse across ticks. Protobuf serialization uses `SerializeToArray()` into pre-sized buffers instead of `SerializeAsString()`.

## Scroll Detection

**v2:** O(rows^2) loop comparing old and new rows to detect scroll offset (with early-exit optimization).

**v3:** Each `Row` maintains a lazy XXH3 hash over its semantic content (cell text, flags, renditions). `Terminal::Display` builds an `unordered_map<uint64_t, int>` of old row hashes, then looks up each new row's hash for O(rows) scroll detection. Hashes are invalidated on row mutation and recomputed on demand.

The improvement is most visible on large terminals (100+ rows). On a typical 24-row terminal, the difference is negligible.

## Reconnection Resilience

### SRTT Reset

**v2:** After a network gap, the SRTT retains its stale value. If SRTT was 30ms before the gap, the sender continues using 30ms-based timing even though the reconnected path may have different characteristics. The SRTT converges slowly via exponential moving average (alpha = 0.125).

**v3:** When no packet is received for >3 seconds, the connection is treated as a reconnection event:
- SRTT resets to 1000ms, RTTVAR resets to 500ms (conservative starting point)
- The first 4 RTT samples after reconnection use alpha = 0.5 instead of 0.125 for faster convergence

### Burst Mode

On reconnection, `TransportSender` enters burst mode: send interval drops to 4ms for 500ms. This allows rapid state resynchronization, pushing the full terminal state to the client quickly. After 500ms, normal adaptive timing resumes.

### Progressive State Resync

During reconnection, diffs are sent at increasing priority levels:

| Priority | Content                    | Purpose                        |
|----------|----------------------------|--------------------------------|
| 0        | Cursor position + current row | Immediate visual feedback    |
| 1        | Full visible screen        | Complete display restoration   |
| 2        | Full state (same as normal)| Return to normal operation     |

The user sees the cursor and current line almost immediately, then the rest of the screen fills in. In v2, the entire screen state is sent as a single diff, which takes longer to arrive on a degraded link.

## Wire Protocol Compatibility

v3 clients and servers identify themselves with `MOSH_PROTOCOL_VERSION = 3`. On version mismatch, the connection fails with:

```
Network protocol mismatch. Got version N, expected version 3.
```

The `mosh` wrapper script accepts both 16-byte (v2) and 32-byte (v3) base64-encoded keys in the `MOSH CONNECT` handshake, allowing the wrapper to work with either version's server.

## Dependencies

| Dependency | v2                          | v3                  |
|------------|-----------------------------|---------------------|
| Crypto     | OpenSSL / Nettle / CommonCrypto | libsodium       |
| Compression| zlib                        | libzstd             |
| Hashing    | (none)                      | libxxhash           |

Install on Debian/Ubuntu:
```bash
sudo apt install libsodium-dev libzstd-dev libxxhash-dev
```

## Summary of Expected User-Visible Differences

**LAN sessions:** Slightly snappier during fast typing due to 8ms minimum send interval.

**High-latency links (VPN, satellite):** Faster acknowledgments (30ms vs 100ms ACK delay). Adaptive timing matches v2 behavior at high SRTT.

**Reconnection after network loss:** Visibly faster recovery. Cursor appears almost immediately, screen fills in progressively, burst mode pushes state rapidly. v2 ramps up slowly from stale SRTT values.

**Day-to-day use on a good connection:** Likely imperceptible. The biggest user-visible improvement is reconnection behavior.
