#!/usr/bin/env python3
"""
Raw MQTT Signal Listener for CAN Telemetry Data.

Captures raw incoming signals from an MQTT broker (default port 1883),
subscribes to topics (default '#'), and prints the raw data (timestamp,
topic, hex dump, decoded text, and raw byte representation).
"""

import argparse
import os
import signal
import ssl
import struct
import sys
import threading
import time
from collections import deque
from datetime import datetime

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print(
        "ERROR: 'paho-mqtt' is not installed.\n"
        "Please run: pip3 install -r requirements.txt",
        file=sys.stderr,
    )
    sys.exit(1)


# Global state
message_counter = 0
# Cumulative payload bytes received. This is a floor on what the publishing
# SIM was billed for, not the real figure -- it excludes MQTT packet headers,
# TCP/IP framing, keepalives and any retransmits.
byte_counter = 0
counter_lock = threading.Lock()
running = True
start_time = time.monotonic()
# (monotonic_ts, payload_len) for messages in roughly the last second, used to
# report instantaneous throughput -- the number a stress test actually cares
# about, since the cumulative average is slow to reflect a rate that changes.
recent_samples = deque()
RECENT_WINDOW_S = 1.0


def format_bytes(n: int) -> str:
    """Human-readable byte count."""
    if n < 1024:
        return f"{n} B"
    if n < 1024 * 1024:
        return f"{n / 1024:.1f} KB"
    return f"{n / (1024 * 1024):.2f} MB"


def format_bps(bits_per_sec: float) -> str:
    """Human-readable bit rate."""
    if bits_per_sec < 1000:
        return f"{bits_per_sec:.0f} bps"
    if bits_per_sec < 1_000_000:
        return f"{bits_per_sec / 1000:.1f} Kbps"
    return f"{bits_per_sec / 1_000_000:.2f} Mbps"


def get_timestamp() -> str:
    """Return current timestamp with millisecond precision."""
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


def format_hex(data: bytes, bytes_per_line: int = 16) -> str:
    """Format bytes as a clean hex dump with ASCII sidebar."""
    if not data:
        return "  (empty payload)"
    
    lines = []
    for i in range(0, len(data), bytes_per_line):
        chunk = data[i : i + bytes_per_line]
        hex_bytes = " ".join(f"{b:02X}" for b in chunk)
        ascii_chars = "".join(chr(b) if 32 <= b <= 126 else "." for b in chunk)
        lines.append(f"  {i:04X}  {hex_bytes:<{bytes_per_line * 3}}  |{ascii_chars}|")
    return "\n".join(lines)


def format_text_if_printable(data: bytes) -> str | None:
    """Try to decode bytes as UTF-8/ASCII if it represents printable text or JSON."""
    try:
        decoded = data.decode("utf-8")
        # Check if the string is mostly printable text
        if any(c.isprintable() for c in decoded) and not any(
            ord(c) < 32 and c not in "\r\n\t" for c in decoded
        ):
            return decoded.strip()
    except (UnicodeDecodeError, AttributeError):
        pass
    return None


def on_connect(client, userdata, flags, rc, properties=None):
    """Callback when client connects to broker (handles v1 and v2 API)."""
    # In Paho MQTT v2, rc is a ReasonCode object; in v1 it is an integer.
    is_success = False
    if hasattr(rc, "is_failure"):
        is_success = not rc.is_failure
    elif rc == 0:
        is_success = True

    if is_success:
        topic = userdata.get("topic", "esp32_cellular_telemetry/batch")
        print(f"\n{'='*70}", flush=True)
        print(f"[{get_timestamp()}] [+] CONNECTED to MQTT Broker at {userdata['host']}:{userdata['port']}", flush=True)
        client.subscribe(topic)
        print(f"[{get_timestamp()}] [*] Subscribed to topic: {topic}", flush=True)
        print(f"[{get_timestamp()}] [*] Listening for raw signals... (Press Ctrl+C to stop)", flush=True)
        print(f"{'='*70}\n", flush=True)
    else:
        print(f"[{get_timestamp()}] [-] Connection failed with reason/code: {rc}", flush=True)
        if "not authorised" in str(rc).lower() or str(rc) == "5":
            print("    [!] Broker rejected the credentials. On a hosted broker such as")
            print("        HiveMQ Cloud, create them under Access Management first, and")
            print("        pass them with -u/-P (the cluster refuses anonymous clients).")
        elif userdata.get("host") in ("192.168.4.1",):
            print("    [!] Hint: 192.168.4.1 is the ESP32 (client publisher), NOT an MQTT broker.")
            print("        Run Mosquitto on your laptop and point listener to your laptop IP (e.g. 192.168.4.2).")
        elif userdata.get("host") in ("localhost", "127.0.0.1"):
            print("    [!] Hint: Ensure Mosquitto broker is running: 'mosquitto -c mosquitto.conf -d'")


