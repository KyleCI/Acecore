#!/usr/bin/env python3
"""Erase, flash and provision a Powercell collector from scratch, giving it the next free serial.

    py tools\\factory_flash.py --model pc-dsp011-5kw --port COM5                 # bench unit
    py tools\\factory_flash.py --model pc-dsp011-5kw --port COM5 --register      # + api-dev registration
    py tools\\factory_flash.py --model pc-dsp011-5kw --port COM5 --register --lock
    py tools\\factory_flash.py --model pc-dsp011-5kw --port COM5 --dry-run       # read MAC, show plan, change nothing
    py tools\\factory_flash.py --abandon ACE-000012 --reason "board dead"        # never issue that serial again
    py tools\\factory_flash.py --status                                          # journal summary

Steps (each one is written to factory_state/reservations.jsonl before the next starts):
  1. factory_state/factory.env must exist with API_KEY (and ADMIN_KEY for --register). Values are never printed.
  2. The merged image for --model is checked: SHA256 against SHA256SUMS, and the embedded
     POWERCELL_FW|collector|<model>|<version>|END identity string must match --model.
  3. esptool read_mac (the only thing read from the board before the plan).
  4. Serial choice. A MAC already in units.csv or in the journal gets the SAME serial back
     (factory.py --reprovision). Otherwise the next free number from serial_counter.json is
     reserved (journal "reserved", then the counter is bumped). With no serial_counter.json the
     first serial is ACE-000004 (Caleb 18 Nov 2026: ACE-000001..3 already issued); those three
     are journalled "pre_issued" and never issued again. --start-at N (N >= 4) overrides.
     Burned and already-issued serials are never reused.
  5. esptool erase_flash, then write_flash 0x0 <merged bin>, then verify_flash.   -> "flashed"
  6. tools/factory.py <port> --serial S --reprovision --model <PC5K|PC3_5K|PC1_5K> [--register] [--lock]
                                                                                    -> "provisioned"/"registered"
  7. INFO read back: serial, hardware_model, model, secret=set, api_key=set.       -> "verified"
  Any failure -> "failed" with the step name. The serial stays tied to that MAC; just rerun.

The COM port is opened by this script only after esptool has exited.
Needs pyserial (pip install pyserial) and esptool (PlatformIO's tool-esptoolpy, or pip install esptool).
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import factory  # noqa: E402  (same folder; reuses its tested helpers)

DEFAULT_STATE = os.path.join(HERE, "factory_state")
DEFAULT_BINS = os.path.join(os.path.dirname(HERE), "bins")
JOURNAL = "reservations.jsonl"
LOCKFILE = ".factory_flash.lock"
# "pre_issued": ACE-000001..3, issued before the journal; no MAC, never reissued (factory.PRE_ISSUED_SERIALS).
STATES = ("reserved", "flashed", "provisioned", "registered", "verified", "failed", "burned", "pre_issued")
# A serial whose board got this far has the serial written to it (or the backend knows it).
ISSUED_STATES = ("provisioned", "registered", "verified")

# OTA image model -> NVS model code (factory.py --model / SET model).
NVS_MODEL = {
    "pc-dsp011-5kw": "PC5K", "pc-stm01-5kw": "PC5K",
    "pc-dsp011-35kw": "PC3_5K", "pc-stm01-35kw": "PC3_5K",
    "pc-dsp011-15kw": "PC1_5K", "pc-stm01-15kw": "PC1_5K",
}
assert set(NVS_MODEL) == set(factory.OTA_MODELS)
FLASH_ARGS = ["--flash_mode", "dio", "--flash_freq", "40m", "--flash_size", "4MB"]
IDENT_RE = re.compile(rb"POWERCELL_FW\|([a-z]+)\|([a-z0-9-]+)\|([0-9][0-9A-Za-z.\-]*)\|END")


class FlashError(Exception):
    pass


# ---------- pure helpers (unit tested) ----------
def nvs_model_for(ota_model):
    if ota_model not in NVS_MODEL:
        raise ValueError("--model must be one of " + ", ".join(sorted(NVS_MODEL)))
    return NVS_MODEL[ota_model]


def norm_mac(mac):
    """'f4:2d:c9:c0:90:a8' -> 'F42DC9C090A8' (the firmware device_id)."""
    h = re.sub(r"[^0-9A-Fa-f]", "", str(mac or "")).upper()
    if len(h) != 12:
        raise ValueError("bad MAC %r" % (mac,))
    return h


def parse_read_mac(text):
    m = re.search(r"MAC:\s*([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})", text or "")
    if not m:
        raise FlashError("esptool read_mac printed no MAC")
    return norm_mac(m.group(1))


def image_identities(data):
    return [(k.decode(), m.decode(), v.decode()) for k, m, v in IDENT_RE.findall(data)]


def check_image(data, ota_model):
    """Return the firmware version if the image identifies as exactly one collector/<ota_model>."""
    ids = set(image_identities(data))
    if not ids:
        raise FlashError("image has no POWERCELL_FW identity string")
    models = {m for _, m, _ in ids}
    if models != {ota_model} or {k for k, _, _ in ids} != {"collector"}:
        raise FlashError("image identifies as %s, not %s" % (sorted(models), ota_model))
    versions = {v for _, _, v in ids}
    if len(versions) != 1:
        raise FlashError("image carries several versions %s" % sorted(versions))
    return versions.pop()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_sums(path):
    sums = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 2:
                sums[parts[-1].lstrip("*").replace("\\", "/").split("/")[-1]] = parts[0].lower()
    return sums


def find_image(bins_dir, ota_model):
    names = sorted(n for n in os.listdir(bins_dir) if n.startswith(ota_model + "_") and n.endswith("_merged.bin"))
    # "pc-dsp011-5kw_" never matches "pc-dsp011-15kw_..." (prefix includes the underscore).
    if len(names) != 1:
        raise FlashError("expected one %s_*_merged.bin in %s, found %d" % (ota_model, bins_dir, len(names)))
    return os.path.join(bins_dir, names[0])


def read_journal(path):
    rows = []
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for n, line in enumerate(f, 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    rows.append(json.loads(line))
                except ValueError:
                    raise FlashError("%s line %d is not JSON; fix it by hand, never delete the file" % (path, n))
    return rows


def journal_view(rows):
    """serial -> {"mac", "model", "state", "history"} using the last event per serial."""
    out = {}
    for r in rows:
        s = r.get("serial")
        if not s:
            continue
        cur = out.setdefault(s, {"mac": r.get("mac"), "model": r.get("model"), "state": None, "history": []})
        if r.get("mac"):
            cur["mac"] = r["mac"]
        cur["state"] = r.get("state")
        cur["history"].append(r.get("state"))
    return out


def append_journal(path, event):
    if event.get("state") not in STATES:
        raise ValueError("bad state %r" % (event.get("state"),))
    event = dict(event)
    event.setdefault("ts", datetime.datetime.now().astimezone().isoformat(timespec="seconds"))
    line = json.dumps(event, sort_keys=True) + "\n"
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "a", encoding="utf-8") as f:
        f.write(line)
        f.flush()
        os.fsync(f.fileno())


def csv_rows(csv_path):
    import csv
    if not os.path.exists(csv_path):
        return []
    with open(csv_path, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def plan_serial(mac, units, journal_rows, counter, start_at=None):
    """Decide the serial for this board. Pure: changes nothing.

    Returns dict(serial, mode, reason, next_counter). mode is "reuse" (same MAC seen before)
    or "new" (caller must journal "reserved" and save next_counter before flashing).
    counter is the loaded serial_counter.json dict, or None if the file does not exist.
    """
    mac = norm_mac(mac)
    for r in units:
        if factory._norm_id(r.get("device_id")) == mac and factory.SERIAL_RE.match(r.get("serial") or ""):
            return {"serial": r["serial"], "mode": "reuse", "next_counter": None,
                    "reason": "this board (%s) is %s in units.csv" % (mac, r["serial"])}
    view = journal_view(journal_rows)
    for s, v in view.items():
        if v["mac"] == mac and v["state"] != "burned":
            return {"serial": s, "mode": "reuse", "next_counter": None,
                    "reason": "this board (%s) holds %s in the journal (last state %s)" % (mac, s, v["state"])}
    if start_at is not None and int(start_at) < factory.MIN_NEW_SERIAL:
        raise FlashError("--start-at %d refused: %s were already issued, new serials start at %d"
                         % (int(start_at), ", ".join(factory.PRE_ISSUED_SERIALS), factory.MIN_NEW_SERIAL))
    if counter is None:
        counter = {"prefix": "ACE-", "next": int(start_at or factory.MIN_NEW_SERIAL)}
    elif start_at is not None and int(start_at) < int(counter["next"]):
        raise FlashError("--start-at %d is below the counter's next (%s); refusing to go backwards"
                         % (int(start_at), counter["next"]))
    n = max(int(counter["next"]), int(start_at or 0), factory.MIN_NEW_SERIAL)
    used = {r.get("serial") for r in units} | set(view) | set(factory.PRE_ISSUED_SERIALS)
    serial = factory.format_serial(counter.get("prefix", "ACE-"), n)
    while serial in used:
        n += 1
        serial = factory.format_serial(counter.get("prefix", "ACE-"), n)
    nc = dict(counter)
    nc["next"] = n + 1
    return {"serial": serial, "mode": "new", "next_counter": nc, "reason": "next free serial"}


def pre_issued_events(journal_rows):
    """Journal events still needed to record ACE-000001..3 as pre_issued (none once recorded)."""
    view = journal_view(journal_rows)
    return [{"serial": s, "mac": None, "model": None, "state": "pre_issued",
             "note": "issued before the factory_flash journal (Caleb 18 Nov 2026); never reissued"}
            for s in factory.PRE_ISSUED_SERIALS if s not in view]


def abandon(journal_path, units, serial, reason):
    """Mark a serial burned so it is never issued again. Refused for a serial in units.csv."""
    if not factory.SERIAL_RE.match(serial or ""):
        raise FlashError("%r is not ACE-NNNNNN" % (serial,))
    if any(r.get("serial") == serial for r in units):
        raise FlashError("%s is in units.csv (a provisioned unit); it cannot be abandoned here" % serial)
    view = journal_view(read_journal(journal_path))
    if serial not in view:
        raise FlashError("%s is not in the journal" % serial)
    if view[serial]["state"] == "burned":
        return False
    append_journal(journal_path, {"serial": serial, "mac": view[serial]["mac"], "model": view[serial]["model"],
                                  "state": "burned", "note": reason or "abandoned"})
    return True


def factory_args(port, serial, nvs_model, state_dir, env_path, register, lock):
    a = [port, "--serial", serial, "--reprovision", "--model", nvs_model, "--state", state_dir, "--env", env_path]
    if register:
        a.append("--register")
    if lock:
        a.append("--lock")
    return a


def check_readback(info, serial, ota_model, nvs_model):
    """Return a list of problems in the INFO read back after provisioning (empty = good)."""
    probs = []
    if info.get("serial") != serial:
        probs.append("serial=%s (want %s)" % (info.get("serial"), serial))
    if info.get("hardware_model") != ota_model:
        probs.append("hardware_model=%s (want %s)" % (info.get("hardware_model"), ota_model))
    if info.get("model") != nvs_model:
        probs.append("model=%s (want %s)" % (info.get("model"), nvs_model))
    if info.get("secret") != "set":
        probs.append("secret=%s" % info.get("secret"))
    if info.get("api_key") != "set":
        probs.append("api_key=%s" % info.get("api_key"))
    return probs


def check_env(env_path, register):
    if not os.path.exists(env_path):
        raise FlashError("%s not found: copy factory.env.example to factory.env and fill it in" % env_path)
    env = factory.read_env(env_path)
    if not factory._filled(env.get("API_KEY")):
        raise FlashError("API_KEY is empty in " + env_path)
    if register and not factory._filled(env.get("ADMIN_KEY")):
        raise FlashError("--register needs ADMIN_KEY in " + env_path)
    return env


class StateLock:
    """Exclusive lock file in factory_state so two runs never hand out the same serial."""

    def __init__(self, state_dir):
        self.path = os.path.join(state_dir, LOCKFILE)
        self.fd = None

    def __enter__(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        try:
            self.fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        except FileExistsError:
            raise FlashError("%s exists: another factory_flash run is active. If none is, delete that file." % self.path)
        os.write(self.fd, ("pid=%d\n" % os.getpid()).encode())
        return self

    def __exit__(self, *exc):
        if self.fd is not None:
            os.close(self.fd)
            os.remove(self.path)


# ---------- esptool ----------
def esptool_cmd(override=None):
    """PlatformIO's bundled esptool first, then `python -m esptool`."""
    if override:
        return override.split()
    pio = shutil.which("pio") or shutil.which("platformio")
    if not pio:
        for cand in (os.path.expanduser(r"~\.platformio\penv\Scripts\pio.exe"),
                     os.path.expanduser("~/.platformio/penv/bin/pio")):
            if os.path.exists(cand):
                pio = cand
                break
    if pio:
        return [pio, "pkg", "exec", "-p", "tool-esptoolpy", "--", "esptool.py"]
    try:
        import esptool  # noqa: F401
        return [sys.executable, "-m", "esptool"]
    except ImportError:
        raise FlashError("esptool not found: install PlatformIO, or run: py -m pip install esptool")


