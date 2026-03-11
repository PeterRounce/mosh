# Mosh Performance & Stability Overhaul — Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Improve perceived latency and reconnection resilience through a full transport layer overhaul.

**Architecture:** Nine coordinated changes across the hot path: dirty flag caching, adaptive timing, epoll event loop, zstd compression, pre-allocated buffers, hash-based scroll detection, libsodium crypto, reconnection resilience, and modern C++ cleanup. Wire protocol version bumps from 2 to 3.

**Tech Stack:** C++17, libsodium, zstd, xxHash, epoll, protobuf (existing)

**Spec:** `docs/superpowers/specs/2026-03-11-perf-stability-overhaul-design.md`

---

## Chunk 1: Modern C++ Cleanup (Spec Section 9)

Foundational changes that make subsequent work cleaner. Touch only hot-path files that later tasks will modify.

### Task 1: constexpr Timing Constants

**Files:**
- Modify: `src/network/transportsender.h:48-54`

- [ ] **Step 1: Change timing constants to constexpr**

In `src/network/transportsender.h`, replace:
```cpp
const int SEND_INTERVAL_MIN = 20;       /* ms between frames */
const int SEND_INTERVAL_MAX = 250;      /* ms between frames */
const int ACK_INTERVAL = 3000;          /* ms between empty acks */
const int ACK_DELAY = 100;              /* ms before delayed ack */
const int SHUTDOWN_RETRIES = 16;        /* number of shutdown packets to send before giving up */
const int ACTIVE_RETRY_TIMEOUT = 10000; /* attempt to resend at frame rate */
```
with:
```cpp
constexpr int SEND_INTERVAL_MIN = 20;       /* ms between frames */
constexpr int SEND_INTERVAL_MAX = 250;      /* ms between frames */
constexpr int ACK_INTERVAL = 3000;          /* ms between empty acks */
constexpr int ACK_DELAY = 100;              /* ms before delayed ack */
constexpr int SHUTDOWN_RETRIES = 16;        /* number of shutdown packets to send before giving up */
constexpr int ACTIVE_RETRY_TIMEOUT = 10000; /* attempt to resend at frame rate */
```

- [ ] **Step 2: Build and verify**

Run: `make -j$(nproc)`
Expected: Clean build, no errors.

- [ ] **Step 3: Run tests**

Run: `make check`
Expected: All tests pass (XFAIL tests expected to fail as normal).

- [ ] **Step 4: Commit**

```bash
git add src/network/transportsender.h
git commit -m "refactor: make timing constants constexpr"
```

---

### Task 2: make_chaff() Stack Allocation

**Files:**
- Modify: `src/network/transportsender-impl.h` (make_chaff method, ~lines 295-303)

- [ ] **Step 1: Read current make_chaff() implementation**

Read `src/network/transportsender-impl.h` and locate the `make_chaff()` method. It returns `const std::string` containing random bytes up to 16 bytes.

- [ ] **Step 2: Replace std::string with std::array**

Change `make_chaff()` to return `std::array<char, 16>` and a size, or use a fixed char buffer:

```cpp
template<class MyState>
void TransportSender<MyState>::make_chaff( char* chaff_buf, size_t& chaff_len )
{
  uint32_t chaff_val = prng.uint32();
  chaff_len = chaff_val % 16;
  if ( chaff_len > 0 ) {
    prng.fill( chaff_buf, chaff_len );
  }
}
```

Update the call site in `send_in_fragments()` (~line 316) where `inst.set_chaff( make_chaff() )` is called. Replace with:
```cpp
char chaff_buf[16];
size_t chaff_len;
make_chaff( chaff_buf, chaff_len );
inst.set_chaff( std::string( chaff_buf, chaff_len ) );
```

- [ ] **Step 3: Build and test**

Run: `make -j$(nproc) && make check`
Expected: Clean build, all tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/network/transportsender-impl.h
git commit -m "perf: use stack buffer for make_chaff() instead of std::string"
```

---

### Task 2b: std::string_view for Read-Only Parameters

**Files:**
- Modify: `src/network/compressor.h:50-51` (compress_str/uncompress_str signatures)
- Modify: `src/network/compressor.cc` (implementation)
- Modify: `src/statesync/completeterminal.h:82` (apply_string signature)
- Modify: `src/statesync/completeterminal.cc` (apply_string implementation)

- [ ] **Step 1: Add string_view to compressor interface**

In `src/network/compressor.h`, change:
```cpp
std::string compress_str( const std::string& input );
std::string uncompress_str( const std::string& input );
```
to:
```cpp
std::string compress_str( std::string_view input );
std::string uncompress_str( std::string_view input );
```

Update `compressor.cc` accordingly — `string_view` provides `.data()` and `.size()` just like `std::string`, so callers don't change.

- [ ] **Step 2: Add string_view to apply_string**

In `src/statesync/completeterminal.h`, change `apply_string` to take `std::string_view`. Same for `src/statesync/user.h` if applicable.

- [ ] **Step 3: Build and test**

Run: `make -j$(nproc) && make check`
Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/network/compressor.h src/network/compressor.cc \
        src/statesync/completeterminal.h src/statesync/completeterminal.cc \
        src/statesync/user.h src/statesync/user.cc
git commit -m "refactor: use std::string_view for read-only parameters in hot path"
```

---

## Chunk 2: Dirty Flag + State Diff Optimization (Spec Section 1)

### Task 3: Add Generation Counter to Framebuffer

**Files:**
- Modify: `src/terminal/terminalframebuffer.h:362-485` (Framebuffer class)
- Modify: `src/terminal/terminalframebuffer.cc` (mutation methods)
- Test: `src/tests/framebuffer-gen.cc` (new unit test)

- [ ] **Step 1: Write failing test for framebuffer generation counter**

Create `src/tests/framebuffer-gen.cc`:
```cpp
#include "src/terminal/terminalframebuffer.h"
#include "src/util/fatal_assert.h"

int main()
{
  Terminal::Framebuffer fb( 80, 24 );

  uint64_t gen0 = fb.generation();

  /* Mutation should increment generation */
  fb.get_mutable_cell( 0, 0 )->reset( Terminal::Cell() );
  uint64_t gen1 = fb.generation();
  fatal_assert( gen1 > gen0 );

  /* No mutation should keep generation stable */
  uint64_t gen2 = fb.generation();
  fatal_assert( gen2 == gen1 );

  /* Scroll should increment generation */
  fb.scroll( 1, 1 );
  uint64_t gen3 = fb.generation();
  fatal_assert( gen3 > gen1 );

  /* Resize should increment generation */
  fb.resize( 120, 40 );
  uint64_t gen4 = fb.generation();
  fatal_assert( gen4 > gen3 );

  return 0;
}
```

