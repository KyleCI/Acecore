#!/usr/bin/env python3
"""Provision a Powercell collector over USB serial from a KEY=VALUE file.

Usage:  python tools/provision.py --port COM5 --file powercell-dev-provision.env

Keys read from the file (others are ignored, so the backend .env can be used too):
  API_KEY       -> SET api_key      (required)
  LOCAL_TOKEN   -> SET local_tok    (LAN Basic auth password for user "powercell")
  WIFI_SSID     -> SET wifi_ssid    (optional; or use the setup AP)
  WIFI_PASS     -> SET wifi_pass
  OTA_URL       -> SET ota_url      (optional; the dev build defaults to https://api-dev.acecore.tech)
  GW_NAME       -> SET gw_name      (optional, e.g. ACE9678)
  DEVICE_SECRET -> SET device_secret (64 hex; factory identity / WS + fingerprint HMAC; same as console SECRET)

v17: DEV_SECRET is no longer accepted (merged into DEVICE_SECRET / factory identity). If present in the
file it is ignored with a clear message — use DEVICE_SECRET or the console SECRET command.

API_KEY is required unless --only-present is given (e.g. a file with just DEVICE_SECRET).

Secret values are never printed: each step shows only the key name and OK/ERR.
Needs pyserial (installed together with esptool: pip install esptool).
"""
import argparse
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial missing: pip install esptool (or pip install pyserial)")

ORDER = [("API_KEY", "api_key", True), ("WIFI_SSID", "wifi_ssid", False), ("WIFI_PASS", "wifi_pass", False),
         ("OTA_URL", "ota_url", False), ("GW_NAME", "gw_name", False),
         ("DEVICE_SECRET", "device_secret", False),
         ("LOCAL_TOKEN", "local_tok", False)]
SAFE_INFO = ("device_id=", "ap_ssid=", "local_token=", "api_key=")


def read_env(path):
    vals = {}
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            v = v.strip()
            if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
                v = v[1:-1]
            vals[k.strip()] = v
    return vals


def wait_reply(port, timeout=3.0):
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        buf += port.read(256)
        for line in buf.decode("utf-8", "replace").splitlines():
            s = line.strip()
            if s.startswith("OK") or s.startswith("ERR"):
                return s
    return "TIMEOUT"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--file", required=True)
    ap.add_argument("--no-reboot", action="store_true")
    ap.add_argument("--only-present", action="store_true", help="do not require API_KEY; set only keys in the file")
    a = ap.parse_args()
    vals = read_env(a.file)
    if vals.get("DEV_SECRET"):
        print("dev_secret  ignored (v17: use DEVICE_SECRET / factory SECRET; legacy DEV_SECRET merged)")
    if not vals.get("API_KEY") and not a.only_present:
        sys.exit("API_KEY missing in the file (use --only-present to set only the keys given)")
    ds = vals.get("DEVICE_SECRET", "")
    if ds and (len(ds) != 64 or any(c not in "0123456789abcdefABCDEF" for c in ds)):
        sys.exit(f"DEVICE_SECRET must be 64 hex chars (got length {len(ds)})")
    with serial.Serial(a.port, 115200, timeout=0.2) as port:
        print("waiting for the board to boot (opening the port resets it)...")
        time.sleep(4)
        port.reset_input_buffer()
        failed = 0
        for env_key, dev_key, required in ORDER:
            v = vals.get(env_key, "")
            if not v:
                if required and not a.only_present:
                    failed += 1
                print(f"{dev_key:10s} skipped (not in file)")
                continue
            port.reset_input_buffer()
            port.write(f"SET {dev_key} {v}\n".encode())
            reply = wait_reply(port)
            ok = reply.startswith("OK")
            failed += 0 if ok else 1
            # Only the device's fingerprint line is safe to show for the local token.
            shown = reply if (dev_key == "local_tok" and ok) else ("OK" if ok else reply.split(" ")[0])
            print(f"{dev_key:10s} {shown}")
        port.reset_input_buffer()
        port.write(b"INFO\n")
        time.sleep(1.0)
        for line in port.read(4096).decode("utf-8", "replace").splitlines():
            if line.startswith(SAFE_INFO):
                print("  " + line.split(" wifi_ssid=")[0])
        if not a.no_reboot:
            port.write(b"REBOOT\n")
            print("rebooting; open the serial monitor at 115200 to watch it join Wi-Fi and the WebSocket")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
