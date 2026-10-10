<p align="center">
  <img src="docs/logo.svg" alt="CoreRealtime" width="560">
</p>

<p align="center">
  <strong>Servidor realtime clean-room do protocolo Photon — só Linux ARM64.</strong><br>
  Fluxo Name / Master / Game · UDP + TCP · ENet · GpBinary 1.6/1.8 · Cripto DH · SQL Lobbies
</p>

<p align="center">
  <img src="https://img.shields.io/badge/arch-aarch64-39c5cf?style=for-the-badge" alt="aarch64">
  <img src="https://img.shields.io/badge/C++-23-3fb950?style=for-the-badge" alt="C++23">
  <img src="https://img.shields.io/badge/UDP+%2B_TCP-0052cc?style=for-the-badge" alt="UDP+TCP">
  <img src="https://img.shields.io/badge/license-BSD--3--Clause-8b949e?style=for-the-badge" alt="BSD-3-Clause">
</p>

---

## ⚡ Por que CoreRealtime?

| | |
|---|---|
| ⚡ **UDP + TCP iguais** | Mesmo fluxo Photon nos dois transportes. TCP com framing de prefixo de tamanho, mesmas operações e respostas. |
| 🔒 **Cripto de verdade** | Diffie-Hellman Oakley-768 + AES-256-CBC, com handshake de init-encryption por peer. |
| 🎯 **Um alvo só** | `aarch64-unknown-linux-gnu`. Sem `#ifdef` de Windows, 3DS ou WASM. |
| 🗂️ **SQL Lobbies** | Filtros `C0..C9` com LIKE/GLOB, AND/OR/NOT e cadeia de até 3 queries. |
| 📦 **Núcleo mínimo** | Apps, lobbies, salas, peers, matchmaking. Sem webserver, plugins, FFI ou telemetria. |
| 🧩 **Core separado** | Biblioteca de protocolo (`Core/`) como submódulo próprio, versionada à parte. |

## 🚀 Quickstart

```bash
# 1. build (Ubuntu ARM64 ou cross)
cmake -B build --toolchain cmake/toolchain-aarch64-ubuntu-unknown.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 2. configure (troque pelos IPs da sua máquina)
cp config.example.yml config.yml

# 3. rode
./build/corerealtime config.yml
```

```
[ServerManager] [info] CoreRealtime ready — listening on 6 server(s)
```

1. Libere no firewall as UDP **5055–5058** e **27000–27002** (+ TCP **4530/4531/4533** se usar proxies).
2. Aponte `ns.exitgames.com` e `ns.photonengine.io` para o IP do servidor.
3. Abra o jogo — ele conecta achando que é a nuvem oficial.

## 🔌 Portas

| Servidor | UDP | TCP (proxy) | Faz o quê |
|---|---|---|---|
| NameServer | `5058` · `27000` | `4533` | Entrada, região e autenticação |
| MasterServer | `5055` · `27001` | `4530` | Lobbies e matchmaking |
| GameServer | `5056` · `27002` | `4531` | Salas e relay de eventos |

## 📁 Estrutura

- `src/`, `include/` — núcleo mínimo do servidor
- `Core/` — lib de protocolo (submódulo [Core](https://github.com/clebsonisaquedeandradesilva/Core))
- `cmake/toolchain-aarch64-ubuntu-unknown.cmake` — toolchain única
- `debian/` — pacote ARM64 (`corerealtime`)
- `config.example.yml` — configuração de exemplo comentada

## 📄 Licença

BSD-3-Clause. Projeto independente — sem afiliação com a Exit Games GmbH
("Photon" é usado só para descrever compatibilidade).