def on_disconnect(client, userdata, disconnect_flags_or_rc, reason_code=None, properties=None):
    """Callback when client disconnects from broker."""
    print(f"[{get_timestamp()}] [!] Disconnected from broker. Attempting to reconnect...")


def on_message(client, userdata, msg):
    """Callback when a message arrives."""
    global message_counter, byte_counter
    payload = msg.payload
    now_m = time.monotonic()
    with counter_lock:
        message_counter += 1
        byte_counter += len(payload)
        current_num = message_counter
        total_bytes = byte_counter

        recent_samples.append((now_m, len(payload)))
        while recent_samples and now_m - recent_samples[0][0] > RECENT_WINDOW_S:
            recent_samples.popleft()
        # Divide by the fixed window, not the span between the oldest and
        # newest sample still in it -- a burst of messages arriving close
        # together (e.g. right after a gap) would otherwise shrink that span
        # toward zero and report an absurd spike for a handful of bytes.
        window_span = min(now_m - start_time, RECENT_WINDOW_S)
        inst_bps = sum(n for _, n in recent_samples) * 8 / max(window_span, 1e-6)

    timestamp = get_timestamp()
    topic = msg.topic
    length = len(payload)
    text_repr = format_text_if_printable(payload)

    # Extrapolate observed throughput to an hourly figure -- the number that
    # actually matters when the publisher is on a metered SIM.
    elapsed = max(now_m - start_time, 1e-6)
    per_hour = total_bytes / elapsed * 3600
    data_totals = f"{format_bytes(total_bytes)} total, ~{format_bytes(int(per_hour))}/hr, now {format_bps(inst_bps)}"

    if userdata.get("brief"):
        import json
        try:
            parsed = json.loads(payload.decode("utf-8"))
            if "stress_seq" in parsed:
                print(f"[{timestamp}] #{current_num:04d} {topic} [stress seq={parsed['stress_seq']}] ({length}B, {data_totals})")
            else:
                seq = parsed.get("seq", "-")
                frames = len(parsed.get("frames", []))
                dropped = parsed.get("dropped_frames", 0)
                dev = parsed.get("device_id", "dev")
                print(f"[{timestamp}] #{current_num:04d} {topic} [{dev} seq={seq} frames={frames} dropped={dropped}] ({length}B, {data_totals})")
        except Exception:
            text = text_repr or f"{length} bytes"
            print(f"[{timestamp}] #{current_num:04d} {topic} ({text})")
        return

    print(f"\n--- [MESSAGE #{current_num}] {timestamp} ---")
    print(f"  Topic   : {topic}")
    print(f"  QoS / R : {msg.qos} / {'Retained' if msg.retain else 'Live'}")
    print(f"  Length  : {length} bytes")
    print(f"  Data    : {data_totals} at this rate")
    print("  Hex Dump:")
    print(format_hex(payload))
    if text_repr:
        print(f"  Text    : {text_repr}")
    print(f"  Raw Byte: {payload!r}")


def run_simulator(host: str, port: int, stop_event: threading.Event):
    """
    Background simulation publisher to generate mock CAN signals
    for local testing when not connected to the actual hardware.
    """
    time.sleep(1.0)
    print(f"[{get_timestamp()}] [SIMULATOR] Starting mock CAN signal generator...")
    try:
        try:
            sim_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="sim_can_node")
        except AttributeError:
            sim_client = mqtt.Client(client_id="sim_can_node")

        sim_client.connect(host, port, keepalive=60)
        sim_client.loop_start()

        can_id = 0x100
        seq = 0
        while not stop_event.is_set():
            seq += 1
            # Mock Binary CAN Frame: ID (uint32), DLC (uint8), Data (8 bytes)
            # e.g., Motor RPM (uint16), Inverter Temp (int16), Battery Voltage (uint16), Current (int16)
            rpm = (3000 + (seq * 50)) % 12000
            temp = 45 + (seq % 10)
            voltage = 380 + (seq % 20)
            current = 150 + (seq % 30)
            data_payload = struct.pack(">HhhH", rpm, temp, voltage, current)
            raw_can_frame = struct.pack(">IB8s", can_id, 8, data_payload)

            sim_client.publish("esp32_cellular_telemetry/batch", raw_can_frame, qos=0)

            # Every 3 messages, also send a JSON format batch frame
            if seq % 3 == 0:
                json_msg = f'{{"can_id": "0x{can_id:X}", "rpm": {rpm}, "temp_c": {temp}, "v_pack": {voltage}}}'
                sim_client.publish("esp32_cellular_telemetry/batch", json_msg.encode("utf-8"), qos=0)

            can_id = 0x100 + (seq % 5)
            stop_event.wait(1.0)

        sim_client.loop_stop()
        sim_client.disconnect()
        print(f"[{get_timestamp()}] [SIMULATOR] Stopped.")
    except Exception as e:
        print(f"[{get_timestamp()}] [SIMULATOR ERROR] {e}")


