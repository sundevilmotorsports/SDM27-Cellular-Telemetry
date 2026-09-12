#!/usr/bin/env python3
"""
Direct-to-modem USB throughput bench for the SIM7670G.

Talks AT commands straight to the modem over its OWN microUSB port (the one
next to the boot button, wired directly to the SIM7670G chip -- NOT the
LilyGo board's Type-C port, which goes to the ESP32-S3). The ESP32 is not in
this data path at all: flash it with MODEM_USB_BENCH_MODE=1 first (see
README.md) so it powers the modem on and then leaves UART1 alone, so this
script has the modem's AT interface to itself.

Mirrors the exact same AT command sequence net_task.cpp uses for MQTT
publish (TinyGsmClientSIM7672.h: CGDCONT/NETOPEN/CIPOPEN/CIPSEND for
plaintext, or CSSLCFG/CCHSTART/CCHOPEN/CCHSEND for TLS) so the throughput
number is comparable to the firmware's own stress test -- just without the
ESP32<->modem UART1 link (115200 baud) in the middle.

WARNING: like the firmware's stress test, this moves real data over the
cellular link on whatever SIM is in the modem. Every byte it manages to
send is billed on a metered SIM. There is no dry-run mode.

Usage:
    python3 usb_modem_bench.py --probe               # find the AT port
    python3 usb_modem_bench.py --port /dev/cu.usbmodemXXXX --duration 20
"""

import argparse
import os
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print(
        "ERROR: 'pyserial' is not installed.\n"
        "Please run: pip3 install -r requirements.txt",
        file=sys.stderr,
    )
    sys.exit(1)


def parse_env_file(path):
    """Same tiny parser load_env.py uses, so this script reads the same .env."""
    values = {}
    if not os.path.isfile(path):
        return values
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, value = line.partition("=")
            key = key.strip()
            value = value.strip()
            if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
                value = value[1:-1]
            values[key] = value
    return values


class ModemError(Exception):
    pass


class ModemAT:
    """Minimal line-oriented AT command driver over a pyserial port."""

    def __init__(self, port, timeout_s=5.0, verbose=False):
        self.ser = serial.Serial(port, baudrate=115200, timeout=0.1)
        self.default_timeout = timeout_s
        self.verbose = verbose

    def close(self):
        self.ser.close()

    def _log(self, direction, data):
        if self.verbose:
            print(f"  {direction} {data!r}", file=sys.stderr)

    def send_raw(self, data: bytes):
        self._log(">>", data)
        self.ser.write(data)
        self.ser.flush()

    def read_until_any(self, needles, timeout_s):
        """Reads until one of `needles` (bytes) appears in the buffer, or
        times out. Returns (matched_needle_or_None, full_buffer_bytes)."""
        buf = b""
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            chunk = self.ser.read(4096)
            if chunk:
                buf += chunk
                self._log("<<", chunk)
                for needle in needles:
                    if needle in buf:
                        return needle, buf
        return None, buf

    def cmd(self, at_cmd, expect=b"OK", timeout_s=None, extra_ok=(b"ERROR",)):
        """Sends `AT<at_cmd>\\r\\n`, waits for `expect` (or an error marker).
        Raises ModemError on timeout or if only an error marker is seen."""
        timeout_s = timeout_s if timeout_s is not None else self.default_timeout
        self.send_raw(b"AT" + at_cmd.encode() + b"\r\n")
        needle, buf = self.read_until_any([expect] + list(extra_ok), timeout_s)
        if needle is None:
            raise ModemError(
                f"timed out waiting for {expect!r} after AT{at_cmd} "
                f"(got: {buf!r})"
            )
        if needle in extra_ok and needle != expect:
            raise ModemError(f"AT{at_cmd} -> {buf!r}")
        return buf

    def expect(self, expect, timeout_s=None, extra_ok=(b"ERROR",)):
        """Like cmd(), but doesn't send anything first -- for waiting on a
        response after send_raw() has already written data to the modem."""
        timeout_s = timeout_s if timeout_s is not None else self.default_timeout
        needle, buf = self.read_until_any([expect] + list(extra_ok), timeout_s)
        if needle is None:
            raise ModemError(f"timed out waiting for {expect!r} (got: {buf!r})")
        if needle in extra_ok and needle != expect:
            raise ModemError(f"expected {expect!r}, got {buf!r}")
        return buf


