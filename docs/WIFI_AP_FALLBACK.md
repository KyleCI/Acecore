# Wi-Fi hotspot fallback

Caleb accepted the 30 second AP hotspot fallback for both STM32 and DSP.

All four product images use the same timing (`pc::onb::kApFallbackMs`, `kApGraceMs`, and `kWifiRetryMs`):

- `esp32dev_stm01_5kw` (`pc-stm01-5kw`)
- `esp32dev_stm01_35kw` (`pc-stm01-35kw`)
- `esp32dev_dsp011_5kw` (`pc-dsp011-5kw`)
- `esp32dev_dsp011_35kw` (`pc-dsp011-35kw`)

A saved network that drops opens the setup hotspot after 30 seconds. The hotspot stays up for 120 seconds after the station returns. A unit with no saved network opens the hotspot immediately. If the station drops again after that grace, the 30 second timer starts over. A Wi-Fi test during onboarding keeps the hotspot up. The saved Wi-Fi password is kept. The station is started once with `WiFi.begin` and then retried with `WiFi.reconnect` every 5 seconds.
