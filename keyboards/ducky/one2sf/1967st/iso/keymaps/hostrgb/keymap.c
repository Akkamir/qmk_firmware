// Copyright 2019 /u/KeepItUnder
// SPDX-License-Identifier: GPL-2.0-or-later

#include QMK_KEYBOARD_H
#include "raw_hid.h"

enum Layer {
    _QWERTY,
    _FUNCTION,
    _COLOUR
};

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {

    [_QWERTY] = LAYOUT(
        QK_GESC, KC_1,    KC_2,    KC_3,    KC_4,    KC_5,    KC_6,    KC_7,    KC_8,    KC_9,    KC_0,    KC_MINS, KC_EQL,  KC_BSPC, KC_DEL,
        KC_TAB,  KC_Q,    KC_W,    KC_E,    KC_R,    KC_T,    KC_Y,    KC_U,    KC_I,    KC_O,    KC_P,    KC_LBRC, KC_RBRC,          KC_PGUP,
        KC_CAPS, KC_A,    KC_S,    KC_D,    KC_F,    KC_G,    KC_H,    KC_J,    KC_K,    KC_L,    KC_SCLN, KC_QUOT, KC_NUHS, KC_ENT,  KC_PGDN,
        KC_LSFT, KC_NUBS, KC_Z,    KC_X,    KC_C,    KC_V,    KC_B,    KC_N,    KC_M,    KC_COMM, KC_DOT,  KC_SLSH, KC_RSFT, KC_UP,
        KC_LCTL, KC_LGUI, KC_LALT,                            KC_SPC,                    KC_RALT, MO(1),   KC_RCTL, KC_LEFT, KC_DOWN, KC_RGHT
    ),

    [_FUNCTION] = LAYOUT(
        KC_GRV,  KC_F1,   KC_F2,   KC_F3,   KC_F4,   KC_F5,   KC_F6,   KC_F7,   KC_F8,   KC_F9,   KC_F10,  KC_F11,  KC_F12,  KC_DEL,  _______,
        _______, MS_BTN1, MS_UP,   MS_BTN2, MS_WHLU, _______, KC_INS,  _______, KC_UP,   KC_PAUS, KC_PGUP, KC_HOME, KC_PSCR,          KC_HOME,
        _______, MS_LEFT, MS_DOWN, MS_RGHT, MS_WHLD, _______, KC_SCRL, KC_LEFT, KC_DOWN, KC_RGHT, KC_PGDN, KC_END,  _______, _______, KC_END,
        _______, _______, RM_TOGG, RM_NEXT, RM_HUEU, RM_HUED, RM_SATU, RM_SATD, KC_MUTE, KC_VOLD, KC_VOLU, _______, _______, _______,
        _______, _______, _______,                            QK_BOOT,                   MO(2),   _______, _______, _______, _______, _______
    ),

    [_COLOUR] = LAYOUT(
        _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______,
        _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______,          _______,
        _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______,
        _______, _______, RM_TOGG, RM_NEXT, RM_HUEU, RM_HUED, RM_SATU, RM_SATD, _______, _______, _______, _______, _______, _______,
        _______, _______, _______,                            _______,                   _______, _______, _______, _______, _______, _______
    ),

};

// Alt + Esc types the grave key ("@" on a French Mac layout), like the stock Ducky firmware.
const key_override_t alt_esc_grave = ko_make_basic(MOD_MASK_ALT, QK_GESC, KC_GRV);

const key_override_t *key_overrides[] = {
    &alt_esc_grave,
};

/*
 * Host RGB protocol over raw HID (32-byte reports, usage page 0xFF60 / usage 0x61).
 * Byte 0 is the command; the reply echoes it with byte 1 = status (0 ok, 0xFF error).
 */
enum host_rgb_command {
    HOST_RGB_PING = 0x01, // reply: [0x01, status, protocol version, LED count]
    HOST_RGB_MODE = 0x02, // [0x02, on]: 1 = host colours replace the effect, 0 = back to the effect
    HOST_RGB_SET  = 0x03, // [0x03, first LED, count, r, g, b, ...] up to 9 LEDs per report
    HOST_RGB_FILL = 0x04, // [0x04, r, g, b] every LED
};

#define HOST_RGB_PROTOCOL_VERSION 1

static uint8_t host_colors[RGB_MATRIX_LED_COUNT][3];
static bool    host_mode = false;

void raw_hid_receive(uint8_t *data, uint8_t length) {
    uint8_t status = 0;

    switch (data[0]) {
        case HOST_RGB_PING:
            data[2] = HOST_RGB_PROTOCOL_VERSION;
            data[3] = RGB_MATRIX_LED_COUNT;
            break;
        case HOST_RGB_MODE:
            host_mode = data[1] != 0;
            if (host_mode) {
                rgb_matrix_enable_noeeprom();
            }
            break;
        case HOST_RGB_SET: {
            uint8_t first = data[1];
            uint8_t count = data[2];
            if (count > (length - 3) / 3 || first + count > RGB_MATRIX_LED_COUNT) {
                status = 0xFF;
                break;
            }
            memcpy(host_colors[first], &data[3], count * 3);
            break;
        }
        case HOST_RGB_FILL:
            for (uint8_t i = 0; i < RGB_MATRIX_LED_COUNT; i++) {
                memcpy(host_colors[i], &data[1], 3);
            }
            break;
        default:
            status = 0xFF;
            break;
    }

    data[1] = status;
    raw_hid_send(data, length);
}

bool rgb_matrix_indicators_advanced_user(uint8_t led_min, uint8_t led_max) {
    if (!host_mode) {
        return true;
    }
    for (uint8_t i = led_min; i < led_max; i++) {
        rgb_matrix_set_color(i, host_colors[i][0], host_colors[i][1], host_colors[i][2]);
    }
    return false;
}
