from __future__ import annotations

import asyncio
import json
import os
from typing import Any
from urllib.error import HTTPError
from urllib.parse import parse_qs, quote, urlparse
from urllib.request import Request, urlopen

from websockets.asyncio.server import serve

from auth import IdentityError, assert_connection_ids, assert_message_identity, client_is_tls, presented_api_key, presented_secret

# The device waits 20 s for ota_none or ota_available, then uses HTTPS.
OTA_REPLY_TIMEOUT_SECONDS = 8


def _internal_base() -> str:
    return os.environ.get("PHP_INTERNAL_URL", "http://127.0.0.1:8081").rstrip("/")


def _token() -> str:
    return os.environ.get("INTERNAL_SERVICE_TOKEN", "").strip()


def _call(method: str, path: str, body: dict[str, Any] | None = None, headers: dict[str, str] | None = None, timeout: float = 8) -> dict[str, Any]:
    payload = None if body is None else json.dumps(body).encode("utf-8")
    request_headers = {
        "X-Internal-Token": _token(),
        "Accept": "application/json",
        "Content-Type": "application/json",
    }
    if headers:
        request_headers.update(headers)
    request = Request(_internal_base() + path, data=payload, headers=request_headers, method=method)
    with urlopen(request, timeout=timeout) as response:
        raw = response.read().decode("utf-8")
    decoded = json.loads(raw) if raw else {}
    if not isinstance(decoded, dict):
        raise RuntimeError("Internal API returned an unexpected payload.")
    return decoded


def _header_map(websocket) -> dict[str, str]:
    headers = {}
    source = websocket.request.headers
    raw = getattr(source, "raw_items", None)
    pairs = raw() if callable(raw) else source.items()
    for key, value in pairs:
        if isinstance(key, bytes):
            key = key.decode("latin1")
        if isinstance(value, bytes):
            value = value.decode("latin1")
        headers[str(key)] = str(value)
    return headers


def frames_from_poll(result: dict[str, Any], tls: bool) -> list[dict[str, Any]]:
    """Turn a command poll into socket frames.

    set_local_token and set_passcode carry secrets, so they are sent only
    when the device socket is TLS. claim_result and unclaim are not secret
    and are forwarded on either transport.
    """
    frames: list[dict[str, Any]] = []
    token = result.get("local_token")
    if tls and isinstance(token, dict) and token.get("type") == "set_local_token" and "command_id" not in token:
        frames.append(token)
    for command in result.get("commands") or []:
        if isinstance(command, dict):
            frames.append(command)
    for item in result.get("onboarding") or []:
        if not isinstance(item, dict):
            continue
        if item.get("type") == "set_passcode" and not tls:
            continue
        frames.append(item)
    return frames


_ESTIMATE_CONTRACT = "estimate.load_ac_energy.v1"
_ESTIMATE_STREAM = "estimate.load_ac_energy"
_ACK_KEYS = {
    "status",
    "ack",
    "replay",
    "contract",
    "stream",
    "record_id",
    "device_id",
    "meter_epoch",
    "boot_id",
    "seq",
    "clock_correction_version",
    "simulation",
}
_ESTIMATE_ERRORS = {
    "conflicting_duplicate": "The same identity was already stored with a different record.",
    "counter_discontinuity": "The mWh counter does not continue.",
    "monotonic_overlap": "The monotonic interval overlaps an existing record.",
    "out_of_order": "Monotonic time went backwards.",
    "utc_overlap": "The UTC interval overlaps another live estimate record.",
    "clock_correction_changed_energy": "A clock correction cannot change the energy counter.",
    "model_pf": "estimate.load_ac_energy.v1 assumes PF 0.800.",
    "device_mismatch": "device_id does not match the authenticated device.",
    "unsupported_stream": "This path accepts only estimate.load_ac_energy.",
    "unsupported_contract": "contract must be estimate.load_ac_energy.v1.",
    "energy_formula": "interval_estimated_mwh does not match the assumed-PF formula.",
    "decimal_string": "A counter was not a decimal string.",
    "sample_validity": "sample_validity does not match the coverage.",
    "measurement_unknown": "invalid and absent samples cannot carry estimated energy.",
    "model": "model does not match estimate.load_ac_energy.v1.",
    "rejected": "Estimate was rejected.",
}


def _unavailable_estimate() -> dict[str, Any]:
    return {
        "status": "error",
        "ack": False,
        "error": "unavailable",
        "message": "Estimate was not saved.",
        "contract": _ESTIMATE_CONTRACT,
        "stream": _ESTIMATE_STREAM,
    }


