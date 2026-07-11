/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Voice channel for the local IM gateway.
 *
 *   PTT held   -> record INMP441 (I2S std, 16 kHz) into PSRAM
 *   PTT release-> WAV -> OpenAI Whisper -> cap_im_local_emit_text("voice",...)
 *   agent reply-> OpenAI TTS (raw 24 kHz PCM stream) -> MAX98357A (I2S std)
 *
 * The component owns its own I2S channels (allocated with I2S_NUM_AUTO) and is
 * independent of the board-manager audio devices; disable the Lua audio module
 * if it is configured for the same pins/ports.
 */
#include "cap_im_voice.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_APP_CLAW_CAP_IM_VOICE

static const char *TAG = "cap_im_voice";

#define VOICE_MIC_SAMPLE_RATE   16000
#define VOICE_TTS_SAMPLE_RATE   24000 /* OpenAI tts-1 "pcm" output */
#define VOICE_MIC_CHUNK_SAMPLES 512
#define VOICE_WAV_HEADER_SIZE   44
#define VOICE_HTTP_TIMEOUT_MS   30000
#define VOICE_STT_RESPONSE_MAX  8192
#define VOICE_TTS_QUEUE_DEPTH   4
#define VOICE_CHAT_ID           "voice"
#define VOICE_SENDER_ID         "voice_user"

#define OPENAI_STT_URL "https://api.openai.com/v1/audio/transcriptions"
#define OPENAI_TTS_URL "https://api.openai.com/v1/audio/speech"
#define MULTIPART_BOUNDARY "clawVoiceBoundary7MA4YWxkTrZu0gW"

static i2s_chan_handle_t s_rx_chan;
static i2s_chan_handle_t s_tx_chan;
static QueueHandle_t s_tts_queue; /* char* items, heap-owned */
static int16_t *s_record_buf;     /* PSRAM, max utterance */
static size_t s_record_capacity;  /* in samples */
static volatile bool s_recording;

/* ------------------------------------------------------------------ I2S -- */

static esp_err_t voice_i2s_init(void)
{
    /* Microphone: INMP441, 24-bit data in 32-bit Philips slots, left only. */
    i2s_chan_config_t rx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&rx_cfg, NULL, &s_rx_chan), TAG, "rx chan");

    i2s_std_config_t rx_std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_CAP_IM_VOICE_MIC_BCLK_GPIO,
            .ws   = CONFIG_CAP_IM_VOICE_MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din  = CONFIG_CAP_IM_VOICE_MIC_SD_GPIO,
        },
    };
    rx_std.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_chan, &rx_std), TAG, "rx init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_chan), TAG, "rx enable");

    /* Speaker: MAX98357A, 16-bit mono at the TTS output rate. */
    i2s_chan_config_t tx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&tx_cfg, &s_tx_chan, NULL), TAG, "tx chan");

    i2s_std_config_t tx_std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_TTS_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_CAP_IM_VOICE_SPK_BCLK_GPIO,
            .ws   = CONFIG_CAP_IM_VOICE_SPK_LRC_GPIO,
            .dout = CONFIG_CAP_IM_VOICE_SPK_DIN_GPIO,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_chan, &tx_std), TAG, "tx init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_chan), TAG, "tx enable");
    return ESP_OK;
}

static int voice_mic_read(int16_t *out, size_t max_samples)
{
    static int32_t raw[VOICE_MIC_CHUNK_SAMPLES];
    size_t want = max_samples > VOICE_MIC_CHUNK_SAMPLES ? VOICE_MIC_CHUNK_SAMPLES
                                                        : max_samples;
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(s_rx_chan, raw, want * sizeof(int32_t),
                                     &bytes_read, pdMS_TO_TICKS(200));
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        return -1;
    }

    size_t n = bytes_read / sizeof(int32_t);
    for (size_t i = 0; i < n; i++) {
        /* 24-bit sample in the top of the slot; >>14 adds ~12 dB gain. */
        int32_t s = raw[i] >> 14;
        if (s > INT16_MAX) s = INT16_MAX;
        if (s < INT16_MIN) s = INT16_MIN;
        out[i] = (int16_t)s;
    }
    return (int)n;
}

/* ------------------------------------------------------------------ WAV -- */

