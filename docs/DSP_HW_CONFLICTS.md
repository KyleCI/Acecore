# DSP hardware conflicts (Dubem pin map, 2026-11-10)

These apply to `esp32dev_dsp011_5kw` and `esp32dev_dsp011_35kw` only. STM images keep the previous pin map and the JK-PB BMS.

1. GPIO15 power polarity. `pinDefines.h` says the NPN driver is HIGH = ON. Caleb overruled that on 2026-11-10. `DSP_PWR_ACTIVE_LEVEL` stays 0, so LOW = ON is the confirmed value. The off level written at the start of `setup()` is HIGH. That write is after reset, so it is not the GPIO15 strap sample.

2. BMS UART direction and baud. Caleb confirmed on 2026-11-10: BMS TXD to ESP RX GPIO16 and BMS RXD from ESP TX GPIO17, at 9600. `pinDefines.h` lists `JK_RX 17`, `JK_TX 16`, and `JK_BAUD 115200` (the old JK stream). `DSP_BMS_UART_SWAP` stays 0. Set it to 1 only to try that other pair; the baud stays 9600. STM images stay on JK at 115200, RX 17, TX 16.

3. SD chip select GPIO5 is the same pin as `PIN_SPI_CS_MPPT`. That macro was an unused MPPT chip select held high. The DSP SD card now owns GPIO5. It is driven high (idle, card deselected) before the SPI bus starts. GPIO5 is a strapping pin and must not be low at reset. STM images still use SD CS GPIO14 and still hold GPIO5 high as the unused MPPT select.

4. GPIO33 was the DSP SD chip select and is also `PIN_STM32_RX` on the original map. DSP images do not start the STM32 UART. GPIO33 is now PZEM TX. SD moved to GPIO5.

5. GPIO32 was `PIN_LED_AMBER`. It is now PZEM RX. The DSP image has no amber LED. The warning indication uses the blue LED on GPIO13.

6. GPIO14 was the original SD chip select. On the DSP board it is the green LED. Dubem notes that GPIO14 emits a PWM burst during ROM boot, so the green LED can flicker before `setup()`. The DSP image does not use GPIO14 as a chip select.

7. GPIO27 was the green LED. It is now the front light (NPN, HIGH = ON). The firmware drives it low at boot and does not turn it on by itself.

8. GPIO12 is the red LED and a strapping pin (MTDI). The firmware writes it low before `pinMode` and does not enable a pull-up.

9. DS3231 is not in Dubem's pin file. I2C stays SDA GPIO21 / SCL GPIO22. None of the new DSP pins use 21 or 22.

10. Only grid voltage is read from the ADC, on GPIO39 (ADC1, 11 dB, `analogReadMilliVolts`). Output volts and current come from the PZEM. GPIO34 (Dubem's inverter VOUT) and GPIO35 (Dubem's grid pin) are not read. GPIO39 is the same input circuit as hardware v0.2. The scale is the named constant `kScale` 357.54 (`-D DSP_GRID_K=` overrides it; an existing `pc_cfg/grid_k` float is used when present). There is no remote write for that key. `grid_v_calibrated` is false. A sample above about 2450 mV or below about 150 mV sets `grid_v_clipped`.

    Hardware caveat, no firmware fix: at 230 V the pin peaks near 2.56 V, above the ESP32 ADC's accurate range, so the reported grid voltage caps around 200 V. The LM324 output can reach about 3.5 V against a 3.6 V absolute maximum. The board change is about 2.4 Mohm per upper leg plus a clamp. Dubem still has to confirm that change.

11. Dubem's BMS task also integrates pack voltage and current into an energy counter. That call is not ported. DSP AC energy remains the PZEM path. STM AC energy remains the `V * I * 0.8` estimate.

12. `esp32dev_dsp011_35kw` was already defined next to the 5 kW DSP image. It was not added in this change.
