#!/usr/bin/env python3
"""Run the production alert policy on the host; no hardware or radio required."""
import pathlib
import shutil
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
compiler = shutil.which("c++") or shutil.which("g++")
if not compiler:
    raise SystemExit("Install a C++ compiler to run the battery alert tests.")
with tempfile.TemporaryDirectory(prefix="powercell-battery-tests-") as directory:
    binary = pathlib.Path(directory) / "battery-alert-tests"
    subprocess.run([
        compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror",
        "-I", str(root / "include"), str(root / "src/battery_alert_policy.cpp"),
        str(root / "test/battery_alert_policy_test.cpp"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