- [ ] **Step 2: Add test to build system**

In `src/tests/Makefile.am`, add `framebuffer-gen` to `check_PROGRAMS` and `TESTS`:
```makefile
check_PROGRAMS = ocb-aes encrypt-decrypt base64 nonce-incr inpty is-utf8-locale framebuffer-gen
TESTS = ocb-aes encrypt-decrypt base64 nonce-incr framebuffer-gen local.test $(displaytests)
```

Add build rules:
```makefile
framebuffer_gen_SOURCES = framebuffer-gen.cc
framebuffer_gen_CPPFLAGS = -I$(srcdir)/../terminal -I$(srcdir)/../util $(PROTOBUF_CFLAGS)
framebuffer_gen_LDADD = ../terminal/libmoshterminal.a ../util/libmoshutil.a ../protobufs/libmoshprotos.a $(PROTOBUF_LIBS) $(TINFO_LIBS)
```

- [ ] **Step 3: Run test to verify it fails**

Run: `make -j$(nproc) && cd src/tests && ./framebuffer-gen`
Expected: FAIL — `generation()` method doesn't exist yet.

- [ ] **Step 4: Add generation counter to Framebuffer**

In `src/terminal/terminalframebuffer.h`, add to the Framebuffer class private section:
```cpp
uint64_t generation_;
```

Add public accessor:
```cpp
uint64_t generation() const { return generation_; }
```

Add private helper:
```cpp
void bump_generation() { ++generation_; }
```

Initialize `generation_( 0 )` in the Framebuffer constructor.

- [ ] **Step 5: Instrument mutation paths**

In `src/terminal/terminalframebuffer.cc`, call `bump_generation()` at the end of:
- `Framebuffer::scroll()` (~line 296+ area — the `insert_line`/`delete_line` methods)
- `Framebuffer::resize()` (~line 396)
- `Framebuffer::reset()` (~line 375)
- `Framebuffer::insert_cell()` (~line 365)
- `Framebuffer::delete_cell()` (~line 370)

In `src/terminal/terminalframebuffer.h`, call `bump_generation()` in:
- `Framebuffer::get_mutable_row()` (~line 425) — any caller getting a mutable row may mutate it
- `Framebuffer::get_mutable_cell()` (~line 438)

Also bump in the `Framebuffer` methods that change non-cell state (title, bell, mouse mode, cursor visibility) — these are typically set via `ds` (DrawState) which is a public member. Wrap these with generation bumps in `terminalfunctions.cc` and `terminaldispatcher.cc` where they're called.

- [ ] **Step 6: Run test to verify it passes**

Run: `make -j$(nproc) && cd src/tests && ./framebuffer-gen`
Expected: PASS

- [ ] **Step 7: Run full test suite**

Run: `make check`
Expected: All tests pass.

- [ ] **Step 8: Commit**

```bash
git add src/terminal/terminalframebuffer.h src/terminal/terminalframebuffer.cc \
        src/terminal/terminalfunctions.cc src/terminal/terminaldispatcher.cc \
        src/tests/framebuffer-gen.cc src/tests/Makefile.am
git commit -m "feat: add generation counter to Framebuffer for dirty tracking"
```

---

### Task 4: Add Diff Caching to TransportSender

**Files:**
- Modify: `src/network/transportsender.h:57-172` (TransportSender class)
- Modify: `src/network/transportsender-impl.h` (tick, attempt_prospective_resend_optimization)
- Modify: `src/statesync/completeterminal.h:45-86` (Complete class — expose generation)
- Test: existing e2e tests validate correctness

- [ ] **Step 1: Expose framebuffer generation from Complete**

In `src/statesync/completeterminal.h`, add to the Complete class:
```cpp
uint64_t get_fb_generation() const { return terminal.get_fb().generation(); }
```

- [ ] **Step 2: Add cache fields to TransportSender**

In `src/network/transportsender.h`, add private members to TransportSender:
```cpp
/* Diff caching — Section 1 of perf spec */
std::string cached_diff_;
uint64_t last_local_generation_;
uint64_t last_assumed_receiver_num_;

std::string cached_resend_diff_;
uint64_t resend_base_num_;
uint64_t resend_local_generation_;

/* Pre-allocated diff output buffers — Section 5 of perf spec */
std::string diff_buffer_;
std::string resend_diff_buffer_;
```

Initialize all to 0 in the constructor (`src/network/transportsender-impl.h` constructor, ~line 50).

- [ ] **Step 3: Add cache check to tick()**

In `src/network/transportsender-impl.h`, in `tick()` (~line 154), wrap the `diff_from()` call:
```cpp
uint64_t current_gen = current_state.get_fb_generation();
uint64_t current_assumed_num = assumed_receiver_state->num;

std::string diff;
if ( current_gen == last_local_generation_
     && current_assumed_num == last_assumed_receiver_num_
     && !cached_diff_.empty() ) {
  diff = cached_diff_;
} else {
  diff = current_state.diff_from( assumed_receiver_state->state );
  cached_diff_ = diff;
  last_local_generation_ = current_gen;
  last_assumed_receiver_num_ = current_assumed_num;
}
```

**Important:** `get_fb_generation()` only works for `Terminal::Complete` (server->client direction). `TransportSender<UserStream>` is instantiated in the client. To avoid a compile error, add a stub to `src/statesync/user.h`:
```cpp
uint64_t get_fb_generation() const { return ++user_gen_counter_; }
```
with `mutable uint64_t user_gen_counter_ = 0;` — this always returns a new value, so the cache never activates for `UserStream`, which is the desired behavior (user input diffs are cheap).

- [ ] **Step 4: Add cache check to attempt_prospective_resend_optimization()**

In the resend optimization method (~line 398+), wrap its `diff_from()` call similarly using `cached_resend_diff_`, `resend_base_num_` (tracking `sent_states.front().num`), and `resend_local_generation_`.

- [ ] **Step 5: Invalidate caches on reconnection**

Where `assumed_receiver_state` is reset (search for assignments to `assumed_receiver_state` in transportsender-impl.h), add:
```cpp
cached_diff_.clear();
cached_resend_diff_.clear();
```

- [ ] **Step 6: Build and run full test suite**

