# CoreRealtime

CoreRealtime is a clean-room implementation of the Photon Realtime server
(Name/Master/Game flow), targeting **only Linux ARM64
(`aarch64-unknown-linux-gnu`, Ubuntu/unknown)**.

> Not affiliated with, endorsed by, or sponsored by Exit Games GmbH.
> "Photon" is used descriptively for compatibility purposes only.

## Build

Prerequisites: CMake 3.16+, a C++23 compiler, `aarch64-linux-gnu` cross
toolchain (or a native ARM64 host), git.

```bash
cmake -B build --toolchain cmake/toolchain-aarch64-ubuntu-unknown.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This produces `build/corerealtime`.

## Configure & run

```bash
cp config.example.yml config.yml
# edit external_address entries to your server IP, open UDP
# 5055-5058 and 27000-27002, then:
./build/corerealtime config.yml
```

Point the game's Photon domains (`ns.exitgames.com`,
`ns.photonengine.io`) at your server IP.

## Layout

- `src/`, `include/` — minimal server core (no webserver, plugins, FFI,
  Tracy, WASI or other platforms; see `CMakeLists.txt`)
- `Core/` — protocol library (vendored in-tree, no submodules left)
- `cmake/toolchain-aarch64-ubuntu-unknown.cmake` — the only toolchain
- `debian/` — ARM64 packaging (`corerealtime` package)
- `config.example.yml` — annotated example configuration
