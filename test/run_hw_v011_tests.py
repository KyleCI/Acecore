#!/usr/bin/env python3
"""Host checks for hardware v0.1.1. No device, flash, or network."""
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]

def reject_case_folded_headers():
    """Windows opens LoRa.h as lora.h. A project header must not use that name."""
    project = {path.name.casefold(): path.name for path in (root / "include").glob("*.h")}
    requested = set()
    for folder in (root / "include", root / "src", root / "lib"):
        for path in folder.rglob("*"):
            if path.suffix not in {".h", ".cpp"}:
                continue
            requested.update(re.findall(r'#include\s*[<"]([^>"]+)[>"]', path.read_text(errors="replace")))
    collisions = []
    for name in requested:
        base = pathlib.Path(name).name
        existing = project.get(base.casefold())
        if existing and existing != base:
            collisions.append(f"{existing} collides with {base}")
    if collisions:
        sys.exit("case-insensitive include collision:\n" + "\n".join(collisions))

reject_case_folded_headers()
compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
if not compiler:
    sys.exit("A C++ compiler is required.")

sources = [
    root / "src/memory_byte_store.cpp",
    root / "src/slot_log.cpp",
    root / "src/sd_log.cpp",
    root / "src/pzem_v3.cpp",
    root / "src/lagos_time.cpp",
    root / "src/ds3231_time.cpp",
    root / "src/ac_energy_log.cpp",
    root / "src/diag_blackbox.cpp",
    root / "src/serial_spool.cpp",
    root / "test/hw_v011_tests.cpp",
]


def compile_one(args, label):
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        sys.exit(f"{label} failed")
    return result


