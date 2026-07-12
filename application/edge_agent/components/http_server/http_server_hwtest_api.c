/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hardware test bench (/hwtest): functional tests for the peripherals of a
 * DIY voice/robot kit — I2C scan, raw GPIO, SSD1306 OLED pattern, DC motors
 * with auto-stop, HC-SR04 ultrasonic ranging, and (via cap_im_voice) mic
 * record + cloud transcription and speaker tone/TTS.
 *
 * Pins are request parameters, not compile-time config, so the bench can
 * exercise hardware before it is wired into any firmware feature.
 */
#include "http_server_priv.h"

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "cap_im_voice.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_APP_CLAW_LUA_MODULE_CAMERA
#include "camera_hal.h"
#include "esp_board_manager_includes.h"
#endif

static const char *TAG = "http_hwtest";

extern const uint8_t hwtest_html_start[] asm("_binary_hwtest_html_start");
extern const uint8_t hwtest_html_end[] asm("_binary_hwtest_html_end");

/* GPIOs that must never be touched by the bench on ESP32-S3:
 * 19/20 = USB D-/D+, 26..32 = flash, 35..37 = octal PSRAM. */
static bool hwtest_pin_blocked(int pin)
{
    if (pin < 0 || pin > 48) {
        return true;
    }
    if (pin == 19 || pin == 20) {
        return true;
    }
    if (pin >= 26 && pin <= 37) {
        return true;
    }
    return false;
}

static int hwtest_json_int(cJSON *root, const char *key, int fallback)
{
    cJSON *item = cJSON_GetObjectItem(root, key);

    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static esp_err_t hwtest_reply_error(httpd_req_t *req, const char *message)
{
    cJSON *root = cJSON_CreateObject();

    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message);
    return http_server_send_json_response(req, root);
}

/* ------------------------------------------------------------------ info -- */

static esp_err_t hwtest_info_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "uptime_s",
                            (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(root, "heap_free",
                            heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "psram_free",
                            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#if CONFIG_APP_CLAW_CAP_IM_VOICE
    cJSON_AddBoolToObject(root, "voice_enabled", true);
    cJSON_AddNumberToObject(root, "mic_pins_bclk_ws_sd0",
                            CONFIG_CAP_IM_VOICE_MIC_BCLK_GPIO * 10000 +
                            CONFIG_CAP_IM_VOICE_MIC_WS_GPIO * 100 +
                            CONFIG_CAP_IM_VOICE_MIC_SD_GPIO);
    cJSON_AddNumberToObject(root, "spk_pins_bclk_lrc_din0",
                            CONFIG_CAP_IM_VOICE_SPK_BCLK_GPIO * 10000 +
                            CONFIG_CAP_IM_VOICE_SPK_LRC_GPIO * 100 +
                            CONFIG_CAP_IM_VOICE_SPK_DIN_GPIO);
#else
    cJSON_AddBoolToObject(root, "voice_enabled", false);
#endif
    return http_server_send_json_response(req, root);
}

/* --------------------------------------------------------------- i2c scan -- */

static esp_err_t hwtest_i2cscan_handler(httpd_req_t *req)
{
    char value[16];
    int sda = 2, scl = 1;

    if (http_server_query_get(req, "sda", value, sizeof(value)) == ESP_OK) {
        sda = atoi(value);
    }
    if (http_server_query_get(req, "scl", value, sizeof(value)) == ESP_OK) {
        scl = atoi(value);
    }
    if (hwtest_pin_blocked(sda) || hwtest_pin_blocked(scl) || sda == scl) {
        return hwtest_reply_error(req, "invalid sda/scl pins");
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1, /* auto-select a free controller */
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        return hwtest_reply_error(req, esp_err_to_name(err));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON *found = cJSON_AddArrayToObject(root, "found");
    for (int addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, 20) == ESP_OK) {
            char hex[8];
            snprintf(hex, sizeof(hex), "0x%02X", addr);
            cJSON_AddItemToArray(found, cJSON_CreateString(hex));
        }
    }
    i2c_del_master_bus(bus);
    return http_server_send_json_response(req, root);
}

/* ------------------------------------------------------------------ gpio -- */

