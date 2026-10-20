# WireGuard hub on the home Docker server

The home server is the hub. Every device dials *out* to it, so only one UDP port is open on
your router and the ESP32 is never reachable from the hotspot or the public internet.

| Peer | Tunnel address | Role |
|---|---|---|
| server | 10.8.0.1 | hub |
| esp32 | 10.8.0.2 | the controller (API listens here) |
| laptop | 10.8.0.3 | your admin/testing client |
| homeassistant | 10.8.0.4 | Home Assistant |

Addresses are handed out in the order of `PEERS` below. The commands in this file were written
from the container's documentation and have not been run against your server yet: if a path or
variable name differs, trust `docker logs wireguard` and the image docs over this file.

## 1. Router and DNS

1. Forward **UDP 51820** on the router to the Docker host. Nothing else.
2. Give the home connection a stable name: a DDNS hostname (DuckDNS, Cloudflare, your router's
   own service) pointing at your public IP. Use that name as `SERVERURL` below.
3. If the host has a firewall, allow it: `sudo ufw allow 51820/udp`.

## 2. docker-compose.yml

```yaml
services:
  wireguard:
    image: lscr.io/linuxserver/wireguard:latest
    container_name: wireguard
    cap_add:
      - NET_ADMIN
    environment:
      - PUID=1000
      - PGID=1000
      - TZ=<your timezone, e.g. America/New_York>
      - SERVERURL=<your DDNS hostname>
      - SERVERPORT=51820
      - PEERS=esp32,laptop,homeassistant
      - INTERNAL_SUBNET=10.8.0.0
      - ALLOWEDIPS=10.8.0.0/24      # split tunnel: clients only send tunnel traffic through it
    volumes:
      - ./config:/config
    ports:
      - 51820:51820/udp
    sysctls:
      - net.ipv4.conf.all.src_valid_mark=1
    restart: unless-stopped
```

Start it: `docker compose up -d`, then `docker compose logs -f wireguard` until it prints the peer
configs. They are written to `./config/`:

```
config/peer_esp32/            peer_esp32.conf, privatekey-peer_esp32, publickey-peer_esp32
config/peer_laptop/
config/peer_homeassistant/
config/server/                publickey-server
```

(`ls config` to confirm the layout on your image version.)

## 3. Isolate the ESP32

The ESP32 should be reachable *by* the laptop and Home Assistant but must never be able to start
connections *to* them, so a compromised device cannot pivot into your home network.

Edit the server template so the rules survive restarts: in `config/templates/server.conf`
(the `[Interface]` section), add

```
PostUp = iptables -I FORWARD 1 -i %i -s 10.8.0.2 -m conntrack --ctstate NEW -j DROP
PostDown = iptables -D FORWARD -i %i -s 10.8.0.2 -m conntrack --ctstate NEW -j DROP
```

then regenerate the server config (change `PEERS` slightly and change it back, or delete
`config/wg_confs/wg0.conf` and restart the container, depending on the image version) and
restart. Replies still flow because only NEW connections from 10.8.0.2 are dropped.

## 4. Fill in the ESP32 secrets

In the firmware's untracked `src/secrets.h`:

| Constant | Value |
|---|---|
| `WG_ENABLED` | `true` |
| `WG_LOCAL_IP` | `10.8.0.2` |
| `WG_PRIVATE_KEY` | contents of `config/peer_esp32/privatekey-peer_esp32` |
| `WG_SERVER_PUBLIC_KEY` | contents of `config/server/publickey-server` |
| `WG_ENDPOINT` | your DDNS hostname |
| `WG_PORT` | `51820` |

The private key is a secret: it must never go into git or into chat. Anyone holding it can
impersonate the ESP32 on the tunnel (they still cannot sign API commands).

## 5. Laptop and Home Assistant

Import `config/peer_laptop/peer_laptop.conf` into the WireGuard app on the laptop, and the
`homeassistant` file into the Home Assistant host's WireGuard client. Because of
`ALLOWEDIPS=10.8.0.0/24` only tunnel traffic uses the VPN.

## 6. Verify

1. `docker exec wireguard wg show` lists three peers. `esp32` shows a "latest handshake" within a
   minute or two of the ESP32 booting on the hotspot.
2. On the ESP32 console, `wg` reports `started`.
3. With the laptop tunnel up: `ping 10.8.0.2` answers.
4. Turn the hotspot off and on: the handshake time on the server resets after the ESP32
   reconnects.

## Notes about the ESP32 side

- The library (`ciniml/WireGuard-ESP32` 0.1.5) has no preshared-key option and sends a keepalive
  every 10 s by default, which keeps the carrier/hotspot NAT mapping open. That is about
  60 bytes every 10 s, roughly 15 MB per month on cellular.
- Starting the tunnel makes it the ESP32's default route, so everything the ESP32 sends goes
  through the home server after that. NTP therefore has to finish first, which the firmware
  already does (the tunnel starts only after Wi-Fi is up and the clock is synced).
- The library resolves the endpoint name once, at start, with up to 5 tries. If your DDNS address
  changes while the car is away, restart the tunnel (Wi-Fi reconnect does this automatically).
- The ESP32 needs correct time: WireGuard handshakes carry a timestamp, and the server rejects
  one that is older than the last it saw.
