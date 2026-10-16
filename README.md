<p align="center">
  <img src="docs/logo.svg" alt="CoreRealtime" width="560">
</p>

<p align="center">
  <strong>Clean-room Photon protocol realtime server — Linux x86_64.</strong><br>
  Name / Master / Game flow · UDP + TCP · ENet · GpBinary 1.6/1.8 · DH crypto · SQL lobbies
</p>

<p align="center">
  <img src="https://img.shields.io/badge/arch-x86_64-39c5cf?style=for-the-badge" alt="x86_64">
  <img src="https://img.shields.io/badge/C++-23-3fb950?style=for-the-badge" alt="C++23">
  <img src="https://img.shields.io/badge/UDP+%2B_TCP-0052cc?style=for-the-badge" alt="UDP+TCP">
  <img src="https://img.shields.io/badge/license-BSD--3--Clause-8b949e?style=for-the-badge" alt="BSD-3-Clause">
</p>

---

## ⚡ Why CoreRealtime?

| | |
|---|---|
| ⚡ **UDP + TCP alike** | Same Photon flow on both transports. TCP uses length-prefixed framing, same operations and responses. |
| 🔒 **Real crypto** | Oakley-768 Diffie-Hellman + AES-256-CBC, with per-peer init-encryption handshake. |
| 🎯 **One target** | `x86_64-unknown-linux-gnu`. No Windows, 3DS or WASM `#ifdef`s. |
| 🗂️ **SQL lobbies** | `C0..C9` filters with LIKE/GLOB, AND/OR/NOT and chains of up to 3 queries. |
| 📦 **Minimal core** | Apps, lobbies, rooms, peers, matchmaking. No webserver, plugins, FFI or telemetry. |
| 🧩 **Standalone Core** | Protocol library (`Core/`) as its own submodule, versioned separately. |

## 🚀 Quickstart

```bash
# 1. build (x86_64 Ubuntu)
cmake -B build --toolchain cmake/toolchain-x86_64-linux-gnu.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 2. configure (set your machine IPs)
cp config.example.yml config.yml

# 3. run
./build/corerealtime config.yml
```

Or via Docker:

```bash
docker build -t corerealtime .
docker run -d --restart unless-stopped \
  -p 5055-5058:5055-5058/udp -p 27000-27002:27000-27002/udp \
  -p 4530:4530/tcp -p 4531:4531/tcp -p 4533:4533/tcp \
  -v ./config.yml:/srv/config.yml:ro \
  corerealtime
```

```
[ServerManager] [info] CoreRealtime ready — listening on 6 server(s)
```

1. Open UDP **5055–5058** and **27000–27002** in the firewall (+ TCP **4530/4531/4533** for proxies).
2. Point `ns.exitgames.com` and `ns.photonengine.io` at your server IP.
3. Launch the game — it connects thinking it's the official cloud.

## 🔌 Ports

| Server | UDP | TCP (proxy) | Does |
|---|---|---|---|
| NameServer | `5058` · `27000` | `4533` | Entry, region and authentication |
| MasterServer | `5055` · `27001` | `4530` | Lobbies and matchmaking |
| GameServer | `5056` · `27002` | `4531` | Rooms and event relay |

## 📁 Layout

- `src/`, `include/` — minimal server core
- `Core/` — protocol library ([Core](https://github.com/clebsonisaquedeandradesilva/Core) submodule)
- `cmake/toolchain-x86_64-linux-gnu.cmake` — the single toolchain
- `debian/` — x86_64 packaging (`corerealtime`)
- `config.example.yml` — annotated example configuration

## 📄 License

BSD-3-Clause. Independent project — no affiliation with Exit Games GmbH
("Photon" is used only to describe compatibility).
