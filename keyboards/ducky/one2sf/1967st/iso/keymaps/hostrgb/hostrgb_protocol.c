// Copyright 2026 Akkamir
// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Host RGB protocol v2 over raw HID (32-byte reports, usage page 0xFF60 / usage 0x61).
 * Request: byte 0 = command, arguments from byte 1. Reply: byte 0 = command, byte 1 = status,
 * payload from byte 2.
 *
 * Layers, highest first: host mode (live frames from the CLI, never saved), overlay (per-key
 * colours, saved in the user EEPROM block), base effect (rgb_matrix config, saved natively).
 * rgb_matrix only calls the indicator hooks while it is enabled, so "base off" is our own flag:
 * LEDs without a custom colour are painted black instead of disabling rgb_matrix.
 */

#include QMK_KEYBOARD_H
#include <stddef.h>
#include <string.h>
#include "raw_hid.h"
#include "eeconfig.h"
#include "hostrgb_protocol.h"

#define PROTOCOL_VERSION 2
#define OVERLAY_VERSION 1
#define HOST_LEDS_PER_REPORT 9
#define OVERLAY_PER_REPORT 7
#define EFFECTS_PER_REPORT 28

enum status {
    STATUS_OK              = 0,
    STATUS_UNKNOWN_COMMAND = 1,
    STATUS_BAD_ARGUMENT    = 2,
    STATUS_FLASH_ERROR     = 3, // reserved: QMK's EEPROM API does not report write failures
};

enum command {
    CMD_PING          = 0x01,
    CMD_MODE          = 0x02,
    CMD_SET           = 0x03,
    CMD_FILL          = 0x04,
    CMD_GET_INFO      = 0x10,
    CMD_GET_EFFECTS   = 0x11,
    CMD_GET_STATE     = 0x12,
    CMD_SET_BASE      = 0x13,
    CMD_GET_OVERLAY   = 0x14,
    CMD_SET_OVERLAY   = 0x15,
    CMD_CLEAR_OVERLAY = 0x16,
    CMD_SAVE          = 0x17,
};

// Stable ids exposed to the host; QMK mode numbers depend on which effects are compiled in.
static const struct {
    uint8_t id;
    uint8_t mode;
} effects[] = {
    {1, RGB_MATRIX_SOLID_COLOR},
    {2, RGB_MATRIX_BREATHING},
    {3, RGB_MATRIX_GRADIENT_LEFT_RIGHT},
    {4, RGB_MATRIX_CYCLE_ALL},
    {5, RGB_MATRIX_CYCLE_LEFT_RIGHT},
    {6, RGB_MATRIX_RAINBOW_MOVING_CHEVRON},
    {7, RGB_MATRIX_HUE_WAVE},
    {8, RGB_MATRIX_PIXEL_RAIN},
    {9, RGB_MATRIX_DIGITAL_RAIN},
    {10, RGB_MATRIX_SOLID_REACTIVE_SIMPLE},
    {11, RGB_MATRIX_SPLASH},
    {12, RGB_MATRIX_MULTISPLASH},
    {13, RGB_MATRIX_TYPING_HEATMAP},
    {14, RGB_MATRIX_BAND_SAT},
};
#define EFFECT_COUNT ARRAY_SIZE(effects)

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  base_on; // 0: LEDs without a custom colour stay dark
    uint8_t  mask[(RGB_MATRIX_LED_COUNT + 7) / 8];
    uint8_t  rgb[RGB_MATRIX_LED_COUNT][3];
    uint16_t checksum;
} overlay_t;

_Static_assert(sizeof(overlay_t) == EECONFIG_USER_DATA_SIZE, "EECONFIG_USER_DATA_SIZE must match overlay_t");

static overlay_t overlay;
static bool      dirty;
static bool      host_mode;
static uint8_t   host_colors[RGB_MATRIX_LED_COUNT][3];

static uint16_t overlay_checksum(const overlay_t *block) {
    const uint8_t *bytes = (const uint8_t *)block;
    uint16_t       sum   = 0;
    for (size_t i = 0; i < offsetof(overlay_t, checksum); i++) {
        sum += bytes[i];
    }
    return sum;
}

static void overlay_clear(uint8_t base_on) {
    memset(&overlay, 0, sizeof(overlay));
    overlay.version = OVERLAY_VERSION;
    overlay.base_on = base_on;
}

static bool overlay_has(uint8_t led) {
    return overlay.mask[led / 8] & (1 << (led % 8));
}

