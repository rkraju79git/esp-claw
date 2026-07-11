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

```bash
cd application/edge_agent
idf.py set-target esp32s3
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
