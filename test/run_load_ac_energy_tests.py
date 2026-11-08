#!/usr/bin/env python3
"""Host tests for estimate.load_ac_energy.v1. No hardware, radio, or live telemetry."""
import os
import pathlib
import shutil
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
compiler = shutil.which("g++") or shutil.which("c++")
if not compiler:
    raise SystemExit("Install a C++ compiler to run the load AC energy tests.")
with tempfile.TemporaryDirectory(prefix="powercell-load-ac-") as directory:
    binary = pathlib.Path(directory) / "load-ac-energy-tests"
    subprocess.run(
        [
            compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror",
            "-I", str(root / "include"),
            str(root / "src/load_ac_energy.cpp"),
            str(root / "test/load_ac_energy_test.cpp"),
            "-o", str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)

import json
fixture = json.loads((root / "contracts/examples/SYNTHETIC_estimate.load_ac_energy.v1.json").read_text())
if fixture.get("label") != "SYNTHETIC" or fixture.get("synthetic") is not True:
    raise SystemExit("synthetic fixture must be labelled SYNTHETIC")
records = fixture["records"]
interval = records[0]
if interval["pf_assumed_millis"] != 800 or interval["interval_estimated_mwh"] != "184000":
    raise SystemExit("synthetic example must use PF 800 and 184000 mWh")
if interval["pf_is_assumption"] is not True or interval["synthetic"] is not True:
    raise SystemExit("synthetic example must stay an assumption")
if interval["bms_dc_used"] is not False:
    raise SystemExit("synthetic example must not use BMS DC")
status = records[1]
if status["estimate_available"] is not False or status["pf_assumed_millis"] != 800:
    raise SystemExit("unavailable synthetic status drifted")
for banned in ("inverter_energy", "energy_wh", "consumed_wh", "output_energy"):
    if banned in interval or banned in status:
        raise SystemExit("ambiguous energy alias in fixture")

schema_path = os.environ.get("ACE_ENERGY_SCHEMA", "")
print("backend contract compatibility: GAP")
print("unread https://github.com/letambari/official-powercell-app/pull/30")
print("commit e7f86c700cd930046f54e75dfec3998536953f54")
print("schema powercell-app/backend/contracts/estimate.load_ac_energy.v1.schema.json")
print("collector wire was not retargeted to an unread schema")
if schema_path:
    schema_file = pathlib.Path(schema_path)
    if not schema_file.is_file():
        raise SystemExit(f"ACE_ENERGY_SCHEMA is not a file: {schema_path}")
    schema = json.loads(schema_file.read_text())
    required = schema.get("required", [])
    consts = {}
    for key, prop in schema.get("properties", {}).items():
        if isinstance(prop, dict) and "const" in prop:
            consts[key] = prop["const"]
    for record in records:
        missing = [key for key in required if key not in record]
        if missing:
            raise SystemExit("fixture missing schema keys: " + ", ".join(missing))
        for key, value in consts.items():
            if key in record and record[key] != value:
                raise SystemExit(f"fixture {key}={record[key]!r} schema const {value!r}")
    print(f"backend contract compatibility: checked {schema_file} required={len(required)}")
