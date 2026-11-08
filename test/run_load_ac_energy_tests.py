#!/usr/bin/env python3
"""Host tests for estimate.load_ac_energy.v1. No hardware, radio, or live telemetry."""
import importlib.util
import pathlib
import shutil
import subprocess
import sys
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
            str(root / "src/stm32_text_validity.cpp"),
            str(root / "test/load_ac_energy_test.cpp"),
            "-o", str(binary),
        ],
        check=True,
    )
    completed = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
    print(completed.stdout, end="")
    if completed.stderr:
        print(completed.stderr, end="")

import json
sys_path = pathlib.Path(__file__).resolve().parent / "validate_energy_schema.py"
subprocess.run([sys.executable, str(sys_path)], check=True)
wire = [json.loads(line.split(" ", 1)[1]) for line in completed.stdout.splitlines()
        if line.startswith("WIREJSON ") or line.startswith("STATUSJSON ")]
if not wire:
    raise SystemExit("no emitted WIREJSON record to validate")
validator = pathlib.Path(__file__).resolve().parent / "validate_energy_schema.py"
# Reuse the validator by importing its function.
import importlib.util
spec = importlib.util.spec_from_file_location("energy_schema", validator)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
for index, record in enumerate(wire):
    mod.validate_record(record, f"emitted[{index}]")
    if record["simulation"] is not False:
        raise SystemExit("device emission must not be marked simulation")
print(f"emitted records validated against schema: {len(wire)}")