def probe_ports(verbose):
    candidates = [p.device for p in serial.tools.list_ports.comports()]
    if not candidates:
        print("No serial ports found at all -- is the modem's microUSB plugged in "
              "and powered? (flash MODEM_USB_BENCH_MODE=1 first, see README.md)")
        return None
    print(f"Probing {len(candidates)} port(s) for an AT-responsive one: {candidates}")
    for dev in candidates:
        try:
            m = ModemAT(dev, timeout_s=1.5, verbose=verbose)
            m.cmd("", timeout_s=1.5)  # bare "AT"
            print(f"  {dev}: responded to AT -- this is likely the AT command port")
            m.close()
            return dev
        except Exception as e:
            print(f"  {dev}: no AT response ({e})")
    print(
        "\nNone of the discovered ports answered 'AT'. If the modem's microUSB is "
        "connected but you're holding (or recently held) its boot button, it may "
        "have enumerated in firmware-flash/bootloader mode instead of normal AT "
        "mode -- unplug/replug without touching the boot button and try again. "
        "Also confirm the ESP32 has been flashed with MODEM_USB_BENCH_MODE=1 so "
        "it has actually powered the modem on."
    )
    return None


def wait_for_registration(m, timeout_s):
    print(f"Waiting up to {timeout_s:.0f}s for network registration (AT+CEREG?)...")
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        buf = m.cmd("+CEREG?", timeout_s=5.0)
        # +CEREG: <n>,<stat> -- stat 1 = home, 5 = roaming
        text = buf.decode(errors="replace")
        for stat in ("+CEREG: 0,1", "+CEREG: 0,5", "+CEREG: 2,1", "+CEREG: 2,5"):
            if stat in text:
                print(f"  registered ({stat.split(',')[1]})")
                return True
        time.sleep(2)
    return False


def setup_pdp(m, apn, apn_user, apn_pass):
    if apn_user:
        m.cmd(f'+CGAUTH=1,0,"{apn_user}","{apn_pass}"')
    m.cmd(f'+CGDCONT=1,"IP","{apn}","0.0.0.0",0,0')


def open_plain_socket(m, host, port):
    m.cmd("+CIPMODE=0")
    m.cmd("+CIPSENDMODE=0")
    m.cmd("+CIPCCFG=10,0,0,0,1,0,75000")
    m.cmd("+CIPTIMEOUT=75000,15000,15000")
    print("Opening the socket service (AT+NETOPEN, activates the PDP context)...")
    m.cmd("+NETOPEN", expect=b"+NETOPEN: 0", timeout_s=75)
    m.cmd("+CIPRXGET=1")
    print(f"Opening TCP connection to {host}:{port} (AT+CIPOPEN)...")
    m.cmd(f'+CIPOPEN=0,"TCP","{host}",{port}', expect=b"+CIPOPEN:", timeout_s=30)


def close_plain_socket(m):
    try:
        m.cmd("+CIPCLOSE=0", timeout_s=15)
    except ModemError:
        pass
    try:
        m.cmd("+NETCLOSE", expect=b"+NETCLOSE: 0", timeout_s=60)
    except ModemError:
        pass


def mqtt_string(s: bytes) -> bytes:
    return len(s).to_bytes(2, "big") + s


def mqtt_remaining_length(n: int) -> bytes:
    out = bytearray()
    while True:
        byte = n % 128
        n //= 128
        if n > 0:
            byte |= 0x80
        out.append(byte)
        if n == 0:
            return bytes(out)


def mqtt_connect_packet(client_id, username="", password="", keepalive=60):
    var_header = b"\x00\x04MQTT\x04"  # protocol name "MQTT", level 4 (3.1.1)
    flags = 0x02  # clean session
    if username:
        flags |= 0x80
    if password:
        flags |= 0x40
    var_header += bytes([flags]) + keepalive.to_bytes(2, "big")
    payload = mqtt_string(client_id.encode())
    if username:
        payload += mqtt_string(username.encode())
    if password:
        payload += mqtt_string(password.encode())
    body = var_header + payload
    return bytes([0x10]) + mqtt_remaining_length(len(body)) + body


def mqtt_publish_packet(topic, payload_bytes):
    # QoS 0, no packet identifier -- matches PubSubClient::publish()'s
    # default, so this is the same wire-level MQTT the firmware sends.
    body = mqtt_string(topic.encode()) + payload_bytes
    return bytes([0x30]) + mqtt_remaining_length(len(body)) + body


