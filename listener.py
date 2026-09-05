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
import struct
import sys
import threading
import time
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
counter_lock = threading.Lock()
running = True


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
        print(f"\n{'='*70}")
        print(f"[{get_timestamp()}] [+] CONNECTED to MQTT Broker at {userdata['host']}:{userdata['port']}")
        client.subscribe(topic)
        print(f"[{get_timestamp()}] [*] Subscribed to topic: {topic}")
        print(f"[{get_timestamp()}] [*] Listening for raw signals... (Press Ctrl+C to stop)")
        print(f"{'='*70}\n")
    else:
        print(f"[{get_timestamp()}] [-] Connection failed with reason/code: {rc}")
        if userdata.get("host") in ("192.168.4.1",):
            print("    [!] Hint: 192.168.4.1 is the ESP32 (client publisher), NOT an MQTT broker.")
            print("        Run Mosquitto on your laptop and point listener to your laptop IP (e.g. 192.168.4.2).")
        elif userdata.get("host") in ("localhost", "127.0.0.1"):
            print("    [!] Hint: Ensure Mosquitto broker is running: 'mosquitto -c mosquitto.conf -d'")


def on_disconnect(client, userdata, disconnect_flags_or_rc, reason_code=None, properties=None):
    """Callback when client disconnects from broker."""
    print(f"[{get_timestamp()}] [!] Disconnected from broker. Attempting to reconnect...")


def on_message(client, userdata, msg):
    """Callback when a message arrives."""
    global message_counter
    with counter_lock:
        message_counter += 1
        current_num = message_counter

    timestamp = get_timestamp()
    payload = msg.payload
    topic = msg.topic
    length = len(payload)
    text_repr = format_text_if_printable(payload)
    hex_dump = format_hex(payload)

    print(f"\n--- [MESSAGE #{current_num}] {timestamp} ---")
    print(f"  Topic   : {topic}")
    print(f"  QoS / R : {msg.qos} / {'Retained' if msg.retain else 'Live'}")
    print(f"  Length  : {length} bytes")
    print("  Hex Dump:")
    print(hex_dump)
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


def main():
    parser = argparse.ArgumentParser(
        description="Capture and print raw MQTT signals on port 1883 (e.g. CAN telemetry data)."
    )
    parser.add_argument(
        "-H",
        "--host",
        default=os.getenv("MQTT_HOST", "localhost"),
        help="MQTT broker hostname or IP address (default: localhost, or $MQTT_HOST)",
    )
    parser.add_argument(
        "-p",
        "--port",
        type=int,
        default=int(os.getenv("MQTT_PORT", "1883")),
        help="MQTT broker port (default: 1883, or $MQTT_PORT)",
    )
    parser.add_argument(
        "-t",
        "--topic",
        default=os.getenv("MQTT_TOPIC", "esp32_cellular_telemetry/batch"),
        help="MQTT topic to subscribe to (default: 'esp32_cellular_telemetry/batch', or $MQTT_TOPIC)",
    )
    parser.add_argument(
        "-u",
        "--username",
        default=os.getenv("MQTT_USER", None),
        help="Username for broker authentication (optional, or $MQTT_USER)",
    )
    parser.add_argument(
        "-P",
        "--password",
        default=os.getenv("MQTT_PASSWORD", None),
        help="Password for broker authentication (optional, or $MQTT_PASSWORD)",
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

    args = parser.parse_args()

    # If --ap is set and host wasn't overridden from default localhost, use SoftAP laptop IP
    if args.ap and args.host == os.getenv("MQTT_HOST", "localhost"):
        args.host = "192.168.4.2"

    # User context dictionary passed to callbacks
    userdata = {
        "host": args.host,
        "port": args.port,
        "topic": args.topic,
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
        print(f"[{get_timestamp()}] Total raw messages captured: {message_counter}")
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

    print(f"[{get_timestamp()}] Connecting to MQTT broker at {args.host}:{args.port}...")
    print(f"[{get_timestamp()}] Subscribing to: '{args.topic}'")
    if args.host in ("localhost", "127.0.0.1"):
        print("Note: To allow the ESP32 on WiFi to reach your broker, ensure Mosquitto was started with:")
        print("      mosquitto -c mosquitto.conf -d (listening on 0.0.0.0, not just 127.0.0.1)")
    elif args.host == "192.168.4.1":
        print("WARNING: 192.168.4.1 is the ESP32, which is an MQTT client, not a broker!")
        print("         The broker should run on your laptop (192.168.4.2).")
    print("Tip: If broker or device is not online yet, listener will automatically keep retrying.\n")

    try:
        # connect_async allows loop_forever to retry automatically even if the broker is not yet reachable
        client.connect_async(args.host, args.port, keepalive=60)
        client.loop_forever(retry_first_connection=True)
    except KeyboardInterrupt:
        handle_sigint(None, None)
    except Exception as e:
        print(f"[{get_timestamp()}] Error: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
