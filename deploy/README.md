# VPS broker setup

Why a VPS at all: the ESP32 publishes over cellular, so it sits behind
carrier-grade NAT with no inbound reachability and no route to a private LAN
address. Subscribers are on arbitrary networks with the same problem. Neither
end can be the meeting point, so the broker has to be the one machine with a
stable public address that both sides dial *out* to.

Once it's up, any computer on any network subscribes with `mosquitto_sub`,
`listener.py`, or `web/dashboard.html` — no tunnels, no port forwarding, no
reflashing when your laptop's IP changes.

---

## Part A — provision the server (browser, ~10 min)

You need any Linux box with a public IPv4 address. The cheapest sensible
options:

| Provider | Plan | Cost |
|---|---|---|
| **Hetzner** | CX22 (2 vCPU, 4 GB, Falkenstein/Ashburn) | ~€3.79/mo |
| DigitalOcean | Basic droplet, 1 GB | $6/mo |
| Vultr | Regular, 1 GB | $5/mo |
| Oracle Cloud | Always Free (VM.Standard.A1) | Free, slower signup |

Mosquitto is tiny — the smallest instance any of them sells is far more than
enough. Pick a region near you to keep latency down; it does not affect data
usage.

Steps (Hetzner; the others differ only in menu names):

1. Create an account at <https://console.hetzner.cloud>, then **New Project**.
2. **Add Server**:
   - Location: nearest to you
   - Image: **Ubuntu 24.04**
   - Type: **CX22** (shared vCPU, x86)
   - Networking: leave **Public IPv4** enabled — this is the whole point;
     an IPv6-only server will not be reachable from some carrier networks.
   - SSH key: paste your public key (below). Password auth is offered as an
     alternative; use the key.
3. Create, and note the **public IPv4 address** it assigns.

If you don't already have an SSH key, generate one on your laptop first and
paste the `.pub` contents into the provider's SSH-key field:

```
ssh-keygen -t ed25519 -C "sdm27-broker"
cat ~/.ssh/id_ed25519.pub
```

Confirm you can get in before going further:

```
ssh root@<server-ip>
```

## Part B — set up the broker (~2 min)

From this repo on your laptop, copy the two deploy files over and run the
setup script. It installs mosquitto, applies the config, creates the account,
opens the firewall, and self-tests:

```
scp deploy/mosquitto-vps.conf deploy/setup-broker.sh root@<server-ip>:/tmp/
ssh root@<server-ip> "chmod +x /tmp/setup-broker.sh && \
    MQTT_USER=esp32 MQTT_PASS='<password from .env>' /tmp/setup-broker.sh"
```

The script is idempotent — re-run it to rotate the password or recover from a
partial failure. It refuses to finish if anonymous access still works, which
is the failure mode that would quietly leave your broker open to the internet.

**Provider firewalls:** `ufw` alone is not always enough. AWS, GCP and Oracle
put a separate security-group layer in front of the instance, and ports 1883
and 9001 must be opened there too. Hetzner and DigitalOcean have no such layer
by default, so the script's `ufw` rules are sufficient.

## Part C — verify from your laptop, before flashing anything

Prove the broker is reachable *from outside* and that auth is enforced.
Subscribe in one terminal:

```
mosquitto_sub -h <server-ip> -p 1883 -u esp32 -P '<password>' -t 'sdm27/#' -v
```

and publish from another:

```
mosquitto_pub -h <server-ip> -p 1883 -u esp32 -P '<password>' -t 'sdm27/test' -m hello
```

`hello` should appear. Then confirm anonymous access is refused:

```
mosquitto_sub -h <server-ip> -p 1883 -t 'sdm27/#'
```

This should **fail**. If it succeeds, stop — the broker is world-readable.

Do this before touching the board. If you flash first and see nothing arrive,
you won't know whether the broker or the modem is at fault, and the modem side
has far more ways to fail.

## Part D — point the firmware at it

Uncomment and fill in `MQTT_BROKER_HOST` in `.env` (git-ignored) with the
server's IP or hostname, then:

```
pio run -t upload && pio device monitor
```

Watch for, in order: modem AT response → network registration → PDP context up
→ time sync → MQTT connected → `published batch seq=0`.

Subscribers, on any network:

```
mosquitto_sub -h <server-ip> -p 1883 -u esp32 -P '<password>' -t 'sdm27/#' -v
python3 listener.py -H <server-ip> -u esp32 -P '<password>' -t 'sdm27/dd09e89e/batch' --brief
```

Or open `web/dashboard.html` in any browser and fill in the host, WS port
`9001`, topic, and the same credentials.

---

## Security note

This setup is plaintext MQTT on port 1883. The password crosses the internet in
the clear on every connect, and so does every telemetry batch — anyone
positioned on the path can read both.

That's a deliberate trade for this PoC: TLS costs a ~4–5 KB handshake on every
reconnect, which is real money on a metered SIM, and the payload is simulated
CAN data (an incrementing counter). It stops being an acceptable trade the
moment real vehicle data flows. Before that: enable TLS on port 8883 with a
Let's Encrypt certificate, and switch the firmware to
`GsmClientSecureSIM7672` (already available in the pinned TinyGSM fork — see
`src/net_task.cpp`).

Treat the current password as throwaway and don't reuse it anywhere.

For a real fleet, also give each device its own account and an ACL file, so one
leaked device credential can't read everyone's data. The single shared `esp32`
account here is a PoC shortcut.
