<?php

declare(strict_types=1);

use Ratchet\ConnectionInterface;
use Ratchet\Http\HttpServer;
use Ratchet\MessageComponentInterface;
use Ratchet\Server\IoServer;
use Ratchet\WebSocket\WsServer;
use React\EventLoop\Loop;
use React\Socket\ConnectionInterface as SocketConnection;
use React\Socket\SocketServer;

require __DIR__ . '/../vendor/autoload.php';
require __DIR__ . '/../main/wp-includes/web-contents.php';

const WS_MAX_DEVICE_MESSAGE_BYTES = 16384;
const WS_MAX_BRIDGE_REQUEST_BYTES = 16384;
const WS_COMMAND_ID_MAX_LENGTH = 47;

/** @param array<string,mixed> $context */
function wsLog(string $level, string $message, array $context = []): void
{
    $line = sprintf('[%s] [%s] %s', gmdate('Y-m-d\TH:i:s\Z'), strtoupper($level), $message);
    if ($context !== []) {
        $json = json_encode($context, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
        if ($json !== false) {
            $line .= ' | ' . $json;
        }
    }
    $line .= PHP_EOL;
    error_log($line, 3, __DIR__ . '/websocket-server.log');
    fwrite(in_array(strtoupper($level), ['ERROR', 'WARNING'], true) ? STDERR : STDOUT, $line);
}

function normalizeWsDeviceId(string $deviceId): ?string
{
    $deviceId = strtoupper(trim($deviceId));
    return preg_match('/^(?:[0-9A-F]{2}:){5}[0-9A-F]{2}$/', $deviceId) === 1
        ? $deviceId
        : null;
}

final class PowerCellWebSocketServer implements MessageComponentInterface
{
    /** @var array<string,ConnectionInterface> */
    private array $devices = [];
    private mysqli $database;
    /** @var callable */
    private $databaseFactory;
    private ?string $deviceOwnershipColumn = null;

    public function __construct(mysqli $database, callable $databaseFactory)
    {
        $this->database = $database;
        $this->databaseFactory = $databaseFactory;
        $this->detectDeviceOwnershipColumn();
        wsLog('INFO', 'PowerCell WebSocket server initialized.', [
            'device_ownership_column' => $this->deviceOwnershipColumn ?? 'none',
        ]);
    }

    public function onOpen(ConnectionInterface $connection): void
    {
        $request = $connection->httpRequest;
        $path = $request->getUri()->getPath();
        parse_str($request->getUri()->getQuery(), $query);
        $rawDeviceId = (string)($query['device_id'] ?? '');
        $deviceId = normalizeWsDeviceId($rawDeviceId);
        $apiKey = trim($request->getHeaderLine('API-KEY'));

        wsLog('INFO', 'Incoming WebSocket connection.', [
            'path' => $path,
            'device_id' => $rawDeviceId,
            'api_key_present' => $apiKey !== '',
        ]);

        if ($path !== '/ws/device' || $deviceId === null || $apiKey === '') {
            wsLog('WARNING', 'WebSocket connection rejected.', [
                'path_valid' => $path === '/ws/device',
                'device_id_valid' => $deviceId !== null,
                'api_key_present' => $apiKey !== '',
            ]);
            $connection->close();
            return;
        }

        $authorization = $this->authorizeDevice($apiKey, $deviceId);
        if (!$authorization['authorized']) {
            wsLog('WARNING', 'WebSocket authentication failed.', [
                'device_id' => $deviceId,
                'reason' => $authorization['reason'],
            ]);
            $connection->close();
            return;
        }

        if (isset($this->devices[$deviceId])) {
            $oldConnection = $this->devices[$deviceId];
            unset($this->devices[$deviceId]);
            try {
                $oldConnection->close();
            } catch (Throwable $exception) {
                wsLog('WARNING', 'Could not close replaced connection.', [
                    'device_id' => $deviceId,
                    'error' => $exception->getMessage(),
                ]);
            }
        }

        $this->devices[$deviceId] = $connection;
        $connection->deviceId = $deviceId;
        $connection->authenticated = true;

        wsLog('INFO', 'Device WebSocket connected successfully.', [
            'device_id' => $deviceId,
            'connected_devices' => count($this->devices),
        ]);

        $connection->send(json_encode([
            'type' => 'connection_ack',
            'status' => 'connected',
            'device_id' => $deviceId,
            'server_time' => time(),
        ], JSON_UNESCAPED_SLASHES));
    }

    public function onMessage(ConnectionInterface $from, $message): void
    {
        $deviceId = isset($from->deviceId) ? (string)$from->deviceId : 'unknown';
        $raw = (string)$message;
        if (strlen($raw) > WS_MAX_DEVICE_MESSAGE_BYTES) {
            wsLog('WARNING', 'Oversized device message rejected.', ['device_id' => $deviceId]);
            return;
        }

        try {
            $data = json_decode($raw, true, 32, JSON_THROW_ON_ERROR);
        } catch (JsonException $exception) {
            wsLog('WARNING', 'Invalid JSON received from device.', [
                'device_id' => $deviceId,
                'error' => $exception->getMessage(),
            ]);
            return;
        }
        if (!is_array($data)) {
            return;
        }

        $type = (string)($data['type'] ?? '');
        if (!in_array($type, ['state_snapshot', 'command_result', 'telemetry', 'pong'], true)) {
            wsLog('WARNING', 'Unsupported message type received from device.', [
                'device_id' => $deviceId,
                'type' => $type,
            ]);
            return;
        }

        $reportedId = normalizeWsDeviceId((string)($data['gateway_id'] ?? ''));
        if ($reportedId === null || $reportedId !== $deviceId) {
            wsLog('WARNING', 'Device identity mismatch.', [
                'connected_device_id' => $deviceId,
                'reported_gateway_id' => $data['gateway_id'] ?? null,
                'type' => $type,
            ]);
            return;
        }

        if ($type === 'command_result') {
            $this->processCommandResult($deviceId, $data);
        } elseif ($type === 'state_snapshot') {
            $this->processStateSnapshot($deviceId, $data);
        } elseif ($type === 'telemetry') {
            wsLog('INFO', 'Telemetry received from device.', [
                'device_id' => $deviceId,
                'bytes' => strlen($raw),
            ]);
            $this->logDeviceEvent($deviceId, $data);
        }
    }

    /** @param array<string,mixed> $data */
    private function processCommandResult(string $deviceId, array $data): void
    {
        $commandId = trim((string)($data['command_id'] ?? ''));
        $result = (string)($data['result'] ?? '');
        $desiredState = (string)($data['desired_state'] ?? '');
        $reportedState = (string)($data['reported_state'] ?? '');
        if ($reportedState === '' && $result === 'confirmed') {
            $reportedState = $desiredState;
        }
        $error = (string)($data['error'] ?? $data['reason'] ?? '');

        if ($commandId === '' || !in_array($result, ['confirmed', 'rejected'], true)) {
            wsLog('WARNING', 'Invalid command_result received.', [
                'device_id' => $deviceId,
                'command_id' => $commandId,
                'result' => $result,
            ]);
            return;
        }

        wsLog($result === 'confirmed' ? 'INFO' : 'WARNING', 'Device command result received.', [
            'device_id' => $deviceId,
            'command_id' => $commandId,
            'target_type' => $data['target_type'] ?? null,
            'target_id' => $data['target_id'] ?? null,
            'desired_state' => $desiredState,
            'reported_state' => $reportedState,
            'result' => $result,
            'error' => $error,
        ]);
        $this->logDeviceEvent($deviceId, $data);
    }

    /** @param array<string,mixed> $data */
    private function processStateSnapshot(string $deviceId, array $data): void
    {
        wsLog('INFO', 'State snapshot received from device.', [
            'device_id' => $deviceId,
            'gateway_name' => $data['gateway_name'] ?? null,
            'socket_count' => is_array($data['sockets'] ?? null) ? count($data['sockets']) : 0,
        ]);
        $this->logDeviceEvent($deviceId, $data);
    }

    public function onClose(ConnectionInterface $connection): void
    {
        $deviceId = isset($connection->deviceId) ? (string)$connection->deviceId : 'unknown';
        if (isset($this->devices[$deviceId]) && $this->devices[$deviceId] === $connection) {
            unset($this->devices[$deviceId]);
            wsLog('WARNING', 'Device disconnected.', [
                'device_id' => $deviceId,
                'connected_devices' => count($this->devices),
            ]);
        } else {
            wsLog('INFO', 'Stale WebSocket connection closed.', ['device_id' => $deviceId]);
        }
    }

    public function onError(ConnectionInterface $connection, Exception $exception): void
    {
        $deviceId = isset($connection->deviceId) ? (string)$connection->deviceId : 'unknown';
        wsLog('ERROR', 'WebSocket connection error.', [
            'device_id' => $deviceId,
            'error' => $exception->getMessage(),
        ]);
        $connection->close();
    }

    /**
     * @param array<string,mixed> $request
     * @return array<string,mixed>
     */
    public function relayCommand(array $request): array
    {
        $apiKey = trim((string)($request['api_key'] ?? ''));
        $deviceId = normalizeWsDeviceId((string)($request['device_id'] ?? ''));
        $command = $request['command'] ?? null;

        if ($apiKey === '') {
            return $this->errorResponse(401, 'Missing API key.', 'missing_api_key');
        }
        if ($deviceId === null) {
            return $this->errorResponse(400, 'A valid device_id is required.', 'invalid_device_id');
        }

        $authorization = $this->authorizeDevice($apiKey, $deviceId);
        if (!$authorization['authorized']) {
            return $this->errorResponse(401, $authorization['reason'], $authorization['error_type']);
        }
        if (!is_array($command)) {
            return $this->errorResponse(400, 'command must be a JSON object.', 'invalid_command');
        }

        $validation = $this->validateCommand($command);
        if (!$validation['valid']) {
            return $this->errorResponse(400, $validation['reason'], $validation['error_type'], [
                'device_id' => $deviceId,
                'field' => $validation['field'] ?? null,
                'received' => $validation['received'] ?? null,
                'allowed' => $validation['allowed'] ?? null,
            ]);
        }
        if (!isset($this->devices[$deviceId])) {
            return $this->errorResponse(503, 'Device is offline.', 'device_offline', [
                'device_id' => $deviceId,
                'command_id' => $command['command_id'],
                'delivery_status' => 'not_sent',
            ]);
        }

        try {
            $payload = json_encode($command, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE | JSON_THROW_ON_ERROR);
            $this->devices[$deviceId]->send($payload);
        } catch (Throwable $exception) {
            wsLog('ERROR', 'WebSocket send to ESP32 failed.', [
                'device_id' => $deviceId,
                'command_id' => $command['command_id'],
                'error' => $exception->getMessage(),
            ]);
            return $this->errorResponse(502, 'Failed to send command to the device.', 'websocket_send_failed', [
                'delivery_status' => 'not_sent',
            ]);
        }

        wsLog('INFO', 'Command successfully delivered to ESP32 WebSocket.', [
            'device_id' => $deviceId,
            'command_id' => $command['command_id'],
            'target_type' => $command['target_type'],
            'target_id' => $command['target_id'],
            'desired_state' => $command['desired_state'],
            'expires_at' => $command['expires_at'],
        ]);

        return [
            'status' => 'success',
            'code' => 202,
            'message' => 'Command sent; awaiting device confirmation.',
            'data' => $command + [
                'device_id' => $deviceId,
                'delivery_status' => 'sent',
            ],
        ];
    }

    /**
     * @param array<string,mixed> $command
     * @return array<string,mixed>
     */
    private function validateCommand(array $command): array
    {
        if (($command['type'] ?? null) !== 'set_state') {
            return $this->invalid('invalid_command_type', 'type must be set_state.', 'type', $command['type'] ?? null);
        }

        $commandId = $command['command_id'] ?? null;
        if (!is_string($commandId) || strlen($commandId) < 1 || strlen($commandId) > WS_COMMAND_ID_MAX_LENGTH) {
            return $this->invalid('invalid_command_id', 'command_id must contain 1 to 47 characters.', 'command_id', $commandId);
        }

        $targetType = $command['target_type'] ?? null;
        if (!is_string($targetType) || !in_array($targetType, ['gateway', 'socket'], true)) {
            return $this->invalid('invalid_target_type', 'target_type must be gateway or socket.', 'target_type', $targetType, ['gateway', 'socket']);
        }
        if (!array_key_exists('target_id', $command)) {
            return $this->invalid('missing_target_id', 'target_id is required.', 'target_id', null);
        }

        $targetId = $command['target_id'];
        if ($targetType === 'gateway' && (!is_string($targetId) || trim($targetId) === '')) {
            return $this->invalid('invalid_gateway_target_id', 'Gateway target_id must be a non-empty string.', 'target_id', $targetId);
        }
        if ($targetType === 'socket' && (filter_var($targetId, FILTER_VALIDATE_INT) === false || (int)$targetId < 0)) {
            return $this->invalid('invalid_socket_target_id', 'Socket target_id must be a non-negative integer.', 'target_id', $targetId);
        }

        $desiredState = $command['desired_state'] ?? null;
        if (!is_string($desiredState) || !in_array($desiredState, ['on', 'off'], true)) {
            return $this->invalid('invalid_desired_state', 'desired_state must be on or off.', 'desired_state', $desiredState, ['on', 'off']);
        }

        $expiresAt = $command['expires_at'] ?? null;
        if (!is_int($expiresAt)) {
            return $this->invalid('invalid_expiration', 'expires_at must be an integer Unix timestamp.', 'expires_at', $expiresAt);
        }
        if ($expiresAt <= time()) {
            return $this->invalid('command_expired', 'expires_at must be in the future.', 'expires_at', $expiresAt);
        }

        return ['valid' => true];
    }

    /** @return array<string,mixed> */
    private function invalid(string $type, string $reason, string $field, $received, array $allowed = []): array
    {
        return [
            'valid' => false,
            'error_type' => $type,
            'reason' => $reason,
            'field' => $field,
            'received' => $received,
            'allowed' => $allowed,
        ];
    }

    /** @return array{authorized:bool,reason:string,error_type:string} */
    private function authorizeDevice(string $apiKey, string $deviceId): array
    {
        for ($attempt = 0; $attempt < 2; ++$attempt) {
            if (!$this->ensureDatabaseConnection()) {
                continue;
            }

            if ($this->deviceOwnershipColumn !== null) {
                $column = $this->deviceOwnershipColumn;
                $sql = sprintf('SELECT 1 FROM api_user WHERE api_key = ? AND `%s` = ? LIMIT 1', $column);
                $statement = $this->database->prepare($sql);
                if ($statement === false) {
                    $this->replaceDatabaseConnection();
                    continue;
                }
                $statement->bind_param('ss', $apiKey, $deviceId);
            } else {
                $statement = $this->database->prepare('SELECT 1 FROM api_user WHERE api_key = ? LIMIT 1');
                if ($statement === false) {
                    $this->replaceDatabaseConnection();
                    continue;
                }
                $statement->bind_param('s', $apiKey);
            }

            if (!$statement->execute()) {
                $statement->close();
                $this->replaceDatabaseConnection();
                continue;
            }
            $statement->store_result();
            $valid = $statement->num_rows === 1;
            $statement->close();
            if (!$valid) {
                return ['authorized' => false, 'reason' => 'Invalid or unauthorized API key.', 'error_type' => 'device_not_authorized'];
            }
            if ($this->deviceOwnershipColumn === null) {
                wsLog('WARNING', 'API key accepted without per-device ownership enforcement.', ['device_id' => $deviceId]);
            }
            return ['authorized' => true, 'reason' => 'Authorized.', 'error_type' => ''];
        }
        return ['authorized' => false, 'reason' => 'Database authorization check failed.', 'error_type' => 'database_authorization_failed'];
    }

    private function ensureDatabaseConnection(): bool
    {
        try {
            if ($this->database->ping()) {
                return true;
            }
        } catch (Throwable $exception) {
            wsLog('WARNING', 'Database ping failed.', ['error' => $exception->getMessage()]);
        }
        return $this->replaceDatabaseConnection();
    }

    private function replaceDatabaseConnection(): bool
    {
        try {
            $database = call_user_func($this->databaseFactory);
            if (!$database instanceof mysqli || $database->connect_errno) {
                return false;
            }
            $this->database->close();
            $this->database = $database;
            return true;
        } catch (Throwable $exception) {
            wsLog('ERROR', 'Database reconnection failed.', ['error' => $exception->getMessage()]);
            return false;
        }
    }

    private function detectDeviceOwnershipColumn(): void
    {
        if (!$this->ensureDatabaseConnection()) {
            return;
        }
        $result = @$this->database->query('SHOW COLUMNS FROM api_user');
        if (!$result) {
            return;
        }
        $columns = [];
        while ($row = $result->fetch_assoc()) {
            $columns[] = $row['Field'] ?? '';
        }
        $result->free();
        foreach (['device_id', 'gateway_id'] as $candidate) {
            if (in_array($candidate, $columns, true)) {
                $this->deviceOwnershipColumn = $candidate;
                return;
            }
        }
        wsLog('WARNING', 'No device ownership field found in api_user.', [
            'expected_optional_columns' => ['device_id', 'gateway_id'],
        ]);
    }

    /** @param array<string,mixed> $data */
    private function logDeviceEvent(string $deviceId, array $data): void
    {
        $json = json_encode([
            'received_at' => gmdate('c'),
            'device_id' => $deviceId,
            'message' => $data,
        ], JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
        if ($json !== false) {
            @file_put_contents(__DIR__ . '/websocket-events.log', $json . PHP_EOL, FILE_APPEND | LOCK_EX);
        }
    }

    /** @return array<string,mixed> */
    private function errorResponse(int $code, string $message, string $errorType, array $extra = []): array
    {
        return [
            'status' => 'error',
            'code' => $code,
            'message' => $message,
            'data' => array_merge(['error' => ['type' => $errorType]], $extra),
        ];
    }
}

mysqli_report(MYSQLI_REPORT_OFF);
$databaseFactory = static function () use ($db_host, $db_user, $db_pass, $db_name): mysqli {
    return @new mysqli($db_host, $db_user, $db_pass, $db_name);
};
$database = $databaseFactory();
if ($database->connect_errno) {
    wsLog('ERROR', 'Initial database connection failed.', [
        'errno' => $database->connect_errno,
        'error' => $database->connect_error,
    ]);
    exit(1);
}

$loop = Loop::get();
$server = new PowerCellWebSocketServer($database, $databaseFactory);
$webSocketPort = (int)(getenv('WEBSOCKET_PORT') ?: 8081);
$bridgePort = (int)(getenv('WEBSOCKET_BRIDGE_PORT') ?: 8082);

$webSocketSocket = new SocketServer("127.0.0.1:{$webSocketPort}", [], $loop);
new IoServer(new HttpServer(new WsServer($server)), $webSocketSocket, $loop);

$bridge = new SocketServer("127.0.0.1:{$bridgePort}", [], $loop);
$bridge->on('connection', function (SocketConnection $connection) use ($server): void {
    $buffer = '';
    $handled = false;
    wsLog('INFO', 'HTTP command bridge client connected.');

    $connection->on('data', function ($chunk) use (&$buffer, &$handled, $connection, $server): void {
        if ($handled) {
            return;
        }
        $buffer .= (string)$chunk;
        if (strlen($buffer) > WS_MAX_BRIDGE_REQUEST_BYTES) {
            $handled = true;
            $connection->end(json_encode([
                'status' => 'error',
                'code' => 413,
                'message' => 'Bridge request is too large.',
                'data' => ['error' => ['type' => 'bridge_request_too_large']],
            ]) . "\n");
            return;
        }

        $newline = strpos($buffer, "\n");
        if ($newline === false) {
            return;
        }
        $handled = true;
        $line = trim(substr($buffer, 0, $newline));

        try {
            $request = json_decode($line, true, 32, JSON_THROW_ON_ERROR);
            $response = is_array($request)
                ? $server->relayCommand($request)
                : [
                    'status' => 'error',
                    'code' => 400,
                    'message' => 'Bridge request must be a JSON object.',
                    'data' => ['error' => ['type' => 'invalid_bridge_request']],
                ];
        } catch (JsonException $exception) {
            $response = [
                'status' => 'error',
                'code' => 400,
                'message' => 'Invalid bridge request JSON.',
                'data' => ['error' => ['type' => 'invalid_bridge_json']],
            ];
        } catch (Throwable $exception) {
            wsLog('ERROR', 'Unhandled command bridge exception.', ['error' => $exception->getMessage()]);
            $response = [
                'status' => 'error',
                'code' => 500,
                'message' => 'Internal WebSocket server error.',
                'data' => ['error' => ['type' => 'internal_server_error']],
            ];
        }

        $encoded = json_encode($response, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
        $connection->end(($encoded !== false ? $encoded : '{"status":"error","code":500}') . "\n");
    });

    $connection->on('error', function (Throwable $exception): void {
        wsLog('ERROR', 'Command bridge socket error.', ['error' => $exception->getMessage()]);
    });
});

wsLog('INFO', 'PowerCell WebSocket server starting.');
wsLog('INFO', 'WebSocket listener ready.', ['host' => '127.0.0.1', 'port' => $webSocketPort]);
wsLog('INFO', 'Command bridge ready.', ['host' => '127.0.0.1', 'port' => $bridgePort]);
$loop->run();
