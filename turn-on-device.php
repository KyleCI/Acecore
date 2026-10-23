<?php

declare(strict_types=1);

/**
 * REST-to-WebSocket command bridge.
 *
 * Public request:
 *   POST /api/turn-on-device.php?device_id=A4:F0:0F:77:DF:48
 *   API-KEY: <device API key>
 *   Content-Type: application/json
 *
 * The Ratchet process must already be running and listening on the private
 * command bridge configured by WEBSOCKET_BRIDGE_HOST/WEBSOCKET_BRIDGE_PORT.
 */

const MAX_REQUEST_BYTES = 16384;
const CONNECT_TIMEOUT_SECONDS = 3.0;
const READ_TIMEOUT_SECONDS = 5;

header('Content-Type: application/json; charset=utf-8');
header('Cache-Control: no-store');

if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
    header('Allow: POST, OPTIONS');
    http_response_code(204);
    exit;
}

if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    header('Allow: POST, OPTIONS');
    respond(405, 'error', 'Only POST requests are accepted.');
}

$deviceId = normalizeDeviceId((string) ($_GET['device_id'] ?? ''));
if ($deviceId === null) {
    respond(400, 'error', 'A valid device_id MAC address is required.');
}

$apiKey = requestHeader('API-KEY');
if ($apiKey === '') {
    respond(401, 'error', 'The API-KEY header is required.');
}

$contentLength = (int) ($_SERVER['CONTENT_LENGTH'] ?? 0);
if ($contentLength > MAX_REQUEST_BYTES) {
    respond(413, 'error', 'Request body is too large.');
}

$rawBody = file_get_contents('php://input', false, null, 0, MAX_REQUEST_BYTES + 1);
if ($rawBody === false || $rawBody === '') {
    respond(400, 'error', 'A JSON request body is required.');
}
if (strlen($rawBody) > MAX_REQUEST_BYTES) {
    respond(413, 'error', 'Request body is too large.');
}

try {
    $command = json_decode($rawBody, true, 16, JSON_THROW_ON_ERROR);
} catch (JsonException $exception) {
    respond(400, 'error', 'Malformed JSON request body.');
}

if (!is_array($command)) {
    respond(400, 'error', 'The JSON body must be an object.');
}

// A new app action may omit command_id. Generate it at this HTTP boundary so
// Ratchet and the ESP32 receive one stable correlation/idempotency key. A
// caller may still supply the original ID when deliberately retrying the same
// logical command.
$commandIdGenerated = !array_key_exists('command_id', $command) ||
    $command['command_id'] === null ||
    $command['command_id'] === '';

if ($commandIdGenerated) {
    $command['command_id'] = createCommandId();
}

// Let normal app requests omit expiry; explicit values are preserved for
// deliberate retries and protocol testing.
if (!array_key_exists('expires_at', $command)) {
    $command['expires_at'] = time() + 30;
}

$validationError = validateCommand($command);
if ($validationError !== null) {
    respond(400, 'error', $validationError);
}

$bridgeRequest = json_encode([
    'api_key' => $apiKey,
    'device_id' => $deviceId,
    'command' => $command,
], JSON_UNESCAPED_SLASHES | JSON_THROW_ON_ERROR);

$bridgeHost = getenv('WEBSOCKET_BRIDGE_HOST') ?: '127.0.0.1';
$bridgePort = parsePort(getenv('WEBSOCKET_BRIDGE_PORT') ?: '8082');
$bridgeAddress = sprintf('tcp://%s:%d', $bridgeHost, $bridgePort);

$errorNumber = 0;
$errorMessage = '';
$socket = @stream_socket_client(
    $bridgeAddress,
    $errorNumber,
    $errorMessage,
    CONNECT_TIMEOUT_SECONDS,
    STREAM_CLIENT_CONNECT
);

if ($socket === false) {
    error_log(sprintf(
        'WebSocket bridge unavailable at %s: [%d] %s',
        $bridgeAddress,
        $errorNumber,
        $errorMessage
    ));
    respond(503, 'error', 'The device command service is unavailable.');
}

stream_set_timeout($socket, READ_TIMEOUT_SECONDS);
$wirePayload = $bridgeRequest . "\n";
$payloadLength = strlen($wirePayload);
$written = 0;

while ($written < $payloadLength) {
    $count = fwrite($socket, substr($wirePayload, $written));
    if ($count === false || $count === 0) {
        fclose($socket);
        respond(502, 'error', 'Failed to send the command to the command service.');
    }
    $written += $count;
}

