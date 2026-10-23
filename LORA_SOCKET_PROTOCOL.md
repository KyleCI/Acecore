# PowerCell LoRa smart-socket protocol

The collector uses an SX1278 at 433 MHz through RadioLib. Both ends must use:

- bandwidth: 125 kHz
- spreading factor: 7
- coding rate: 4/5
- sync word: `0x12`
- output power: 10 dBm
- RadioLib CRC handling

All protocol structs are packed (`#pragma pack(push, 1)`). Multi-byte values
use the ESP32's little-endian representation. The production packet is:

```cpp
struct LoraPacket {
    uint16_t magic;       // 0x5043
    uint8_t version;      // 1
    uint8_t type;         // 1 telemetry, 2 command, 3 ACK, 4 overcurrent
    uint8_t deviceID;     // stable socket ID, 1..255
    uint16_t sequence;    // increment for each telemetry/command packet
    float voltage;
    float current;
    float power;
    float energy;
    uint8_t relayState;   // 0 off, 1 on
    uint8_t flags;        // bit 0 = overcurrent active
};
```

## Flow

1. A socket periodically transmits a telemetry packet.
2. An unknown socket appears in the collector's existing approval queue as a
   synthetic MAC beginning `4C:4F:52:41:00` (`LORA`).
3. After approval, its telemetry enters the existing WebSocket upload payload.
4. The collector responds to telemetry with an ACK carrying the same sequence.
5. Backend relay commands become LoRa command packets.
6. The socket applies the relay state and returns an ACK with the command
   sequence and its resulting `relayState`.

The collector retries an unacknowledged command twice at 750 ms intervals and
accepts an ACK only when its device ID and sequence match the pending command.
Sockets should treat repeated command sequences idempotently: reapply or verify
the requested state and return the same ACK without causing side effects.

For transition testing, the collector also accepts the minimal project's
legacy `{float current, float voltage, bool relayState}` packet, both packed
(9 bytes) and naturally aligned (12 bytes). Legacy packets map to socket ID 1.
They support telemetry only; reliable command acknowledgement requires the
production packet.

## Collector pins

LoRa uses a separate HSPI bus because VSPI is occupied by the MPPT slave:

- SCK: GPIO14
- MISO: GPIO4
- MOSI: GPIO2
- NSS/CS: GPIO33
- RESET: GPIO22
- DIO0: GPIO21

These values are compile-time settings in `include/config.h` and must match the
production PCB wiring. GPIO2 is a boot-strapping pin; ensure the LoRa module
does not pull it high or low during reset, or remap MOSI to a board-safe pin.
