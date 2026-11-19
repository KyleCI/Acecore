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
3. **Serials already issued:** ACE-000001, ACE-000002 and ACE-000003 were issued before this
   tool (Caleb, 18 Nov 2026). The tool never issues them again. If `serial_counter.json` /
   `units.csv` from the laptop that made those units exist, copy them into `tools\factory_state\`
   and they are honoured (still never 1 to 3). `--start-at N` overrides the first number and is
   refused below 4.
4. Close the PlatformIO Serial Monitor. Find the COM port in Device Manager under Ports.

## First run
```
py tools\factory_flash.py --model pc-dsp011-5kw --port COM5 --register
```
With an empty `tools\factory_state` (no `serial_counter.json`) this issues **ACE-000004** first.
It records ACE-000001 to ACE-000003 in `factory_state\reservations.jsonl` as `pre_issued`
(no board MAC, never issued again), then reserves ACE-000004 for the board on the cable.
Run `--dry-run` first if you want to see the serial without writing anything.

**One laptop only, and back up `tools\factory_state`.** The serial counter lives only in that
folder. There is no backend endpoint that reserves serials yet, so a second laptop, or a lost
folder, would issue the same numbers again. Copy the whole `tools\factory_state` folder
(`serial_counter.json`, `reservations.jsonl`, `units.csv`, `labels\`) to a backup after every
session. Never put `factory.env` anywhere shared.

**Reflashing ACE-000001 to ACE-000003:** if the board's MAC is in `units.csv` the tool gives it
the same serial back. If it is not, do not use `factory_flash.py` for that board (it would get a
new serial). Flash the image, then provision with the serial given explicitly:
`py tools\factory.py COM5 --serial ACE-000002 --reprovision --model PC5K`.

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