static esp_err_t hwtest_gpio_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    esp_err_t err = http_server_parse_json_body(req, &body);

    if (err != ESP_OK) {
        return hwtest_reply_error(req, "bad JSON");
    }

    int pin = hwtest_json_int(body, "pin", -1);
    int level = hwtest_json_int(body, "level", 0);
    char mode[8] = "in";
    http_server_json_read_string(body, "mode", mode, sizeof(mode));
    cJSON_Delete(body);

    if (hwtest_pin_blocked(pin)) {
        return hwtest_reply_error(req, "pin blocked or out of range");
    }

    cJSON *root = cJSON_CreateObject();
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
    };
    if (strcmp(mode, "out") == 0) {
        cfg.mode = GPIO_MODE_OUTPUT;
        gpio_config(&cfg);
        gpio_set_level(pin, level ? 1 : 0);
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddNumberToObject(root, "pin", pin);
        cJSON_AddNumberToObject(root, "level", level ? 1 : 0);
    } else {
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&cfg);
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddNumberToObject(root, "pin", pin);
        cJSON_AddNumberToObject(root, "level", gpio_get_level(pin));
    }
    return http_server_send_json_response(req, root);
}

/* ----------------------------------------------------------------- motor -- */

static esp_err_t hwtest_motor_handler(httpd_req_t *req)
{
    cJSON *body = NULL;

    if (http_server_parse_json_body(req, &body) != ESP_OK) {
        return hwtest_reply_error(req, "bad JSON");
    }

    int in1 = hwtest_json_int(body, "in1", 21);
    int in2 = hwtest_json_int(body, "in2", 47);
    int in3 = hwtest_json_int(body, "in3", 41);
    int in4 = hwtest_json_int(body, "in4", 42);
    int left = hwtest_json_int(body, "left", 0);   /* -1, 0, 1 */
    int right = hwtest_json_int(body, "right", 0); /* -1, 0, 1 */
    int ms = hwtest_json_int(body, "ms", 1000);
    cJSON_Delete(body);

    int pins[4] = { in1, in2, in3, in4 };
    for (int i = 0; i < 4; i++) {
        if (hwtest_pin_blocked(pins[i])) {
            return hwtest_reply_error(req, "motor pin blocked or out of range");
        }
    }
    if (ms < 0 || ms > 3000) {
        return hwtest_reply_error(req, "ms must be 0..3000");
    }

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << in1) | (1ULL << in2) | (1ULL << in3) |
                        (1ULL << in4),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);

    gpio_set_level(in1, left > 0);
    gpio_set_level(in2, left < 0);
    gpio_set_level(in3, right > 0);
    gpio_set_level(in4, right < 0);

    if (left != 0 || right != 0) {
        /* Auto-stop: never leave motors running after the request. */
        vTaskDelay(pdMS_TO_TICKS(ms));
        gpio_set_level(in1, 0);
        gpio_set_level(in2, 0);
        gpio_set_level(in3, 0);
        gpio_set_level(in4, 0);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "ran_ms", (left || right) ? ms : 0);
    return http_server_send_json_response(req, root);
}

/* ------------------------------------------------------------ ultrasonic -- */

static esp_err_t hwtest_ultrasonic_handler(httpd_req_t *req)
{
    cJSON *body = NULL;

    if (http_server_parse_json_body(req, &body) != ESP_OK) {
        return hwtest_reply_error(req, "bad JSON");
    }
    int trig = hwtest_json_int(body, "trig", 17);
    int echo = hwtest_json_int(body, "echo", 18);
    cJSON_Delete(body);

    if (hwtest_pin_blocked(trig) || hwtest_pin_blocked(echo) || trig == echo) {
        return hwtest_reply_error(req, "invalid trig/echo pins");
    }

    gpio_config_t out_cfg = { .pin_bit_mask = 1ULL << trig,
                              .mode = GPIO_MODE_OUTPUT };
    gpio_config_t in_cfg = { .pin_bit_mask = 1ULL << echo,
                             .mode = GPIO_MODE_INPUT };
    gpio_config(&out_cfg);
    gpio_config(&in_cfg);

    /* 10 µs trigger pulse. */
    gpio_set_level(trig, 0);
    esp_rom_delay_us(4);
    gpio_set_level(trig, 1);
    esp_rom_delay_us(10);
    gpio_set_level(trig, 0);

    /* Wait for echo to rise (25 ms timeout), then measure the pulse. */
    int64_t deadline = esp_timer_get_time() + 25000;
    while (gpio_get_level(echo) == 0) {
        if (esp_timer_get_time() > deadline) {
            return hwtest_reply_error(req, "no echo (sensor missing?)");
        }
    }
    int64_t rise = esp_timer_get_time();
    deadline = rise + 30000;
    while (gpio_get_level(echo) == 1) {
        if (esp_timer_get_time() > deadline) {
            return hwtest_reply_error(req, "echo stuck high");
        }
    }
    int64_t width_us = esp_timer_get_time() - rise;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "echo_us", (double)width_us);
    cJSON_AddNumberToObject(root, "distance_cm", (double)width_us / 58.0);
    return http_server_send_json_response(req, root);
}

