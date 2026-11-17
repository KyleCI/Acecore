# Factory rating

The rated active power is chosen when the factory builds and flashes the
ESP32 image. It is not an owner setting and it is not written by the app.

No new credential is required. The factory already chooses which firmware
file to upload. There is no rating password, no HTTP write, and no serial
or socket command. Flashing the application image does not erase NVS, the
login store, or the boot path.

## Images

| Environment | `firmware.bin` directory | Product name (display only) | Embedded `factory_variant` | Rated active W | Usable W | Overload |
| --- | --- | --- | --- | --- | --- | --- |
| `esp32dev_stm01_5kw` | `.pio/build/esp32dev_stm01_5kw/` | Powercell Ultra | `unrated` | none | none | off |
| `esp32dev_stm01_35kw` | `.pio/build/esp32dev_stm01_35kw/` | Powercell Prime Plus | `unrated` | none | none | off |
| `esp32dev_dsp011_5kw` | `.pio/build/esp32dev_dsp011_5kw/` | Powercell Ultra | `pc-dsp011-5kw` | 5000 | 4500 | armed |
| `esp32dev_dsp011_35kw` | `.pio/build/esp32dev_dsp011_35kw/` | Powercell Prime Plus | `pc-dsp011-35kw` | 3500 | 3150 | armed |
| `esp32dev_stm01_15kw` | `.pio/build/esp32dev_stm01_15kw/` | Powercell Prime | `unrated` | none | none | off |
| `esp32dev_dsp011_15kw` | `.pio/build/esp32dev_dsp011_15kw/` | Powercell Prime | `pc-dsp011-15kw` | 1500 (PROVISIONAL) | 1350 (PROVISIONAL) | armed |

**PROVISIONAL (2.5.33):** the Powercell Prime 1.5 kVA / 3 kWh rows are scaled
from the 3.5 kVA model (1500 W rated, usable 1500 x 3150 / 3500 = 1350 W)
until Caleb confirms the real 1.5 kVA hardware specs. The board, pins and
JK-PB BMS are the same as the matching 35kw image.

Usable watts are 90 percent of those three rated values only. A 5 kW unit gets
the `esp32dev_dsp011_5kw` image. A 3.5 kW unit gets the `esp32dev_dsp011_35kw`
image. A 1.5 kW unit gets the `esp32dev_dsp011_15kw` image. The STM32 images do not guess a rating.

After a build, `strings firmware.bin` on a rated image contains that image's
`factory_variant` and does not contain the other product's variant string.
The word `unrated` is also present as a fallback literal, so the factory
matches `pc-dsp011-5kw`, `pc-dsp011-35kw` or `pc-dsp011-15kw` rather than looking for the
absence of `unrated`. After boot, `GET /v1/system-rating` returns the same
variant. This repository does not flash a board.

## What boot does with a stored value

`system_cfg/rated` is not created or updated. If that key is absent, the
image rating is used. If the key is present and equal to the image, the
image rating is used and the key is left as it is. If the key is present
and different, or not 5000, 3500 or 1500, overload stays off (`rating_mismatch`
or `rating_invalid`) and the key is not rewritten. An image with no rating,
or a rating other than 5000, 3500 or 1500, stays off (`rating_unprovisioned` or
`unknown_rating`) even if the key holds one of those.

Changing a unit between 5 kW, 3.5 kW and 1.5 kW is a factory flash of
the other image. A normal flash leaves NVS in place, so a unit that never
had `system_cfg/rated` follows the new image. A leftover disagreeing key
keeps overload off until the factory removes that namespace with its own
NVS tool. This firmware will not erase it.

The app can still save a lower AC ceiling, from 50 W through the armed
usable watts. That ceiling cannot raise or turn off the factory bound.
While the rating is not armed, a ceiling save is rejected.
