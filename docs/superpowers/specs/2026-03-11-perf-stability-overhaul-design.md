# Mosh Performance & Stability Overhaul — Design Spec

**Date:** 2026-03-11
**Target:** Debian & Ubuntu (Linux only)
**Goals:** Improve perceived latency for interactive use; improve behavior when network is lost for seconds or longer and then returns.
**Approach:** Full transport layer overhaul — all hot-path layers (state diff, transport sender, compression, crypto, event loop) are addressed together.

---

## 1. Dirty Flag + State Diff Optimization

**Problem:** `diff_from()` runs every tick (~20ms) even when the terminal hasn't changed. After reconnection, the full state diff is recomputed and serialized unnecessarily.

**Design:**

- Add a `generation` counter to `Terminal::Framebuffer` (not `Row` — `Row` already has a `gen` field, but `Framebuffer` has no aggregate generation stamp). Increment on any mutation path: cell writes (`terminalfunctions.cc`), scrolls, resizes, title/bell/mouse-mode/cursor-visibility changes (`terminaldispatcher.cc`, `terminalframebuffer.cc`).
- In `TransportSender`, track two pieces of state for cache validity:
  - `last_local_generation_` — the framebuffer generation at last diff computation.
  - `last_assumed_receiver_num_` — the sequence number of `assumed_receiver_state` at last diff computation. This captures when `update_assumed_receiver_state()` advances the assumed receiver (which happens every tick an ack is processed), which would change the diff result even if the terminal is idle.
- In `tick()`, skip `diff_from()` and reuse the cached diff only when **both** the local generation and assumed receiver num are unchanged.
- `attempt_prospective_resend_optimization()` also calls `diff_from()` against a different base (`sent_states.front().state`). This call uses a **separate** cache (`resend_diff_cache_`) with its own generation/num tracking, since it diffs against a different base state.
- On reconnection (when `assumed_receiver_state` resets), force a full diff regardless of generation.

**Impact:** Eliminates ~95% of diff computation during idle sessions. After reconnection, the first diff is still full-cost but subsequent idle ticks are free.

---

## 2. Adaptive Send Timing

**Problem:** `SEND_INTERVAL_MIN = 20ms` caps at 50fps regardless of link quality. `ACK_DELAY = 100ms` slows the RTT feedback loop. After reconnection, SRTT may be stale, causing sluggish convergence.

**Design:**

- Make `SEND_INTERVAL_MIN` adaptive based on SRTT:
  - SRTT < 10ms (LAN): min interval = 8ms (~125fps)
  - SRTT 10-50ms (regional): min interval = 15ms (~67fps)
  - SRTT > 50ms (WAN): keep 20ms (~50fps)
- Reduce `ACK_DELAY` from 100ms to 30ms — tightens the feedback loop, especially on fast links.
- Add reconnection SRTT reset: when a connection resumes after >3s silence, reset SRTT/RTTVAR to initial values and re-probe rather than using stale estimates. This prevents the sender from over- or under-pacing based on pre-disconnect conditions.
- **Burst mode override:** Section 8 defines a reconnection burst mode (4ms interval for 500ms). This operates as a **separate override** that bypasses `send_interval()` entirely during the burst window. The adaptive SRTT-based logic resumes after the burst window expires. This avoids the contradiction where SRTT reset to 1000ms would produce `ceil(1000/2) = 500ms` intervals during the burst period. Implementation: add a `burst_until_` timestamp to `TransportSender`; when `now < burst_until_`, `send_interval()` returns the burst interval directly without consulting SRTT.
- Keep `SEND_INTERVAL_MAX = 250ms` and `ACK_INTERVAL = 3000ms` unchanged — these are reasonable for idle/degraded links.

**Impact:** Noticeably snappier typing on LAN. Faster state convergence after reconnection due to better RTT estimation.

---

## 3. epoll Event Loop + fd Allocation Fix

**Problem:** `select()` rebuilds fd_set every iteration. `network->fds()` allocates a new `std::vector<int>` per tick. On a 50fps loop that's ~50 heap allocations/sec doing nothing useful.

**Design:**

