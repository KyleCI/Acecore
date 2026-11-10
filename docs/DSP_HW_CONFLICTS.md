# DSP hardware conflicts (Dubem pin map, 2026-11-10)

These apply to `esp32dev_dsp011_5kw` and `esp32dev_dsp011_35kw` only. STM images keep the previous pin map and the JK-PB BMS.

1. GPIO15 power polarity. Resolved: HIGH = ON. Caleb confirmed on 2026-11-10 at 11:53 WAT that the v0.1.1 transistor needs HIGH on its base, matching Dubem's `pinDefines.h` comment. That supersedes the 05:20 LOW = ON choice. `DSP_PWR_ACTIVE_LEVEL` is 1. Off is GPIO15 LOW. The boot latch drives LOW and holds it before `setup()`, and again as the first line of `setup()`, and keeps that level until an allowed ON. GPIO15 is a strapping pin (MTDO) with a default pull-up, so a cold power-on still presents HIGH until the application runs. That window is a board concern, not something the ROM lets firmware close. Switching fix (2026-11-10): GPIO15 resets into its IO_MUX JTAG function (MTDO). The firmware through cbf48bd/a1e3bad only used `gpio_set_level`/`gpio_set_direction`, which never select the GPIO function, so the pad ignored every write and ON never reached the transistor. Every DSP write now runs `v011_dsp_pad_steps()`: hold release, level, `esp_rom_gpio_pad_select_gpio`, pulls off, output enable, level, and hold only at the off level. Dubem's firmware uses `pinMode(PWR_PIN, OUTPUT)` + `digitalWrite` (which select the GPIO function) and switches the same board.

2. BMS UART direction, driver and baud. Corrected 2026-11-10 from Dubem's working DSP firmware ("dspic collector"): both DSP ratings listen for JK-PB frames at 115200 on ESP RX GPIO17 / TX GPIO16 (`pinDefines.h` JK_RX 17, JK_TX 16, JK_BAUD 115200; `main.cpp` `JK_UART.begin(JK_BAUD, SERIAL_8N1, JK_RX, JK_TX)`). This is the same pair and protocol as the STM images. It replaces the earlier RX GPIO16 / TX GPIO17 JBD 9600 note, which read no BMS data on Caleb's board. `DSP_BMS_JKPB=1` on both DSP envs. The JBD poller stays in the tree (`DSP_BMS_JKPB=0`, RX16/TX17) but no product image uses it. `DSP_BMS_UART_SWAP=1` swaps the selected pair.

3. SD chip select GPIO5 is the same pin as `PIN_SPI_CS_MPPT`. That macro was an unused MPPT chip select held high. The DSP SD card now owns GPIO5. It is driven high (idle, card deselected) before the SPI bus starts. GPIO5 is a strapping pin and must not be low at reset. STM images still use SD CS GPIO14 and still hold GPIO5 high as the unused MPPT select.

4. GPIO33 was the DSP SD chip select and is also `PIN_STM32_RX` on the original map. DSP images do not start the STM32 UART. SD moved to GPIO5. GPIO33 is unused on DSP images since 10 Nov 2026 (PZEM moved to TX 32 / RX 35).

5. GPIO32 was `PIN_LED_AMBER`. It is now PZEM TX (ESP TX -> PZEM RX). PZEM RX is GPIO35, input-only with no internal pull-up, so the board needs a pull-up on the PZEM TX line. The DSP image has no amber LED. The warning indication uses the blue LED on GPIO13.

6. GPIO14 was the original SD chip select. On the DSP board it is the green LED. Dubem notes that GPIO14 emits a PWM burst during ROM boot, so the green LED can flicker before `setup()`. The DSP image does not use GPIO14 as a chip select.

7. GPIO27 was the green LED. It is now the front light (NPN, HIGH = ON). Since 2026-11-10 21:10 WAT (Caleb approved) it follows the inverter exactly as Dubem's firmware does (`provisioning.cpp` `applyDeviceState`): HIGH with GPIO15 ON, LOW with every OFF (boot latch, battery cutoff, overload, payment lock, OTA prepare, factory reset/wipe). `gateway` owns the pin (`frontLight()` with `pinMode`/`digitalWrite`); `leds` no longer touches it.

8. GPIO12 is the red LED and a strapping pin (MTDI). The firmware writes it low before `pinMode` and does not enable a pull-up.

9. DS3231 is not in Dubem's pin file. I2C stays SDA GPIO21 / SCL GPIO22. None of the new DSP pins use 21 or 22.

10. Only grid voltage is read from the ADC, on GPIO39 (ADC1, 11 dB, `analogReadMilliVolts`). Output volts and current come from the PZEM. GPIO34 (Dubem's inverter VOUT) and GPIO35 (Dubem's grid pin) are not read. GPIO39 is the same input circuit as hardware v0.2. The scale is the named constant `kScale` 357.54 (`-D DSP_GRID_K=` overrides it; an existing `pc_cfg/grid_k` float is used when present). There is no remote write for that key. `grid_v_calibrated` is false. A sample above about 2450 mV or below about 150 mV sets `grid_v_clipped`.

    Hardware caveat, no firmware fix: at 230 V the pin peaks near 2.56 V, above the ESP32 ADC's accurate range, so the reported grid voltage caps around 200 V. The LM324 output can reach about 3.5 V against a 3.6 V absolute maximum. The board change is about 2.4 Mohm per upper leg plus a clamp. Dubem still has to confirm that change.

11. Dubem's BMS task also integrates pack voltage and current into an energy counter. That call is not ported. DSP AC energy remains the PZEM path. STM AC energy remains the `V * I * 0.8` estimate.

12. `esp32dev_dsp011_35kw` was already defined next to the 5 kW DSP image. It was not added in this change.

13. Front light GPIO27. Resolved: follows the inverter (item 7).

14. RTC. Dubem's working firmware drives a DS1307 at 0x68 (`clock_rtc.cpp`). This firmware's driver is written for a DS3231 at the same address. The time registers match, but the DS3231 status/OSF register does not exist on a DS1307, so the RTC can be reported as untrusted. Not a cause of missing data or on/off.

15. Cloud host. Dubem's firmware connects to `api.acecore.tech`. This firmware connects to `api-dev.acecore.tech`. A board provisioned for production needs its api_key and registration on api-dev, or nothing reaches the app.
