# Powercell factory flash (erase, flash, next serial, provision)

One command erases an ESP32 collector, writes the full image, gives it the next free
serial (ACE-NNNNNN) and provisions it through the existing `tools/factory.py` flow.

```
py tools\factory_flash.py --model pc-dsp011-5kw --port COM5
```

Add `--register` to register on api-dev (needs `ADMIN_KEY`), and `--register --lock` for a customer unit.
Run `--dry-run` first. It only reads the board's MAC and prints the serial it would use. It writes nothing.

| --model          | Product                   | NVS model set |
|------------------|---------------------------|---------------|
| pc-dsp011-5kw    | DSP Ultra 5 kVA           | PC5K          |
| pc-dsp011-35kw   | DSP Prime Plus 3.5 kVA    | PC3_5K        |
| pc-dsp011-15kw   | DSP Prime 1.5 kVA         | PC1_5K        |
| pc-stm01-5kw     | STM Ultra 5 kVA           | PC5K          |
| pc-stm01-35kw    | STM Prime Plus 3.5 kVA    | PC3_5K        |
| pc-stm01-15kw    | STM Prime 1.5 kVA         | PC1_5K        |

## One-time setup (Windows)
1. Python 3 (`py`) and `py -m pip install pyserial`. For esptool the script uses PlatformIO's
   bundled one (`pio pkg exec -p tool-esptoolpy -- esptool.py`). If PlatformIO isn't installed,
   run `py -m pip install esptool`.
2. Copy `tools\factory_state\factory.env.example` to `tools\factory_state\factory.env` and fill in
   `API_KEY` (and `ADMIN_KEY` for `--register`). Never share or commit that file.
3. **Seeding (important):** copy `serial_counter.json` and `units.csv` from the `factory_state`
   folder of the laptop that provisioned the earlier units (ACE-000001, ACE-000003, ...)
   into `tools\factory_state\`. Without them the script refuses to start. Use
   `--start-at N` only if those files are truly lost, with N above every serial ever issued.
   Run the tool from one laptop only. The counter is local and nothing on the backend
   reserves serials, so two laptops could issue the same number.
4. Close the PlatformIO Serial Monitor. Find the COM port in Device Manager under Ports.

## What it does
1. Checks `factory.env` (values never printed), the image SHA256 against `bins\SHA256SUMS`,
   and that the image's built-in identity string matches `--model`.
2. `esptool read_mac`. If this board's MAC is already in `units.csv` or the journal, it gets the
   **same serial back** (an erase wipes the serial, so this restores it). Otherwise the next free
   serial is reserved in `factory_state\reservations.jsonl` before anything is flashed.
3. `erase_flash` (wipes serial, secret, Wi-Fi, owner, everything), then `write_flash 0x0` the merged
   image, then `verify_flash`.
4. Runs `factory.py COMx --serial ACE-NNNNNN --reprovision --model <code>` (+ `--register`/`--lock`).
   The board makes a new secret, and the hotspot becomes Powercell-NNNNNN.
5. Reads `INFO` back and checks serial, hardware_model, model, secret and API key. Prints the label.
   The label file is in `factory_state\labels\ACE-NNNNNN.txt`.

## Recovery
- **Anything failed:** fix the cause (cable, port, boot button, Monitor open) and run **the same command
  again**. The journal ties the serial to this board's MAC, so the board gets the same serial back.
- **esptool can't connect:** hold BOOT while it says "Connecting...", or add `--baud 115200`.
- **Registration failed:** factory.py rotates the secret. Rerun the same command with `--register`.
- **Board is dead / will never be used:** `py tools\factory_flash.py --abandon ACE-NNNNNN --reason "dead"`.
  That serial is marked burned and never issued again.
- **"lock file exists":** another run is active. If none is (e.g. the PC crashed), delete
  `factory_state\.factory_flash.lock`.
- `py tools\factory_flash.py --status` lists every serial with its last state and MAC.
- Never delete or hand-edit `reservations.jsonl`, `serial_counter.json` or `units.csv`. Back the folder up.

## Packaging (build machine)
`python tools/make_factory_bins.py --zips <folder with acecore_fw_*_base.zip> --out bins` writes
`bins/<model>_v<version>_merged.bin` (merge_bin, dio 40m 4MB) plus `bins/SHA256SUMS`.