- Replace `Select` class internals with `epoll_create1()` / `epoll_ctl()` / **`epoll_pwait()`** (not `epoll_wait`).
- **Critical: signal handling.** The current `Select` uses `pselect(2)` specifically for atomic signal unmasking — signals (SIGWINCH, SIGTERM, SIGINT, SIGHUP, SIGPIPE, SIGCONT) are blocked via `sigprocmask(SIG_BLOCK)` outside the wait, and unblocked only during `pselect` via its signal mask parameter. `epoll_pwait` (Linux 2.6.19+) provides the same atomicity guarantee. Plain `epoll_wait` does **not** — using it would introduce a TOCTOU race where signals between the `got_signal[]` check and the wait call are missed until the next timeout. The existing `empty_sigset` in `Select` is passed directly to `epoll_pwait`.
- Keep the existing `Select` API surface (`add_fd`, `clear_fds`, `select`) so callers (stmclient, mosh-server, ntester) don't change.
- Add/remove fds via `epoll_ctl` only when the fd set actually changes, rather than rebuilding every tick.
- Change `Network::Transport::fds()` to return `const std::vector<int>&` (reference to cached member) instead of constructing a new vector each call.
- For the rare case where fds change (socket recreation after roaming), update the cached list and call `epoll_ctl` to sync.

**Scope:** Linux only (Debian/Ubuntu). The `#ifdef` stays inside `Select` — no platform branching in callers.

**Impact:** Zero per-tick heap allocations for the event loop in steady state. Eliminates per-tick syscall overhead from fd_set copy.

---

## 4. zstd Compression

**Problem:** zlib's `compress()`/`uncompress()` reinitialize internal z_stream context on every call. For mosh's small diffs (typically < 1KB), the setup cost dominates.

**Design:**

- Replace zlib with zstd (`libzstd-dev` package).
- Use `ZSTD_CCtx` / `ZSTD_DCtx` persistent contexts — allocated once, reused across calls via `ZSTD_compressCCtx()` / `ZSTD_decompressDCtx()`.
- Compression level: zstd level 1 (fastest) — at < 1KB payloads, higher levels give negligible ratio improvement but cost more CPU.
- Keep the existing `Compressor` singleton pattern, just swap internals.
- Pre-allocate output buffer as a member. Size at construction time using `ZSTD_compressBound()` with a conservative max payload assumption (the existing `BUFFER_SIZE = 2048*2048` static buffer). If a payload exceeds the pre-allocated size, fall back to dynamic allocation (should never happen in practice given mosh's packet sizes).
- Return `std::string_view` or write into caller-provided buffer instead of returning `std::string` by value.
- **Error handling:** zstd returns error codes as negative `size_t` values, checked via `ZSTD_isError()`. Replace the current `dos_assert(Z_OK == compress(...))` pattern with `fatal_assert(!ZSTD_isError(result))` to match mosh's existing assertion conventions.

**Wire compatibility:** Breaking change. Protocol version in `transportfragment` header must be bumped. Both client and server must be updated together.

**Impact:** ~3-5x faster compression/decompression for small payloads. Context reuse eliminates per-call initialization. Pre-allocated buffer eliminates heap allocation.

---

## 5. Pre-allocated Serialization Buffers

**Problem:** `SerializeAsString()` allocates a new `std::string` on every call. `diff_from()` returns `std::string` by value. `network_order_string()` creates small strings for timestamps. All in the hot path.

**Design:**

- In `TransportSender`, add **two** persistent diff buffers: `diff_buffer_` for the main `tick()` diff and `resend_diff_buffer_` for `attempt_prospective_resend_optimization()`, which computes a second diff against a different base state (`sent_states.front().state`) in the same tick. Using a single buffer would corrupt the result.
- Change `diff_from()` signature to `void diff_from(..., std::string* output)` — writes into caller's buffer, which retains its allocation across ticks. Since `Complete::diff_from()` is currently declared `const`, the internal reusable `Display` frame buffer must be declared `mutable`. The `const` on the method signature is preserved (the method is logically const — it doesn't change observable terminal state, only internal rendering scratch space).
- In `TransportFragment`, add a persistent `std::vector<uint8_t> serialize_buffer_` member. Switch from `inst.SerializeAsString()` to `inst.SerializeToArray(serialize_buffer_.data(), size)`. Call `ByteSizeLong()` first to determine required size.
- Replace `network_order_string()` with direct `memcpy` into the packet buffer.
- In `Complete::diff_from()`, the internal `display.new_frame()` builds a string via `str.reserve(w*h*4)` — change to write into a reusable `mutable` member buffer with exponential growth (only grows, never shrinks).