/* ------------------------------------------------------------------ oled -- */

static esp_err_t hwtest_oled_pattern(i2c_master_dev_handle_t dev)
{
    /* Minimal SSD1306 128x64 init. */
    static const uint8_t init_seq[] = {
        0x00, /* command stream */
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14,
        0x20, 0x00, /* horizontal addressing */
        0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40,
        0xA4, 0xA6, 0xAF,
    };
    esp_err_t err = i2c_master_transmit(dev, init_seq, sizeof(init_seq), 200);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t window[] = { 0x00, 0x21, 0x00, 0x7F, 0x22, 0x00, 0x07 };
    err = i2c_master_transmit(dev, window, sizeof(window), 200);
    if (err != ESP_OK) {
        return err;
    }

    /* Checkerboard: proves data path + addressing visually. */
    uint8_t chunk[65];
    chunk[0] = 0x40; /* data stream */
    for (int block = 0; block < 16; block++) {
        for (int i = 0; i < 64; i++) {
            chunk[1 + i] = ((block + (i / 8)) % 2) ? 0x0F : 0xF0;
        }
        err = i2c_master_transmit(dev, chunk, sizeof(chunk), 200);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t hwtest_oled_handler(httpd_req_t *req)
{
    cJSON *body = NULL;

    if (http_server_parse_json_body(req, &body) != ESP_OK) {
        return hwtest_reply_error(req, "bad JSON");
    }
    int sda = hwtest_json_int(body, "sda", 2);
    int scl = hwtest_json_int(body, "scl", 1);
    int addr = hwtest_json_int(body, "addr", 0x3C);
    cJSON_Delete(body);

    if (hwtest_pin_blocked(sda) || hwtest_pin_blocked(scl) || sda == scl) {
        return hwtest_reply_error(req, "invalid sda/scl pins");
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        return hwtest_reply_error(req, esp_err_to_name(err));
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t dev = NULL;
    err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err == ESP_OK) {
        err = hwtest_oled_pattern(dev);
        i2c_master_bus_rm_device(dev);
    }
    i2c_del_master_bus(bus);

    if (err != ESP_OK) {
        return hwtest_reply_error(req, esp_err_to_name(err));
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "result", "checkerboard drawn");
    return http_server_send_json_response(req, root);
}

/* ------------------------------------------------------------- mic / spk -- */

static esp_err_t hwtest_mic_handler(httpd_req_t *req)
{
    cJSON *body = NULL;

    if (http_server_parse_json_body(req, &body) != ESP_OK) {
        return hwtest_reply_error(req, "bad JSON");
    }
    int seconds = hwtest_json_int(body, "seconds", 3);
    cJSON_Delete(body);

    char transcript[512];
    int peak = 0;
    esp_err_t err = cap_im_voice_test_mic(seconds, transcript,
                                          sizeof(transcript), &peak);
    if (err != ESP_OK) {
        return hwtest_reply_error(req, esp_err_to_name(err));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "peak", peak);
    cJSON_AddNumberToObject(root, "peak_pct", peak * 100 / 32767);
    cJSON_AddStringToObject(root, "transcript", transcript);
    return http_server_send_json_response(req, root);
}

static esp_err_t hwtest_speaker_handler(httpd_req_t *req)
{
    cJSON *body = NULL;

    if (http_server_parse_json_body(req, &body) != ESP_OK) {
        return hwtest_reply_error(req, "bad JSON");
    }
    char type[8] = "tone";
    char text[256] = "Speaker test successful.";
    http_server_json_read_string(body, "type", type, sizeof(type));
    http_server_json_read_string(body, "text", text, sizeof(text));
    int freq = hwtest_json_int(body, "freq", 440);
    int ms = hwtest_json_int(body, "ms", 1000);
    cJSON_Delete(body);

    esp_err_t err;
    if (strcmp(type, "tts") == 0) {
        err = cap_im_voice_say(text);
    } else {
        err = cap_im_voice_test_tone(freq, ms);
    }
    if (err != ESP_OK) {
        return hwtest_reply_error(req, esp_err_to_name(err));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return http_server_send_json_response(req, root);
}

/* ---------------------------------------------------------------- camera -- */

#if CONFIG_APP_CLAW_LUA_MODULE_CAMERA

#define HWTEST_FOURCC_JPEG   0x4745504A /* v4l2_fourcc('J','P','E','G') */

/* Per-frame dequeue timeout when capturing. */
#define HWTEST_CAM_CAPTURE_TIMEOUT_MS 1500

/* The DVP driver keeps a small ring of buffers that it fills continuously. If
 * nobody dequeues between captures the ring fills and the frames in it go stale,
 * so a re-capture would hand back the SAME old image. Flush this many frames
 * (drain the ring) before keeping one, so every capture is a fresh frame.
 * The ring is 3 buffers; 3 flushes guarantees the kept frame post-dates the
 * request. At 10 fps this is ~0.4 s. */
#define HWTEST_CAM_FLUSH_FRAMES 3

static const char *hwtest_camera_dev_path(void)
{
    dev_camera_handle_t *handle = NULL;
    esp_err_t err = esp_board_manager_get_device_handle(
        ESP_BOARD_DEVICE_NAME_CAMERA, (void **)&handle);

    if (err != ESP_OK || !handle || !handle->dev_path) {
        return NULL;
    }
    return handle->dev_path;
}

static inline uint8_t hwtest_clamp_u8(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v);
}

/* Build a BMP from a YUYV frame, downsampling from sw x sh to dw x dh.
 *
 *  - color=false: 8-bit grayscale (luma plane only) — ~1/3 the bytes, fastest.
 *  - color=true : 24-bit RGB, decoding the YUYV chroma (BT.601, integer math).
 *
 * Returns a SPIRAM buffer the caller must free, or NULL on alloc failure. */
static uint8_t *hwtest_yuyv_to_bmp(const uint8_t *yuyv, uint32_t sw, uint32_t sh,
                                   int dw, int dh, bool color, size_t *out_size)
{
    const int bpp = color ? 24 : 8;
    const int palette_bytes = color ? 0 : 256 * 4;   /* 8-bit needs a palette */
    const int header_bytes = 54 + palette_bytes;
    const int row_stride = color ? ((dw * 3 + 3) & ~3) : ((dw + 3) & ~3);
    const size_t pixels = (size_t)row_stride * dh;
    const size_t total = header_bytes + pixels;

    uint8_t *bmp = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (!bmp) {
        return NULL;
    }
    memset(bmp, 0, header_bytes);
    bmp[0] = 'B';
    bmp[1] = 'M';
    uint32_t v = total;         memcpy(bmp + 2, &v, 4);
    v = header_bytes;           memcpy(bmp + 10, &v, 4); /* pixel data offset */
    v = 40;                     memcpy(bmp + 14, &v, 4); /* DIB header size */
    v = dw;                     memcpy(bmp + 18, &v, 4);
    v = dh;                     memcpy(bmp + 22, &v, 4);
    uint16_t planes = 1, bpp16 = (uint16_t)bpp;
    memcpy(bmp + 26, &planes, 2);
    memcpy(bmp + 28, &bpp16, 2);
    v = pixels;                 memcpy(bmp + 34, &v, 4); /* image size */
    if (!color) {
        v = 256;                memcpy(bmp + 46, &v, 4); /* colors used */
        uint8_t *pal = bmp + 54;                         /* grayscale palette */
        for (int i = 0; i < 256; i++) {
            *pal++ = (uint8_t)i; *pal++ = (uint8_t)i; *pal++ = (uint8_t)i; *pal++ = 0;
        }
    }

    /* BMP rows are bottom-up. YUYV packs two pixels as [Y0 U Y1 V]. */
    for (int y = 0; y < dh; y++) {
        uint32_t sy = (uint32_t)y * sh / dh;
        const uint8_t *srow = yuyv + (size_t)sy * sw * 2;
        uint8_t *drow = bmp + header_bytes + (size_t)(dh - 1 - y) * row_stride;
        for (int x = 0; x < dw; x++) {
            uint32_t sx = (uint32_t)x * sw / dw;
            uint8_t luma = srow[sx * 2];
            if (!color) {
                *drow++ = luma;
                continue;
            }
            /* Shared chroma from the even pixel of the YUYV pair. */
            const uint8_t *pair = srow + (sx & ~1u) * 2;
            int u = (int)pair[1] - 128;
            int vv = (int)pair[3] - 128;
            int r = luma + ((359 * vv) >> 8);
            int g = luma - ((88 * u + 183 * vv) >> 8);
            int b = luma + ((454 * u) >> 8);
            *drow++ = hwtest_clamp_u8(b);   /* BMP is BGR */
            *drow++ = hwtest_clamp_u8(g);
            *drow++ = hwtest_clamp_u8(r);
        }
    }
    *out_size = total;
    return bmp;
}

/* Send a large body in small pieces. httpd_resp_send() pushes the whole buffer
 * in one write, which returns EAGAIN and aborts when the socket buffer fills;
 * chunked writes let TCP drain between pieces. */
static esp_err_t hwtest_send_in_chunks(httpd_req_t *req, const uint8_t *data,
                                       size_t len)
{
    /* Make send() block for buffer space instead of failing immediately with
     * EAGAIN (errno 11). Without this the ~300 KB image aborts mid-transfer on
     * a slow client and the browser hangs on a half-finished response. */
    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd >= 0) {
        struct timeval snd_to = { .tv_sec = 10, .tv_usec = 0 };
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));
    }

    const size_t chunk = 4096;
    for (size_t off = 0; off < len; off += chunk) {
        size_t n = (len - off < chunk) ? (len - off) : chunk;
        esp_err_t err = httpd_resp_send_chunk(req, (const char *)data + off, n);
        if (err != ESP_OK) {
            return err;
        }
    }
    return httpd_resp_send_chunk(req, NULL, 0); /* terminate the response */
}

