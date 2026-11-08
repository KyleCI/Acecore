#!/usr/bin/env python3
"""Validate emitted records and the backend fixture against the vendored schema."""
import hashlib
import json
import pathlib
import re
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
UP = ROOT / "contracts/upstream/e7f86c700cd930046f54e75dfec3998536953f54"
DENOM = 3600000000000


def sha_ok():
    for line in (UP / "SHA256SUMS").read_text().splitlines():
        digest, name = line.split()
        got = hashlib.sha256((UP / name).read_bytes()).hexdigest()
        if got != digest:
            raise SystemExit(f"hash mismatch {name}")


def uint64(value, where):
    if not isinstance(value, str) or not re.fullmatch(r"0|[1-9][0-9]*", value):
        raise SystemExit(f"{where} is not a uint64 decimal string: {value!r}")
    return int(value)


def instant_ok(value, where):
    if value is None:
        return
    if isinstance(value, str):
        if not re.search(r"(Z|[+-]\d\d:\d\d|[+-]\d{4})$", value):
            raise SystemExit(f"{where} instant missing Z or offset: {value!r}")
        return
    if isinstance(value, dict):
        return
    raise SystemExit(f"{where} instant type {type(value).__name__}")


def validate_record(rec, label):
    schema = json.loads((UP / "estimate.load_ac_energy.v1.schema.json").read_text())
    required = schema["required"]
    props = schema["properties"]
    extra = set(rec) - set(props)
    if extra:
        raise SystemExit(f"{label} extra keys {sorted(extra)}")
    missing = [key for key in required if key not in rec]
    if missing:
        raise SystemExit(f"{label} missing {missing}")
    if rec["contract"] != "estimate.load_ac_energy.v1":
        raise SystemExit(label + " contract")
    if not isinstance(rec["simulation"], bool):
        raise SystemExit(label + " simulation")
    if not isinstance(rec["device_id"], str) or not 1 <= len(rec["device_id"]) <= 120:
        raise SystemExit(label + " device_id")
    if rec["stream"] != "estimate.load_ac_energy":
        raise SystemExit(label + " stream")
    for key in ("meter_epoch", "seq", "monotonic_start_ms", "monotonic_end_ms",
                "counter_start_mwh", "counter_end_mwh", "interval_estimated_mwh",
                "covered_ms", "interval_ms"):
        uint64(rec[key], f"{label}.{key}")
    if not re.fullmatch(r"[A-Za-z0-9._:-]{1,64}", rec["boot_id"]):
        raise SystemExit(label + " boot_id")
    version = rec["clock_correction_version"]
    if not isinstance(version, int) or isinstance(version, bool) or not 0 <= version <= 1000000:
        raise SystemExit(label + " clock_correction_version")
    model = rec["model"]
    if set(model) != set(props["model"]["required"]):
        raise SystemExit(f"{label} model keys {sorted(model)}")
    for key, prop in props["model"]["properties"].items():
        if "const" in prop and model[key] != prop["const"]:
            raise SystemExit(f"{label} model.{key}={model[key]!r}")
        if "enum" in prop and model[key] not in prop["enum"]:
            raise SystemExit(f"{label} model.{key}")
    if model["pf_kind"] != "assumed" or model["pf_assumption_millis"] != 800:
        raise SystemExit(label + " pf model")
    instant_ok(rec["utc_start"], label + ".utc_start")
    instant_ok(rec["utc_end"], label + ".utc_end")
    if rec["time_quality"] not in props["time_quality"]["enum"]:
        raise SystemExit(label + " time_quality")
    if rec["sample_validity"] not in props["sample_validity"]["enum"]:
        raise SystemExit(label + " sample_validity")
    modes = rec["source_mode_coverage"]
    if set(modes) != {"inverter_output_only_ms", "common_output_ms", "unknown_ms"}:
        raise SystemExit(label + " mode coverage keys")
    mode_sum = 0
    for key in modes:
        mode_sum += uint64(modes[key], f"{label}.{key}")
    covered = uint64(rec["covered_ms"], label)
    interval = uint64(rec["interval_ms"], label)
    start = uint64(rec["monotonic_start_ms"], label)
    end = uint64(rec["monotonic_end_ms"], label)
    if end < start:
        raise SystemExit(label + " monotonic backwards")
    if interval != end - start and not (start == 0 and end == 0 and interval == 0):
        if interval != end - start:
            raise SystemExit(f"{label} interval_ms {interval} != span {end - start}")
    if covered > interval:
        raise SystemExit(label + " covered exceeds interval")
    if mode_sum != covered and not (covered == 0 and mode_sum == interval):
        raise SystemExit(f"{label} mode ms {mode_sum} covered {covered} interval {interval}")
    energy = uint64(rec["interval_estimated_mwh"], label)
    c0 = uint64(rec["counter_start_mwh"], label)
    c1 = uint64(rec["counter_end_mwh"], label)
    if c1 - c0 != energy:
        raise SystemExit(f"{label} counter delta {c1 - c0} != interval {energy}")
    inputs = rec["inputs"]
    if inputs is None:
        if rec["sample_validity"] not in ("invalid", "absent"):
            raise SystemExit(label + " null inputs require invalid or absent")
        if energy != 0:
            raise SystemExit(label + " invalid/absent energy must be 0")
    else:
        if set(inputs) != {"ac_vrms_mv", "ac_irms_ma", "pf_assumed_millis", "ac_v_valid", "ac_i_valid"}:
            raise SystemExit(label + " input keys")
        volts = uint64(inputs["ac_vrms_mv"], label + ".v")
        amps = uint64(inputs["ac_irms_ma"], label + ".i")
        pf = inputs["pf_assumed_millis"]
        if not isinstance(pf, int) or isinstance(pf, bool) or not 0 <= pf <= 1000:
            raise SystemExit(label + " input pf")
        if not isinstance(inputs["ac_v_valid"], bool) or not isinstance(inputs["ac_i_valid"], bool):
            raise SystemExit(label + " valid flags")
        if model["pf_assumption_millis"] != 800 or pf != 800:
            raise SystemExit(label + " pf applied more than the 800 assumption or differs")
        if inputs["ac_v_valid"] and inputs["ac_i_valid"]:
            expect = (volts * amps * 800 * covered) // DENOM
            if energy != expect:
                raise SystemExit(f"{label} mWh {energy} != floor formula {expect}")
        elif energy != 0:
            raise SystemExit(label + " non-valid inputs must be 0 mWh")
    if rec["sample_validity"] in ("invalid", "absent") and energy != 0:
        raise SystemExit(label + " absent/invalid nonzero")
    return energy


