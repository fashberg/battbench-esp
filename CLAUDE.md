# BattBench-ESP – notes for Claude

ESP-IDF firmware (PlatformIO) for an ESP32-S3-N16R8 that bridges a SkyRC MC5000 / MC3000 to BattBench over Wi-Fi.
User documentation: `README.md`. The BattBench side is `battbench/device_net.py` (fetching, `NetReader.command`) and
`battbench/device_skyrc.py` (decoding) in the BattBench repository.

## Language and publishing
- Public repository: README, this file, comments and commit messages in **English**. The web page is English and
  German (`src/web/index.html`, German texts in its translation table).
- No local paths, IP addresses, MAC / Bluetooth addresses or device names of a real setup in the repository
  (screenshots included). Protocol details are described as observed behaviour or credited to the projects named
  in `src/charger.h`.
- Branches: work on `devel` (pushed to `origin` only); `main` holds one squash commit per release and is identical
  on `origin` and `github`.
- Release: bump `FW_VERSION`, squash `devel` onto `main` (same files), tag `v<FW_VERSION>` and push `main` + tag to
  `origin` and `github`. The tag on github starts `.github/workflows/release.yml` (Ubuntu, `pioarduino==6.1.19`):
  it checks tag = `FW_VERSION`, builds and publishes `battbench-esp-<version>-factory.bin` (flash at 0x0) and
  `-update.bin` (OTA) as a GitHub release, notes = the squash commit's message.

## Build
- **pio only from PowerShell / cmd**, never from Git Bash: ESP-IDF stops under MSYS ("MSys/Mingw is not supported").
  `& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run` (build), `run -t upload` (USB), `device monitor`.
- The pio core must match the platform: `espressif32` 55.03.39 needs `pioarduino==6.1.19` (SCons 4.8.1); 6.2.0 swaps
  SCons in the middle of the build (`No module named 'SCons.Tool.FortranCommon'`). Fix in `~\.platformio`:
  `penv\Scripts\uv.exe pip install --python=penv\Scripts\python.exe "pioarduino==6.1.19"`. Never
  `pip install platformio` into the penv (it replaces pioarduino).
- OTA: `curl -X POST --data-binary @.pio/build/esp32s3-n16r8/firmware.bin http://<esp>/api/ota` (restarts; the flash
  log survives). Bump `FW_VERSION` in `src/config.h` for every firmware BattBench should tell apart.

## Layout
| File | Content |
|---|---|
| `src/main.c` | start: settings, storage, Wi-Fi, web server, BLE |
| `src/config.c` | settings (NVS `bb`), boot counter, table of chargers (3-bit index `src`) |
| `src/charger.c` | MC5000 / MC3000 protocol (query, check, decode), same as BattBench's `device_skyrc.py` |
| `src/ble.c` | NimBLE central: scan, connect, FFE0 / FFE1, notifications, poll loop, `ble_command` (one command between two queries, run by the poll loop), Bluetooth pause / off |
| `src/store.c` | RAM ring, thinning, flash log (format in the comment at the top), time anchors, query by number |
| `src/net.c` | Wi-Fi station + access point as fallback, SNTP, mDNS (`_http._tcp`, `_battbench._tcp`, TXT `id`, `fw`) |
| `src/web.c`, `src/web/index.html` | JSON API and web page |
| `src/led.c` | status LED (colours in the comment at the top) |
| `partitions.csv` | nvs, 2 × 3 MB OTA, `bblog` 9.9 MB |

## Protocol (observed on an MC5000, fw 2.02)
- Without the handshake `57` the MC5000 repeats its greeting `0F 04 06 01 01 08` on every status query. The handshake
  is sent as the SkyRC app does: `57 00`, the own Bluetooth address (least significant byte first), 10 × `00`. It does
  not interrupt running tasks. Never the init sequence `74` / `65` / `FE` (restarts or stops tasks).
- Status answer `0F 16 91 …`, 24 bytes (one more than in older captures); checksum = sum from `91` up to the
  checksum. Handles: FFE1 = 17, CCCD = 18; the MTU is large enough for 208-byte packets.
- `/api/command` lets only `94` / `FE` / `98` / `5F` / `96` / `95` through, for one slot (mask 1, 2, 4 or 8) and with
  a correct checksum: `FE 00` would stop all slots, `94 00` program all of them, `97` resets the charger. `98` (voltage
  curve of the slot's current phase) comes as up to 3 packets `0F len 98 mask pages page interval mV…`;
  `run_command` collects them into one reply.

## BattBench side
- Sequence numbers only grow (blocks of 10,000 reserved in NVS); BattBench stores the last one saved per `logid` in
  its database (`settings`, key `esp:<logid>`). `POST /api/clear` starts a new `logid`.
- The packets in `/api/records` are the charger's own answers; BattBench decodes them with
  `device_skyrc.SkyCharger.sample`. BattBench asks with `fmt=bin` (binary, delta coded like the flash log, format in
  `src/store.h`, decoded by `device_net.decode_records`); without it the answer is JSON (older BattBench). The charger gets the same device key as over direct Bluetooth
  (`MC5000-ble-<address>`).
- BattBench reads `/api/status` (`ble.state`, `on`, `paused_secs`) every 10 s to show and mirror a Bluetooth pause.

## Tests
- Host test of `store.c` and `charger.c`: `test/host` (gcc in WSL / Linux, stubs for flash / NVS / timer with NOR
  semantics). `python make_csv.py [battbench.db]` on Windows writes `n8.csv` from a BattBench database (not in the
  repository), then `./run.sh` in WSL; afterwards `python test_bin.py <BattBench checkout>` decodes the binary fetch
  format written by the test (`records.bin`) with BattBench's `device_net.decode_records`. Covers 83,000 readings
  over 5.2 days, restarts (power cut / `store_flush`), overflow of a small flash and of the RAM ring, thinning
  0 / 10 / 60 s, the estimate of unfetched readings.
- On the device: test commands are read-only unless the user asks otherwise (`91`, `98`, `5F`, `96`); never `FE 00`,
  `97`, `60` / `88` (bootloader).
