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
| `pc-dsp011-5kw` | DSP, usable 4500 W, `PC5K` | `esp32dev_hw_v011_5kw` |
| `pc-dsp011-35kw` | DSP, usable 3150 W, no `PC3K` | `esp32dev_hw_v011_35kw` |
| `pc-stm01-5kw` | STM32, 2.5.11 code paths | `esp32dev_stm01_5kw` |
| `pc-stm01-35kw` | STM32, 2.5.11 code paths | `esp32dev_stm01_35kw` |

The two STM32 images share the 2.5.11 collector behaviour. They differ only by the model string in the OTA check and the identity. They do not add a factory rating or a power limit. Both use firmware version `2.5.13`, the same number as the DSP images. The model inside the identity keeps a DSP file from installing on an STM32 unit, and the other way around.

Release files are named `<model>_v<version>.bin`. Examples: `pc-dsp011-5kw_v2.5.13.bin` and `pc-stm01-35kw_v2.5.13.bin`.

`POST /api/ota/check` sends the model. It is also the `model` field on `GET /v2/setup/info`. On the DSP images, `powercell_model` stays `PC5K` or null.

An offer is refused when:

- `model` is missing or is not this image's model, or
- the file does not contain `POWERCELL_FW|collector|<this model>|<offered version>|END`.

A wrong or missing model is refused before download. The reason is logged as `MODEL_REJECTED`.

## What the backend has to do

Register `pc-dsp011-5kw`, `pc-dsp011-35kw`, `pc-stm01-5kw`, and `pc-stm01-35kw`. Publish each image only under its own model, using the release file name above. Do not publish any of them as `esp32dev`.

Until a model is registered, a unit of that model gets no OTA offer.
