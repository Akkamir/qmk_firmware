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
 * Effect 15 ("heatmap on a background") is QMK's typing heatmap drawn at full brightness over the base
 * colour (whose brightness is set by the user); it shares the QMK mode with effect 13 and is told
 * apart by a flag. Effect 16 ("heatmap on custom colours") draws the heat over each key's custom colour,
 * in the base hue, whitening as keys get hotter; it is told apart by another flag.
 *
 * Agent indicators (protocol v3) are drawn last, above every layer including the host mode: up to 3 LEDs with a
 * colour, steady or breathing, never saved, cleared when the host has not refreshed them for 5 s. Fn + Delete /
 * Page Up / Page Down send an unsolicited event report [0x30, 0xA5, slot] to the host (0xA5 is never a status,
 * so the report cannot be mistaken for a reply). Indicators show only while rgb_matrix is enabled.
 */

#include QMK_KEYBOARD_H
#include <stddef.h>
#include <string.h>
#include <lib/lib8tion/lib8tion.h>
#include "raw_hid.h"
#include "eeconfig.h"
#include "hostrgb_protocol.h"

#define PROTOCOL_VERSION 3
#define OVERLAY_VERSION 1
#define HOST_LEDS_PER_REPORT 9
#define OVERLAY_PER_REPORT 7
#define EFFECTS_PER_REPORT 28
#define HOST_EXIT_FADE_MS 1500
#define INDICATOR_COUNT 3
#define INDICATOR_TIMEOUT_MS 5000
#define EVENT_AGENT_KEY 0x30
#define EVENT_MARKER 0xA5

void mbi5043_fade_in(uint16_t duration_ms); // LED driver (keyboards/ducky/one2sf/1967st/mbi5043.c)

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
    CMD_SET_INDICATORS = 0x18,
};

enum indicator_mode {
    INDICATOR_STEADY    = 0,
    INDICATOR_BREATHING = 1,
};

typedef struct {
    uint8_t led;
    uint8_t rgb[3];
    uint8_t mode;
} indicator_t;

static indicator_t indicators[INDICATOR_COUNT];
static uint8_t     indicator_count;
static uint32_t    indicators_at;

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
    {15, RGB_MATRIX_TYPING_HEATMAP}, // with BASE_HEATMAP_FLOOR
    {16, RGB_MATRIX_TYPING_HEATMAP}, // with BASE_HEATMAP_OVERLAY
};
#define HEATMAP_ON_BACKGROUND_ID 15
#define HEATMAP_ON_OVERLAY_ID 16
#define EFFECT_COUNT ARRAY_SIZE(effects)

#define BASE_ON 0x01
#define BASE_HEATMAP_FLOOR 0x02
#define BASE_HEATMAP_OVERLAY 0x04

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  base_flags; // BASE_ON: LEDs without a custom colour show the effect; BASE_HEATMAP_*: effects 15, 16
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

static void overlay_clear(uint8_t base_flags) {
    memset(&overlay, 0, sizeof(overlay));
    overlay.version    = OVERLAY_VERSION;
    overlay.base_flags = base_flags;
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
    overlay_clear(BASE_ON);
    overlay.checksum = overlay_checksum(&overlay);
    eeconfig_update_user_datablock(&overlay, 0, sizeof(overlay));
}

void hostrgb_init(void) {
    eeconfig_read_user_datablock(&overlay, 0, sizeof(overlay));
    if (overlay.version != OVERLAY_VERSION || overlay.checksum != overlay_checksum(&overlay)) {
        overlay_clear(BASE_ON);
    }
    dirty = false;
}

