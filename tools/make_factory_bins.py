#!/usr/bin/env python3
"""Packaging step: turn USB base zips (bootloader/partitions/boot_app0/firmware) into one
merged image per model for factory_flash.py.

    python tools/make_factory_bins.py --zips <folder with acecore_fw_*_base.zip> --out bins

Writes bins/<model>_v<version>_merged.bin (esptool merge_bin, dio 40m 4MB, offsets
0x1000 bootloader, 0x8000 partitions, 0xe000 boot_app0, 0x10000 firmware) and bins/SHA256SUMS.
The model and version come from the identity string inside firmware.bin, and the merged image
is checked again afterwards. Does not touch any board.
"""
import argparse
import glob
import os
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import factory_flash as ff  # noqa: E402

PARTS = (("0x1000", "bootloader.bin"), ("0x8000", "partitions.bin"), ("0xe000", "boot_app0.bin"), ("0x10000", "firmware.bin"))


def merge(base, zpath, out_dir):
    with tempfile.TemporaryDirectory() as tmp, zipfile.ZipFile(zpath) as z:
        names = {os.path.basename(n): n for n in z.namelist()}
        for _, part in PARTS:
            if part not in names:
                raise SystemExit("%s has no %s" % (zpath, part))
            with open(os.path.join(tmp, part), "wb") as f:
                f.write(z.read(names[part]))
        with open(os.path.join(tmp, "firmware.bin"), "rb") as f:
            ids = set(ff.image_identities(f.read()))
        if len(ids) != 1:
            raise SystemExit("%s firmware.bin identities: %s" % (zpath, sorted(ids)))
        _, model, version = ids.pop()
        out = os.path.join(out_dir, "%s_v%s_merged.bin" % (model, version))
        cmd = base + ["--chip", "esp32", "merge_bin", "-o", out] + ff.FLASH_ARGS
        for off, part in PARTS:
            cmd += [off, os.path.join(tmp, part)]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    with open(out, "rb") as f:
        if ff.check_image(f.read(), model) != version:
            raise SystemExit("merged image check failed for " + out)
    return out, model, version


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zips", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--esptool")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    base = ff.esptool_cmd(a.esptool)
    lines = []
    for z in sorted(glob.glob(os.path.join(a.zips, "acecore_fw_*_base.zip"))):
        out, model, version = merge(base, z, a.out)
        sha = ff.sha256_file(out)
        lines.append("%s  %s" % (sha, os.path.basename(out)))
        print("%s  %s  (%s v%s)" % (sha, os.path.basename(out), model, version))
    if not lines:
        raise SystemExit("no acecore_fw_*_base.zip in " + a.zips)
    with open(os.path.join(a.out, "SHA256SUMS"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
