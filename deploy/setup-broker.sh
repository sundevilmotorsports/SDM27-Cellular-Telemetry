#!/usr/bin/env bash
#
# Server-side broker setup for the SDM27 telemetry PoC. Run this ON the VPS,
# not on your laptop. Idempotent -- safe to re-run after a failed attempt or to
# rotate the password.
#
# Usage:
#   sudo MQTT_USER=esp32 MQTT_PASS='<password>' ./setup-broker.sh
#
# Reads the password from the environment rather than an argument so it stays
# out of your shell history. It is still briefly visible in `ps` while
# mosquitto_passwd runs -- fine on a single-user box, worth knowing if the
# server is shared.

set -euo pipefail

CONF_SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/mosquitto-vps.conf"
CONF_DEST="/etc/mosquitto/mosquitto.conf"
PASSWD_FILE="/etc/mosquitto/passwd"

die() { echo "ERROR: $*" >&2; exit 1; }

[[ $EUID -eq 0 ]] || die "must run as root (use sudo)"
[[ -f "$CONF_SRC" ]] || die "mosquitto-vps.conf not found next to this script at $CONF_SRC"
[[ -n "${MQTT_USER:-}" ]] || die "MQTT_USER not set"
[[ -n "${MQTT_PASS:-}" ]] || die "MQTT_PASS not set"

command -v apt-get >/dev/null || die "this script assumes Debian/Ubuntu (apt-get not found)"

echo "==> Installing mosquitto"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq mosquitto mosquitto-clients

echo "==> Installing config to $CONF_DEST"
# Keep one backup of whatever was there before, so a bad run is recoverable.
if [[ -f "$CONF_DEST" && ! -f "$CONF_DEST.orig" ]]; then
    cp "$CONF_DEST" "$CONF_DEST.orig"
    echo "    (previous config saved to $CONF_DEST.orig)"
fi
install -m 0644 "$CONF_SRC" "$CONF_DEST"

echo "==> Creating broker account '$MQTT_USER'"
# -c creates/overwrites the file; only pass it when the file doesn't exist yet,
# otherwise re-running would silently wipe other accounts.
if [[ -f "$PASSWD_FILE" ]]; then
    mosquitto_passwd -b "$PASSWD_FILE" "$MQTT_USER" "$MQTT_PASS"
else
    mosquitto_passwd -c -b "$PASSWD_FILE" "$MQTT_USER" "$MQTT_PASS"
fi
chown root:mosquitto "$PASSWD_FILE"
chmod 0640 "$PASSWD_FILE"

echo "==> Opening firewall"
if command -v ufw >/dev/null && ufw status | grep -q "Status: active"; then
    ufw allow 1883/tcp >/dev/null
    ufw allow 9001/tcp >/dev/null
    echo "    ufw: 1883/tcp and 9001/tcp allowed"
else
    echo "    ufw not active -- skipping. Make sure 1883 and 9001 are open in"
    echo "    your provider's firewall / security group, or the modem can't connect."
fi

echo "==> Restarting mosquitto"
systemctl enable mosquitto >/dev/null 2>&1 || true
systemctl restart mosquitto
sleep 1
systemctl is-active --quiet mosquitto || {
    journalctl -u mosquitto -n 20 --no-pager >&2
    die "mosquitto failed to start (log above)"
}

echo "==> Verifying"
# Both listeners bound?
for port in 1883 9001; do
    if ss -lnt 2>/dev/null | grep -q ":$port "; then
        echo "    listening on $port"
    else
        die "nothing listening on port $port"
    fi
done

# Authenticated round trip through the broker itself.
# -W rather than timeout(1), matching deploy/start-local-broker.sh, which has
# to run on macOS where timeout doesn't exist.
mosquitto_sub -W 10 -h 127.0.0.1 -p 1883 -u "$MQTT_USER" -P "$MQTT_PASS" \
    -t 'sdm27/selftest' -C 1 > /tmp/mqtt_selftest.out &
SUB_PID=$!
sleep 1
mosquitto_pub -h 127.0.0.1 -p 1883 -u "$MQTT_USER" -P "$MQTT_PASS" \
    -t 'sdm27/selftest' -m 'ok'
wait $SUB_PID 2>/dev/null || true
grep -q ok /tmp/mqtt_selftest.out \
    && echo "    authenticated publish/subscribe works" \
    || die "self-test round trip failed"
rm -f /tmp/mqtt_selftest.out

# Anonymous access must be refused -- this is the check that matters most on a
# public IP, and the one most likely to be silently wrong.
if mosquitto_sub -W 5 -h 127.0.0.1 -p 1883 -t 'sdm27/#' -C 1 >/dev/null 2>&1; then
    die "anonymous subscribe SUCCEEDED -- broker is open to the internet, fix before continuing"
else
    echo "    anonymous access correctly refused"
fi

echo
echo "Broker ready. From your laptop, confirm it's reachable from outside:"
echo "  mosquitto_sub -h <this-server-ip> -p 1883 -u $MQTT_USER -P '<password>' -t 'sdm27/#' -v"
