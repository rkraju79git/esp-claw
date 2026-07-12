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
- The Web IM UI shows only channel `"web"` conversations. Voice exchanges have
  their own live log page — see below.

## Built-in web pages

Both pages are embedded in the firmware (no frontend toolchain needed) and are
served from the device — open `http://<device-ip>/...`:

### `/voice` — live voice pipeline log

Streams every stage over the existing `/ws/webim` WebSocket:
`recording_start` → `recording_stop` (duration) → `transcribing` →
`transcript` (what Whisper heard) → `reply` (what the agent said) →
`speaking_start` / `speaking_end`, plus `error` events. History survives page
reloads via `GET /api/voice/events` (last 32 events).

### `/hwtest` — hardware test bench

Functional tests for every kit peripheral; pins are entered on the page, so
hardware can be verified before it is wired into any firmware feature:

| Test | What it actually does |
|---|---|
| System | Uptime, internal heap + PSRAM free, voice pin config |
| I2C scan | Probes 0x08–0x77 on any SDA/SCL pins (finds OLED @0x3C, sensors) |
| OLED | Initializes an SSD1306 over raw I2C and draws a checkerboard |
| Mic | Records N seconds, reports peak level + cloud Whisper transcript |
| Speaker | Sine test tone (freq/duration) or spoken TTS phrase |
| Motors | Fwd/Rev/Left/Right/Stop on any 4 driver pins, firmware auto-stop |
| Ultrasonic | HC-SR04 trigger/echo pulse timing → distance in cm |
| Raw GPIO | Set any safe pin high/low or read it (blocked: USB/flash/PSRAM pins) |

Camera capture is not in the bench yet: the OceanLabz camera's DVP pin mapping
is board-specific and must be added as a board profile before esp_video can
drive it. The I2C scan will still detect the camera's SCCB interface.

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