def fields_after(buf, marker):
    """Returns the comma-separated fields on the line following `marker`
    (bytes), e.g. b"...+CCHSEND: 0,0\\r\\n..." -> ["0", "0"]."""
    text = buf.decode(errors="replace")
    idx = text.index(marker.decode())
    line_end = text.index("\n", idx)
    after = text[idx + len(marker.decode()):line_end]
    return [f.strip() for f in after.split(",")]


def send_plain_chunk(m, payload):
    m.cmd(f"+CIPSEND=0,{len(payload)}", expect=b">", timeout_s=15)
    m.send_raw(payload)
    buf = m.expect(b"+CIPSEND:", timeout_s=15)
    # +CIPSEND: <mux>,<requested>,<confirmed>
    fields = fields_after(buf, b"+CIPSEND:")
    if len(fields) >= 3 and fields[2] != fields[1]:
        raise ModemError(f"partial send: requested {fields[1]}, confirmed {fields[2]}")


def open_ssl_socket(m, host, port):
    m.cmd('+CSSLCFG="sslversion",0,4')
    m.cmd('+CSSLCFG="enableSNI",0,1')
    m.cmd('+CSSLCFG="authmode",0,0')
    m.cmd("+CCHSET=1,1")
    print("Starting the CCH SSL service (AT+CCHSTART, activates the PDP context)...")
    m.cmd("+CCHSTART", timeout_s=10)
    m.cmd("+CCHSSLCFG=0,0")
    print(f"Opening TLS connection to {host}:{port} (AT+CCHOPEN)...")
    m.cmd(f'+CCHOPEN=0,"{host}",{port},2', expect=b"+CCHOPEN:", timeout_s=30)


def close_ssl_socket(m):
    try:
        m.cmd("+CCHCLOSE=0", expect=b"+CCHCLOSE:", timeout_s=10)
    except ModemError:
        pass
    try:
        m.cmd("+CCHSTOP", expect=b"+CCHSTOP:", timeout_s=10)
    except ModemError:
        pass


def send_ssl_chunk(m, payload):
    m.cmd(f"+CCHSEND=0,{len(payload)}", expect=b">", timeout_s=15)
    m.send_raw(payload)
    buf = m.expect(b"+CCHSEND:", timeout_s=15)
    # +CCHSEND: <session_id>,<err> -- err 0 means success
    fields = fields_after(buf, b"+CCHSEND:")
    if len(fields) >= 2 and fields[1] != "0":
        raise ModemError(f"CCHSEND reported error {fields[1]}")