static void overlay_set(uint8_t led, bool custom, const uint8_t rgb[3]) {
    if (custom) {
        overlay.mask[led / 8] |= (1 << (led % 8));
        memcpy(overlay.rgb[led], rgb, 3);
    } else {
        overlay.mask[led / 8] &= ~(1 << (led % 8));
        memset(overlay.rgb[led], 0, 3);
    }
}

static uint8_t overlay_count(void) {
    uint8_t count = 0;
    for (uint8_t led = 0; led < RGB_MATRIX_LED_COUNT; led++) {
        count += overlay_has(led);
    }
    return count;
}

void hostrgb_reset_storage(void) {
    overlay_clear(1);
    overlay.checksum = overlay_checksum(&overlay);
    eeconfig_update_user_datablock(&overlay, 0, sizeof(overlay));
}

void hostrgb_init(void) {
    eeconfig_read_user_datablock(&overlay, 0, sizeof(overlay));
    if (overlay.version != OVERLAY_VERSION || overlay.checksum != overlay_checksum(&overlay)) {
        overlay_clear(1);
    }
    dirty = false;
}

static uint8_t effect_id_for_mode(uint8_t mode) {
    for (uint8_t i = 0; i < EFFECT_COUNT; i++) {
        if (effects[i].mode == mode) return effects[i].id;
    }
    return 0;
}

static int16_t mode_for_effect_id(uint8_t id) {
    for (uint8_t i = 0; i < EFFECT_COUNT; i++) {
        if (effects[i].id == id) return effects[i].mode;
    }
    return -1;
}

#ifdef HOSTRGB_DEBUG
#    include <hal.h>
// Diagnostic command 0x7E. Sub 0: flash descriptor seen now and FMC registers. Sub 1 addr32: 24 bytes
// read from the memory-mapped flash.
static uint8_t handle_debug(const uint8_t *args, uint8_t *out) {
    if (args[1] == 0) {
        const flash_descriptor_t *desc = efl_lld_get_descriptor(&EFLD1);
        uint32_t values[6] = {(uint32_t)desc->size, (uint32_t)desc->sectors_count, FMC->ISPCON, FMC->DFBADR, (uint32_t)EFLD1.state, CLK->AHBCLK};
        memcpy(out, values, sizeof(values));
        return 0;
    }
    if (args[1] == 2) {
        // Controlled erase + program of sector 124 (0xF800, first sector of the wear-leveling store).
        uint32_t       values[7] = {0};
        const uint32_t pattern   = 0x12345678;
        BaseFlash     *flash     = (BaseFlash *)(void *)&EFLD1;
        values[0]                = eflStart(&EFLD1, NULL);
        values[1]                = FMC->ISPCON;
        values[2]                = flashStartEraseSector(flash, 124);
        values[3]                = flashWaitErase(flash);
        values[4]                = flashProgram(flash, 0xF800, 4, (const uint8_t *)&pattern);
        values[5]                = *(volatile const uint32_t *)0xF800;
        eflStop(&EFLD1);
        values[6] = CLK->AHBCLK;
        memcpy(out, values, sizeof(values));
        return 0;
    }
    uint32_t addr;
    memcpy(&addr, &args[2], 4);
    memcpy(out, (const void *)addr, 24);
    return 0;
}
#endif

