# Backend API key rotation

The firmware does not compile in the backend API key. `include/secrets.h.example` is an empty local template (`POWERCELL_API_KEY`, plus empty Wi-Fi placeholders). Copying it to `include/secrets.h` does not put the key into a product image: no firmware source includes that header. `include/secrets.h` is gitignored. Do not commit a filled copy, and do not print the key.

The key the device uses is the NVS string `pc_secure` / `api_key`. The cloud WebSocket and OTA send it as `Authorization: Bearer <that value>`. `settings::apiKey()` is the only read. A unit flashed from 1.0.x also copies `powercell_ota` / `api_key` into `pc_secure` once, if `pc_secure` does not already have it.

This is not the factory identity secret. That secret is the 64-hex device secret (`SECRET` / `SET device_secret`). Rotating the backend API key does not replace it.

## After the backend key changes

Replace the key on each unit before `LOCK`:

- Factory station: `tools/factory_state/factory.env` key `API_KEY`, then `tools/factory.py`. The tool sends `SET api_key` over USB and does not print the value.
- One-off USB: `tools/provision.py` with a key file whose `API_KEY` line is the new value. That file stays out of git. The tool sends `SET api_key` and prints only OK or ERR.
- Console, before `LOCK`: `SET api_key <new value>`.

A new factory image does not need a rebuild for the new key. The key enters the unit at provision time, from `factory.env` or the provision file, into NVS. Units already programmed keep the old NVS value until they are provisioned again. `LOCK` blocks a later `SET api_key`.
