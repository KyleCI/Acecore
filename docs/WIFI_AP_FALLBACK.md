# Wi-Fi hotspot fallback

Caleb accepted the 30 second AP hotspot fallback on the DSP boards (`esp32dev_hw_v011_5kw` and `esp32dev_hw_v011_35kw`).

On those images, a saved network that drops opens the setup hotspot after 30 seconds. The hotspot stays up for 120 seconds after the station returns. A unit with no saved network opens the hotspot immediately. The saved Wi-Fi password is kept. The station is started once with `WiFi.begin` and then retried with `WiFi.reconnect` every 5 seconds.

The STM32 images (`esp32dev_stm01_5kw` and `esp32dev_stm01_35kw`) keep the 2.5.11 timing: the hotspot opens after 5 minutes.
