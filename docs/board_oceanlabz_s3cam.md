# Board reference: ESP32-S3-WROOM CAM (N16R8 + OV3660)

The OceanLabz DIY AI Voice/Vision kit board, as identified from the module
markings and PCB.

## Identity

| Item | Value |
|---|---|
| Module | ESP32-S3-WROOM-1 **N16R8** (16 MB flash, **8 MB octal PSRAM**) |
| Camera | **OV3660**, 3 MP, DVP parallel interface, on the FPC connector |
| USB | Dual USB-C: native USB (GPIO19/20) + USB-UART bridge |
| RGB LED | WS2812 on **GPIO48** |
| Buttons | EN/RST, BOOT (GPIO0) |

## Reserved / unusable GPIOs

| GPIO | Reserved for |
|---|---|
| 26–32 | SPI flash |
| 33–37 | **Octal PSRAM** (N16R8) — not just 35–37; 33 and 34 too |
| 19, 20 | Native USB D-/D+ |
| 0, 3, 45, 46 | Strapping pins — usable but avoid for anything that must be quiet at boot |

## Camera DVP pinout (OV3660)

This is the **Freenove ESP32-S3 CAM** mapping — the same one ESP-Claw's
`esp_sparkbot` board profile already encodes, verified against this hardware.
A "sequential 33–43" mapping seen in some vendor docs is **wrong** for N16R8:
it collides with the octal PSRAM pins above.

| Signal | GPIO | | Signal | GPIO |
|---|---|---|---|---|
| SIOD / SDA | 4 | | D0 | 11 |
| SIOC / SCL | 5 | | D1 | 9 |
| VSYNC | 6 | | D2 | 8 |
| HREF | 7 | | D3 | 10 |
| PCLK | 13 | | D4 | 12 |
| XCLK | 15 | | D5 | 18 |
| PWDN | -1 | | D6 | 17 |
| RESET | -1 | | D7 | 16 |

Camera occupies **14 GPIOs**: 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 17, 18.

## The core constraint

With the camera active, flash + octal PSRAM + USB + camera consume most of the
GPIO bank. Only these remain free:

```
1, 2, 14, 21, 38, 39, 40, 41, 42, 47   (+ GPIO48 = onboard RGB LED)
```

That is **not enough** to also run the full voice pipeline (6 pins) + two
motors (4) + ultrasonic (2) at the same time. The camera and a
sensor-rich robot are effectively an either/or on this board.

## Two supported configurations

### Mode A — Voice + motors + sensors (camera unplugged)

The camera pins are free, so voice keeps its simple defaults.

| Function | GPIO |
|---|---|
| Mic (INMP441) SCK / WS / SD | 4 / 5 / 6 |
| Amp (MAX98357A) BCLK / LRC / DIN | 15 / 16 / 7 |
| Push-to-talk | 0 (BOOT) |
| Motors IN1..IN4 | 21 / 47 / 41 / 42 |
| Ultrasonic TRIG / ECHO | 17 / 18 |
| Spare | 1, 2, 14, 38, 39, 40, 48(LED) |

### Mode B — Camera vision bot + voice (2 motors, no ultrasonic)

Camera uses the pinout above; voice is relocated to camera-safe pins.

| Function | GPIO |
|---|---|
| Camera (OV3660) | 4,5,6,7,8,9,10,11,12,13,15,16,17,18 |
| Mic (INMP441) SCK / WS / SD | 38 / 39 / 40 |
| Amp (MAX98357A) BCLK / LRC / DIN | 41 / 42 / 2 |
| Push-to-talk | 0 (BOOT) |
| Motors IN1..IN4 | 1 / 14 / 21 / 47 |
| Ultrasonic | none — out of pins |

Mode B ships as a ready-made board profile: **`oceanlabz_s3cam`** (under
`boards/community/`). It configures the OV3660 camera on the pinout above,
enables the voice channel on the relocated pins, and leaves audio to
`cap_im_voice` (no board-manager I2S, so no pin clash).

```bash
cd application/edge_agent
idf.py set-target esp32s3
idf.py gen-bmgr-config -c ./boards -b oceanlabz_s3cam
idf.py menuconfig    # Voice IM Channel -> set your OpenAI API key
idf.py build flash monitor
```

### Testing the camera

Open `http://<device-ip>/hwtest` → **Camera** section → **Capture photo**.
The board grabs a frame from the OV3660 and returns a browser-viewable image:
if the sensor is delivering JPEG it is served as-is; otherwise the firmware
builds a 320×240 grayscale BMP from the YUV luminance plane (no on-device JPEG
encoder needed), which is enough to confirm the camera sees the scene and the
DVP wiring is correct. Motors on this profile are 1/14/21/47.