def load_env_file(path: str = ".env") -> dict:
    """Read key-value pairs from .env file into os.environ if not already set."""
    if not os.path.isfile(path):
        script_dir = os.path.dirname(os.path.abspath(__file__))
        path = os.path.join(script_dir, ".env")
        if not os.path.isfile(path):
            return {}
    loaded = {}
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, _, value = line.partition("=")
                key = key.strip()
                value = value.strip()
                if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
                    value = value[1:-1]
                if key not in os.environ:
                    os.environ[key] = value
                loaded[key] = value
    except Exception:
        pass
    return loaded


def main():
    load_env_file()

    default_host = os.getenv("MQTT_HOST") or os.getenv("MQTT_BROKER_HOST", "localhost")
    default_port = int(os.getenv("MQTT_PORT") or os.getenv("MQTT_BROKER_PORT", "1883"))
    default_topic = os.getenv("MQTT_TOPIC") or os.getenv("MQTT_TOPIC_TELEMETRY", "esp32_cellular_telemetry/batch")
    default_user = os.getenv("MQTT_USER") or os.getenv("MQTT_USERNAME", None)
    default_pass = os.getenv("MQTT_PASSWORD", None)

    parser = argparse.ArgumentParser(
        description="Capture and print raw MQTT signals on port 1883 (e.g. CAN telemetry data)."
    )
    parser.add_argument(
        "-H",
        "--host",
        default=default_host,
        help=f"MQTT broker hostname or IP address (default: {default_host})",
    )
    parser.add_argument(
        "-p",
        "--port",
        type=int,
        default=default_port,
        help=f"MQTT broker port (default: {default_port})",
    )
    parser.add_argument(
        "-t",
        "--topic",
        default=default_topic,
        help=f"MQTT topic to subscribe to (default: '{default_topic}')",
    )
    parser.add_argument(
        "-u",
        "--username",
        default=default_user,
        help="Username for broker authentication (optional)",
    )
    parser.add_argument(
        "-P",
        "--password",
        default=default_pass,
        help="Password for broker authentication (optional)",
    )
    parser.add_argument(
        "-c",
        "--client-id",
        default=f"telemetry_listener_{os.getpid()}",
        help="MQTT client ID",
    )
    parser.add_argument(
        "-s",
        "--simulate",
        action="store_true",
        help="Run an internal mock CAN publisher to test reception without hardware",
    )
    parser.add_argument(
        "--ap",
        action="store_true",
        help="Shortcut for SoftAP mode: sets host to 192.168.4.2 (Mac broker on esp32-telemetry)",
    )
    parser.add_argument(
        "--stress",
        action="store_true",
        help="Shortcut for watching a TELEMETRY_STRESS_TEST run: subscribes to "
        "'<topic>/stress' instead of the normal telemetry topic. Combine with "
        "-b to see live throughput per message.",
    )
    parser.add_argument(
        "-b",
        "--brief",
        action="store_true",
        help="Print a compact one-line summary per message, with running data "
        "totals, instead of the full hex dump. Recommended for JSON batches, "
        "which are several KB each",
    )
    parser.add_argument(
        "--tls",
        action="store_true",
        help="Connect over TLS. Implied by --port 8883. Required for hosted "
        "brokers such as HiveMQ Cloud, which refuse plaintext connections",
    )
    parser.add_argument(
        "--cafile",
        default=os.getenv("MQTT_CAFILE", None),
        help="CA bundle for TLS verification (default: $MQTT_CAFILE, else "
        "certifi's bundle if installed, else a common system bundle). Only "
        "needed for a broker using a private CA",
    )
    parser.add_argument(
        "--insecure",
        action="store_true",
        help="Skip TLS certificate verification. Debugging only -- this makes "
        "the connection vulnerable to interception",
    )

    args = parser.parse_args()

    # If --ap is set and host wasn't overridden from default localhost, use SoftAP laptop IP
    if args.ap and args.host == os.getenv("MQTT_HOST", "localhost"):
        args.host = "192.168.4.2"

    # If --stress is set and topic wasn't overridden, watch the stress topic instead
    if args.stress and args.topic == default_topic:
        args.topic = f"{default_topic}/stress"

    # User context dictionary passed to callbacks
    userdata = {
        "host": args.host,
        "port": args.port,
        "topic": args.topic,
        "brief": args.brief,
    }

    # Initialize MQTT client with API version compatibility
    try:
        client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=args.client_id,
            userdata=userdata,
        )
    except AttributeError:
        client = mqtt.Client(
            client_id=args.client_id,
            userdata=userdata,
        )

    # Set authentication if provided
    if args.username:
        client.username_pw_set(args.username, args.password)

    # 8883 is the registered port for MQTT over TLS, so treat it as implying
    # --tls: pointing this at a hosted broker and forgetting the flag otherwise
    # fails with an opaque timeout rather than anything that names the cause.
    use_tls = args.tls or args.port == 8883
    if use_tls:
        # An explicit --cafile wins; otherwise prefer certifi, then a common
        # system bundle. Some Python installs have no usable default trust
        # store, so leaving this to OpenSSL's defaults can fail verification
        # against a perfectly valid public certificate.
        ca_certs = args.cafile
        if ca_certs is None:
            try:
                import certifi
                ca_certs = certifi.where()
            except ImportError:
                for path in ("/etc/ssl/cert.pem", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/certs/ca-certificates.crt"):
                    if os.path.exists(path):
                        ca_certs = path
                        break
        # Exactly one tls_set() call: paho raises ValueError on a second one.
        client.tls_set(ca_certs=ca_certs)
        if args.insecure:
            client.tls_insecure_set(True)
            print(
                f"[{get_timestamp()}] [!] TLS certificate verification DISABLED "
                "-- connection is encrypted but the broker is unauthenticated"
            )

    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message

    # Handle graceful exit on Ctrl+C
    stop_event = threading.Event()

    def handle_sigint(sig, frame):
        print(f"\n[{get_timestamp()}] Stopping listener...")
        stop_event.set()
        client.disconnect()
        client.loop_stop()
        elapsed = max(time.monotonic() - start_time, 1e-6)
        print(f"[{get_timestamp()}] Total raw messages captured: {message_counter}")
        print(
            f"[{get_timestamp()}] Total payload received: {format_bytes(byte_counter)} "
            f"over {elapsed / 60:.1f} min "
            f"(~{format_bytes(int(byte_counter / elapsed * 3600))}/hr)"
        )
        print(f"[{get_timestamp()}] Note: actual SIM usage is higher -- excludes MQTT/TCP overhead.")
        sys.exit(0)

    signal.signal(signal.SIGINT, handle_sigint)

    # If simulation mode is enabled, start simulation thread
    sim_thread = None
    if args.simulate:
        sim_thread = threading.Thread(
            target=run_simulator,
            args=(args.host, args.port, stop_event),
            daemon=True,
        )
        sim_thread.start()

    print(
        f"[{get_timestamp()}] Connecting to MQTT broker at {args.host}:{args.port} "
        f"({'TLS' if use_tls else 'plaintext'})..."
    )
    print(f"[{get_timestamp()}] Subscribing to: '{args.topic}'")
    if args.host in ("localhost", "127.0.0.1"):
        print("Note: To allow the ESP32 on WiFi to reach your broker, ensure Mosquitto was started with:")
        print("      mosquitto -c mosquitto.conf -d (listening on 0.0.0.0, not just 127.0.0.1)")
    elif args.host == "192.168.4.1":
        print("WARNING: 192.168.4.1 is the ESP32, which is an MQTT client, not a broker!")
        print("         The broker should run on your laptop (192.168.4.2).")
    print("Tip: If broker or device is not online yet, listener will automatically keep retrying.\n")

    try:
        client.connect(args.host, args.port, keepalive=60)
        client.loop_forever()
    except KeyboardInterrupt:
        handle_sigint(None, None)
    except Exception as e:
        print(f"[{get_timestamp()}] Connection Error: {e}", flush=True)
        sys.exit(1)


if __name__ == "__main__":
    main()
