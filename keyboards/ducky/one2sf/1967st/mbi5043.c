// Copyright 2026 Akkamir
// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Custom RGB matrix driver for the Ducky One 2 SF (DKON1967ST).
 *
 * Hardware, as recovered from the stock Ducky V1.12 firmware:
 *   - three MBI5043GP 16-channel drivers, one per colour, sharing DCLK (PD4) and LE (PD3);
 *     serial data in on PB14 (red), PB13 (green), PB12 (blue);
 *   - GCLK: PWM0 on PA12 at 2 MHz;
 *   - five multiplexed LED rows switched high on PC4, PC5, PB3, PB2, PD8;
 *   - PD5 gates LED power, active low;
 *   - LED (row, col) matches the switch matrix (row, col), 15 columns per row.
 *
 * Rows are refreshed from TIMER1 (see mcuconf.h for why not TIMER0).
 *
 * The refresh sequence mirrors the stock firmware: per column, 16 bits MSB first with LE held
 * high over the last DCLK edge (data latch), then 2 idle DCLKs and LE high over 3 DCLKs
 * (global latch), then the next row is switched on.
 *
 * mbi5043_fade_in() brings published frames up from black over a duration (used when the host
 * mode hands the LEDs back to the effects, so the lighting does not snap on).
 */

#include "quantum.h"

#ifdef RGB_MATRIX_ENABLE

#    define LED_ROWS 5
#    define LED_COLS 15

#    ifndef MBI5043_REFRESH_INTERVAL_US
#        define MBI5043_REFRESH_INTERVAL_US 512 // one row slot = 1024 GCLK cycles at 2 MHz
#    endif

static uint16_t        framebuffer[LED_ROWS][LED_COLS][3]; // streamed by the row refresh interrupt
static uint16_t        pending[LED_ROWS][LED_COLS][3];     // written by rgb_matrix, published on flush
static uint8_t         led_row[RGB_MATRIX_LED_COUNT];
static uint8_t         led_col[RGB_MATRIX_LED_COUNT];
static volatile uint8_t current_row;
static bool             fading;
static uint16_t         fade_start;
static uint16_t         fade_duration;

static inline void row_write(uint8_t row, uint32_t level) {
    switch (row) {
        case 0: PC4 = level; break;
        case 1: PC5 = level; break;
        case 2: PB3 = level; break;
        case 3: PB2 = level; break;
        case 4: PD8 = level; break;
    }
}

#    define DCLK_PULSE() \
        do {             \
            PD4 = 1;     \
            PD4 = 0;     \
        } while (0)

static void refresh_row(GPTDriver *gptp) {
    (void)gptp;

    row_write(current_row, 0);
    current_row = (current_row + 1) % LED_ROWS;

    for (uint8_t col = 0; col < LED_COLS; col++) {
        const uint16_t r = framebuffer[current_row][col][0];
        const uint16_t g = framebuffer[current_row][col][1];
        const uint16_t b = framebuffer[current_row][col][2];

        for (uint16_t mask = 0x8000; mask >= 2; mask >>= 1) {
            PB14 = (r & mask) != 0;
            PB13 = (g & mask) != 0;
            PB12 = (b & mask) != 0;
            DCLK_PULSE();
        }
        PD3  = 1; // data latch
        PB14 = r & 1;
        PB13 = g & 1;
        PB12 = b & 1;
        DCLK_PULSE();
        PD3 = 0;
    }

    DCLK_PULSE();
    DCLK_PULSE();
    PD3 = 1; // global latch
    DCLK_PULSE();
    DCLK_PULSE();
    DCLK_PULSE();
    PD3 = 0;

    row_write(current_row, 1);
}

