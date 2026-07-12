/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * No custom device factories are required: the OV3660 DVP camera is brought up
 * entirely by the board manager from board_devices.yaml, and audio is owned by
 * the cap_im_voice component (not the board manager). This translation unit
 * exists so the board build has a setup_device.c like every other board.
 */