**Impact:** Eliminates 3-4 heap allocations per sent packet. ~150-200 fewer allocations/sec on a 50fps LAN session. Buffers stabilize quickly and never reallocate in steady state.

---

## 6. Hash-Based Scroll Detection

**Problem:** Scroll detection in `new_frame()` compares row 0 of the new framebuffer against every old row using full `Row::operator==()`. Worst case O(rows^2) cell comparisons.

**Design:**

- Add a `hash` field to `Row` — 64-bit hash via XXH3 from xxHash (`libxxhash-dev` or vendored header-only).
- **Full row re-hash on mutation, not incremental XOR.** XOR-based incremental hashing is unsound for this use case: XOR is order-independent (`XOR(A,B) == XOR(B,A)`) and self-canceling, so rows with transposed cells or repeated writes would produce false hash matches, causing scroll detection to skip row writes and display corrupted output. Instead, mark the row's hash as dirty on any cell mutation and recompute via `XXH3_64bits()` over the full cell array when the hash is next read. This lazy rehash approach means the cost is paid at most once per modified row per `new_frame()` call, and unmodified rows (the common case during scrolling) pay zero cost.
- In scroll detection: look up row 0's hash in a `std::unordered_map<uint64_t, int>` mapping hash -> old row index.
- On hash hit, confirm with full `operator==` (collision handling).
- Build the hash map once per `new_frame()` call from the old framebuffer's rows — O(rows) to build, O(1) per lookup.

**Complexity:** O(rows^2) worst case -> O(rows) build + O(1) per lookup. For 200-row terminal: 40,000 comparisons -> ~200.

**Impact:** Faster scroll detection, especially for large terminals. Lazy rehash means unmodified rows (majority during scroll) have zero hashing overhead.

---

## 7. Crypto Backend Upgrade

**Problem:** Custom OCB implementation wraps three backends with hand-rolled AE interface. Doesn't guarantee hardware AES-NI usage. No batch encryption for fragments.

**Design:**

- Switch to **libsodium** (`libsodium-dev`) as the sole crypto backend.
- **Single cipher: XChaCha20-Poly1305** (`crypto_aead_xchacha20poly1305_ietf`). No runtime cipher selection — this avoids the negotiation problem (both sides must agree on the cipher before decrypting, but the spec had no mechanism for this). XChaCha20-Poly1305 is fast on all hardware (no AES-NI dependency), has a 192-bit nonce (eliminates nonce collision concerns), and is libsodium's recommended AEAD construction.
- **Key size:** Expand `Base64Key` from 16 bytes (128-bit, current AES-128-OCB) to 32 bytes (256-bit) for XChaCha20-Poly1305. This changes the session key string format — the base64-encoded key passed from `mosh-server` to `mosh-client` becomes longer. Update `MOSH_KEY` environment variable handling and key parsing accordingly. This is a breaking change already bundled with the protocol version bump.
- **Nonce handling:** XChaCha20-Poly1305 uses a 24-byte nonce (vs current 8-byte `Nonce` that fits in `cc_str()`). Update the `Nonce` class to hold 24 bytes. The nonce is transmitted in the packet header, so the packet format changes accordingly (part of the protocol version bump).
- **Key material security:** Use `sodium_mlock()` on key storage buffers to prevent swap exposure. Use `sodium_memzero()` for key destruction. Replace the existing `AlignedBuffer` + `disable_dumping_core()` pattern with libsodium's secure memory APIs (`sodium_malloc()` / `sodium_free()`).
- **Batch encryption for fragments:** Encrypt the full payload once, then split into fragments with per-fragment authentication tags. Amortizes AEAD setup cost across fragments.
- **Wire compatibility:** Breaking change (different cipher, key size, nonce size). Bump protocol version alongside zstd change.
- **Remove:** Entire `ocb_internal.cc` custom OCB implementation and three-backend abstraction.

**Impact:** Simpler code (single backend, no `#if` chains), better security properties (misuse-resistant API, 192-bit nonces, secure key memory), equivalent or better performance on all hardware.

---

## 8. Reconnection Resilience

**Problem:** When the network drops for seconds+ and returns: SRTT is stale, full state diff must be recomputed and transmitted, no prioritization of getting the user back to interactive quickly.

**Design:**