Run: `make -j$(nproc) && make check`
Expected: All tests pass. The caching is transparent — diffs are identical whether cached or recomputed.

- [ ] **Step 7: Commit**

```bash
git add src/network/transportsender.h src/network/transportsender-impl.h \
        src/statesync/completeterminal.h
git commit -m "perf: add diff caching to TransportSender with generation tracking"
```

---

## Chunk 3: epoll Event Loop (Spec Section 3)

### Task 5: Replace select() with epoll_pwait()

**Files:**
- Modify: `src/util/select.h:51-244` (Select class)
- Test: existing e2e tests validate event loop correctness

- [ ] **Step 1: Read current Select implementation**

Read `src/util/select.h` thoroughly. Note:
- `pselect()` usage at line 149
- Signal handling via `add_signal()` at lines 97-115
- `empty_sigset` for atomic signal unmasking
- `got_signal[]` array for signal delivery tracking
- `clear_fds()` / `add_fd()` / `select()` API

- [ ] **Step 2: Add epoll fd member and initialization**

In the Select class, add private members:
```cpp
#ifdef __linux__
int epoll_fd_;
std::set<int> registered_fds_;  /* track what's registered with epoll */
#endif
```

In the constructor, initialize:
```cpp
#ifdef __linux__
epoll_fd_ = epoll_create1( EPOLL_CLOEXEC );
if ( epoll_fd_ < 0 ) {
  throw std::runtime_error( "epoll_create1 failed" );
}
#endif
```

In the destructor, close:
```cpp
#ifdef __linux__
close( epoll_fd_ );
#endif
```

- [ ] **Step 3: Modify add_fd() to use epoll_ctl()**

```cpp
void add_fd( int fd ) {
#ifdef __linux__
  if ( registered_fds_.find( fd ) == registered_fds_.end() ) {
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if ( epoll_ctl( epoll_fd_, EPOLL_CTL_ADD, fd, &ev ) == 0 ) {
      registered_fds_.insert( fd );
    }
  }
  max_fd = std::max( max_fd, fd );
#else
  /* existing FD_SET code */
#endif
}
```

- [ ] **Step 4: Modify clear_fds() to track removals**

```cpp
void clear_fds() {
#ifdef __linux__
  for ( int fd : registered_fds_ ) {
    epoll_ctl( epoll_fd_, EPOLL_CTL_DEL, fd, nullptr );
  }
  registered_fds_.clear();
#endif
  /* existing FD_ZERO code */
  max_fd = 0;
}
```

Note: Since callers call `clear_fds()` then `add_fd()` every tick, we remove and re-add. This is still cheaper than select()'s fd_set rebuild because epoll_ctl is only called when the set actually changes. Optimization: compare new fd set to old and only call epoll_ctl for differences. But start simple and optimize later if needed.

- [ ] **Step 5: Replace pselect() with epoll_pwait()**

In the `select()` method (~line 118), replace the pselect call:
```cpp
#ifdef __linux__
struct epoll_event events[8];  /* mosh uses 2-3 fds */
int timeout_ms = ( timeout.tv_sec * 1000 ) + ( timeout.tv_nsec / 1000000 );
int nfds = epoll_pwait( epoll_fd_, events, 8, timeout_ms, &empty_sigset );

/* Process results */
FD_ZERO( &read_fds );
if ( nfds > 0 ) {
  for ( int i = 0; i < nfds; i++ ) {
    FD_SET( events[i].data.fd, &read_fds );
  }
}
#else
/* existing pselect code */
#endif
```

Keep the existing `read( fd )` method that checks `FD_ISSET` — it still works since we populate `read_fds` from epoll results.

- [ ] **Step 6: Build and run full test suite**

Run: `make -j$(nproc) && make check`
Expected: All tests pass. The event loop behavior is identical.

- [ ] **Step 7: Commit**

```bash
git add src/util/select.h
git commit -m "perf: replace pselect with epoll_pwait on Linux"
```

---

### Task 6: Eliminate Per-Tick Vector Allocation in fds()

**Files:**
- Modify: `src/network/network.h` (Connection class — cache fds)
- Modify: `src/frontend/stmclient.cc:463-468` (event loop fd handling)

- [ ] **Step 1: Change fds() to return const reference**

In `src/network/network.h`, in the Connection class, change:
```cpp
std::vector<int> fds( void ) const;
```
to:
```cpp
const std::vector<int>& fds( void ) const;
```

Add a private member:
```cpp
std::vector<int> cached_fds_;
```

Update the implementation to populate `cached_fds_` on socket changes and return a reference.

- [ ] **Step 2: Update callers**

In `src/frontend/stmclient.cc` (~line 464), change:
```cpp
std::vector<int> fd_list( network->fds() );
```
to:
```cpp
const std::vector<int>& fd_list( network->fds() );
```

Do the same in `src/frontend/mosh-server.cc` and `src/examples/ntester.cc`.

- [ ] **Step 3: Build and test**

Run: `make -j$(nproc) && make check`
Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/network/network.h src/network/network.cc \
        src/frontend/stmclient.cc src/frontend/mosh-server.cc \
        src/examples/ntester.cc
git commit -m "perf: return const ref from fds() to avoid per-tick vector allocation"
```

---

## Chunk 4: Pre-allocated Serialization Buffers (Spec Section 5)

### Task 7: Change diff_from() to Output Parameter

**Files:**
- Modify: `src/statesync/completeterminal.h:80` (diff_from signature)
- Modify: `src/statesync/completeterminal.cc:69-94` (diff_from implementation)
- Modify: `src/statesync/user.h:88` (UserStream::diff_from signature)
- Modify: `src/statesync/user.cc` (UserStream::diff_from implementation)
- Modify: `src/network/transportsender-impl.h` (all diff_from call sites)

- [ ] **Step 1: Change Complete::diff_from() signature**

In `src/statesync/completeterminal.h`:
```cpp
void diff_from( const Complete& existing, std::string* output ) const;
```

Add `mutable` to the Display member used internally for frame rendering, and add a mutable frame buffer:
```cpp
mutable Terminal::Display display;
mutable std::string frame_buffer_;
```

- [ ] **Step 2: Update Complete::diff_from() implementation**

In `src/statesync/completeterminal.cc`, change the implementation to write into `*output` instead of returning a string. Use `frame_buffer_` for the internal `new_frame()` call.

- [ ] **Step 3: Change UserStream::diff_from() similarly**

Update `src/statesync/user.h` and its implementation to match the new signature: `void diff_from( const UserStream& existing, std::string* output ) const;`

- [ ] **Step 4: Update all callers in transportsender-impl.h**

Update `tick()` (~line 154) and `attempt_prospective_resend_optimization()` (~line 404) to pass buffer pointers. Use the `diff_buffer_` and `resend_diff_buffer_` from Task 4.

- [ ] **Step 5: Build and test**

Run: `make -j$(nproc) && make check`
Expected: All tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/statesync/completeterminal.h src/statesync/completeterminal.cc \
        src/statesync/user.h src/statesync/user.cc \
        src/network/transportsender-impl.h
git commit -m "perf: change diff_from() to output parameter to reuse buffers"
```