static uint8_t effect_id_for_mode(uint8_t mode) {
    if (mode == RGB_MATRIX_TYPING_HEATMAP && (overlay.base_flags & BASE_HEATMAP_FLOOR)) return HEATMAP_ON_BACKGROUND_ID;
    if (mode == RGB_MATRIX_TYPING_HEATMAP && (overlay.base_flags & BASE_HEATMAP_OVERLAY)) return HEATMAP_ON_OVERLAY_ID;
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

        case CMD_MODE: {
            bool was_host = host_mode;
            host_mode     = args[1] != 0;
            if (host_mode) rgb_matrix_enable_noeeprom();
            if (was_host && !host_mode) mbi5043_fade_in(HOST_EXIT_FADE_MS); // effects come back gently
            return STATUS_OK;
        }

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
            out[0]    = (overlay.base_flags & BASE_ON) != 0;
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
            overlay.base_flags = (args[1] != 0 ? BASE_ON : 0) | (args[2] == HEATMAP_ON_BACKGROUND_ID ? BASE_HEATMAP_FLOOR : 0) |
                                 (args[2] == HEATMAP_ON_OVERLAY_ID ? BASE_HEATMAP_OVERLAY : 0);
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
            overlay_clear(overlay.base_flags);
            dirty = true;
            return STATUS_OK;

        case CMD_SET_INDICATORS: {
            uint8_t count = args[1];
            if (count > INDICATOR_COUNT) return STATUS_BAD_ARGUMENT;
            for (uint8_t i = 0; i < count; i++) {
                const uint8_t *entry = &args[2 + 5 * i];
                if (entry[0] >= RGB_MATRIX_LED_COUNT || entry[4] > INDICATOR_BREATHING) return STATUS_BAD_ARGUMENT;
            }
            for (uint8_t i = 0; i < count; i++) {
                const uint8_t *entry = &args[2 + 5 * i];
                indicators[i]        = (indicator_t){.led = entry[0], .rgb = {entry[1], entry[2], entry[3]}, .mode = entry[4]};
            }
            indicator_count = count;
            indicators_at   = timer_read32();
            return STATUS_OK;
        }

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

// Effect 15: QMK's heat colours, saturated and at full brightness, over the base colour.
static void paint_heatmap_on_background(uint8_t led_min, uint8_t led_max) {
    const hsv_t base       = rgb_matrix_get_hsv();
    const rgb_t background = hsv_to_rgb(base);
    for (uint8_t row = 0; row < MATRIX_ROWS; row++) {
        for (uint8_t col = 0; col < MATRIX_COLS; col++) {
            uint8_t led = g_led_config.matrix_co[row][col];
            if (led == NO_LED || led < led_min || led >= led_max || overlay_has(led)) continue;
            uint8_t heat = g_rgb_frame_buffer[row][col];
            rgb_t   hot  = hsv_to_rgb((hsv_t){170 - qsub8(heat, 85), 255, scale8((qadd8(170, heat) - 170) * 3, RGB_MATRIX_MAXIMUM_BRIGHTNESS)});
            rgb_matrix_set_color(led, MAX(hot.r, background.r), MAX(hot.g, background.g), MAX(hot.b, background.b));
        }
    }
}

// Effect 16: each key shows its custom colour (or the base colour) at the base brightness; typing blends
// in the base hue at full brightness, then whitens it as the key gets hotter.
static void paint_heatmap_on_overlay(uint8_t led_min, uint8_t led_max) {
    const hsv_t   base       = rgb_matrix_get_hsv();
    const rgb_t   background = hsv_to_rgb(base);
    const uint8_t value      = base.v;
    for (uint8_t row = 0; row < MATRIX_ROWS; row++) {
        for (uint8_t col = 0; col < MATRIX_COLS; col++) {
            uint8_t led = g_led_config.matrix_co[row][col];
            if (led == NO_LED || led < led_min || led >= led_max) continue;
            rgb_t bg = background;
            if (overlay_has(led)) {
                bg = (rgb_t){.r = scale(overlay.rgb[led][0], value), .g = scale(overlay.rgb[led][1], value), .b = scale(overlay.rgb[led][2], value)};
            }
            uint8_t heat   = g_rgb_frame_buffer[row][col];
            uint8_t amount = (qadd8(170, heat) - 170) * 3;                        // 0..255 over heat 0..85
            uint8_t white  = qsub8(heat, 85) * 3 / 2;                            // 0..255 over heat 85..255
            rgb_t   hot    = hsv_to_rgb((hsv_t){base.h, 255 - white, RGB_MATRIX_MAXIMUM_BRIGHTNESS});
            rgb_matrix_set_color(led, blend8(bg.r, hot.r, amount), blend8(bg.g, hot.g, amount), blend8(bg.b, hot.b, amount));
        }
    }
}

void hostrgb_agent_key(uint8_t slot) {
    uint8_t report[32] = {EVENT_AGENT_KEY, EVENT_MARKER, slot};
    raw_hid_send(report, sizeof(report));
}

// Agent indicators, above everything. Breathing goes from 30 % to 100 % over ~2 s.
static void paint_indicators(uint8_t led_min, uint8_t led_max) {
    if (indicator_count && timer_elapsed32(indicators_at) > INDICATOR_TIMEOUT_MS) indicator_count = 0; // host gone
    const uint8_t breath = 77 + scale8(sin8(timer_read() / 8), 178);
    for (uint8_t i = 0; i < indicator_count; i++) {
        const indicator_t *indicator = &indicators[i];
        if (indicator->led < led_min || indicator->led >= led_max) continue;
        const uint8_t level = indicator->mode == INDICATOR_BREATHING ? breath : 255;
        rgb_matrix_set_color(indicator->led, scale8(indicator->rgb[0], level), scale8(indicator->rgb[1], level), scale8(indicator->rgb[2], level));
    }
}

static void paint_layers(uint8_t led_min, uint8_t led_max) {
    if (host_mode) {
        for (uint8_t led = led_min; led < led_max; led++) {
            rgb_matrix_set_color(led, host_colors[led][0], host_colors[led][1], host_colors[led][2]);
        }
        return;
    }
    if ((overlay.base_flags & BASE_ON) && (overlay.base_flags & BASE_HEATMAP_FLOOR) && rgb_matrix_get_mode() == RGB_MATRIX_TYPING_HEATMAP) {
        paint_heatmap_on_background(led_min, led_max);
    }
    if ((overlay.base_flags & BASE_ON) && (overlay.base_flags & BASE_HEATMAP_OVERLAY) && rgb_matrix_get_mode() == RGB_MATRIX_TYPING_HEATMAP) {
        paint_heatmap_on_overlay(led_min, led_max);
        return;
    }
    const uint8_t value = rgb_matrix_get_val();
    for (uint8_t led = led_min; led < led_max; led++) {
        if (overlay_has(led)) {
            rgb_matrix_set_color(led, scale(overlay.rgb[led][0], value), scale(overlay.rgb[led][1], value), scale(overlay.rgb[led][2], value));
        } else if (!(overlay.base_flags & BASE_ON)) {
            rgb_matrix_set_color(led, 0, 0, 0);
        }
    }
}

bool rgb_matrix_indicators_advanced_user(uint8_t led_min, uint8_t led_max) {
    paint_layers(led_min, led_max);
    paint_indicators(led_min, led_max);
    return false;
}