with tempfile.TemporaryDirectory(prefix="hw-v011-") as directory:
    binary = pathlib.Path(directory) / "hw-v011-tests"
    compile_one([
        compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-I", str(root / "include"), *[str(path) for path in sources],
        "-o", str(binary),
    ], "host tests")
    subprocess.run([str(binary)], check=True)

    wifi_bin = pathlib.Path(directory) / "wifi-fallback-tests"
    compile_one([
        compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-I", str(root / "include"),
        str(root / "src/wifi_fallback.cpp"),
        str(root / "test/wifi_fallback_test.cpp"),
        "-o", str(wifi_bin),
    ], "wifi fallback tests")
    subprocess.run([str(wifi_bin)], check=True)

    settings_bin = pathlib.Path(directory) / "battery-settings-tests"
    compile_one([
        compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-I", str(root / "include"),
        str(root / "src/battery_settings.cpp"),
        str(root / "test/battery_settings_test.cpp"),
        "-o", str(settings_bin),
    ], "battery settings tests")
    subprocess.run([str(settings_bin)], check=True)

    ac_bin = pathlib.Path(directory) / "ac-output-tests"
    compile_one([
        compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-I", str(root / "include"),
        str(root / "src/ac_output_policy.cpp"),
        str(root / "src/ac_output_settings.cpp"),
        str(root / "src/battery_settings.cpp"),
        str(root / "test/ac_output_policy_test.cpp"),
        "-o", str(ac_bin),
    ], "ac output tests")
    subprocess.run([str(ac_bin)], check=True)

    factory_bin = pathlib.Path(directory) / "factory-contract-tests"
    compile_one([
        compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-I", str(root / "include"),
        "-I", str(root / "lib/pc_onb/src"),
        str(root / "lib/pc_onb/src/sha256.cpp"),
        str(root / "src/factory_station.cpp"),
        str(root / "src/cloud_protocol.cpp"),
        str(root / "test/factory_contract_test.cpp"),
        "-o", str(factory_bin),
    ], "factory contract tests")
    subprocess.run([str(factory_bin)], check=True)
    import importlib.util
    spec = importlib.util.spec_from_file_location("factory_tool", root / "tools" / "factory.py")
    factory_tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(factory_tool)
    proc = subprocess.Popen([str(factory_bin), "repl"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    def station(line):
        proc.stdin.write(line + "\n")
        proc.stdin.flush()
        lines = []
        while True:
            got = proc.stdout.readline()
            if got.strip() == ".":
                break
            lines.append(got)
        return "".join(lines)
    info = station("INFO")
    assert "LABEL" not in info
    assert station("SERIAL ACE-000123").startswith("OK serial set")
    assert station("SET model PC5K").startswith("OK model=")
    assert station("SET model PC3K").startswith("ERR")
    assert station("SET api_key test-key").startswith("OK")
    assert station("SET ota_url https://api.acecore.tech").startswith("OK")
    secret = station("SECRET")
    assert secret.startswith("SECRET ")
    labeled = station("INFO")
    parsed = factory_tool.parse_info(labeled)
    assert parsed.get("serial") == "ACE-000123"
    assert factory_tool.pick_label(parsed["labels"]).startswith("PWC2;S=Powercell-000123;")
    assert "LABEL_QR WIFI:T:WPA;S:Powercell-000123;" in labeled
    assert station("LOCK").startswith("OK locked")
    assert "LABEL" not in station("INFO")
    proc.stdin.close()
    proc.wait(timeout=5)

    manual_bin = pathlib.Path(directory) / "manual-api-tests"
    compile_one([
        compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-I", str(root / "include"),
        "-I", str(root / "lib/pc_onb/src"),
        str(root / "lib/pc_onb/src/sha256.cpp"),
        str(root / "lib/pc_onb/src/onb_core.cpp"),
        str(root / "src/lan_access.cpp"),
        str(root / "src/battery_settings.cpp"),
        str(root / "src/ac_output_settings.cpp"),
        str(root / "src/factory_station.cpp"),
        str(root / "src/local_manual_api.cpp"),
        str(root / "src/wall_clock.cpp"),
        str(root / "src/home_snapshot.cpp"),
        str(root / "test/local_manual_api_test.cpp"),
        "-o", str(manual_bin),
    ], "manual api tests")
    subprocess.run([str(manual_bin)], check=True)

    lan_bin = pathlib.Path(directory) / "lan-access-tests"
    compile_one([
        compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-c",
        "-I", str(root / "include"),
        "-I", str(root / "lib/pc_onb/src"),
        str(root / "src/lan_access.cpp"),
        "-o", str(pathlib.Path(directory) / "lan_access.o"),
    ], "lan access compile")

    def expect_build(defines, ok, label):
        probe = pathlib.Path(directory) / "probe"
        cmd = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
               "-I", str(root / "include"), "-c", str(root / "test/sd_cs_probe.cpp"),
               "-o", str(probe)]
        cmd.extend(defines)
        result = subprocess.run(cmd, capture_output=True, text=True)
        if ok and result.returncode != 0:
            sys.stderr.write(result.stderr)
            sys.exit(f"{label} should compile")
        if not ok and result.returncode == 0:
            sys.exit(f"{label} should be rejected")
        if not ok and "SD chip select" not in result.stderr and "PZEM" not in result.stderr:
            sys.stderr.write(result.stderr)
            sys.exit(f"{label} failed without the pin diagnostic")

    expect_build(["-DPOWERCELL_HW_V011=1", "-DPOWERCELL_SD_CS=33"], True, "v0.1.1 CS GPIO33")
    expect_build(["-DPOWERCELL_SD_CS=33"], False, "original profile CS GPIO33")
    expect_build(["-DPOWERCELL_HW_V011=1", "-DPOWERCELL_SD_CS=25"], False, "v0.1.1 CS on PZEM RX")
    expect_build(["-DPOWERCELL_HW_V011=1", "-DPZEM_RX_GPIO=33", "-DPZEM_TX_GPIO=26"], False,
                 "PZEM RX on the SD select")

    def code_only(path):
        kept = []
        for line in path.read_text().splitlines():
            if line.lstrip().startswith("//"):
                continue
            kept.append(line)
        return "\n".join(kept)

    battery = code_only(root / "src/offline_battery_notifications.cpp")
    provisioning = code_only(root / "src/provisioning.cpp")
    http = code_only(root / "src/local_http.cpp")
    if 'server.on("/data"' not in battery or "addHandler(&events)" not in battery:
        sys.exit("GET /data and /events must be mounted, not left commented")
    if "addMiddleware" not in battery or "home_snapshot_write" not in battery:
        sys.exit("mounted /data must use the snapshot serializer and event auth")
    if 'server.on("/data"' in provisioning or "addHandler(&events)" in provisioning:
        sys.exit("provisioning.cpp must not register a second /data or /events")
    for route in ("/state", "/relay", "/v2/time", "/v2/setup/info", "/v2/login",
                  "/v1/system-rating", "/v1/battery-settings", "/v1/ac-output-settings"):
        if route not in http:
            sys.exit(f"missing mounted route {route}")

    snap_bin = pathlib.Path(directory) / "home-snapshot-tests"
    compile_one([
        compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-I", str(root / "include"),
        str(root / "src/home_snapshot.cpp"),
        str(root / "test/home_snapshot_test.cpp"),
        "-o", str(snap_bin),
    ], "home snapshot serializer")
    printed = subprocess.run([str(snap_bin)], check=True, capture_output=True, text=True).stdout
    snaps = {}
    for line in printed.splitlines():
        if not line.startswith("SNAP "):
            continue
        name, payload = line.split(" ", 2)[1:]
        snaps[name] = json.loads(payload)
    forbidden = {
        "grid_voltage", "gridVoltage", "ac_input_voltage",
        "energy_today_kwh", "energy_today", "today_energy", "today_kwh", "days",
    }

    def walk(node, where):
        if isinstance(node, dict):
            for key, value in node.items():
                if key in forbidden:
                    sys.exit(f"{where} publishes {key}")
                walk(value, where + "." + key)
        elif isinstance(node, list):
            for item in node:
                walk(item, where)

    def near(value, expected):
        if abs(value - expected) > 1e-3:
            sys.exit(f"{value} != {expected}")

    for name, doc in snaps.items():
        walk(doc, name)
        if "rated_kva" in doc:
            sys.exit(f"{name} invents rated_kva")
    fresh = snaps["fresh"]
    near(fresh["last_data"]["power"], 15)
    near(fresh["inverter"]["output_voltage"], 230)
    near(fresh["inverter"]["output_current"], 0.1)
    near(fresh["inverter"]["output_power"], 23)
    if fresh["inverter"]["output_power_quantity"] != "apparent_va":
        sys.exit("output_power must stay apparent VA")
    near(fresh["metrics"]["output_power_w"], 15)
    near(fresh["live"]["output_power_w"], 15)
    near(fresh["metrics"]["inverterLoad"], 23)
    if fresh["metrics"]["inverterLoad"] == fresh["metrics"]["output_power_w"]:
        sys.exit("apparent VA was copied into the active watt key")
    if fresh["metrics"]["output_power_source"] != "pzem004t_v3_active":
        sys.exit("active watts need their source")
    for key in ("output_power", "power", "current", "voltage"):
        if key in fresh["metrics"] or key in fresh["live"]:
            sys.exit(f"metrics/live must not carry bare {key}")
    near(fresh["solar"]["power"], 72)
    near(fresh["solar"]["power_w"], 72)
    if fresh["solar"]["power"] < 1:
        sys.exit("solar.power is watts, not kW")
    near(fresh["battery"]["soc"], 40)
    near(fresh["battery"]["power"], 76.8)
    near(fresh["ac_consumed"]["lifetime_wh"], 1200)
    near(fresh["usable_kw"], 4.5)
    if fresh["powercell_model"] != "PC5K" or fresh["inverter_temperature"] is not None:
        sys.exit("5kW model or temperature shape is wrong")
    if fresh["inverter"]["stm32_online"] is not False:
        sys.exit("v0.1.1 must not claim an STM32")
    stale = snaps["stale"]
    if stale["last_data"]["power"] is not None or stale["metrics"]["output_power_w"] is not None:
        sys.exit("stale active power must be null")
    if "output_power" in stale["inverter"] or stale["metrics"]["inverterLoad"] is not None:
        sys.exit("stale apparent power must not remain as a number")
    if stale["live"]["output_power_w"] is not None or stale["battery"]["soc"] is not None:
        sys.exit("stale live power or BMS SOC must be null")
    if stale["battery"]["power"] not in (None,):
        sys.exit("stale battery power must be null")
    if stale["solar"].get("valid") is not False or "power" in stale["solar"] or "power_w" in stale["solar"]:
        sys.exit("missing PV must not publish a solar power")
    if stale["ac_consumed"]["lifetime_wh"] is not None:
        sys.exit("stale lifetime Wh must be null")
    small = snaps["kw35"]
    near(small["usable_kw"], 3.15)
    near(small["metrics"]["usable_kw"], 3.15)
    if small["powercell_model"] is not None or small["device_id"] is not None:
        sys.exit("3.5kW model stays null and a MAC is not the device id")
    encoded = json.dumps(small)
    for banned in ("PC3K", "PC5K", "PC10K", "rated_kva"):
        if banned in encoded:
            sys.exit(f"3.5kW document contains {banned}")
    converted = snaps["from_kw"]
    near(converted["last_data"]["power"], 15)
    near(converted["metrics"]["output_power_w"], 15)
    near(converted["live"]["output_power_w"], 15)
    near(converted["inverter"]["output_power"], 23)
    if converted["last_data"]["power"] == 0.015:
        sys.exit("a kW source was published without multiplying by 1000")
    stm32 = snaps["stm32"]
    if stm32["last_data"]["power"] is not None or stm32["metrics"]["output_power_w"] is not None:
        sys.exit("STM32 text has no active watts")
    if stm32["live"]["output_power_w"] is not None:
        sys.exit("STM32 apparent VA must not fill output_power_w")
    near(stm32["inverter"]["output_power"], 345)
    near(stm32["metrics"]["inverterLoad"], 345)
    if stm32["inverter"]["output_power_quantity"] != "apparent_va":
        sys.exit("STM32 V*I must stay flagged as apparent VA")
    if stm32["inverter"]["stm32_online"] is not True:
        sys.exit("STM32 profile with a reading reports stm32_online")
    if stm32["last_data"]["power"] is not None or stm32["battery"]["power"] is not None:
        sys.exit("STM32 watt keys stay null when the text frame has no watts")
    marker = snaps["sentinel"]
    encoded_marker = json.dumps(marker)
    if "-1" in encoded_marker or "99999" in encoded_marker:
        sys.exit("power sentinels must be null")
    if marker["last_data"]["power"] is not None or marker["metrics"]["output_power_w"] is not None:
        sys.exit("sentinel active watts must be null")
    if marker["inverter"].get("output_power") is not None or marker["metrics"]["inverterLoad"] is not None:
        sys.exit("sentinel apparent power must be null")
    if marker["battery"]["power"] is not None or marker["solar"]["power"] is not None:
        sys.exit("sentinel battery and solar power must be null")

print("profile pin checks passed")
