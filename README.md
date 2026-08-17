# lmap — Low-latency Multicast Audio Protocol (LMAP/1)

A self-contained C++ implementation of **LMAP/1**: one sender, N receivers,
uncompressed multichannel PCM over UDP/IPv4 multicast on a single LAN.

- **No external dependencies** (POSIX/BSD sockets, `std::thread`, `std::chrono`).
  googletest is fetched only for the test build.
- **Portable** across macOS and Linux. It deliberately avoids the Linux-only
  `recvmmsg`/`clock_nanosleep` syscalls; the wire format is 100% spec-conformant.
- Designed to be consumed as a git submodule / CMake subproject
  (`add_subdirectory(lmap)` → link `lmap::lmap`), or built standalone.

## Status — First milestone

Implemented: wire codec + packet validation (spec §4.5), `Sender`, `Receiver`
(ring-by-timestamp buffering, playout state machine), and simple **drop/insert**
clock correction (spec §7.5b).

Deferred to a later pass: PI clock-recovery loop + adaptive resampler (§7.5a),
raised-cosine concealment ramps (§7.7), redundancy (§8), discovery (§9),
REPORT feedback (§5.3).

## Build & test (standalone)

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Use as a subproject

```cmake
add_subdirectory(lmap)          # LMAP_BUILD_TESTS defaults OFF when not top-level
target_link_libraries(my_app PRIVATE lmap::lmap)
```

## API

See `include/lmap/` — `Sender.h`, `Receiver.h`, `Config.h`. The public headers are
C++17-clean so they can be included from a C++17 consumer; the library internals
build at C++20.
