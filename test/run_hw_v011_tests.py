#!/usr/bin/env python3
"""Host checks for hardware v0.1.1. No device, flash, or network."""
import pathlib
import shutil
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
if not compiler:
    sys.exit("A C++ compiler is required.")

sources = [
    root / "src/memory_byte_store.cpp",
    root / "src/slot_log.cpp",
    root / "src/sd_log.cpp",
    root / "src/pzem_v3.cpp",
    root / "src/lagos_time.cpp",
    root / "src/ds1307_time.cpp",
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

print("profile pin checks passed")