def _is_int(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _safe_token(value: object, limit: int) -> str | None:
    if not isinstance(value, str) or not 1 <= len(value) <= limit:
        return None
    if any(ord(char) < 32 or ord(char) == 127 for char in value):
        return None
    return value


def is_energy_message(message: object) -> bool:
    if not isinstance(message, dict):
        return False
    kind = str(message.get("type") or "").strip().lower()
    if kind == "energy_estimate":
        return True
    stream = message.get("stream")
    record = message.get("record")
    if not isinstance(stream, str) and isinstance(record, dict):
        stream = record.get("stream")
    return stream == _ESTIMATE_STREAM


def durable_estimate_ack(result: object) -> dict[str, Any] | None:
    """Return the commit-backed ACK, or None. Extra keys are not an ACK."""
    if not isinstance(result, dict) or set(result) != _ACK_KEYS:
        return None
    if result.get("status") != "success" or result.get("ack") is not True or not isinstance(result.get("replay"), bool):
        return None
    if result.get("contract") != _ESTIMATE_CONTRACT or result.get("stream") != _ESTIMATE_STREAM:
        return None
    if not _is_int(result.get("record_id")) or int(result["record_id"]) < 1:
        return None
    if not _is_int(result.get("clock_correction_version")) or int(result["clock_correction_version"]) < 0:
        return None
    if not isinstance(result.get("simulation"), bool):
        return None
    device_id = _safe_token(result.get("device_id"), 120)
    meter_epoch = _safe_token(result.get("meter_epoch"), 20)
    boot_id = _safe_token(result.get("boot_id"), 64)
    seq = _safe_token(result.get("seq"), 20)
    if device_id is None or meter_epoch is None or boot_id is None or seq is None:
        return None
    return {
        "status": "success",
        "ack": True,
        "replay": result["replay"],
        "contract": _ESTIMATE_CONTRACT,
        "stream": _ESTIMATE_STREAM,
        "record_id": int(result["record_id"]),
        "device_id": device_id,
        "meter_epoch": meter_epoch,
        "boot_id": boot_id,
        "seq": seq,
        "clock_correction_version": int(result["clock_correction_version"]),
        "simulation": result["simulation"],
    }


def bounded_estimate_error(result: object) -> dict[str, Any] | None:
    if not isinstance(result, dict):
        return None
    if result.get("status") != "error" or result.get("ack") is not False:
        return None
    code = result.get("error")
    if not isinstance(code, str) or code not in _ESTIMATE_ERRORS:
        return None
    if _is_int(result.get("record_id")) and int(result["record_id"]) > 0 and result.get("ack") is True:
        return None
    frame: dict[str, Any] = {
        "status": "error",
        "ack": False,
        "error": code,
        "message": _ESTIMATE_ERRORS[code],
        "contract": _ESTIMATE_CONTRACT,
        "stream": _ESTIMATE_STREAM,
    }
    device_id = _safe_token(result.get("device_id"), 120)
    if device_id is not None:
        frame["device_id"] = device_id
    for key, limit in (("meter_epoch", 20), ("boot_id", 64), ("seq", 20)):
        token = _safe_token(result.get(key), limit)
        if token is not None:
            frame[key] = token
    version = result.get("clock_correction_version")
    if _is_int(version) and 0 <= int(version) <= 1_000_000:
        frame["clock_correction_version"] = int(version)
    return frame


def energy_result_frame(result: object) -> dict[str, Any]:
    ack = durable_estimate_ack(result)
    if ack is not None:
        return ack
    error = bounded_estimate_error(result)
    if error is not None:
        return error
    return _unavailable_estimate()


def telemetry_frames(message: object, result: object) -> list[dict[str, Any]]:
    return telemetry_frames_for_batch([message] if isinstance(message, dict) else [], result)


def telemetry_frames_for_batch(messages: list, result: object) -> list[dict[str, Any]]:
    frames: list[dict[str, Any]] = []
    replies = result.get("replies") if isinstance(result, dict) else None
    if isinstance(replies, list):
        for reply in replies:
            if isinstance(reply, dict) and reply.get("type") == "hello_ack":
                frames.append(reply)
    results = result.get("results") if isinstance(result, dict) else None
    if not isinstance(results, list):
        results = []
    non_energy = False
    for index, message in enumerate(messages):
        if not isinstance(message, dict) or not is_energy_message(message):
            if isinstance(message, dict):
                non_energy = True
            continue
        item = results[index] if index < len(results) else None
        frames.append(energy_result_frame(item))
    if non_energy and (not isinstance(result, dict) or result.get("ack", True)):
        frames.append({"status": "success"})
    return frames


def _estimate_failure_frame(error: HTTPError) -> dict[str, Any]:
    try:
        raw = error.read(4096)
    except Exception:
        return _unavailable_estimate()
    if not raw:
        return _unavailable_estimate()
    try:
        decoded = json.loads(raw.decode("utf-8", errors="replace")[:4096])
    except (UnicodeError, json.JSONDecodeError):
        return _unavailable_estimate()
    parsed = bounded_estimate_error(decoded)
    return parsed if parsed is not None else _unavailable_estimate()


async def _send_commands(websocket, device_id: str, api_key: str, secret: str, header_device_id: str, tls: bool) -> None:
    headers = {"API-KEY": api_key, "X-Forwarded-Proto": "https" if tls else "http"}
    if secret:
        headers["X-Device-Secret"] = secret
    if header_device_id:
        headers["X-Device-ID"] = header_device_id
    result = await asyncio.to_thread(
        _call,
        "GET",
        "/api/internal/ws/commands?device_id=" + quote(device_id, safe=""),
        None,
        headers,
    )
    for frame in frames_from_poll(result, tls):
        await websocket.send(json.dumps(frame))


async def handler(websocket) -> None:
    parsed = urlparse(websocket.request.path)
    if parsed.path.rstrip("/") != "/ws/device":
        await websocket.close(4404, "not found")
        return

    headers = _header_map(websocket)
    tls = client_is_tls(headers)
    query = parse_qs(parsed.query)
    query_device_id = (query.get("device_id") or [""])[0].strip()
    header_device_id = ""
    for key, value in headers.items():
        if key.lower() == "x-device-id":
            header_device_id = value.strip()
            break

    try:
        api_key = presented_api_key(headers, accept_bearer=True)
        secret = presented_secret(headers)
        assert_connection_ids(query_device_id, header_device_id)
    except IdentityError:
        await websocket.close(4401, "unauthorized")
        return

    if query_device_id == "" and header_device_id == "":
        await websocket.close(4401, "unauthorized")
        return

    try:
        auth = await asyncio.to_thread(
            _call,
            "POST",
            "/api/internal/ws/auth",
            {
                "device_id": query_device_id or header_device_id,
                "header_device_id": header_device_id,
                "api_key": api_key,
                "device_secret": secret,
            },
        )
    except Exception:
        await websocket.close(4401, "unauthorized")
        return

    if auth.get("status") != "success":
        await websocket.close(4401, "unauthorized")
        return

    device_id = str(auth.get("device_id") or "")
    if device_id == "":
        await websocket.close(4401, "unauthorized")
        return

    # An unknown unit is not inserted, and commands are not pumped, until
    # device_hello is accepted. Other frames are refused and not forwarded.
    pending_hello = bool(auth.get("pending_hello"))
    pump: asyncio.Task | None = None

    async def pump_commands() -> None:
        while True:
            try:
                await _send_commands(websocket, device_id, api_key, secret, header_device_id, tls)
            except Exception:
                return
            await asyncio.sleep(1)

    def start_pump() -> None:
        nonlocal pump
        if pump is None:
            pump = asyncio.create_task(pump_commands())

    if not pending_hello:
        start_pump()
    try:
        async for raw in websocket:
            if isinstance(raw, bytes):
                raw = raw.decode("utf-8", errors="replace")
            try:
                message = json.loads(raw)
            except json.JSONDecodeError:
                await websocket.send(json.dumps({"status": "error", "message": "Invalid JSON."}))
                continue
            if not isinstance(message, dict):
                await websocket.send(json.dumps({"status": "error", "message": "Invalid message."}))
                continue
            try:
                assert_message_identity(query_device_id or header_device_id, message)
            except IdentityError as error:
                await websocket.send(json.dumps({"status": "error", "message": str(error)}))
                continue
            message_type = str(message.get("type") or "").strip().lower()
            if pending_hello and message_type != "device_hello":
                await websocket.send(json.dumps({"status": "error", "message": "device_hello is required."}))
                continue
            if message_type == "ota_check":
                try:
                    result = await asyncio.to_thread(
                        _call,
                        "POST",
                        "/api/internal/ws/ota-check",
                        {
                            "device_id": device_id,
                            "header_device_id": header_device_id,
                            "api_key": api_key,
                            "device_secret": secret,
                            "message": message,
                        },
                        None,
                        OTA_REPLY_TIMEOUT_SECONDS,
                    )
                except Exception:
                    # No frame. The device uses HTTPS POST /api/ota/check after 20 s.
                    continue
                reply = result.get("reply")
                if isinstance(reply, dict) and reply.get("type") in ("ota_none", "ota_available"):
                    await websocket.send(json.dumps(reply))
                continue
            try:
                result = await asyncio.to_thread(
                    _call,
                    "POST",
                    "/api/internal/ws/telemetry",
                    {
                        "device_id": device_id,
                        "header_device_id": header_device_id,
                        "api_key": api_key,
                        "device_secret": secret,
                        "messages": [message],
                    },
                )
            except HTTPError as error:
                if is_energy_message(message):
                    await websocket.send(json.dumps(_estimate_failure_frame(error)))
                else:
                    await websocket.send(json.dumps({"status": "error", "message": "Telemetry was rejected."}))
                continue
            except Exception:
                if is_energy_message(message):
                    await websocket.send(json.dumps(_unavailable_estimate()))
                else:
                    await websocket.send(json.dumps({"status": "error", "message": "Telemetry was rejected."}))
                continue
            if pending_hello and message_type == "device_hello":
                pending_hello = False
                start_pump()
            # Energy frames get the commit-backed ACK or a bounded error.
            # Other frames keep hello_ack plus the legacy status success.
            for frame in telemetry_frames(message, result):
                await websocket.send(json.dumps(frame))
    finally:
        if pump is not None:
            pump.cancel()


async def main() -> None:
    host = os.environ.get("WS_HOST", "0.0.0.0")
    port = int(os.environ.get("WS_PORT", "8765"))
    async with serve(handler, host, port, ping_interval=20, ping_timeout=20):
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
