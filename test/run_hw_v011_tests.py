#!/usr/bin/env python3
"""Host checks for hardware v0.1.1. No device, flash, or network."""
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
        str(root / "src/local_manual_api.cpp"),
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

print("profile pin checks passed")
