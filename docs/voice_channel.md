# Voice Channel (`cap_im_voice`)

An experimental local **voice channel** for ESP-Claw: talk to the agent with a
push-to-talk button instead of (or in addition to) Telegram/Web IM.

```
hold PTT ──▶ INMP441 (I2S, 16 kHz) ──▶ PSRAM buffer
release  ──▶ WAV ──▶ OpenAI Whisper API ──▶ transcript
transcript ──▶ local IM gateway (channel "voice") ──▶ agent loop + skills
agent reply ──▶ OpenAI TTS (raw 24 kHz PCM) ──▶ MAX98357A speaker
```

Because the transcript enters through the standard local IM gateway
(`cap_im_local`), **every installed skill works by voice automatically** —
"turn on the LED", "go forward", anything you already taught the agent by chat.

## Hardware

Tested target: ESP32-S3 (≥8 MB flash / 8 MB PSRAM) with:

| Part | Default pins (menuconfig-adjustable) |
|---|---|
| INMP441 I2S microphone | SCK=4, WS=5, SD=6, L/R→GND |
| MAX98357A I2S amplifier + speaker | BCLK=15, LRC=16, DIN=7 |
| Push-to-talk button | GPIO 0 (BOOT), active low |

## Build

⚠️ **Use the plain `esp32_S3_DevKitC_1` board profile — not `_breadboard`.**
The breadboard profile configures an ST7789 SPI display, PDM speaker, and
backlight on GPIO 4, 5, 6, 7, 9, 15 and 16 — colliding with every default
audio pin below. The plain profile uses only GPIO 38 (onboard RGB LED) and
conflicts with nothing.

```bash
cd application/edge_agent
idf.py set-target esp32s3
idf.py bmgr -c ./boards -b esp32_S3_DevKitC_1
idf.py menuconfig
```

Enable and configure:

- `Voice IM Channel (cap_im_voice)` → enable, set the **OpenAI API key**
  (Whisper + TTS) and pins. Requires `APP_CLAW_CAP_IM_LOCAL` (on by default
  for the Web IM).

Then `idf.py build flash monitor`.

## Design notes / current limitations

- The component owns its own I2S channels (`I2S_NUM_AUTO`) and is independent
  of the board-manager audio devices. If your board YAML also configures I2S
  audio (`i2s_audio_in`/`i2s_audio_out`) on the same ports/pins, disable the
  Lua audio module (`CONFIG_APP_CLAW_LUA_MODULE_AUDIO=n`) or the pins will
  conflict.
- Outbound replies for channel `"voice"` are routed from the shared local IM
  outbound callback in `http_server_webim_api.c` to
  `cap_im_voice_handle_outbound()`. Web IM replies are unaffected.
- STT/TTS are cloud APIs (OpenAI); the API key currently comes from Kconfig,
  not the runtime settings store. Wake-word (esp-sr) and on-device VAD are
  natural follow-ups — PTT keeps v1 simple and false-trigger-free.
- Long agent replies are spoken in full; there is no barge-in yet.
- The Web IM UI shows only channel `"web"` conversations; voice exchanges
  (channel `"voice"`) do not appear there. Watch `idf.py monitor` to see
  transcripts and replies logged.

## GPIO budget (plain DevKitC_1 profile)

| GPIO | Used by |
|---|---|
| 0 | Push-to-talk (BOOT button) |
| 4, 5, 6 | INMP441 mic (SCK, WS, SD) |
| 15, 16, 7 | MAX98357A amp (BCLK, LRC, DIN) |
| 38 | Board profile: onboard RGB LED (RMT) — do not reuse |
| 35, 36, 37 | Reserved by octal PSRAM — never usable |
| 3, 45, 46 | Strapping pins — avoid |
| 21, 47, 41, 42, 39, 40, 1, 2, 8, 10–14, 17, 18 | Free for skills (motors, sensors, ...) |
