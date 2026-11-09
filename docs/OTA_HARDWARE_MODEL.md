# OTA hardware model

Stock `esp32dev` and the hardware v0.1.1 images must not install each other's firmware.

Deployed 2.5.11 accepts an image only when the offered version differs from `2.5.11` and the file contains:

```text
POWERCELL_FW|collector|esp32dev|<offered version>|END
```

It does not read a model field on the offer. A hardware image does not contain that string, so a stock unit rejects it with no stock firmware change.

## Models

| Image | `POWERCELL_HARDWARE_MODEL` | Version | Usable power |
|---|---|---|---|
| `esp32dev` | `esp32dev` | `2.5.11` | from NVS, or none |
| `esp32dev_hw_v011_5kw` | `esp32dev-hw011-5kw` | `2.5.13` | 4500 W, `PC5K` |
| `esp32dev_hw_v011_35kw` | `esp32dev-hw011-35kw` | `2.5.13` | 3150 W, no `PC3K` |

The two ratings do not share a model. A newer 5 kW image would otherwise be a valid update for a 3.5 kW unit and would move its overload trip from 3150 W to 4500 W. The pin map is the same. The rating is not.

`POST /api/ota/check` sends this model string. It is also the `model` field on `GET /v2/setup/info`. `powercell_model` stays `PC5K` or null.

A hardware image refuses an offer when:

- the file does not contain `POWERCELL_FW|collector|<its model>|<offered version>|END`, or
- the offer includes `model` and that value is not its model.

A missing `model` field is left to the identity scan. A wrong model is refused before download. The reason is logged as `MODEL_REJECTED`.

## What the backend has to do

Register `esp32dev-hw011-5kw` and `esp32dev-hw011-35kw`. Publish each hardware image only under its own model. Do not publish them as `esp32dev`.

Until those models exist, a hardware unit gets no OTA offer. That is the safe state: it will not install a stock image.

Stock units stay on model `esp32dev`. Nothing on the backend has to change for them to keep rejecting hardware images.