static uint8_t handle_command(uint8_t *data, uint8_t length) {
    uint8_t args[32]; // replies overwrite the request in place
    memcpy(args, data, length);
    memset(&data[1], 0, length - 1);
    uint8_t *out = &data[2];

    switch (args[0]) {
        case CMD_PING:
            out[0] = PROTOCOL_VERSION;
            out[1] = RGB_MATRIX_LED_COUNT;
            return STATUS_OK;

        case CMD_MODE:
            host_mode = args[1] != 0;
            if (host_mode) rgb_matrix_enable_noeeprom();
            return STATUS_OK;

        case CMD_SET: {
            uint8_t first = args[1], count = args[2];
            if (count > HOST_LEDS_PER_REPORT || first + count > RGB_MATRIX_LED_COUNT) return STATUS_BAD_ARGUMENT;
            memcpy(host_colors[first], &args[3], count * 3);
            return STATUS_OK;
        }

        case CMD_FILL:
            for (uint8_t led = 0; led < RGB_MATRIX_LED_COUNT; led++) {
                memcpy(host_colors[led], &args[1], 3);
            }
            return STATUS_OK;

        case CMD_GET_INFO:
            out[0] = PROTOCOL_VERSION;
            out[1] = RGB_MATRIX_LED_COUNT;
            out[2] = EFFECT_COUNT;
            out[3] = 1; // setup persisted in flash
            return STATUS_OK;

        case CMD_GET_EFFECTS: {
            uint8_t first = args[1];
            if (first > EFFECT_COUNT) return STATUS_BAD_ARGUMENT;
            uint8_t count = MIN(EFFECTS_PER_REPORT, EFFECT_COUNT - first);
            out[0]        = first;
            out[1]        = count;
            for (uint8_t i = 0; i < count; i++) {
                out[2 + i] = effects[first + i].id;
            }
            return STATUS_OK;
        }

        case CMD_GET_STATE: {
            hsv_t hsv = rgb_matrix_get_hsv();
            out[0]    = overlay.base_on;
            out[1]    = effect_id_for_mode(rgb_matrix_get_mode());
            out[2]    = hsv.h;
            out[3]    = hsv.s;
            out[4]    = hsv.v;
            out[5]    = rgb_matrix_get_speed();
            out[6]    = host_mode;
            out[7]    = overlay_count();
            out[8]    = dirty;
            return STATUS_OK;
        }

        case CMD_SET_BASE: {
            int16_t mode = mode_for_effect_id(args[2]);
            if (mode < 0) return STATUS_BAD_ARGUMENT;
            overlay.base_on = args[1] != 0;
            rgb_matrix_enable_noeeprom();
            rgb_matrix_mode_noeeprom(mode);
            rgb_matrix_sethsv_noeeprom(args[3], args[4], args[5]);
            rgb_matrix_set_speed_noeeprom(args[6]);
            dirty = true;
            return STATUS_OK;
        }

        case CMD_GET_OVERLAY: {
            uint8_t first = args[1];
            if (first >= RGB_MATRIX_LED_COUNT) return STATUS_BAD_ARGUMENT;
            uint8_t count = MIN(OVERLAY_PER_REPORT, RGB_MATRIX_LED_COUNT - first);
            out[0]        = first;
            out[1]        = count;
            for (uint8_t i = 0; i < count; i++) {
                out[2 + 4 * i] = overlay_has(first + i);
                memcpy(&out[3 + 4 * i], overlay.rgb[first + i], 3);
            }
            return STATUS_OK;
        }

        case CMD_SET_OVERLAY: {
            uint8_t first = args[1], count = args[2];
            if (count > OVERLAY_PER_REPORT || first + count > RGB_MATRIX_LED_COUNT) return STATUS_BAD_ARGUMENT;
            for (uint8_t i = 0; i < count; i++) {
                const uint8_t *entry = &args[3 + 4 * i];
                overlay_set(first + i, entry[0] & 1, &entry[1]);
            }
            dirty = true;
            return STATUS_OK;
        }

        case CMD_CLEAR_OVERLAY:
            overlay_clear(overlay.base_on);
            dirty = true;
            return STATUS_OK;

        case CMD_SAVE:
            overlay.checksum = overlay_checksum(&overlay);
            eeconfig_update_user_datablock(&overlay, 0, sizeof(overlay));
            eeconfig_update_rgb_matrix(&rgb_matrix_config);
            dirty = false;
            return STATUS_OK;

#ifdef HOSTRGB_DEBUG
        case 0x7E:
            return handle_debug(args, out);
#endif
        default:
            return STATUS_UNKNOWN_COMMAND;
    }
}

void raw_hid_receive(uint8_t *data, uint8_t length) {
    data[1] = handle_command(data, length);
    raw_hid_send(data, length);
}

static inline uint8_t scale(uint8_t channel, uint8_t value) {
    return (uint16_t)channel * value / 255;
}

bool rgb_matrix_indicators_advanced_user(uint8_t led_min, uint8_t led_max) {
    if (host_mode) {
        for (uint8_t led = led_min; led < led_max; led++) {
            rgb_matrix_set_color(led, host_colors[led][0], host_colors[led][1], host_colors[led][2]);
        }
        return false;
    }
    const uint8_t value = rgb_matrix_get_val();
    for (uint8_t led = led_min; led < led_max; led++) {
        if (overlay_has(led)) {
            rgb_matrix_set_color(led, scale(overlay.rgb[led][0], value), scale(overlay.rgb[led][1], value), scale(overlay.rgb[led][2], value));
        } else if (!overlay.base_on) {
            rgb_matrix_set_color(led, 0, 0, 0);
        }
    }
    return false;
}
