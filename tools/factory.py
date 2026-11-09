#!/usr/bin/env python3
"""One-command factory provisioning for a Powercell collector (fw 2.5.x).

    python tools\\factory.py COM11                 # bench unit: serial, API key, label
    python tools\\factory.py COM11 --register      # also register on api-dev (needs ADMIN_KEY)
    python tools\\factory.py COM11 --register --lock   # customer unit: register + LOCK
    python tools\\factory.py COM11 --dry-run       # read INFO, show the plan, change nothing
    py tools\\factory.py COM11 --serial ACE-000002 --reprovision --register --model PC5K
                                                # erased unit: restore its old serial (no counter bump)

What it does (real console commands from src/console.cpp):
  INFO -> parse state; refuse if locked, or if a serial is already set (unless --reuse)
  ROTATE_SECRET only if the unit has no factory secret (the board normally generates it itself)
  SET api_key / SET ota_url from factory_state/factory.env
  SERIAL ACE-NNNNNN from factory_state/serial_counter.json (counter bumps only after "OK serial set")
  --register: SECRET (read once, never printed) -> POST {API}/api/devices/register (X-Admin-Key)
              -> SET local_tok <token from the response>
  REBOOT -> INFO -> capture LABEL lines -> factory_state/labels/<serial>.txt
  --lock: LOCK (firmware requires serial + secret reported, i.e. --register) -> REBOOT

gw_name is NOT set: once a serial exists the firmware uses the serial (ACE-NNNNNN) as the gateway
name and Powercell-NNNNNN as the hotspot SSID (src/settings.cpp), and ignores SET gw_name.
Needs pyserial: pip install pyserial
"""
import argparse
import csv
import datetime
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_STATE = os.path.join(HERE, "factory_state")
DEFAULT_OTA = "https://api-dev.acecore.tech"
SERIAL_RE = re.compile(r"^ACE-\d{6}$")  # fid::validSerial
CSV_FIELDS = ["serial", "device_id", "mac", "gw_name", "ap_ssid", "fw", "registered", "locked", "timestamp"]


# ---------- pure helpers (unit tested) ----------
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


def parse_info(text):
    """Parse INFO output into a dict. Secret values are never present in INFO."""
    st = {"labels": []}
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("LABEL"):
            st["labels"].append(line)
            continue
        for k, v in re.findall(r"(\w+)=(\S*)", line):
            st.setdefault(k, v)
    return st


def format_serial(prefix, n):
    s = f"{prefix}{n:06d}"
    if not SERIAL_RE.match(s):
        raise ValueError(f"{s!r} is not a valid firmware serial (ACE-NNNNNN)")
    return s


def load_counter(path, prefix="ACE-"):
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            c = json.load(f)
    else:
        c = {"prefix": prefix, "next": 1}
    c.setdefault("prefix", prefix)
    c.setdefault("next", 1)
    return c


def save_counter(path, c):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(c, f, indent=2)
    os.replace(tmp, path)


def log_rows(csv_path, serial):
    if not os.path.exists(csv_path):
        return []
    with open(csv_path, newline="", encoding="utf-8") as f:
        return [r for r in csv.DictReader(f) if r.get("serial") == serial]


def _norm_id(v):
    return re.sub(r"[^0-9A-F]", "", str(v or "").upper())


def check_reprovision(csv_path, serial, device_id, unit_serial):
    """--serial X --reprovision safety check. Returns (ok, message).
    Allowed only for the same board: the unit must have no serial (erased) or already X,
    and every units.csv row for X must carry this device_id (the ESP32 MAC)."""
    if not SERIAL_RE.match(serial or ""):
        return False, f"{serial!r} is not a valid serial (ACE-NNNNNN)"
    if unit_serial not in ("unset", "", None, serial):
        return False, f"unit already has serial {unit_serial}, not {serial}"
    rows = log_rows(csv_path, serial)
    others = [r for r in rows if _norm_id(r.get("device_id")) != _norm_id(device_id)]
    if others:
        return False, (f"{serial} belongs to device_id {others[0].get('device_id')} in units.csv, "
                       f"this board is {device_id}; refusing (a serial is never moved to another board)")
    if not rows:
        return True, f"WARNING: {serial} not found in units.csv; the backend matches by device_id {device_id}"
    return True, f"{serial} was issued to this board ({device_id}) in units.csv"


def serial_in_log(csv_path, serial):
    if not os.path.exists(csv_path):
        return False
    with open(csv_path, newline="", encoding="utf-8") as f:
        return any(r.get("serial") == serial for r in csv.DictReader(f))


MODELS = ("PC3K", "PC5K", "PC10K")  # firmware pc::live::findModel (2.5.4)


def normalize_model(m):
    """'pc5k', '5K', '5kva' -> 'PC5K'; raises ValueError for anything else."""
    s = "".join(str(m).split()).upper()
    if s.endswith("KVA"):
        s = s[:-2]
    if not s.startswith("PC"):
        s = "PC" + s
    if s not in MODELS:
        raise ValueError(f"model must be one of {', '.join(MODELS)}")
    return s


