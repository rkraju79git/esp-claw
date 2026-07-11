/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "cap_im_local.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Channel name used on the local IM gateway for voice traffic. */
#define CAP_IM_VOICE_CHANNEL "voice"

/*
 * Initialize I2S mic/speaker, start the push-to-talk and TTS playback tasks.
 * Call after app_claw_start() so the local IM gateway is available.
 */
esp_err_t cap_im_voice_start(void);

/*
 * Handle an outbound agent reply addressed to the voice channel: queue it for
 * text-to-speech playback. Called from the local IM outbound callback
 * (see http_server_webim_api.c routing).
 */
esp_err_t cap_im_voice_handle_outbound(const cap_im_local_message_t *message);

#ifdef __cplusplus
}
#endif