def run_esptool(base, port, args, baud):
    cmd = base + ["--chip", "esp32", "--port", port, "--baud", str(baud)] + args
    print("  $ esptool " + " ".join(cmd[len(base):]))
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    out = p.stdout or ""
    if p.returncode != 0:
        tail = "\n".join(out.strip().splitlines()[-8:])
        raise FlashError("esptool %s failed (exit %d):\n%s" % (args[0], p.returncode, tail))
    return out


# ---------- main ----------
def main(argv=None):
    ap = argparse.ArgumentParser(description="Erase + flash + provision a Powercell with the next serial")
    ap.add_argument("--model", choices=sorted(NVS_MODEL), help="image model, e.g. pc-dsp011-5kw")
    ap.add_argument("--port", help="e.g. COM5")
    ap.add_argument("--register", action="store_true", help="register on the backend (needs ADMIN_KEY)")
    ap.add_argument("--lock", action="store_true", help="LOCK after labelling (customer units; needs --register)")
    ap.add_argument("--dry-run", action="store_true", help="read the MAC and print the plan; write nothing")
    ap.add_argument("--start-at", type=int, help="first serial number (>= 4; default 4 when there is no serial_counter.json)")
    ap.add_argument("--abandon", metavar="ACE-NNNNNN", help="mark a journal serial burned (never reissued)")
    ap.add_argument("--reason", default="", help="note for --abandon")
    ap.add_argument("--status", action="store_true", help="print the journal summary")
    ap.add_argument("--state", default=DEFAULT_STATE)
    ap.add_argument("--env", help="default <state>/factory.env")
    ap.add_argument("--bins", default=DEFAULT_BINS, help="folder with <model>_v<ver>_merged.bin + SHA256SUMS")
    ap.add_argument("--baud", type=int, default=460800, help="esptool baud (use 115200 if 460800 fails)")
    ap.add_argument("--esptool", help="override the esptool command, e.g. \"py -m esptool\"")
    a = ap.parse_args(argv)

    jpath = os.path.join(a.state, JOURNAL)
    cpath = os.path.join(a.state, "serial_counter.json")
    upath = os.path.join(a.state, "units.csv")
    try:
        if a.status:
            for s, v in sorted(journal_view(read_journal(jpath)).items()):
                print("%s  %-11s mac=%s model=%s" % (s, v["state"], v["mac"], v["model"]))
            return 0
        if a.abandon:
            with StateLock(a.state):
                done = abandon(jpath, csv_rows(upath), a.abandon, a.reason)
            print(("%s marked burned; it will never be issued again." if done else "%s was already burned.") % a.abandon)
            return 0
        if not a.model or not a.port:
            ap.error("--model and --port are required")
        if a.lock and not a.register:
            raise FlashError("--lock needs --register")
        if a.start_at is not None and a.start_at < factory.MIN_NEW_SERIAL:
            raise FlashError("--start-at %d refused: %s were already issued, new serials start at %d"
                             % (a.start_at, ", ".join(factory.PRE_ISSUED_SERIALS), factory.MIN_NEW_SERIAL))
        nvs_model = nvs_model_for(a.model)
        env_path = a.env or os.path.join(a.state, "factory.env")
        check_env(env_path, a.register)
        print("factory.env OK (values not shown)")

        image = find_image(a.bins, a.model)
        sums_path = os.path.join(a.bins, "SHA256SUMS")
        if not os.path.exists(sums_path):
            raise FlashError("SHA256SUMS missing in " + a.bins)
        want = read_sums(sums_path).get(os.path.basename(image))
        got = sha256_file(image)
        if not want or want != got:
            raise FlashError("%s SHA256 %s does not match SHA256SUMS (%s)" % (os.path.basename(image), got, want))
        with open(image, "rb") as f:
            version = check_image(f.read(), a.model)
        print("image %s OK: %s v%s sha256 %s..%s" % (os.path.basename(image), a.model, version, got[:8], got[-4:]))

        base = esptool_cmd(a.esptool)
        mac = parse_read_mac(run_esptool(base, a.port, ["read_mac"], a.baud))
        print("board MAC / device_id: " + mac)

        with StateLock(a.state):
            counter = factory.load_counter(cpath) if os.path.exists(cpath) else None
            plan = plan_serial(mac, csv_rows(upath), read_journal(jpath), counter, a.start_at)
            serial = plan["serial"]
            print("serial: %s (%s; %s)" % (serial, plan["mode"], plan["reason"]))
            fargs = factory_args(a.port, serial, nvs_model, a.state, env_path, a.register, a.lock)
            if a.dry_run:
                print("\nDRY RUN. Would: erase_flash, write_flash 0x0 %s, verify_flash,\n  factory.py %s\n"
                      "Nothing was written to the board, counter or journal." % (os.path.basename(image), " ".join(fargs)))
                return 0
            for pe in pre_issued_events(read_journal(jpath)):
                append_journal(jpath, pe)
            ev = {"serial": serial, "mac": mac, "model": a.model, "version": version}
            if plan["mode"] == "new":
                append_journal(jpath, dict(ev, state="reserved"))   # journal first, then the counter
                factory.save_counter(cpath, plan["next_counter"])
            else:
                append_journal(jpath, dict(ev, state="reserved", note="retry for the same board"))

            step = "erase_flash"
            try:
                print("\n[1/4] erasing the whole flash (serial, secret, settings) ...")
                run_esptool(base, a.port, ["erase_flash"], a.baud)
                step = "write_flash"
                print("[2/4] writing %s at 0x0 ..." % os.path.basename(image))
                run_esptool(base, a.port, ["write_flash"] + FLASH_ARGS + ["0x0", image], a.baud)
                step = "verify_flash"
                run_esptool(base, a.port, ["verify_flash", "0x0", image], a.baud)
                append_journal(jpath, dict(ev, state="flashed"))

                step = "factory.py"
                print("[3/4] provisioning with factory.py (opens %s now that esptool has exited) ..." % a.port)
                time.sleep(2)
                p = subprocess.run([sys.executable, os.path.join(HERE, "factory.py")] + fargs)
                if p.returncode != 0:
                    raise FlashError("factory.py exited %d (see its FAILED line above)" % p.returncode)
                append_journal(jpath, dict(ev, state="registered" if a.register else "provisioned"))

                step = "readback"
                print("[4/4] reading INFO back ...")
                con = factory.Console(a.port)
                try:
                    con.boot_wait(6)
                    info = con.info(2.5)
                finally:
                    con.close()
                probs = check_readback(info, serial, a.model, nvs_model)
                if probs:
                    raise FlashError("INFO check: " + "; ".join(probs))
                append_journal(jpath, dict(ev, state="verified"))
            except (FlashError, OSError, ImportError) as e:
                append_journal(jpath, dict(ev, state="failed", note="%s: %s" % (step, str(e).splitlines()[0][:200])))
                raise FlashError("%s\n%s stays tied to this board (%s). Fix the cause and run the same command "
                                 "again; it will reuse %s. If the board is dead: --abandon %s"
                                 % (e, serial, mac, serial, serial))

        label_path = os.path.join(a.state, "labels", serial + ".txt")
        print("\n" + "=" * 60)
        print("DONE  %s  %s  v%s  NVS model %s  device_id %s" % (serial, a.model, version, nvs_model, mac))
        print("hotspot %s  registered=%s locked=%s" % (info.get("ap_ssid"), a.register, a.lock))
        if os.path.exists(label_path):
            print("label file: " + label_path)
        print("=" * 60)
        return 0
    except FlashError as e:
        print("FAILED: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