static void voice_write_wav_header(uint8_t *hdr, uint32_t pcm_bytes,
                                   uint32_t sample_rate)
{
    uint32_t byte_rate = sample_rate * 2; /* mono, 16-bit */
    uint32_t riff_size = 36 + pcm_bytes;

    memcpy(hdr, "RIFF", 4);
    memcpy(hdr + 4, &riff_size, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    uint32_t fmt_size = 16;
    memcpy(hdr + 16, &fmt_size, 4);
    uint16_t fmt = 1, channels = 1, block_align = 2, bits = 16;
    memcpy(hdr + 20, &fmt, 2);
    memcpy(hdr + 22, &channels, 2);
    memcpy(hdr + 24, &sample_rate, 4);
    memcpy(hdr + 28, &byte_rate, 4);
    memcpy(hdr + 32, &block_align, 2);
    memcpy(hdr + 34, &bits, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &pcm_bytes, 4);
}

/* ------------------------------------------------------------------ STT -- */

static char *voice_transcribe(const int16_t *pcm, size_t n_samples)
{
    const char *api_key = CONFIG_CAP_IM_VOICE_OPENAI_API_KEY;
    if (!api_key[0]) {
        ESP_LOGE(TAG, "CONFIG_CAP_IM_VOICE_OPENAI_API_KEY is not set");
        return NULL;
    }

    const char *part1 =
        "--" MULTIPART_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "whisper-1\r\n"
        "--" MULTIPART_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    const char *part3 = "\r\n--" MULTIPART_BOUNDARY "--\r\n";

    uint32_t pcm_bytes = n_samples * sizeof(int16_t);
    uint8_t wav_header[VOICE_WAV_HEADER_SIZE];
    voice_write_wav_header(wav_header, pcm_bytes, VOICE_MIC_SAMPLE_RATE);

    size_t content_length = strlen(part1) + VOICE_WAV_HEADER_SIZE + pcm_bytes +
                            strlen(part3);

    esp_http_client_config_t cfg = {
        .url = OPENAI_STT_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = VOICE_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return NULL;
    }

    char *transcript = NULL;
    char *auth = NULL;
    char *response = NULL;

    if (asprintf(&auth, "Bearer %s", api_key) < 0) {
        goto cleanup;
    }
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type",
                               "multipart/form-data; boundary=" MULTIPART_BOUNDARY);

    if (esp_http_client_open(client, content_length) != ESP_OK) {
        ESP_LOGE(TAG, "STT: failed to open connection");
        goto cleanup;
    }

    bool write_ok =
        esp_http_client_write(client, part1, strlen(part1)) >= 0 &&
        esp_http_client_write(client, (const char *)wav_header,
                              VOICE_WAV_HEADER_SIZE) >= 0 &&
        esp_http_client_write(client, (const char *)pcm, pcm_bytes) >= 0 &&
        esp_http_client_write(client, part3, strlen(part3)) >= 0;
    if (!write_ok) {
        ESP_LOGE(TAG, "STT: request body write failed");
        goto cleanup;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    response = calloc(1, VOICE_STT_RESPONSE_MAX);
    if (!response) {
        goto cleanup;
    }
    int total = 0, r;
    while (total < VOICE_STT_RESPONSE_MAX - 1 &&
           (r = esp_http_client_read(client, response + total,
                                     VOICE_STT_RESPONSE_MAX - 1 - total)) > 0) {
        total += r;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "STT: HTTP %d: %.200s", status, response);
        goto cleanup;
    }

    cJSON *root = cJSON_Parse(response);
    if (root) {
        cJSON *text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text) && text->valuestring && text->valuestring[0]) {
            transcript = strdup(text->valuestring);
        }
        cJSON_Delete(root);
    }

cleanup:
    free(response);
    free(auth);
    esp_http_client_cleanup(client);
    return transcript;
}

/* ------------------------------------------------------------------ TTS -- */

static void voice_speak(const char *text)
{
    const char *api_key = CONFIG_CAP_IM_VOICE_OPENAI_API_KEY;
    cJSON *body = cJSON_CreateObject();
    if (!body) {
        return;
    }
    cJSON_AddStringToObject(body, "model", "tts-1");
    cJSON_AddStringToObject(body, "voice", CONFIG_CAP_IM_VOICE_TTS_VOICE);
    cJSON_AddStringToObject(body, "input", text);
    cJSON_AddStringToObject(body, "response_format", "pcm");
    char *payload = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!payload) {
        return;
    }

    esp_http_client_config_t cfg = {
        .url = OPENAI_TTS_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = VOICE_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    char *auth = NULL;
    char *chunk = NULL;

    if (!client) {
        free(payload);
        return;
    }
    if (asprintf(&auth, "Bearer %s", api_key) < 0) {
        goto cleanup;
    }
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    if (esp_http_client_open(client, strlen(payload)) != ESP_OK ||
        esp_http_client_write(client, payload, strlen(payload)) < 0) {
        ESP_LOGE(TAG, "TTS: request failed");
        goto cleanup;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "TTS: HTTP %d", status);
        goto cleanup;
    }

    chunk = malloc(4096);
    if (!chunk) {
        goto cleanup;
    }
    int r;
    while ((r = esp_http_client_read(client, chunk, 4096)) > 0) {
        size_t written = 0;
        /* Raw 24 kHz s16le mono PCM straight to the speaker. */
        i2s_channel_write(s_tx_chan, chunk, r, &written, portMAX_DELAY);
    }