$responseLine = fgets($socket, MAX_REQUEST_BYTES + 1);
$streamMetadata = stream_get_meta_data($socket);
fclose($socket);

if ($responseLine === false) {
    if (($streamMetadata['timed_out'] ?? false) === true) {
        respond(504, 'error', 'The command service timed out.');
    }
    respond(502, 'error', 'The command service returned no response.');
}

try {
    $bridgeResponse = json_decode(trim($responseLine), true, 16, JSON_THROW_ON_ERROR);
} catch (JsonException $exception) {
    error_log('Invalid command bridge response: ' . trim($responseLine));
    respond(502, 'error', 'The command service returned an invalid response.');
}

if (!is_array($bridgeResponse)) {
    respond(502, 'error', 'The command service returned an invalid response.');
}

// Ensure the app always receives the effective ID even if an older bridge
// response omits it.
if (!isset($bridgeResponse['data']) || !is_array($bridgeResponse['data'])) {
    $bridgeResponse['data'] = [];
}
$bridgeResponse['data']['command_id'] = $command['command_id'];
$bridgeResponse['data']['command_id_source'] = $commandIdGenerated
    ? 'server'
    : 'client';

$statusCode = (int) ($bridgeResponse['code'] ?? 502);
if ($statusCode < 100 || $statusCode > 599) {
    $statusCode = 502;
}

http_response_code($statusCode);
echo json_encode($bridgeResponse, JSON_UNESCAPED_SLASHES | JSON_THROW_ON_ERROR);
exit;

/** @return never */
function respond(int $code, string $status, string $message, ?array $data = null): void
{
    http_response_code($code);
    $response = [
        'status' => $status,
        'code' => $code,
        'message' => $message,
    ];
    if ($data !== null) {
        $response['data'] = $data;
    }
    echo json_encode($response, JSON_UNESCAPED_SLASHES | JSON_THROW_ON_ERROR);
    exit;
}

function requestHeader(string $name): string
{
    if (function_exists('getallheaders')) {
        foreach (getallheaders() as $headerName => $value) {
            // Accept normal HTTP case differences, but standardize clients on
            // API-KEY. API_Key also arrives here from some existing clients.
            if (strcasecmp(str_replace('_', '-', $headerName), $name) === 0) {
                return trim((string) $value);
            }
        }
    }

    $serverKey = 'HTTP_' . strtoupper(str_replace('-', '_', $name));
    return trim((string) ($_SERVER[$serverKey] ?? ''));
}

function normalizeDeviceId(string $deviceId): ?string
{
    $deviceId = strtoupper(trim($deviceId));
    if (preg_match('/^(?:[0-9A-F]{2}:){5}[0-9A-F]{2}$/', $deviceId) !== 1) {
        return null;
    }
    return $deviceId;
}

function createCommandId(): string
{
    return 'cmd-' . bin2hex(random_bytes(16));
}

/** @param array<string, mixed> $command */
function validateCommand(array $command): ?string
{
    if (($command['type'] ?? null) !== 'set_state') {
        return 'type must be set_state.';
    }

    $commandId = $command['command_id'] ?? null;
    if (!is_string($commandId) || strlen($commandId) < 1 || strlen($commandId) > 47) {
        return 'command_id must be a string containing 1 to 47 characters.';
    }

    if (!in_array($command['target_type'] ?? null, ['gateway', 'socket'], true)) {
        return 'target_type must be gateway or socket.';
    }

    if (!array_key_exists('target_id', $command)) {
        return 'target_id is required.';
    }

    if ($command['target_type'] === 'gateway' &&
        (!is_string($command['target_id']) || trim($command['target_id']) === '')) {
        return 'Gateway target_id must be a non-empty string.';
    }

    if ($command['target_type'] === 'socket' &&
        (filter_var($command['target_id'], FILTER_VALIDATE_INT) === false ||
         (int) $command['target_id'] < 0)) {
        return 'Socket target_id must be a non-negative integer.';
    }

    if (!in_array($command['desired_state'] ?? null, ['on', 'off'], true)) {
        return 'desired_state must be on or off.';
    }

    if (!array_key_exists('expires_at', $command) || !is_int($command['expires_at'])) {
        return 'expires_at must be a Unix timestamp expressed as an integer.';
    }

    if ($command['expires_at'] <= time()) {
        return 'expires_at must be in the future.';
    }

    return null;
}

function parsePort(string $value): int
{
    $port = filter_var($value, FILTER_VALIDATE_INT, [
        'options' => ['min_range' => 1, 'max_range' => 65535],
    ]);
    return $port === false ? 8082 : $port;
}