static esp_err_t hwtest_camera_handler(httpd_req_t *req)
{
    const char *dev_path = hwtest_camera_dev_path();

    if (!dev_path) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return hwtest_reply_error(req, "camera device not found");
    }

    /* Open once and keep it open for the life of the process. This DVP driver
     * re-initializes the controller on every open and de-registers the video
     * device on close, so open/close per request fails the second time
     * ("Failed to open /dev/video2, errno=2"). We never close it. */
    if (!camera_is_open()) {
        esp_err_t open_err = camera_open(dev_path, NULL);
        if (open_err != ESP_OK) {
            return hwtest_reply_error(req, esp_err_to_name(open_err));
        }
    }

    /* Never cache: each capture must show the live frame, not a stored one. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    /* Output options from the UI dropdown: ?w=&h= pick the preview size,
     * ?color=0 forces grayscale. Defaults (no params) are the native frame
     * size in colour — the maximum quality the stream delivers. Requested
     * sizes are clamped to the native frame below (we downsample, never up). */
    int req_w = 0, req_h = 0;
    bool color = true;
    char qbuf[16];
    if (http_server_query_get(req, "w", qbuf, sizeof(qbuf)) == ESP_OK) {
        req_w = atoi(qbuf);
    }
    if (http_server_query_get(req, "h", qbuf, sizeof(qbuf)) == ESP_OK) {
        req_h = atoi(qbuf);
    }
    if (http_server_query_get(req, "color", qbuf, sizeof(qbuf)) == ESP_OK) {
        color = (atoi(qbuf) != 0);
    }

    /* Flush the driver's buffer ring, then keep the next frame. Without the
     * flush a re-capture returns a stale frame that was sitting in the ring
     * (the image "doesn't change"); this drains those so the kept frame is
     * captured after this request arrived. */
    uint8_t *frame = NULL;
    size_t frame_bytes = 0;
    camera_frame_info_t info = {0};
    for (int i = 0; i <= HWTEST_CAM_FLUSH_FRAMES; i++) {
        if (frame) {
            camera_release_frame(frame);
            frame = NULL;
        }
        esp_err_t err = camera_capture_frame(HWTEST_CAM_CAPTURE_TIMEOUT_MS,
                                             &frame, &frame_bytes, &info);
        if (err != ESP_OK) {
            /* Leave the device open — closing de-registers /dev/videoN and the
             * next request fails with errno=2. A transient capture error is
             * recoverable on the next poll. */
            return hwtest_reply_error(req, esp_err_to_name(err));
        }
    }

    esp_err_t send_err;
    if (info.pixel_format == HWTEST_FOURCC_JPEG ||
        strcmp(info.pixel_format_str, "JPEG") == 0) {
        httpd_resp_set_type(req, "image/jpeg");
        send_err = hwtest_send_in_chunks(req, frame, frame_bytes);
    } else if (frame_bytes >= (size_t)info.width * info.height * 2 &&
               info.width && info.height) {
        /* Clamp the requested size to the native frame (downsample only). */
        int dw = (req_w > 0 && req_w < (int)info.width) ? req_w : (int)info.width;
        int dh = (req_h > 0 && req_h < (int)info.height) ? req_h : (int)info.height;
        size_t bmp_size = 0;
        uint8_t *bmp = hwtest_yuyv_to_bmp(frame, info.width, info.height,
                                          dw, dh, color, &bmp_size);
        if (bmp) {
            httpd_resp_set_type(req, "image/bmp");
            /* Large payload: send in chunks so a full socket buffer doesn't
             * abort the whole response with EAGAIN. */
            send_err = hwtest_send_in_chunks(req, bmp, bmp_size);
            free(bmp);
        } else {
            send_err = hwtest_reply_error(req, "preview alloc failed");
        }
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "captured %ux%u %s, %u bytes (no browser preview for format)",
                 (unsigned)info.width, (unsigned)info.height,
                 info.pixel_format_str, (unsigned)frame_bytes);
        send_err = hwtest_reply_error(req, msg);
    }

    camera_release_frame(frame);
    /* Intentionally do NOT camera_close() — keep the DVP device open so the
     * next capture reuses it. See the open-once note above. */
    return send_err;
}

