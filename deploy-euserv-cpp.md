# Deploy: euserv VS2-free com o servidor C++ (x86_64)

O binário `build/corerealtime-linux-x86_64` (compilado na Ubuntu 24.04)
roda direto no container — **não precisa compilar na box**.
Arquitetura tem que ser x86_64 e o SO Ubuntu 22.04/24.04 (glibc compatível).

> ATENÇÃO: o binário lê o arquivo **`config.yml` do diretório atual**
> (ignora argumentos). Rode sempre de dentro de `/opt/corerealtime`
> ou ajuste o `WorkingDirectory` do systemd.

## 1. Prepara o pacote (na tua máquina)

```powershell
Copy-Item build\corerealtime-linux-x86_64 deploy-pkg\
Copy-Item config.example.yml deploy-pkg\config.yml
```

Edita `deploy-pkg\config.yml`: troca TODOS os `external_address`
e `proxies` para o IPv6 da box **entre colchetes**:

```yaml
GameServer:
  - port: 5056
    external_address: "[2001:db8::1]:5056"
    proxies:
      - protocol: TCP
        address: "[2001:db8::1]:4531"
```

## 2. Manda pra box

```powershell
scp -r deploy-pkg root@[SEU-IPV6]:/root/
```

Na box:

```bash
sudo useradd -r -s /usr/sbin/nologin corerealtime
sudo mkdir -p /opt/corerealtime
sudo cp deploy-pkg/corerealtime-linux-x86_64 deploy-pkg/config.yml /opt/corerealtime/
sudo chown -R corerealtime:corerealtime /opt/corerealtime
sudo chmod 755 /opt/corerealtime/corerealtime-linux-x86_64
```

## 3. Firewall da box

```bash
sudo ufw allow 22/tcp
sudo ufw allow 5055:5058/udp
sudo ufw allow 27000:27002/udp
sudo ufw allow 4530,4531,4533/tcp
sudo ufw --force enable
```

## 4. Systemd

`/etc/systemd/system/corerealtime.service`:

```ini
[Unit]
Description=CoreRealtime server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=corerealtime
WorkingDirectory=/opt/corerealtime
ExecStart=/opt/corerealtime/corerealtime-linux-x86_64
Restart=on-failure
RestartSec=5
NoNewPrivileges=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now corerealtime
journalctl -u corerealtime -f   # espera "listening on 6 server(s)"
```

## 5. Jogadores sem IPv6 (Portwarp)

Mesma ideia do deploy Rust: 3 túneis UDP no Portwarp para as
portas locais 5058/5055/5056, `external_address` com os endereços
públicos, reinicia o serviço. Ver `deploy-euserv.md` do workspace
Rust para os comandos do agente `pwrp`.
