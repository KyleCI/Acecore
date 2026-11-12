#!/usr/bin/env python3
"""Upload a collector firmware to a Powercell over its hotspot or LAN (POST /ota).

Usage:  py ota_upload.py <firmware.bin> --model pc-dsp011-5kw [--host 192.168.4.1] [--force] [--bearer]

--model is required and must be the unit's model (pc-dsp011-5kw, pc-dsp011-35kw,
pc-stm01-5kw, pc-stm01-35kw). The file must carry that model's identity; the unit
checks it again and refuses another model's image.
The version must differ from the running one. An older version needs --force.
Asks for the local token (the unit's local password) with a hidden prompt.
Default auth is Basic user "powercell"; --bearer sends it as a lan/session token instead.
Standard library only. Never prints the token.
"""
import argparse, base64, getpass, hashlib, http.client, json, os, re, sys, time

MODELS = ("pc-dsp011-5kw", "pc-dsp011-35kw", "pc-stm01-5kw", "pc-stm01-35kw")
TAG = re.compile(rb"POWERCELL_FW\|collector\|([a-z0-9-]{1,32})\|([0-9.]{1,16})\|END")


def identities(data):
    return [(m.group(1).decode(), m.group(2).decode()) for m in TAG.finditer(data)]


def main():
    ap = argparse.ArgumentParser(description="Powercell local OTA upload")
    ap.add_argument("firmware")
    ap.add_argument("--model", required=True, choices=MODELS, help="the unit's model (never assumed)")
    ap.add_argument("--host", default="192.168.4.1")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--user", default="powercell")
    ap.add_argument("--force", action="store_true", help="allow an older version (same version is always refused)")
    ap.add_argument("--bearer", action="store_true", help="send the secret as a Bearer lan token")
    a = ap.parse_args()

    data = open(a.firmware, "rb").read()
    if not data or data[0] != 0xE9:
        sys.exit("Not an ESP32 app image (first byte is not 0xE9). Use firmware.bin, not the merged bin.")
    ids = [i for i in identities(data) if i[0] == a.model]
    if not ids:
        found = ", ".join(sorted({m for m, _ in identities(data)})) or "none"
        sys.exit(f"This image is not built for {a.model} (identity found: {found}).")
    tag = ids[0][1]
    sha = hashlib.sha256(data).hexdigest()
    print(f"File:    {os.path.basename(a.firmware)}  {len(data)} bytes")
    print(f"Model:   {a.model}")
    print(f"Version: {tag}")
    print(f"SHA-256: {sha}")

    secret = getpass.getpass("Local token / password (hidden): ").strip()
    if not secret:
        sys.exit("No token entered.")
    auth = ("Bearer " + secret) if a.bearer else ("Basic " + base64.b64encode(f"{a.user}:{secret}".encode()).decode())
    del secret

    path = "/ota" + ("?force=1" if a.force else "")
    conn = http.client.HTTPConnection(a.host, a.port, timeout=120)
    try:
        conn.putrequest("POST", path)
        conn.putheader("Authorization", auth)
        conn.putheader("Content-Type", "application/octet-stream")
        conn.putheader("Content-Length", str(len(data)))
        conn.putheader("X-Firmware-SHA256", sha)
        conn.endheaders()
        sent, step, t0 = 0, 4096, time.time()
        while sent < len(data):
            conn.send(data[sent:sent + step])
            sent += min(step, len(data) - sent)
            pct = sent * 100 // len(data)
            sys.stdout.write(f"\rUploading {pct:3d}%  {sent // 1024} / {len(data) // 1024} KB  {time.time() - t0:5.1f} s")
            sys.stdout.flush()
        print()
        print("Verifying on the unit...")
        resp = conn.getresponse()
        body = resp.read().decode("utf-8", "replace")
    except (OSError, http.client.HTTPException) as e:
        sys.exit(f"\nUpload failed: {e}. Is the laptop on the Powercell hotspot ({a.host})?")
    finally:
        conn.close()
    try:
        msg = json.loads(body)
    except ValueError:
        msg = {"raw": body[:200]}
    if resp.status == 200 and msg.get("ok"):
        print(f"OK: installed {tag} (was {msg.get('from')}). The unit reboots now.")
        print("Online it confirms after 30 s connected to the server; hotspot-only after 3 healthy minutes.")
        print("If it crashes or never gets healthy within 5 minutes it rolls back on its own.")
        return 0
    print(f"FAILED: HTTP {resp.status} {msg.get('error', msg)}")
    hints = {401: "wrong token/password (or the unit is still in setup)",
             409: "battery critical, busy, image not yet confirmed, or same/older version (--force allows older)",
             422: "SHA-256 or image identity/model mismatch: wrong file, wrong model, or corrupted",
             413: "image too big for the app slot"}
    if resp.status in hints:
        print("Hint: " + hints[resp.status])
    return 1


if __name__ == "__main__":
    sys.exit(main())
