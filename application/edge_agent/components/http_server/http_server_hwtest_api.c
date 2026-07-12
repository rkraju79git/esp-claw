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

#include "cap_im_voice.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