static const PWMConfig gclk_config = {
    .frequency = 18000000, // driver prescaler is HCLK / frequency, applied as divider + 1: 72 MHz / 5 = 14.4 MHz
    .period    = 7,        // 14.4 MHz / 7 ~= 2.06 MHz
    .callback  = NULL,
    .channels =
        {
            {PWM_OUTPUT_ACTIVE_HIGH, NULL, NUC123_PWM_CH0_PIN_PA12},
            {PWM_OUTPUT_DISABLED, NULL, NUC123_PWM_CH1_PIN_NONE},
            {PWM_OUTPUT_DISABLED, NULL, NUC123_PWM_CH2_PIN_NONE},
            {PWM_OUTPUT_DISABLED, NULL, NUC123_PWM_CH3_PIN_NONE},
        },
};

static const GPTConfig refresh_config = {
    .frequency = 1000000,
    .callback  = refresh_row,
};

static inline uint16_t gamma16(uint8_t v) {
    return (uint16_t)((uint32_t)v * v + ((uint32_t)v * v >> 8)); // 255 -> 65279
}

static void mbi5043_init(void) {
    PD5 = 1; // LED power off while configuring
    palSetLineMode(D5, PAL_MODE_OUTPUT_PUSHPULL);

    const pin_t outputs[] = {B14, B13, B12, D4, D3, C4, C5, B3, B2, D8};
    for (uint8_t i = 0; i < ARRAY_SIZE(outputs); i++) {
        palClearLine(outputs[i]);
        palSetLineMode(outputs[i], PAL_MODE_OUTPUT_PUSHPULL);
    }

    for (uint8_t row = 0; row < LED_ROWS; row++) {
        for (uint8_t col = 0; col < LED_COLS; col++) {
            uint8_t index = g_led_config.matrix_co[row][col];
            if (index < RGB_MATRIX_LED_COUNT) {
                led_row[index] = row;
                led_col[index] = col;
            }
        }
    }
    memset(framebuffer, 0, sizeof(framebuffer));
    memset(pending, 0, sizeof(pending));

    pwmStart(&PWMD1, &gclk_config);
    pwmEnableChannel(&PWMD1, 0, gclk_config.period / 2);

    gptStart(&GPTD1, &refresh_config);
    gptStartContinuous(&GPTD1, MBI5043_REFRESH_INTERVAL_US);

    PD5 = 0; // LED power on
}

static void mbi5043_set_color(int index, uint8_t r, uint8_t g, uint8_t b) {
    if (index < 0 || index >= RGB_MATRIX_LED_COUNT) return;
    uint16_t *px = pending[led_row[index]][led_col[index]];
    px[0]        = gamma16(r);
    px[1]        = gamma16(g);
    px[2]        = gamma16(b);
}

static void mbi5043_set_color_all(uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < RGB_MATRIX_LED_COUNT; i++) {
        mbi5043_set_color(i, r, g, b);
    }
}

void mbi5043_fade_in(uint16_t duration_ms) {
    fade_start    = timer_read();
    fade_duration = duration_ms;
    fading        = duration_ms > 0;
}

static void mbi5043_flush(void) {
    // rgb_matrix renders effects over several passes and applies indicators last, so only a
    // finished frame may reach the LEDs.
    uint32_t scale = 0;
    if (fading) {
        uint16_t elapsed = timer_elapsed(fade_start);
        if (elapsed >= fade_duration) {
            fading = false;
        } else {
            scale = gamma16((uint32_t)elapsed * 255 / fade_duration); // perceptually even ramp
        }
    }
    osalSysLock();
    if (fading) {
        uint16_t *dst = &framebuffer[0][0][0];
        uint16_t *src = &pending[0][0][0];
        for (uint16_t i = 0; i < LED_ROWS * LED_COLS * 3; i++) {
            dst[i] = (uint32_t)src[i] * scale >> 16;
        }
    } else {
        memcpy(framebuffer, pending, sizeof(framebuffer));
    }
    osalSysUnlock();
}

const rgb_matrix_driver_t rgb_matrix_driver = {
    .init          = mbi5043_init,
    .flush         = mbi5043_flush,
    .set_color     = mbi5043_set_color,
    .set_color_all = mbi5043_set_color_all,
};

#endif
