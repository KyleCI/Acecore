# Factory rating

The rated active power is chosen when the factory builds and flashes the
ESP32 image. It is not an owner setting and it is not written by the app.

No new credential is required. The factory already chooses which firmware
file to upload. There is no rating password, no HTTP write, and no serial
or socket command. Flashing the application image does not erase NVS, the
login store, or the boot path.

## Images

| Environment | `firmware.bin` directory | Embedded `factory_variant` | Rated active W | Usable W | Overload |
| --- | --- | --- | --- | --- | --- |
| `esp32dev` | `.pio/build/esp32dev/` | `unrated` | none | none | off |
| `esp32dev_hw_v011` | `.pio/build/esp32dev_hw_v011/` | `unrated` | none | none | off |
| `esp32dev_hw_v011_5kw` | `.pio/build/esp32dev_hw_v011_5kw/` | `hw-v0.1.1-5kw` | 5000 | 4500 | armed |
| `esp32dev_hw_v011_35kw` | `.pio/build/esp32dev_hw_v011_35kw/` | `hw-v0.1.1-3.5kw` | 3500 | 3150 | armed |

Usable watts are 90 percent of those two rated values only. A 5 kW unit gets
the `5kw` image. A 3.5 kW unit gets the `35kw` image. The original collector
and the unrated v0.1.1 image do not guess a rating.

After a build, `strings firmware.bin` on a rated image contains that image's
`factory_variant` and does not contain the other product's variant string.
The word `unrated` is also present as a fallback literal, so the factory
matches `hw-v0.1.1-5kw` or `hw-v0.1.1-3.5kw` rather than looking for the
absence of `unrated`. After boot, `GET /v1/system-rating` returns the same
variant. This repository does not flash a board.

## What boot does with a stored value

`system_cfg/rated` is not created or updated. If that key is absent, the
image rating is used. If the key is present and equal to the image, the
image rating is used and the key is left as it is. If the key is present
and different, or not 5000 or 3500, overload stays off (`rating_mismatch`
or `rating_invalid`) and the key is not rewritten. An image with no rating,
or a rating other than 5000 or 3500, stays off (`rating_unprovisioned` or
`unknown_rating`) even if the key holds 5000 or 3500.

Changing a unit from 5 kW to 3.5 kW, or the other way, is a factory flash of
the other image. A normal flash leaves NVS in place, so a unit that never
had `system_cfg/rated` follows the new image. A leftover disagreeing key
keeps overload off until the factory removes that namespace with its own
NVS tool. This firmware will not erase it.

The app can still save a lower AC ceiling, from 50 W through the armed
usable watts. That ceiling cannot raise or turn off the factory bound.
While the rating is not armed, a ceiling save is rejected.