def main():
    env = parse_env_file(os.path.join(os.path.dirname(__file__), ".env"))

    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--probe", action="store_true",
                     help="list serial ports and find the one that answers AT, then exit")
    ap.add_argument("--port", help="modem AT serial port (e.g. /dev/cu.usbmodemXXXX)")
    ap.add_argument("--apn", default=env.get("TELEMETRY_APN"),
                     help="cellular APN (default: TELEMETRY_APN from .env)")
    ap.add_argument("--apn-user", default=env.get("TELEMETRY_APN_USER", ""))
    ap.add_argument("--apn-pass", default=env.get("TELEMETRY_APN_PASS", ""))
    ap.add_argument("--host", default=env.get("MQTT_BROKER_HOST"),
                     help="destination host (default: MQTT_BROKER_HOST from .env)")
    ap.add_argument("--dest-port", type=int,
                     default=int(env.get("MQTT_BROKER_PORT", "0") or 0) or None,
                     help="destination TCP port (default: MQTT_BROKER_PORT from .env)")
    ap.add_argument("--tls", dest="tls", action="store_true", default=None,
                     help="use the CCH SSL/TLS path (default: MQTT_USE_TLS from .env)")
    ap.add_argument("--no-tls", dest="tls", action="store_false")
    ap.add_argument("--mqtt-username", default=env.get("MQTT_USERNAME", ""))
    ap.add_argument("--mqtt-password", default=env.get("MQTT_PASSWORD", ""))
    ap.add_argument("--mqtt-topic",
                     default=env.get("MQTT_TOPIC_TELEMETRY", "esp32_cellular_telemetry/batch")
                     + "/usbbench",
                     help="topic to publish filler payloads to (default: "
                          "MQTT_TOPIC_TELEMETRY from .env + '/usbbench')")
    ap.add_argument("--duration", type=float, default=20.0,
                     help="seconds to burst for (default: 20 -- this spends real "
                          "cellular data, kept short on purpose)")
    ap.add_argument("--chunk-size", type=int, default=1024,
                     help="bytes per AT+CIPSEND/CCHSEND call (default: 1024; the "
                          "modem's own cap is ~1460-1500 bytes per SIMCom's AT "
                          "manual, well above this firmware's 255-byte "
                          "MQTT_MAX_TRANSFER_SIZE workaround -- see platformio.ini)")
    ap.add_argument("--verbose", action="store_true", help="log raw AT traffic")
    args = ap.parse_args()

    if args.probe or not args.port:
        found = probe_ports(args.verbose)
        if args.probe:
            sys.exit(0 if found else 1)
        if not found:
            sys.exit(1)
        args.port = found

    if args.tls is None:
        args.tls = env.get("MQTT_USE_TLS", "0").strip() == "1"

    if not args.apn or args.apn == "your.apn.here":
        print("ERROR: no APN set. Pass --apn or set TELEMETRY_APN in .env.", file=sys.stderr)
        sys.exit(1)
    if not args.host or not args.dest_port:
        print("ERROR: no destination host/port. Pass --host/--dest-port or set "
              "MQTT_BROKER_HOST/MQTT_BROKER_PORT in .env.", file=sys.stderr)
        sys.exit(1)

    print(f"Port: {args.port}  APN: {args.apn}  Dest: {args.host}:{args.dest_port} "
          f"({'TLS/CCH' if args.tls else 'plain/CIP'})")
    print("This will send real data over the cellular link -- billed data on a "
          "metered SIM. Ctrl-C to abort before it starts sending.")

    m = ModemAT(args.port, verbose=args.verbose)
    try:
        m.cmd("E0")  # echo off, matches firmware init
        m.cmd("+CPIN?", timeout_s=5)
        if not wait_for_registration(m, timeout_s=90):
            raise ModemError("network registration timed out -- check antenna/SIM")

        setup_pdp(m, args.apn, args.apn_user, args.apn_pass)

        if args.tls:
            open_ssl_socket(m, args.host, args.dest_port)
            send_chunk = send_ssl_chunk
            close_socket = close_ssl_socket
        else:
            open_plain_socket(m, args.host, args.dest_port)
            send_chunk = send_plain_chunk
            close_socket = close_plain_socket

        client_id = "usb-bench-" + os.urandom(4).hex()
        print(f"MQTT CONNECT as '{client_id}' (this socket carries real MQTT, "
              f"same as the firmware -- a broker will drop a raw-bytes socket)...")
        connect_packet = mqtt_connect_packet(client_id, args.mqtt_username, args.mqtt_password)
        send_chunk(m, connect_packet)
        time.sleep(0.3)  # let the broker process CONNECT before PUBLISHes arrive

        filler = bytes((i % 26) + ord('a') for i in range(args.chunk_size))
        publish_packet = mqtt_publish_packet(args.mqtt_topic, filler)

        print(f"Publishing {len(publish_packet)}-byte MQTT packets "
              f"({args.chunk_size} bytes filler + framing) to '{args.mqtt_topic}' "
              f"for {args.duration:.0f}s...")
        start = time.monotonic()
        last_log = start
        window_bytes = 0
        total_bytes = 0
        total_chunks = 0
        try:
            while time.monotonic() - start < args.duration:
                send_chunk(m, publish_packet)
                total_bytes += len(publish_packet)
                window_bytes += len(publish_packet)
                total_chunks += 1

                now = time.monotonic()
                if now - last_log >= 1.0:
                    bps = window_bytes * 8 / (now - last_log)
                    print(f"  {bps:,.0f} bps, {total_bytes:,} bytes so far "
                          f"({total_chunks} chunks)")
                    window_bytes = 0
                    last_log = now
        except ModemError as e:
            print(f"send failed mid-burst, stopping early: {e}")

        elapsed = time.monotonic() - start
        avg_bps = total_bytes * 8 / elapsed if elapsed > 0 else 0
        print(f"\nDone: {total_bytes:,} bytes over {elapsed:.1f}s, "
              f"avg {avg_bps:,.0f} bps ({total_chunks} chunks, "
              f"{args.chunk_size} bytes/chunk)")
        print("Compare against the firmware's own stress test (README's Uplink "
              "stress test section) to see how much of the ~92 Kbps ceiling was "
              "the ESP32<->modem UART vs. the modem/cellular link itself.")

        close_socket(m)
    except ModemError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\naborted")
    finally:
        m.close()


if __name__ == "__main__":
    main()