- **SRTT reset on reconnection:** When no packet received for >3s and a new packet arrives, reset SRTT and RTTVAR to initial probe values (SRTT=1000ms, RTTVAR=500ms). Use higher alpha (0.5 instead of 0.125) for the first 4 samples, then revert to standard smoothing.
- **Progressive state resync:** Send state in priority order:
  1. Cursor position and current line (immediate visual feedback)
  2. Visible screen content (what the user sees)
  3. Scrollback and off-screen state (background, lower priority)
  Requires splitting `diff_from()` into regions. `Complete` already knows which rows are on-screen vs scrollback.
- **Reconnection burst mode:** Temporarily lower `SEND_INTERVAL_MIN` to 4ms for the first 500ms after reconnection, allowing rapid state convergence. Then fall back to adaptive timing from Section 2.
- **Sent state cleanup on reconnection:** After reconnection, do **not** discard entries from `sent_states` based on timestamps — this would break the ack protocol invariant (`sent_states.front()` must be the last confirmed-received state, and `process_acknowledgment_through()` walks the list linearly by sequence number). Instead, on reconnection reset `assumed_receiver_state` to `sent_states.front()` (the last known-good state) and let the normal ack processing re-advance it. This forces a full diff from the last confirmed state, which is correct behavior — we don't know what the receiver actually has after a disconnect.

**Impact:** User sees cursor and current line within 1-2 RTTs of network return. Stale SRTT doesn't cause mispacing. Overall resync completes faster with zstd and pre-allocated buffers working together.

---

## 9. Modern C++ and Remaining Cleanup

**Problem:** Older C++ idioms in the hot path leave performance on the table.

**Design:**

- **`std::string_view`** for read-only parameters in `diff_from()`, `Display::new_frame()`, and compression/encryption interfaces.
- **Keep `std::deque`** for `UserStream::actions` — `subtract()` removes from the front, which is O(1) for deque but O(n) for vector. The cache-friendliness benefit doesn't justify regressing front-pop performance, especially during paste operations with large action bursts.
- **`constexpr` timing constants** — `SEND_INTERVAL_MIN`, `ACK_DELAY`, etc. become `constexpr`.
- **`make_chaff()` stack allocation** — replace per-packet `std::string` with `std::array<char, 16>`.
- **Move semantics audit** — ensure `std::move` is explicit on all `std::string` returns that the compiler might not elide.

**Scope:** Only touch code in the hot path or in files already modified for Sections 1-8. No changes to untouched code.

**Impact:** Removes remaining unnecessary allocations and copies from steady-state hot path.

---

## New Dependencies

| Dependency | Debian Package | Purpose |
|-----------|---------------|---------|
| zstd | `libzstd-dev` | Compression (Section 4) |
| libsodium | `libsodium-dev` | Crypto (Section 7) |
| xxHash | `libxxhash-dev` | Row hashing (Section 6) |

---

## Wire Protocol Compatibility

Sections 4 (zstd) and 7 (libsodium) are breaking wire protocol changes. Both client and server must be updated together. The protocol version in the transport fragment header must be bumped (`MOSH_PROTOCOL_VERSION` in `network.h`, currently 2 -> 3). This is a single coordinated version bump, not two separate ones.

**Version mismatch handling:** When a client or server receives a packet with an unrecognized protocol version, it must print a clear error message (e.g., "mosh protocol version mismatch: expected 3, got 2. Both client and server must be updated.") and exit cleanly, rather than attempting to decrypt/decompress garbage and crashing or hanging. Add an e2e test that verifies this behavior by forcing a version mismatch.

---

## Implementation Order

Recommended build sequence (respects dependencies):

1. **Section 9** (Modern C++) — foundational cleanup, makes subsequent work cleaner
2. **Section 1** (Dirty flag) — standalone, high impact, no dependencies
3. **Section 3** (epoll) — standalone, enables accurate benchmarking of later changes
4. **Section 5** (Pre-allocated buffers) — prepares interfaces for Sections 4 and 6
5. **Section 6** (Hash-based scroll) — depends on Section 5 buffer patterns
6. **Section 2** (Adaptive timing) — standalone but benefits from Section 3 for testing
7. **Section 4** (zstd) — wire protocol change, bundle with Section 7
8. **Section 7** (Crypto) — wire protocol change, bundle with Section 4
9. **Section 8** (Reconnection) — depends on Sections 1, 2, 4, 5 being in place
