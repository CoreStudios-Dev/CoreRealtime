# Deploy: Hetzner CAX (ARM64, C++) — pago, ~€4/mês

CAX11: 2 vCPU ARM + 4 GB, IPv4 dedicado. Roda o **C++ nativo**.
Pagamento: cartão internacional (RecargaPay pré-pago com saldo
carregado via PIX funciona como Mastercard internacional).

## 1. Conta e servidor

1. Conta na Hetzner + projeto novo.
2. Add server → **CAX11** (ARM64), imagem **Ubuntu 24.04 ARM64**,
   região mais perto do BR (**Virginia, EUA** — não tem São Paulo
   na Hetzner; ping BR→Virginia ~150-180ms).
3. Firewall (console Hetzner): libera SSH `22/tcp`,
   UDP `5055-5058` + `27000-27002`, TCP `4530/4531/4533`.

## 2. Na máquina

```bash
sudo apt update && sudo apt install -y docker.io git
git clone --recurse-submodules https://github.com/clebsonisaquedeandradesilva/CoreRealtime.git
cd CoreRealtime
```

## 3. Configura

Edita `config.yml`: troca TODOS os `external_address` e `proxies`
para o **IPv4 público** do servidor.

## 4. Sobe (build nativo ARM, sem cross)

```bash
sudo docker build -t corerealtime .
sudo docker run -d --restart unless-stopped \
  -p 5055-5058:5055-5058/udp -p 27000-27002:27000-27002/udp \
  -p 4530:4530/tcp -p 4531:4531/tcp -p 4533:4533/tcp \
  -v ./config.yml:/srv/config.yml:ro \
  corerealtime
sudo docker logs -f $(sudo docker ps -ql)   # espera "listening on 6 server(s)"
```

## 5. Aponta o jogo

DNS/`hosts`: `ns.exitgames.com` e `ns.photonengine.io`
→ IPv4 público do servidor.
