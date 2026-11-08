#!/usr/bin/env python3
"""Match a firmware upload frame to the vendored socket forwarder. No network and no database."""
import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / "contracts/upstream/ab64d033ad4a0c03bde1bb62670e924d9cc460bf"


def load_forwarder():
    text = (UPSTREAM / "server.py").read_text()
    start = text.index("_ESTIMATE_CONTRACT")
    end = text.index("def _estimate_failure_frame")
    namespace = {"__name__": "vendored_server_ack"}
    exec(compile("from typing import Any\n" + text[start:end], "server.py", "exec"), namespace)
    return namespace


def check(stdout: str) -> None:
    frames = [line.split(" ", 1)[1] for line in stdout.splitlines() if line.startswith("UPLOADFRAME ")]
    if len(frames) != 1:
        raise SystemExit(f"expected one UPLOADFRAME, found {len(frames)}")
    frame = json.loads(frames[0])
    record = frame["record"]
    if (230000 * 1000 * 800 * 3600000) // 3600000000000 != 184000:
        raise SystemExit("184000 mWh formula changed")
    if record["interval_estimated_mwh"] != "184000" or record["counter_end_mwh"] != "184000":
        raise SystemExit("firmware frame is not the 184000 mWh body")
    if record["simulation"] is not False or record["inputs"]["pf_assumed_millis"] != 800:
        raise SystemExit("firmware frame changed simulation or the power-factor assumption")
    if "api_key" in frame or "device_secret" in frame:
        raise SystemExit("upload frame carries a credential")

    server = load_forwarder()
    if server["is_energy_message"](frame) is not True:
        raise SystemExit("vendored forwarder does not treat the firmware frame as an estimate")
    ack = {
        "status": "success",
        "ack": True,
        "replay": False,
        "contract": record["contract"],
        "stream": record["stream"],
        "record_id": 29,
        "device_id": record["device_id"],
        "meter_epoch": record["meter_epoch"],
        "boot_id": record["boot_id"],
        "seq": record["seq"],
        "clock_correction_version": record["clock_correction_version"],
        "simulation": False,
    }
    if server["durable_estimate_ack"](ack) != ack:
        raise SystemExit("matching ACK was not accepted by the vendored forwarder")
    replay = dict(ack)
    replay["replay"] = True
    if server["durable_estimate_ack"](replay) != replay:
        raise SystemExit("replay ACK was not accepted by the vendored forwarder")
    if server["telemetry_frames"](frame, {"results": [ack], "ack": True}) != [ack]:
        raise SystemExit("energy frame also received a generic success")
    generic = server["energy_result_frame"]({"status": "success"})
    if generic.get("ack") is not False or generic.get("error") != "unavailable" or "record_id" in generic:
        raise SystemExit("generic success was treated as a durable ACK")
    leaked = dict(ack)
    leaked["api_key"] = "do-not-forward"
    leaked_frame = server["energy_result_frame"](leaked)
    if leaked_frame.get("ack") is not False or "api_key" in json.dumps(leaked_frame) or "record_id" in leaked_frame:
        raise SystemExit("extra key was forwarded as an ACK")
    conflict = server["bounded_estimate_error"]({
        "status": "error",
        "ack": False,
        "error": "conflicting_duplicate",
        "message": "secret-value should not pass through",
        "contract": "estimate.load_ac_energy.v1",
        "stream": "estimate.load_ac_energy",
        "device_id": record["device_id"],
        "seq": record["seq"],
        "clock_correction_version": 0,
    })
    if conflict["ack"] is not False or "secret-value" in conflict["message"] or "record_id" in conflict:
        raise SystemExit("conflict frame was not a bounded non-ACK")
    heartbeat = server["telemetry_frames"](
        {"type": "heartbeat", "device_id": record["device_id"]},
        {"results": [{"status": "success"}], "ack": True},
    )
    if heartbeat != [{"status": "success"}]:
        raise SystemExit("heartbeat no longer receives the legacy success frame")

    fixture = json.loads((UPSTREAM / "ack_184000_replay.json").read_text())
    if server["durable_estimate_ack"](fixture["ack"]) != fixture["ack"]:
        raise SystemExit("captured 184000 ACK no longer matches the forwarder")
    if fixture["ack"]["simulation"] is not True or fixture["request"]["simulation"] is not True:
        raise SystemExit("captured fixture is not the synthetic simulation record")
    if fixture["ack"]["device_id"] == record["device_id"] and fixture["ack"]["simulation"] == record["simulation"]:
        raise SystemExit("simulation fixture identity collided with the firmware record")
    schema = json.loads((UPSTREAM / "estimate.load_ac_energy.v1.ack.schema.json").read_text())
    if set(schema["required"]) != set(ack):
        raise SystemExit("ACK schema required keys drifted from the forwarder")
    print("upload ack oracle: firmware 184000 mWh frame matches the vendored /ws/device forwarder")
    print("GAP: no isolated backend database was available, so commitNew -> row -> ACK was not executed")


if __name__ == "__main__":
    import sys
    check(sys.stdin.read() if len(sys.argv) < 2 else pathlib.Path(sys.argv[1]).read_text())