---

### Task 8: Pre-allocated Serialization in TransportFragment

**Files:**
- Modify: `src/network/transportfragment.h:45-69`
- Modify: `src/network/transportfragment.cc:44-74`

- [ ] **Step 1: Replace network_order_string() with memcpy**

In `src/network/transportfragment.cc`, replace `network_order_string()` (lines 44-54) which creates a `std::string` from uint16_t/uint64_t, with a direct `memcpy` into a pre-sized output buffer.

- [ ] **Step 2: Switch SerializeAsString to SerializeToArray**

Where `inst.SerializeAsString()` is called, replace with:
```cpp
size_t size = inst.ByteSizeLong();
serialize_buffer_.resize( size );
inst.SerializeToArray( serialize_buffer_.data(), size );
```

Add `std::vector<uint8_t> serialize_buffer_` as a member of the appropriate class (Fragmenter or the caller in transportsender-impl.h).

- [ ] **Step 3: Build and test**

Run: `make -j$(nproc) && make check`
Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/network/transportfragment.h src/network/transportfragment.cc \
        src/network/transportsender-impl.h
git commit -m "perf: pre-allocate serialization buffers, replace network_order_string with memcpy"
```

---

## Chunk 5: Hash-Based Scroll Detection (Spec Section 6)

### Task 9: Add Row Hashing with XXH3

**Files:**
- Modify: `configure.ac` (add xxhash dependency check)
- Modify: `src/terminal/Makefile.am` (link xxhash)
- Modify: `src/terminal/terminalframebuffer.h:213-240` (Row class — add hash)
- Modify: `src/terminal/terminalframebuffer.cc` (Row mutation — dirty hash)
- Test: `src/tests/row-hash.cc` (new unit test)

- [ ] **Step 1: Add xxhash to configure.ac**

Add a pkg-config check for xxhash:
```m4
PKG_CHECK_MODULES([XXHASH], [libxxhash])
```

Update `src/terminal/Makefile.am` to include `$(XXHASH_CFLAGS)` and `$(XXHASH_LIBS)`.

- [ ] **Step 2: Write failing test**

Create `src/tests/row-hash.cc`:
```cpp
#include "src/terminal/terminalframebuffer.h"
#include "src/util/fatal_assert.h"

int main()
{
  Terminal::Row row1( 80, 0 );
  Terminal::Row row2( 80, 0 );

  /* Identical rows should have same hash */
  fatal_assert( row1.hash() == row2.hash() );

  /* Modified row should have different hash */
  row1.cells[0].reset( Terminal::Cell( 'A' ) );
  fatal_assert( row1.hash() != row2.hash() );

  /* Same content should have same hash regardless of mutation history */
  row2.cells[0].reset( Terminal::Cell( 'A' ) );
  fatal_assert( row1.hash() == row2.hash() );

  return 0;
}
```

Add to `src/tests/Makefile.am`:
```makefile
row_hash_SOURCES = row-hash.cc
row_hash_CPPFLAGS = -I$(srcdir)/../terminal -I$(srcdir)/../util $(PROTOBUF_CFLAGS) $(XXHASH_CFLAGS)
row_hash_LDADD = ../terminal/libmoshterminal.a ../util/libmoshutil.a ../protobufs/libmoshprotos.a $(PROTOBUF_LIBS) $(TINFO_LIBS) $(XXHASH_LIBS)
```

- [ ] **Step 3: Run test to verify it fails**

Run: `./autogen.sh && ./configure && make -j$(nproc) && cd src/tests && ./row-hash`
Expected: FAIL — `hash()` method doesn't exist.

- [ ] **Step 4: Add lazy hash to Row class**

In `src/terminal/terminalframebuffer.h`, add to Row:
```cpp
private:
  mutable uint64_t hash_;
  mutable bool hash_dirty_;

public:
  uint64_t hash() const;
  void invalidate_hash() { hash_dirty_ = true; }
```

In `src/terminal/terminalframebuffer.cc`, implement:
```cpp
uint64_t Row::hash() const
{
  if ( hash_dirty_ ) {
    /* Cannot hash raw Cell bytes due to struct padding (uninitialized padding
       bytes would cause logically-equal Cells to hash differently). Instead,
       hash the semantic content of each cell. */
    XXH3_state_t state;
    XXH3_64bits_reset( &state );
    for ( const auto& cell : cells ) {
      XXH3_64bits_update( &state, cell.contents, sizeof( cell.contents ) );
      /* Include renditions and flags that affect equality */
      uint32_t flags = ( cell.wide ? 1 : 0 ) | ( cell.fallback ? 2 : 0 ) | ( cell.wrap ? 4 : 0 );
      XXH3_64bits_update( &state, &flags, sizeof( flags ) );
      XXH3_64bits_update( &state, &cell.renditions, sizeof( cell.renditions ) );
    }
    hash_ = XXH3_64bits_digest( &state );
    hash_dirty_ = false;
  }
  return hash_;
}
```

Note: This hashes semantic cell content rather than raw bytes, avoiding struct padding issues. The `Cell` struct may have padding between fields that is uninitialized, which would cause logically-equal cells to produce different hashes.

Initialize `hash_dirty_( true )` in Row constructors. Call `invalidate_hash()` in Row mutation methods (reset, cell modifications).

- [ ] **Step 5: Run test to verify it passes**

Run: `make -j$(nproc) && cd src/tests && ./row-hash`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add configure.ac src/terminal/Makefile.am \
        src/terminal/terminalframebuffer.h src/terminal/terminalframebuffer.cc \
        src/tests/row-hash.cc src/tests/Makefile.am
git commit -m "feat: add lazy XXH3 row hashing for scroll detection"
```

---

### Task 10: Use Hash Map in Scroll Detection

**Files:**
- Modify: `src/terminal/terminaldisplay.cc:165-193` (scroll detection loop)

