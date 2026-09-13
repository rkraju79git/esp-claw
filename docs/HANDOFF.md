# Project handoff — OceanLabz ESP32-S3 voice/vision bot

Context brief for anyone (human or Claude session) picking up this project.
Read this together with `docs/board_oceanlabz_s3cam.md` and `docs/voice_channel.md`.

## What this is

A fork of ESP-Claw with a **voice channel** (INMP441 mic + MAX98357A amp +
push-to-talk -> OpenAI Whisper STT / TTS) and a **hardware test bench** at
`/hwtest`, targeting the OceanLabz ESP32-S3-WROOM-1 **N16R8** CAM board
(16 MB flash, 8 MB octal PSRAM, OV3660 DVP camera). Board profile:
`application/edge_agent/boards/community/oceanlabz_s3cam` (Mode B: camera +
voice on relocated pins — see `docs/board_oceanlabz_s3cam.md` for the pinout).

Working branch: **`feature/voice-channel`** (origin = rkraju79git/esp-claw;
upstream espressif/esp-claw master last merged at `74b1870`, Sep 2026).

## Build & flash (developer machine: macOS, repo at
`/Volumes/My Data/AIProjects/espclaw/esp-claw`)

- **ESP-IDF v5.5.4 required** (at `~/esp/esp-idf-v5.5.4`). v5.3 fails: the
  component manager drops capability components whose Kconfig `if:` rules it
  can't evaluate -> missing-header errors in app_claw.
- The user's shell has two helpers (in `~/.zshrc`):
  - `getidf` — activates ESP-IDF v5.5.4 (unsets stale `IDF_PYTHON_ENV_PATH`).
  - `flashbot` — activate IDF, `cd` to `application/edge_agent`, pull
    `feature/voice-channel`, regenerate board config if
    `components/gen_bmgr_codes` is missing, then `idf.py flash monitor`.
- Board manager: `pip install esp-bmgr-assist`, then
  `idf.py bmgr -c ./boards -b oceanlabz_s3cam`. Regenerating is required for
  new `sdkconfig.defaults.board` entries to take effect (delete `sdkconfig`
  and `components/gen_bmgr_codes` first).
- One-shot clean build script: `scripts/build_oceanlabz.sh`.
- Serial monitor: `idf.py monitor`, quit with Ctrl+]. Boot log prints
  `ESP-Claw git version: <commit>` — always verify it matches the intended
  build (an old binary lingers when the flash step was skipped).

## Feature status (as of Sep 2026)

| Area | Status |
|---|---|
| Camera capture on /hwtest | WORKING — color/grayscale + resolution dropdown, fresh frame per capture, ~76 KB gray / ~900 KB color BMP. 180° rotation configured (VFLIP+HMIRROR) but not yet verified on hardware. |
| Camera robustness | Fixed: device kept open forever (close de-registers /dev/video2 -> errno=2 until reboot); settle miss no longer tears down the device. |
| Storage FS | Self-heals: write-probe after mount, reformat if mounted-but-unwritable (was a boot-loop). |
| Voice firmware | Running (`voice channel ready` at boot; mic 38/39/40, spk 41/42/2, PTT = BOOT/GPIO 0). |
| Mic hardware | BLOCKED: INMP441 header pins are NOT soldered (breadboard only) -> wiring check reads peak≈1 (data line stuck high). Needs soldering. `/hwtest` Microphone -> "Wiring check" gives a plain-language verdict (left/right slot stats). |
| Speaker hardware | Not wired yet (same soldering caveat). |
| OpenAI API key | NOT set (placeholder). Set via `idf.py menuconfig` -> Voice IM Channel, or bake with `scripts/build_oceanlabz.sh <key>`. Needed for STT/TTS only; the agent LLM runs on Ollama Cloud and is configured separately in the web UI. |

## Hardware gotchas learned the hard way

- **IP changes on every DHCP renewal** (.107 -> .104 -> .100 so far). Boot log
  line `Wi-Fi STA ready: x.x.x.x` is the truth. Fix permanently with a DHCP
  reservation for MAC `14:c1:9f:51:ff:18`. Fallback: the board's own AP
  `esp-claw-51FF19` -> http://192.168.4.1/.
- **Camera "Get sensor ID failed" at boot** = SCCB (GPIO 4/5) not reaching the
  sensor: ribbon cable unseated (power off, reseat FPC, latch down) or a stray
  wire on GPIO 4/5. When healthy the boot log shows `Detected Camera sensor
  PID=0x3660`.
- **GPIO 2 is the speaker DIN** — an I2C scan on it reads phantom devices.
  GPIO map: camera owns 4-13,15-18; mic 38/39/40; amp 41/42/2; PTT 0;
  19/20 USB, 26-37 flash/PSRAM reserved; free: 1,3,14,21,45,46,47,48(LED).
- **Mic reads all-ones (peak 1, dc -1, nonzero 100%)** = SD line floating:
  unsoldered pins, no 3.3V at the mic, or L/R not grounded.
- Flashing a stock/upstream ESP-Claw image wipes these customizations (and
  uses a wrong 8 MB partition table). Always build from this branch.

## Immediate next steps

1. Flash the post-merge build; verify boot shows the merged commit, camera
   PID detect, `/hwtest` route, `voice channel ready`; confirm capture is now
   right-side-up.
2. Solder the INMP441 (and MAX98357A) header pins; re-run the mic Wiring
   check until it reports "MIC OK", then Record & transcribe (peak should hit
   thousands while talking).
3. Set the OpenAI API key; test Record & transcribe end-to-end, then speaker
   TTS, then hold BOOT to talk to the agent.
4. Optional backlog: DHCP reservation; on-device JPEG for fast color
   captures; wire motors (pins 1/14/21/47 per board doc) for the Looi-bot.
