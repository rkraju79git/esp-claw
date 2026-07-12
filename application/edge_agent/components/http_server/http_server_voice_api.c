/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Voice channel observability: keeps a ring buffer of voice pipeline events,
 * mirrors them to the Web IM WebSocket as {"type":"voice_event",...}, and
 * serves the /voice log page.
 */
#include "http_server_priv.h"

#include <string.h>

#include "cap_im_voice.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#if CONFIG_APP_CLAW_CAP_IM_VOICE

static const char *TAG = "http_voice";

#define VOICE_LOG_ENTRIES 32
#define VOICE_LOG_EVENT_MAX 24
#define VOICE_LOG_DETAIL_MAX 200

typedef struct {
    int64_t ts_ms;
    char event[VOICE_LOG_EVENT_MAX];
    char detail[VOICE_LOG_DETAIL_MAX];
} voice_log_entry_t;

static voice_log_entry_t s_log[VOICE_LOG_ENTRIES];
static size_t s_log_next;
static size_t s_log_count;
static SemaphoreHandle_t s_log_lock;

extern const uint8_t voice_log_html_start[] asm("_binary_voice_log_html_start");
extern const uint8_t voice_log_html_end[] asm("_binary_voice_log_html_end");

static void voice_event_cb(const char *event, const char *detail, void *ctx)
{
    (void)ctx;
    int64_t ts = esp_timer_get_time() / 1000;

    if (s_log_lock && xSemaphoreTake(s_log_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        voice_log_entry_t *e = &s_log[s_log_next];
        e->ts_ms = ts;
        strlcpy(e->event, event ? event : "", sizeof(e->event));
        strlcpy(e->detail, detail ? detail : "", sizeof(e->detail));
        s_log_next = (s_log_next + 1) % VOICE_LOG_ENTRIES;
        if (s_log_count < VOICE_LOG_ENTRIES) {
            s_log_count++;
        }
        xSemaphoreGive(s_log_lock);
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddStringToObject(root, "type", "voice_event");
    cJSON_AddStringToObject(root, "event", event ? event : "");
    cJSON_AddStringToObject(root, "detail", detail ? detail : "");
    cJSON_AddNumberToObject(root, "ts", (double)ts);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload) {
        webim_ws_broadcast_json(payload);
        free(payload);
    }
}

static esp_err_t voice_events_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *events = cJSON_AddArrayToObject(root, "events");

    if (s_log_lock && xSemaphoreTake(s_log_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        size_t start = (s_log_next + VOICE_LOG_ENTRIES - s_log_count) %
                       VOICE_LOG_ENTRIES;
        for (size_t i = 0; i < s_log_count; i++) {
            const voice_log_entry_t *e = &s_log[(start + i) % VOICE_LOG_ENTRIES];
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "ts", (double)e->ts_ms);
            cJSON_AddStringToObject(item, "event", e->event);
            cJSON_AddStringToObject(item, "detail", e->detail);
            cJSON_AddItemToArray(events, item);
        }
        xSemaphoreGive(s_log_lock);
    }

    return http_server_send_json_response(req, root);
}

static esp_err_t voice_page_handler(httpd_req_t *req)
{
    return http_server_send_embedded_file(req, voice_log_html_start,
                                          voice_log_html_end, "text/html");
}

esp_err_t http_server_register_voice_routes(httpd_handle_t server)
{
    if (!s_log_lock) {
        s_log_lock = xSemaphoreCreateMutex();
    }

    const httpd_uri_t handlers[] = {
        { .uri = "/api/voice/events", .method = HTTP_GET, .handler = voice_events_handler },
        { .uri = "/voice", .method = HTTP_GET, .handler = voice_page_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }

    cap_im_voice_set_event_callback(voice_event_cb, NULL);
    ESP_LOGI(TAG, "voice log UI at /voice");
    return ESP_OK;
}

#endif /* CONFIG_APP_CLAW_CAP_IM_VOICE */