def append_csv(csv_path, row):
    os.makedirs(os.path.dirname(csv_path), exist_ok=True)
    new = not os.path.exists(csv_path)
    with open(csv_path, "a", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        if new:
            w.writeheader()
        w.writerow({k: row.get(k, "") for k in CSV_FIELDS})


def pick_label(labels):
    """Return the onboarding label line (LABEL PWC1/PWC2;S=..;P=..;D=..)."""
    for l in labels:
        if re.match(r"^LABEL PWC\d;", l):
            return l[len("LABEL "):]
    return ""


# ---------- serial I/O ----------
class Console:
    def __init__(self, port_name, dry=False):
        import serial  # pyserial
        self.p = serial.Serial(port_name, 115200, timeout=0.2)
        self.dry = dry

    def boot_wait(self, secs=5.0):
        time.sleep(secs)
        self.p.reset_input_buffer()

    def info(self, wait=1.5):
        self.p.reset_input_buffer()
        self.p.write(b"INFO\n")
        end, buf = time.time() + wait, b""
        while time.time() < end:
            buf += self.p.read(512)
        return parse_info(buf.decode("utf-8", "replace"))

    def cmd(self, line, prefixes=("OK", "ERR"), timeout=4.0, show=None):
        """Send one command and return the first reply line starting with a prefix."""
        print(f"  > {show or line}")
        if self.dry:
            return "DRY"
        self.p.reset_input_buffer()
        self.p.write((line + "\n").encode())
        end, buf = time.time() + timeout, ""
        while time.time() < end:
            buf += self.p.read(256).decode("utf-8", "replace")
            for l in buf.splitlines():
                s = l.strip()
                if s.startswith(prefixes):
                    return s
        return "TIMEOUT"

    def raw(self, line):
        print(f"  > {line}")
        if not self.dry:
            self.p.write((line + "\n").encode())

    def close(self):
        self.p.close()


def register_backend(api, admin_key, payload):
    req = urllib.request.Request(api.rstrip("/") + "/api/devices/register", data=json.dumps(payload).encode(),
                                 headers={"Content-Type": "application/json", "X-Admin-Key": admin_key}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        raise RuntimeError(f"HTTP {e.code}: {e.read().decode(errors='replace')[:200]}")


def die(msg):
    print("FAILED: " + msg)
    sys.exit(1)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Powercell factory provisioning")
    ap.add_argument("port")
    ap.add_argument("--state", default=DEFAULT_STATE, help="factory_state folder (counter, env, csv, labels)")
    ap.add_argument("--env", help="default <state>/factory.env")
    ap.add_argument("--reuse", action="store_true", help="unit already has a serial: keep it, redo the rest")
    ap.add_argument("--register", action="store_true", help="register on the backend (POST /api/devices/register)")
    ap.add_argument("--lock", action="store_true", help="LOCK after labelling (customer units only; needs --register)")
    ap.add_argument("--model", help="Powercell model PC3K|PC5K|PC10K (fw 2.5.4+, SET model; stored in NVS)")
    ap.add_argument("--serial", help="with --reprovision: restore this serial (ACE-NNNNNN) on an erased board")
    ap.add_argument("--reprovision", action="store_true",
                    help="erased board: write --serial again (no counter bump), new secret; with --register the "
                         "backend upserts by device_id (MAC) and replaces the secret hash")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args(argv)
    if a.model:
        try:
            a.model = normalize_model(a.model)
        except ValueError as e:
            die(str(e))

    env_path = a.env or os.path.join(a.state, "factory.env")
    if not os.path.exists(env_path):
        die(f"{env_path} not found (copy factory.env.example to factory.env and fill API_KEY)")
    env = read_env(env_path)
    api_key, ota = env.get("API_KEY", ""), env.get("OTA_URL", DEFAULT_OTA)
    api = env.get("API_URL", ota)
    if not api_key:
        die("API_KEY missing in " + env_path)
    if a.register and not env.get("ADMIN_KEY"):
        die("--register needs ADMIN_KEY in " + env_path)
    if a.lock and not a.register:
        die("--lock needs --register (firmware refuses LOCK until the secret has been reported)")
    counter_path = os.path.join(a.state, "serial_counter.json")
    csv_path = os.path.join(a.state, "units.csv")

    con = Console(a.port, dry=a.dry_run)
    print(f"opening {a.port} (the board resets; waiting for boot)...")
    con.boot_wait()
    st = con.info()
    if "device_id" not in st:
        die("no INFO reply; close the Serial Monitor and check the port")
    print(f"unit device_id={st.get('device_id')} fw={st.get('fw')} serial={st.get('serial')} "
          f"secret={st.get('secret')} api_key={st.get('api_key')} locked={st.get('locked')}")
    if st.get("locked") == "1":
        die("unit is LOCKED; nothing to do (FACTORY_WIPE first to re-provision)")

    cur = st.get("serial", "unset")
    if a.serial or a.reprovision:
        if not (a.serial and a.reprovision):
            die("--serial and --reprovision go together")
        ok, msg = check_reprovision(csv_path, a.serial, st.get("device_id"), cur)
        print("reprovision: " + msg)
        if not ok:
            die(msg)
        serial, new_serial = a.serial, cur != a.serial
        c = None
    elif cur != "unset":
        if not a.reuse:
            die(f"unit already has serial {cur}; rerun with --reuse to keep it (no new serial is assigned)")
        serial, new_serial = cur, False
    else:
        c = load_counter(counter_path)
        serial = format_serial(c["prefix"], int(c["next"]))
        while serial_in_log(csv_path, serial):  # never double-assign
            c["next"] += 1
            serial = format_serial(c["prefix"], int(c["next"]))
        new_serial = True
    print(f"serial: {serial}{' (new)' if new_serial else ' (kept)'}")

    if st.get("secret") != "set":
        r = con.cmd("ROTATE_SECRET")
        if not (r.startswith("OK") or r == "DRY"):
            die("could not generate device secret: " + r)
    for key, val in (("api_key", api_key), ("ota_url", ota)):
        r = con.cmd(f"SET {key} {val}", show=f"SET {key} ****")
        if not (r.startswith("OK") or r == "DRY"):
            die(f"SET {key}: {r}")
    if a.model:
        r = con.cmd(f"SET model {a.model}")
        if not (r.startswith("OK model=") or r == "DRY"):
            die(f"SET model: {r} (firmware older than 2.5.4?)")
    if new_serial:
        r = con.cmd(f"SERIAL {serial}")
        if r != "DRY":
            if not r.startswith("OK serial set"):
                die("SERIAL: " + r)
            if c is not None:  # --reprovision keeps the counter
                c["next"] = int(c["next"]) + 1
                save_counter(counter_path, c)

    registered = False
    if a.register:
        r = con.cmd("SECRET", prefixes=("SECRET ", "ERR"), show="SECRET  (value kept in memory, not shown)")
        if r != "DRY":
            if not r.startswith("SECRET "):
                die("SECRET: " + r + " (already reported: the backend has it; use ROTATE_SECRET + rerun to re-register)")
            secret = r.split(" ", 1)[1].strip()
            payload = {"device_id": st["device_id"], "device_secret": secret, "mac": st.get("mac", ""),
                       "gateway_id": serial, "firmware_version": st.get("fw"), "device_kind": "collector"}
            try:
                resp = register_backend(api, env["ADMIN_KEY"], payload)
            except Exception as e:
                con.cmd("ROTATE_SECRET")  # secret left the unit but was not stored; make a fresh one
                die(f"backend registration failed ({e}); unit secret rotated, rerun with --reuse --register")
            finally:
                secret = payload["device_secret"] = None
            tok = (resp.get("local_access") or {}).get("token", "")
            if tok:
                r2 = con.cmd(f"SET local_tok {tok}", show="SET local_tok ****")
                print("  local token: " + r2.split(" fingerprint")[0])
            registered = True
            print("  registered on " + api)

    con.raw("REBOOT")
    if a.dry_run:
        st2 = st
    else:
        time.sleep(1)
        con.boot_wait(6)
        st2 = con.info()
    label = pick_label(st2.get("labels", []))
    if not label and not a.dry_run:
        die("no LABEL line after reboot (is the unit locked?)")

    locked = False
    if a.lock:
        r = con.cmd("LOCK")
        if not (r.startswith("OK") or r == "DRY"):
            die("LOCK: " + r)
        locked = r != "DRY"
        con.raw("REBOOT")
    con.close()

    if a.dry_run:
        print("\nDRY RUN: nothing written to the unit, counter or log.")
        return 0
    row = {"serial": serial, "device_id": st2.get("device_id"), "mac": st2.get("mac"), "gw_name": st2.get("gateway"),
           "ap_ssid": st2.get("ap_ssid"), "fw": st2.get("fw"), "registered": int(registered), "locked": int(locked),
           "timestamp": datetime.datetime.now().astimezone().isoformat(timespec="seconds")}
    append_csv(csv_path, row)
    os.makedirs(os.path.join(a.state, "labels"), exist_ok=True)
    with open(os.path.join(a.state, "labels", serial + ".txt"), "w", encoding="utf-8") as f:
        f.write(label + "\n")
        for l in st2.get("labels", []):
            if l.startswith("LABEL_QR"):
                f.write(l + "\n")
    print("\n" + "=" * 60)
    print(f"  {label}")
    print("=" * 60)
    print(f"serial={serial} hotspot={st2.get('ap_ssid')} registered={registered} locked={locked}")
    if not registered:
        print("NOT registered on the backend: rerun later with --reuse --register, or add it in the admin.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
