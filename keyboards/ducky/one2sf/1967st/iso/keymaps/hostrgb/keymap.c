// Copyright 2019 /u/KeepItUnder
// SPDX-License-Identifier: GPL-2.0-or-later

#include QMK_KEYBOARD_H
#include "hostrgb_protocol.h"

enum Layer {
    _QWERTY,
    _FUNCTION,
    _COLOUR
};

// Fn + Delete / Page Up / Page Down: show the Claude Code session of that agent indicator.
enum custom_keycodes {
    AGENT_1 = SAFE_RANGE,
    AGENT_2,
    AGENT_3,
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
        KC_GRV,  KC_F1,   KC_F2,   KC_F3,   KC_F4,   KC_F5,   KC_F6,   KC_F7,   KC_F8,   KC_F9,   KC_F10,  KC_F11,  KC_F12,  KC_DEL,  AGENT_1,
        _______, MS_BTN1, MS_UP,   MS_BTN2, MS_WHLU, _______, KC_INS,  _______, KC_UP,   KC_PAUS, KC_PGUP, KC_HOME, KC_PSCR,          AGENT_2,
        _______, MS_LEFT, MS_DOWN, MS_RGHT, MS_WHLD, _______, KC_SCRL, KC_LEFT, KC_DOWN, KC_RGHT, KC_PGDN, KC_END,  _______, _______, AGENT_3,
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

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    if (keycode >= AGENT_1 && keycode <= AGENT_3) {
        if (record->event.pressed) hostrgb_agent_key(keycode - AGENT_1);
        return false;
    }
    return true;
}

void keyboard_post_init_user(void) {
    hostrgb_init();
}

void eeconfig_init_user(void) {
    hostrgb_reset_storage();
}