- [ ] **Step 1: Replace O(n^2) loop with hash map lookup**

In `src/terminal/terminaldisplay.cc`, in the scroll detection section of `new_frame()` (~lines 165-193):

Build a hash map from old rows:
```cpp
std::unordered_map<uint64_t, int> old_row_hashes;
for ( int row = 0; row < (int)rows.size(); row++ ) {
  old_row_hashes[rows.at( row )->hash()] = row;
}
```

Then replace the inner loop that compares row 0 against every old row with a hash lookup:
```cpp
const Row* new_row = f.get_row( 0 );
uint64_t new_hash = new_row->hash();
auto it = old_row_hashes.find( new_hash );
if ( it != old_row_hashes.end() ) {
  int candidate_row = it->second;
  const Row* old_row = &*rows.at( candidate_row );
  if ( *new_row == *old_row ) {
    /* Found matching row — check for contiguous scroll */
    // ... existing scroll validation logic
  }
}
```

- [ ] **Step 2: Build and test**

Run: `make -j$(nproc) && make check`
Expected: All tests pass, especially `emulation-scroll.test` and `emulation-multiline-scroll.test`.

- [ ] **Step 3: Commit**

```bash
git add src/terminal/terminaldisplay.cc
git commit -m "perf: use hash map for O(rows) scroll detection instead of O(rows^2)"
```

---

## Chunk 6: Adaptive Send Timing (Spec Section 2)

### Task 11: Make SEND_INTERVAL_MIN Adaptive

**Files:**
- Modify: `src/network/transportsender.h` (add adaptive min calculation)
- Modify: `src/network/transportsender-impl.h:61-71` (send_interval)

- [ ] **Step 1: Add adaptive_send_interval_min()**

In `src/network/transportsender-impl.h`, add a helper method:
```cpp
template<class MyState>
int TransportSender<MyState>::adaptive_send_interval_min( void ) const
{
  double srtt = connection->get_SRTT();
  if ( srtt < 10.0 ) {
    return 8;   /* LAN: ~125fps */
  } else if ( srtt < 50.0 ) {
    return 15;  /* Regional: ~67fps */
  } else {
    return 20;  /* WAN: ~50fps */
  }
}
```

- [ ] **Step 2: Update send_interval() to use adaptive min**

In `send_interval()` (~line 61), replace `SEND_INTERVAL_MIN` with `adaptive_send_interval_min()`:
```cpp
unsigned int TransportSender<MyState>::send_interval( void ) const
{
  int SEND_INTERVAL = lrint( ceil( connection->get_SRTT() / 2.0 ) );
  int min_interval = adaptive_send_interval_min();
  if ( SEND_INTERVAL < min_interval ) {
    SEND_INTERVAL = min_interval;
  } else if ( SEND_INTERVAL > SEND_INTERVAL_MAX ) {
    SEND_INTERVAL = SEND_INTERVAL_MAX;
  }
  return SEND_INTERVAL;
}
```

- [ ] **Step 3: Reduce ACK_DELAY**

In `src/network/transportsender.h`, change:
```cpp
constexpr int ACK_DELAY = 30;  /* ms before delayed ack (was 100) */
```

- [ ] **Step 4: Build and test**

Run: `make -j$(nproc) && make check`
Expected: All tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/network/transportsender.h src/network/transportsender-impl.h
git commit -m "perf: adaptive SEND_INTERVAL_MIN based on SRTT, reduce ACK_DELAY to 30ms"
```

---

### Task 12: Add Burst Mode Override

**Files:**
- Modify: `src/network/transportsender.h` (add burst_until_ field)
- Modify: `src/network/transportsender-impl.h` (send_interval, burst trigger)

- [ ] **Step 1: Add burst mode fields**

In `src/network/transportsender.h`, add private members:
```cpp
uint64_t burst_until_;         /* timestamp (ms) when burst mode ends, 0 = inactive */
static constexpr int BURST_INTERVAL = 4;    /* ms during burst */
static constexpr int BURST_DURATION = 500;  /* ms of burst after reconnection */
```

Initialize `burst_until_( 0 )` in constructor.

- [ ] **Step 2: Integrate burst into send_interval()**

In `send_interval()`:
```cpp
if ( burst_until_ > 0 && Network::timestamp() < burst_until_ ) {
  return BURST_INTERVAL;
}
/* ... existing adaptive logic ... */
```

- [ ] **Step 3: Trigger burst on reconnection detection**

In `tick()` or where the transport detects reconnection (e.g., receiving a packet after a long gap), set:
```cpp
burst_until_ = Network::timestamp() + BURST_DURATION;
```

This will be fully wired in Task 17 (Reconnection Resilience). For now, add the mechanism.

- [ ] **Step 4: Build and test**

Run: `make -j$(nproc) && make check`
Expected: All tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/network/transportsender.h src/network/transportsender-impl.h
git commit -m "feat: add burst mode override for reconnection (mechanism only)"
```

---

## Chunk 7: zstd Compression (Spec Section 4)

### Task 13: Replace zlib with zstd

**Files:**
- Modify: `configure.ac` (replace zlib check with zstd)
- Modify: `src/network/Makefile.am` (link zstd)
- Modify: `src/network/compressor.h` (zstd contexts)
- Modify: `src/network/compressor.cc` (zstd implementation)
- Test: `src/tests/compressor.cc` (new unit test)

- [ ] **Step 1: Write failing test**

Create `src/tests/compressor.cc`:
```cpp
#include "src/network/compressor.h"
#include "src/util/fatal_assert.h"
#include <string>

int main()
{
  Network::Compressor& c = Network::get_compressor();

  /* Round-trip test */
  std::string original = "Hello, mosh! This is a test of zstd compression.";
  std::string compressed = c.compress_str( original );
  std::string decompressed = c.uncompress_str( compressed );
  fatal_assert( decompressed == original );

  /* Empty string */
  std::string empty_c = c.compress_str( "" );
  std::string empty_d = c.uncompress_str( empty_c );
  fatal_assert( empty_d == "" );

  /* Large payload */
  std::string large( 100000, 'x' );
  std::string large_c = c.compress_str( large );
  std::string large_d = c.uncompress_str( large_c );
  fatal_assert( large_d == large );

  return 0;
}
```

Add to `src/tests/Makefile.am`:
```makefile
compressor_SOURCES = compressor.cc
compressor_CPPFLAGS = -I$(srcdir)/../network -I$(srcdir)/../util $(ZSTD_CFLAGS)
compressor_LDADD = ../network/libmoshnetwork.a ../util/libmoshutil.a $(ZSTD_LIBS)
```