#endif /* CONFIG_APP_CLAW_LUA_MODULE_CAMERA */

/* ------------------------------------------------------------------ page -- */

static esp_err_t hwtest_page_handler(httpd_req_t *req)
{
    return http_server_send_embedded_file(req, hwtest_html_start,
                                          hwtest_html_end, "text/html");
}

esp_err_t http_server_register_hwtest_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/hwtest", .method = HTTP_GET, .handler = hwtest_page_handler },
        { .uri = "/api/hwtest/info", .method = HTTP_GET, .handler = hwtest_info_handler },
        { .uri = "/api/hwtest/i2cscan", .method = HTTP_GET, .handler = hwtest_i2cscan_handler },
        { .uri = "/api/hwtest/gpio", .method = HTTP_POST, .handler = hwtest_gpio_handler },
        { .uri = "/api/hwtest/motor", .method = HTTP_POST, .handler = hwtest_motor_handler },
        { .uri = "/api/hwtest/ultrasonic", .method = HTTP_POST, .handler = hwtest_ultrasonic_handler },
        { .uri = "/api/hwtest/oled", .method = HTTP_POST, .handler = hwtest_oled_handler },
        { .uri = "/api/hwtest/mic", .method = HTTP_POST, .handler = hwtest_mic_handler },
        { .uri = "/api/hwtest/speaker", .method = HTTP_POST, .handler = hwtest_speaker_handler },
#if CONFIG_APP_CLAW_LUA_MODULE_CAMERA
        { .uri = "/api/hwtest/camera", .method = HTTP_GET, .handler = hwtest_camera_handler },
#endif
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    ESP_LOGI(TAG, "hardware test bench at /hwtest");
    return ESP_OK;
}
