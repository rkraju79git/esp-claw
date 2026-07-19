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
 * Voice pipeline observability. Events:
 *   "recording_start", "recording_stop" (detail: "3.2 s"), "transcribing",
 *   "transcript" (detail: user text), "reply" (detail: agent text),
 *   "speaking_start", "speaking_end", "error" (detail: description)
 */
typedef void (*cap_im_voice_event_cb_t)(const char *event,
                                        const char *detail,
                                        void *user_ctx);

/* Register an observer for voice pipeline events (single observer). May be
 * called before or after cap_im_voice_start(). */
esp_err_t cap_im_voice_set_event_callback(cap_im_voice_event_cb_t cb,
                                          void *user_ctx);

/* --- Hardware test hooks (used by the /hwtest bench) ------------------- */

/* Speak arbitrary text through the TTS pipeline. */
esp_err_t cap_im_voice_say(const char *text);

/* Record `seconds` (1..10) from the mic, report the peak sample level
 * (0..32767) and the cloud transcription. */
esp_err_t cap_im_voice_test_mic(int seconds,
                                char *transcript,
                                size_t transcript_size,
                                int *peak_out);

/* Play a sine test tone on the speaker. */
esp_err_t cap_im_voice_test_tone(int freq_hz, int duration_ms);

/* Mic wiring diagnostic: raw I2S bus statistics per stereo slot. Values are
 * raw 32-bit slot samples (INMP441: 24-bit data in the high bits). */
typedef struct {
    int32_t left_peak;      /* max |sample| seen on the LEFT slot */
    int32_t right_peak;     /* max |sample| seen on the RIGHT slot */
    int32_t left_dc;        /* mean sample (DC offset), LEFT */
    int32_t right_dc;       /* mean sample (DC offset), RIGHT */
    int     left_nonzero_pct;  /* % of samples that are not exactly 0 */
    int     right_nonzero_pct;
} cap_im_voice_mic_diag_t;

/* Sample ~1/4 s from each stereo slot in turn and report raw bus statistics.
 * Distinguishes: dead SD line (all zero), signal on the wrong channel (L/R
 * pin not grounded), and a healthy mic. Restores the normal left-slot config
 * before returning. */
esp_err_t cap_im_voice_mic_diag(cap_im_voice_mic_diag_t *out);

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