- [ ] **Step 2: Run test to verify it fails**

Run: Build — test will fail once we change the implementation (or won't link before configure changes).

- [ ] **Step 3: Update configure.ac**

Replace zlib check with zstd:
```m4
PKG_CHECK_MODULES([ZSTD], [libzstd])
```

Remove `AC_CHECK_LIB([z], [compress])` or equivalent zlib checks.

- [ ] **Step 4: Rewrite compressor.h**

```cpp
#ifndef COMPRESSOR_H
#define COMPRESSOR_H

#include <string>
#include <vector>
#include <zstd.h>

namespace Network {
class Compressor
{
private:
  ZSTD_CCtx* cctx_;
  ZSTD_DCtx* dctx_;
  std::vector<char> compress_buf_;
  std::vector<char> decompress_buf_;
  static const size_t INITIAL_BUF_SIZE = 2048 * 2048;

public:
  Compressor();
  ~Compressor();

  std::string compress_str( const std::string& input );
  std::string uncompress_str( const std::string& input );

  Compressor( const Compressor& ) = delete;
  Compressor& operator=( const Compressor& ) = delete;
};

Compressor& get_compressor( void );
}

#endif
```

- [ ] **Step 5: Rewrite compressor.cc**

```cpp
#include "compressor.h"
#include "src/util/fatal_assert.h"

using namespace Network;

Compressor::Compressor()
  : cctx_( ZSTD_createCCtx() ),
    dctx_( ZSTD_createDCtx() ),
    compress_buf_( INITIAL_BUF_SIZE ),
    decompress_buf_( INITIAL_BUF_SIZE )
{
  fatal_assert( cctx_ != nullptr );
  fatal_assert( dctx_ != nullptr );
}

Compressor::~Compressor()
{
  ZSTD_freeCCtx( cctx_ );
  ZSTD_freeDCtx( dctx_ );
}

std::string Compressor::compress_str( const std::string& input )
{
  size_t bound = ZSTD_compressBound( input.size() );
  if ( bound > compress_buf_.size() ) {
    compress_buf_.resize( bound );
  }
  size_t result = ZSTD_compressCCtx( cctx_, compress_buf_.data(), compress_buf_.size(),
                                      input.data(), input.size(), 1 /* level */ );
  fatal_assert( !ZSTD_isError( result ) );
  return std::string( compress_buf_.data(), result );
}

std::string Compressor::uncompress_str( const std::string& input )
{
  unsigned long long decompressed_size = ZSTD_getFrameContentSize( input.data(), input.size() );
  if ( decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN || decompressed_size == ZSTD_CONTENTSIZE_ERROR ) {
    decompressed_size = INITIAL_BUF_SIZE;
  }
  if ( decompressed_size > decompress_buf_.size() ) {
    decompress_buf_.resize( decompressed_size );
  }
  size_t result = ZSTD_decompressDCtx( dctx_, decompress_buf_.data(), decompress_buf_.size(),
                                         input.data(), input.size() );
  fatal_assert( !ZSTD_isError( result ) );
  return std::string( decompress_buf_.data(), result );
}

Compressor& Network::get_compressor( void )
{
  static Compressor the_compressor;
  return the_compressor;
}
```

- [ ] **Step 6: Update Makefile.am**

In `src/network/Makefile.am`, add `$(ZSTD_CFLAGS)` to `AM_CXXFLAGS` and `$(ZSTD_LIBS)` to library flags. Remove zlib references.

- [ ] **Step 7: Rebuild and test**

Run: `./autogen.sh && ./configure && make -j$(nproc) && make check`
Expected: All tests pass, including the new compressor test.

- [ ] **Step 8: Commit**

```bash
git add configure.ac src/network/compressor.h src/network/compressor.cc \
        src/network/Makefile.am src/tests/compressor.cc src/tests/Makefile.am
git commit -m "feat: replace zlib with zstd for compression (persistent context, level 1)"
```

---

## Chunk 8: Crypto Backend — libsodium (Spec Section 7)

### Task 14: Add libsodium Dependency and New Key/Nonce Classes

**Files:**
- Modify: `configure.ac` (add libsodium check, remove OpenSSL/Nettle crypto checks)
- Modify: `src/crypto/Makefile.am` (link libsodium, remove ocb_internal.cc)
- Modify: `src/crypto/crypto.h:88-116` (Base64Key to 32 bytes, Nonce to 24 bytes)
- Modify: `src/crypto/crypto.cc` (update key/nonce implementations)
- Test: `src/tests/encrypt-decrypt.cc` (update for new crypto)

- [ ] **Step 1: Update configure.ac for libsodium**

Add:
```m4
PKG_CHECK_MODULES([SODIUM], [libsodium])
```

Remove the OpenSSL/Nettle/Apple CommonCrypto crypto backend selection logic.

- [ ] **Step 2: Expand Base64Key to 32 bytes**

In `src/crypto/crypto.h`, change:
```cpp
class Base64Key
{
private:
  unsigned char key[32];  /* was 16 — now 256-bit for XChaCha20-Poly1305 */
```

Update the constructor, `printable_key()`, and parsing to handle the longer base64 string.

- [ ] **Step 3: Expand Nonce to 24 bytes**

```cpp
class Nonce
{
public:
  static const int NONCE_LEN = 24;  /* was 12 — XChaCha20-Poly1305 */

private:
  char bytes[NONCE_LEN];
```

Update `cc_str()` to return the full 24 bytes (or adjust the packet format to transmit the full nonce). Update `val()` and the constructor.

- [ ] **Step 4: Build test (will fail — Session not updated yet)**

Run: `make -j$(nproc)`
Expected: Build errors in Session class — that's Task 15.

- [ ] **Step 5: Commit key/nonce changes**

```bash
git add configure.ac src/crypto/crypto.h src/crypto/crypto.cc src/crypto/Makefile.am
git commit -m "feat: expand Base64Key to 32 bytes and Nonce to 24 bytes for libsodium"
```

---

### Task 15: Rewrite Session with libsodium XChaCha20-Poly1305

**Files:**
- Modify: `src/crypto/crypto.h:131-157` (Session class)
- Modify: `src/crypto/crypto.cc` (Session implementation)
- Delete: `src/crypto/ocb_internal.cc`
- Delete: `src/crypto/ae.h` (OCB interface)
- Modify: `src/crypto/Makefile.am`
- Test: update `src/tests/ocb-aes.cc` → `src/tests/crypto-test.cc`

- [ ] **Step 1: Rewrite Session class**

In `src/crypto/crypto.h`, replace Session:
```cpp
class Session
{
private:
  unsigned char key[crypto_aead_xchacha20poly1305_ietf_KEYBYTES];
  uint64_t blocks_encrypted;

public:
  static const int RECEIVE_MTU = 2048;
  static const int ADDED_BYTES = crypto_aead_xchacha20poly1305_ietf_ABYTES;

  Session( Base64Key s_key );
  ~Session();

  const std::string encrypt( const Message& plaintext );
  const Message decrypt( const char* str, size_t len );
  const Message decrypt( const std::string& ciphertext ) { return decrypt( ciphertext.data(), ciphertext.size() ); }

  Session( const Session& );
  Session& operator=( const Session& );
};
```

- [ ] **Step 2: Implement with libsodium**

In `src/crypto/crypto.cc`:
```cpp
#include <sodium.h>

Session::Session( Base64Key s_key ) : blocks_encrypted( 0 )
{
  if ( sodium_init() < 0 ) {
    throw CryptoException( "sodium_init failed", true );
  }
  memcpy( key, s_key.data(), sizeof( key ) );
  sodium_mlock( key, sizeof( key ) );
}

Session::~Session()
{
  sodium_memzero( key, sizeof( key ) );
  sodium_munlock( key, sizeof( key ) );
}

const std::string Session::encrypt( const Message& plaintext )
{
  const size_t pt_len = plaintext.text.size();
  const size_t ct_len = pt_len + crypto_aead_xchacha20poly1305_ietf_ABYTES;
  std::string ciphertext( ct_len, '\0' );
  unsigned long long actual_ct_len;

  int ret = crypto_aead_xchacha20poly1305_ietf_encrypt(
    reinterpret_cast<unsigned char*>( &ciphertext[0] ), &actual_ct_len,
    reinterpret_cast<const unsigned char*>( plaintext.text.data() ), pt_len,
    nullptr, 0,  /* no additional data */
    nullptr,     /* nsec unused */
    reinterpret_cast<const unsigned char*>( plaintext.nonce.data() ),
    key );

  fatal_assert( ret == 0 );
  ciphertext.resize( actual_ct_len );
  blocks_encrypted++;
  return ciphertext;
}

const Message Session::decrypt( const char* str, size_t len )
{
  if ( len < Nonce::NONCE_LEN + ADDED_BYTES ) {
    throw CryptoException( "Ciphertext too short" );
  }

  const char* nonce_data = str;
  const char* ct_data = str + Nonce::NONCE_LEN;
  const size_t ct_len = len - Nonce::NONCE_LEN;

  std::string plaintext( ct_len - ADDED_BYTES, '\0' );
  unsigned long long actual_pt_len;

  int ret = crypto_aead_xchacha20poly1305_ietf_decrypt(
    reinterpret_cast<unsigned char*>( &plaintext[0] ), &actual_pt_len,
    nullptr,  /* nsec unused */
    reinterpret_cast<const unsigned char*>( ct_data ), ct_len,
    nullptr, 0,  /* no additional data */
    reinterpret_cast<const unsigned char*>( nonce_data ),
    key );

  if ( ret != 0 ) {
    throw CryptoException( "Packet failed integrity check" );
  }

  plaintext.resize( actual_pt_len );
  return Message( Nonce( nonce_data, Nonce::NONCE_LEN ), plaintext );
}
```

- [ ] **Step 3: Remove OCB files**

Delete `src/crypto/ocb_internal.cc` and `src/crypto/ae.h`. Update `src/crypto/Makefile.am` to remove OCB sources and the `USE_AES_OCB_FROM_OPENSSL` conditional. Add `$(SODIUM_CFLAGS)` and `$(SODIUM_LIBS)`.

- [ ] **Step 4: Update crypto test**

Rename/rewrite `src/tests/ocb-aes.cc` to `src/tests/crypto-test.cc`. Test encrypt/decrypt round-trip with the new XChaCha20-Poly1305. Update `src/tests/Makefile.am` accordingly.

- [ ] **Step 5: Build and test**

Run: `./autogen.sh && ./configure && make -j$(nproc) && make check`
Expected: All crypto tests pass. e2e tests pass.

- [ ] **Step 6: Commit**

```bash
git add -A src/crypto/ src/tests/crypto-test.cc src/tests/Makefile.am configure.ac
git commit -m "feat: replace OCB-AES with libsodium XChaCha20-Poly1305"
```

---

### Task 16: Bump Protocol Version and Add Mismatch Handling

**Files:**
- Modify: `src/network/network.h:53` (bump version)
- Modify: `src/network/transportfragment.cc` (version check on receive)
- Test: `src/tests/version-mismatch.test` (new e2e test)

- [ ] **Step 1: Bump MOSH_PROTOCOL_VERSION**

In `src/network/network.h`:
```cpp
static const unsigned int MOSH_PROTOCOL_VERSION = 3; /* bumped for zstd + libsodium */
```

- [ ] **Step 2: Improve existing version check error message**

The version check already exists in `src/network/networktransport-impl.h` (~line 76), where the assembled `Instruction` protobuf is checked after fragment reassembly. The `protocol_version` field lives in the `Instruction` protobuf, **not** in the raw fragment header — so the check belongs here, not in `transportfragment.cc`. Update the existing check to emit a clearer error message:
```cpp
if ( inst.protocol_version() != MOSH_PROTOCOL_VERSION ) {
  throw NetworkException( "mosh protocol version mismatch: expected "
    + std::to_string( MOSH_PROTOCOL_VERSION )
    + ", got " + std::to_string( inst.protocol_version() )
    + ". Both client and server must be updated.", 0 );
}
```
Note: `NetworkException` takes `(std::string, int)` where the second arg is errno — pass `0` (no errno), not a boolean.

- [ ] **Step 3: Create version mismatch e2e test**

Create `src/tests/version-mismatch.test` as a bash script using the e2e-test framework. The test should:
- Start a mosh session
- Verify the connection works
- Then simulate or force a version mismatch (e.g., by patching the version in a sent packet or by running a mismatched binary)
- Verify the error message contains "protocol version mismatch"
- Verify clean exit (not a crash or hang)

Add to `displaytests` in `src/tests/Makefile.am`.

- [ ] **Step 4: Build and test**

Run: `make -j$(nproc) && make check`
Expected: All tests pass (same-version communication works, new test validates mismatch handling).

- [ ] **Step 5: Commit**

```bash
git add src/network/network.h src/network/networktransport-impl.h \
        src/tests/version-mismatch.test src/tests/Makefile.am
git commit -m "feat: bump protocol version to 3, add version mismatch error handling and test"
```

---

## Chunk 9: Reconnection Resilience (Spec Section 8)

### Task 17: SRTT Reset on Reconnection

**Files:**
- Modify: `src/network/network.h` (add reconnection detection)
- Modify: `src/network/network.cc` (SRTT reset logic)
- Modify: `src/network/transportsender-impl.h` (trigger burst mode)

- [ ] **Step 1: Add reconnection detection to Connection**

In `src/network/network.h`, add to Connection:
```cpp
uint64_t last_recv_timestamp_;     /* timestamp of last received packet */
int reconnection_samples_;         /* count of RTT samples since reconnection */
static constexpr uint64_t RECONNECTION_GAP_MS = 3000;
static constexpr double RECONNECTION_ALPHA = 0.5;
static constexpr int RECONNECTION_FAST_SAMPLES = 4;
```

- [ ] **Step 2: Implement SRTT reset logic**

In `src/network/network.cc`, where RTT samples are processed, add:
```cpp
uint64_t now = timestamp();
if ( last_recv_timestamp_ > 0
     && ( now - last_recv_timestamp_ ) > RECONNECTION_GAP_MS ) {
  /* Connection resumed after gap — reset SRTT */
  SRTT = 1000;
  RTTVAR = 500;
  reconnection_samples_ = 0;
}
last_recv_timestamp_ = now;

/* Use higher alpha for first few samples after reconnection */
double alpha = ( reconnection_samples_ < RECONNECTION_FAST_SAMPLES )
  ? RECONNECTION_ALPHA : 0.125;
reconnection_samples_++;
```

- [ ] **Step 3: Wire burst mode trigger**

In `src/network/transportsender-impl.h`, where incoming packets are processed (after a gap detection), set:
```cpp
burst_until_ = Network::timestamp() + BURST_DURATION;
```

Also reset assumed_receiver_state to the last known-good state:
```cpp
assumed_receiver_state = sent_states.begin();
cached_diff_.clear();
cached_resend_diff_.clear();
```

**Important (from spec):** Do NOT discard entries from `sent_states` based on timestamps. The ack protocol invariant requires `sent_states.front()` to be the last confirmed-received state, and `process_acknowledgment_through()` walks the list linearly by sequence number. Audit `rationalize_states()` to confirm it doesn't prune based on time — it shouldn't, but verify during implementation.

- [ ] **Step 4: Build and test**

Run: `make -j$(nproc) && make check`
Expected: All tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/network/network.h src/network/network.cc \
        src/network/transportsender-impl.h
git commit -m "feat: SRTT reset and burst mode on reconnection after >3s gap"
```

---

### Task 18: Progressive State Resync

**Files:**
- Modify: `src/statesync/completeterminal.h` (add priority diff method)
- Modify: `src/statesync/completeterminal.cc` (implement priority diff)
- Modify: `src/network/transportsender-impl.h` (use priority diff after reconnection)

- [ ] **Step 1: Add priority diff method**

In `src/statesync/completeterminal.h`:
```cpp
void diff_from_priority( const Complete& existing, std::string* output,
                          int priority_level ) const;
```

Priority levels:
- 0 = cursor position + current line only
- 1 = visible screen
- 2 = full state (equivalent to regular diff_from)

- [ ] **Step 2: Implement priority diff**

In `src/statesync/completeterminal.cc`, implement `diff_from_priority()`:
- Level 0: Only diff the cursor row and cursor position
- Level 1: Diff all visible rows (using `get_fb().ds.get_height()` to determine visible area)
- Level 2: Call regular `diff_from()`

The key is to generate a partial `HostMessage` protobuf that only includes the prioritized rows.

- [ ] **Step 3: Use priority diff in TransportSender after reconnection**

In `src/network/transportsender-impl.h`, after reconnection detection, use a priority counter:
```cpp
if ( reconnecting_ ) {
  current_state.diff_from_priority( assumed_receiver_state->state,
                                     &diff_buffer_, reconnection_priority_ );
  if ( reconnection_priority_ < 2 ) {
    reconnection_priority_++;
  } else {
    reconnecting_ = false;
  }
}
```

- [ ] **Step 4: Build and test**

Run: `make -j$(nproc) && make check`
Expected: All tests pass. The `server-network-timeout.test` e2e test exercises reconnection.

- [ ] **Step 5: Commit**

```bash
git add src/statesync/completeterminal.h src/statesync/completeterminal.cc \
        src/network/transportsender-impl.h
git commit -m "feat: progressive state resync — cursor first, then screen, then full"
```

---

## Chunk 10: Integration Testing and Final Validation

### Task 19: Full Integration Test

**Files:**
- All modified files from Tasks 1-18

- [ ] **Step 1: Clean build from scratch**

```bash
make distclean
./autogen.sh
./configure --enable-compile-warnings=error
make -j$(nproc)
```
Expected: Clean build with no warnings.

- [ ] **Step 2: Run full test suite**

```bash
make check
```
Expected: All tests pass (except XFAIL: `e2e-failure.test`, `emulation-attributes-256color8.test`).

- [ ] **Step 3: Run static analysis**

```bash
make cppcheck
```
Expected: No new warnings.

- [ ] **Step 4: Manual smoke test**

Start a local mosh session and verify:
- Typing is responsive
- Scrolling works (run `ls -la /usr/bin`)
- Window resize works
- Ctrl-C works

- [ ] **Step 5: Commit any final fixes**

If any issues found, fix and commit individually with descriptive messages.

---

### Task 20: Dependency Documentation

**Files:**
- Modify: `CLAUDE.md` (update build dependencies)

- [ ] **Step 1: Update CLAUDE.md install dependencies**

Add `libzstd-dev`, `libsodium-dev`, and `libxxhash-dev` to the apt install line:
```bash
sudo apt install -y build-essential protobuf-compiler libprotobuf-dev pkg-config \
    libutempter-dev zlib1g-dev libncurses5-dev libssl-dev bash-completion tmux less \
    libzstd-dev libsodium-dev libxxhash-dev
```

Remove `libssl-dev` if OpenSSL is no longer needed (depends on whether it's used elsewhere).

- [ ] **Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: update build dependencies for zstd, libsodium, xxhash"
```
