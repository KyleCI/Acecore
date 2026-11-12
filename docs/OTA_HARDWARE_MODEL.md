# OTA hardware model

Each image accepts an update only when the offer names that image's model and the file contains the matching identity:

```text
POWERCELL_FW|collector|<model>|<offered version>|END
```

`esp32dev` is not a model in this tree. Nothing here accepts it. Units already in the field on `esp32dev` will be recalled and reflashed at the factory.

The model is the OTA hardware identity. It is not the serial number and it is not the customer product name (Powercell Prime or Ultra).

## Models

| Model | Hardware | In this tree |
|---|---|---|
| `pc-dsp011-5kw` | DSP, usable 4500 W, `PC5K` | `esp32dev_dsp011_5kw` |
| `pc-dsp011-35kw` | DSP, usable 3150 W, NVS `PC3_5K` | `esp32dev_dsp011_35kw` |
| `pc-stm01-5kw` | STM32, 2.5.11 code paths | `esp32dev_stm01_5kw` |
| `pc-stm01-35kw` | STM32, 2.5.11 code paths | `esp32dev_stm01_35kw` |

The two STM32 images share the 2.5.11 collector behaviour. They differ only by the model string in the OTA check and the identity. They do not add a factory rating or a power limit. Both use firmware version `2.5.19`, the same number as the DSP images. The model inside the identity keeps a DSP file from installing on an STM32 unit, and the other way around.

Release files are named `<model>_v<version>.bin`. Examples: `pc-dsp011-5kw_v2.5.19.bin` and `pc-stm01-35kw_v2.5.19.bin`.

`POST /api/ota/check` sends the model. It is also the `model` field on `GET /v2/setup/info`. On the DSP images, the factory rating path sets `powercell_model` to `PC5K` (5 kW) or leaves it null (3.5 kW); an NVS `SET model PC3_5K` still applies for INFO / live telemetry / load_pct.

An offer is refused when:

- `model` is missing or is not this image's model, or
- the file does not contain `POWERCELL_FW|collector|<this model>|<offered version>|END`.

A wrong or missing model is refused before download. The reason is logged as `MODEL_REJECTED`.

## Factory registration

`INFO` prints `hardware_model=<POWERCELL_HARDWARE_MODEL>` on its own line. The existing `model=` line is still the NVS unit model (`PC3K`, `PC3_5K`, `PC5K`, `PC10K`, or `unset`). `tools/factory.py --model PC3_5K` only sends `SET model` and does not choose the OTA model. Factory prints a WARNING (not a hard fail) when `--model` disagrees with the image rating (for example `PC5K` on `pc-dsp011-35kw`).

`--register` reads `hardware_model` from the unit. It refuses to register when the value is missing or is not one of the four models above. The POST body is:

```json
{"device_id":"<12 hex>","device_secret":"<64 hex>","mac":"<colon mac>","gateway_id":"ACE-NNNNNN","firmware_version":"2.5.19","device_kind":"collector","model":"pc-dsp011-5kw"}
```

`model` is the only new field. The same value is stored on the `units.csv` row as `hardware_model` and written into `labels/<serial>.txt`.

## What the backend has to do

`POST /api/devices/register` must accept `model` and store it on the device record the control station displays. If the handler drops unknown JSON fields, the station will still show an empty model. The stored value is one of `pc-dsp011-5kw`, `pc-dsp011-35kw`, `pc-stm01-5kw`, or `pc-stm01-35kw`.

Also register those four models for OTA. Publish each image only under its own model, using the release file name above. Do not publish any of them as `esp32dev`.

Until a model is registered, a unit of that model gets no OTA offer.