cleanup:
    free(chunk);
    free(auth);
    free(payload);
    esp_http_client_cleanup(client);
}

static void voice_tts_task(void *arg)
{
    char *text;

    for (;;) {
        if (xQueueReceive(s_tts_queue, &text, portMAX_DELAY) == pdTRUE) {
            if (text) {
                ESP_LOGI(TAG, "speaking: %.80s%s", text,
                         strlen(text) > 80 ? "..." : "");
                voice_speak(text);
                free(text);
            }
        }
    }
}

/* ------------------------------------------------------------------ PTT -- */

static void voice_ptt_task(void *arg)
{
    gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_CAP_IM_VOICE_PTT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn_cfg);

    size_t n_samples = 0;

    for (;;) {
        bool pressed = gpio_get_level(CONFIG_CAP_IM_VOICE_PTT_GPIO) == 0;

        if (pressed && !s_recording) {
            ESP_LOGI(TAG, "recording...");
            n_samples = 0;
            s_recording = true;
        } else if (!pressed && s_recording) {
            s_recording = false;
            /* Ignore accidental taps shorter than 300 ms. */
            if (n_samples >= VOICE_MIC_SAMPLE_RATE * 3 / 10) {
                ESP_LOGI(TAG, "transcribing %.1f s...",
                         (float)n_samples / VOICE_MIC_SAMPLE_RATE);
                char *transcript = voice_transcribe(s_record_buf, n_samples);
                if (transcript) {
                    ESP_LOGI(TAG, "user: %s", transcript);
                    esp_err_t err = cap_im_local_emit_text(CAP_IM_VOICE_CHANNEL,
                                                           VOICE_CHAT_ID,
                                                           VOICE_SENDER_ID,
                                                           NULL,
                                                           transcript);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "emit failed: %s", esp_err_to_name(err));
                    }
                    free(transcript);
                } else {
                    ESP_LOGW(TAG, "transcription failed or empty");
                }
            }
        }

        if (s_recording) {
            if (n_samples + VOICE_MIC_CHUNK_SAMPLES <= s_record_capacity) {
                int n = voice_mic_read(s_record_buf + n_samples,
                                       VOICE_MIC_CHUNK_SAMPLES);
                if (n > 0) {
                    n_samples += n;
                }
            } else {
                /* Buffer full: keep draining the mic so I2S doesn't overrun,
                 * but drop the data. */
                int16_t scratch[VOICE_MIC_CHUNK_SAMPLES];
                voice_mic_read(scratch, VOICE_MIC_CHUNK_SAMPLES);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

/* --------------------------------------------------------------- public -- */

esp_err_t cap_im_voice_handle_outbound(const cap_im_local_message_t *message)
{
    if (!message || !message->text || !message->text[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_tts_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    char *copy = strdup(message->text);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    if (xQueueSend(s_tts_queue, &copy, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(copy);
        ESP_LOGW(TAG, "TTS queue full, reply dropped");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t cap_im_voice_start(void)
{
    s_record_capacity =
        (size_t)VOICE_MIC_SAMPLE_RATE * CONFIG_CAP_IM_VOICE_MAX_RECORD_SECONDS;
    s_record_buf = heap_caps_malloc(s_record_capacity * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_record_buf, ESP_ERR_NO_MEM, TAG,
                        "PSRAM record buffer (%u KB)",
                        (unsigned)(s_record_capacity * 2 / 1024));

    s_tts_queue = xQueueCreate(VOICE_TTS_QUEUE_DEPTH, sizeof(char *));
    ESP_RETURN_ON_FALSE(s_tts_queue, ESP_ERR_NO_MEM, TAG, "tts queue");

    ESP_RETURN_ON_ERROR(voice_i2s_init(), TAG, "i2s");

    BaseType_t ok;
    ok = xTaskCreate(voice_tts_task, "voice_tts", 8192, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "tts task");
    ok = xTaskCreate(voice_ptt_task, "voice_ptt", 8192, NULL, 6, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "ptt task");

    ESP_LOGI(TAG, "voice channel ready — hold PTT (GPIO %d) to talk",
             CONFIG_CAP_IM_VOICE_PTT_GPIO);
    return ESP_OK;
}

#else /* !CONFIG_APP_CLAW_CAP_IM_VOICE */

esp_err_t cap_im_voice_start(void)
{
    return ESP_OK;
}

esp_err_t cap_im_voice_handle_outbound(const cap_im_local_message_t *message)
{
    (void)message;
    return ESP_OK;
}

#endif /* CONFIG_APP_CLAW_CAP_IM_VOICE */