def fixture_records():
    raw = subprocess.check_output(
        ["php", "-r", "require $argv[1]; echo json_encode(estimate_demo_records('SYNTHETIC'));",
         str(UP / "demo_fixture.php")],
        text=True,
    )
    return json.loads(raw)


def validate_reported(rec, label, remainder_in=0):
    root = pathlib.Path(__file__).resolve().parents[1]
    schema_path = root / "contracts/upstream/c3ac5b340a18ed08b0df9da7fcda4380bb9bd68c/estimate.load_ac_energy.v1.xfer4_reported_vi.schema.json"
    schema = json.loads(schema_path.read_text())
    props = schema["properties"]
    extra = set(rec) - set(props)
    if extra:
        raise SystemExit(f"{label} extra keys {sorted(extra)}")
    missing = [key for key in schema["required"] if key not in rec]
    if missing:
        raise SystemExit(f"{label} missing {missing}")
    if rec["contract"] != "estimate.load_ac_energy.v1" or rec["stream"] != "estimate.load_ac_energy":
        raise SystemExit(label + " envelope")
    if not isinstance(rec["simulation"], bool):
        raise SystemExit(label + " simulation")
    for key in ("meter_epoch", "seq", "monotonic_start_ms", "monotonic_end_ms",
                "counter_start_mwh", "counter_end_mwh", "interval_estimated_mwh",
                "covered_ms", "interval_ms"):
        uint64(rec[key], f"{label}.{key}")
    if not re.fullmatch(r"[A-Za-z0-9._:-]{1,64}", rec["boot_id"]):
        raise SystemExit(label + " boot_id")
    version = rec["clock_correction_version"]
    if not isinstance(version, int) or isinstance(version, bool) or not 0 <= version <= 1000000:
        raise SystemExit(label + " clock")
    model = rec["model"]
    if set(model) != set(props["model"]["required"]):
        raise SystemExit(f"{label} model keys {sorted(set(model) ^ set(props['model']['required']))}")
    for key, prop in props["model"]["properties"].items():
        if model[key] != prop["const"]:
            raise SystemExit(f"{label} model.{key}")
    if model["model_id"] == "ac_vrms_mv_x_ac_irms_ma_x_pf_millis":
        raise SystemExit(label + " reported record uses the RMS path")
    start = uint64(rec["monotonic_start_ms"], label)
    end = uint64(rec["monotonic_end_ms"], label)
    if end <= start or uint64(rec["interval_ms"], label) != end - start:
        raise SystemExit(label + " interval")
    covered = uint64(rec["covered_ms"], label)
    if covered > end - start:
        raise SystemExit(label + " covered")
    modes = rec["source_mode_coverage"]
    if set(modes) != {"inverter_output_only_ms", "common_output_ms", "unknown_ms"}:
        raise SystemExit(label + " coverage keys")
    if uint64(modes["common_output_ms"], label) != 0 or uint64(modes["unknown_ms"], label) != 0:
        raise SystemExit(label + " non-inverter coverage")
    if uint64(modes["inverter_output_only_ms"], label) != covered:
        raise SystemExit(label + " inverter coverage")
    energy = uint64(rec["interval_estimated_mwh"], label)
    if uint64(rec["counter_end_mwh"], label) - uint64(rec["counter_start_mwh"], label) != energy:
        raise SystemExit(label + " counter delta")
    validity = rec["sample_validity"]
    if validity not in ("reported", "clamped_or_zero", "gap"):
        raise SystemExit(label + " sample_validity")
    inputs = rec["inputs"]
    gap = rec.get("gap_reason", None)
    if validity == "gap":
        if inputs is not None or gap not in ("grid", "transfer", "fault", "stale", "truncated", "txdrop") or covered != 0:
            raise SystemExit(label + " gap shape")
        volts = amps = 0
    else:
        if gap is not None or not isinstance(inputs, dict):
            raise SystemExit(label + " inputs")
        if set(inputs) != {"reported_voltage_mv", "reported_current_ma", "pf_assumed_millis"}:
            raise SystemExit(label + " input keys")
        if inputs["pf_assumed_millis"] != 800:
            raise SystemExit(label + " pf applied in inputs")
        volts = uint64(inputs["reported_voltage_mv"], label)
        amps = uint64(inputs["reported_current_ma"], label)
        if validity == "reported" and (volts <= 0 or amps < 180):
            raise SystemExit(label + " reported sample")
        if validity == "clamped_or_zero" and amps != 0:
            raise SystemExit(label + " clamp")
    numerator = volts * amps * 800 * covered + remainder_in
    if energy != numerator // DENOM:
        raise SystemExit(f"{label} energy {energy} != {numerator // DENOM}")
    instant_ok(rec["utc_start"], label)
    instant_ok(rec["utc_end"], label)
    if (rec["utc_start"] is None) != (rec["utc_end"] is None):
        raise SystemExit(label + " utc pair")
    return energy


def main():
    sha_ok()
    records = fixture_records()
    if len(records) != 9:
        raise SystemExit("fixture length")
    energies = [validate_record(rec, f"fixture[{i}]") for i, rec in enumerate(records)]
    if energies[1] != 184000:
        raise SystemExit(f"230V 1A 1h is {energies[1]} not 184000")
    if any(rec["simulation"] is not True for rec in records):
        raise SystemExit("fixture must stay synthetic")
    if records[8]["time_quality"] != "uncertain":
        raise SystemExit("fixture uncertain record")
    print(f"schema+fixture: {len(records)} records, 230V/1A/1h={energies[1]} mWh, pf=800 once")


if __name__ == "__main__":
    main()
