// Copyright 2019 /u/KeepItUnder
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#define GRAVE_ESC_GUI_OVERRIDE
#define MK_3_SPEED
#define MK_C_OFFSET_UNMOD   400    /* Cursor offset per movement (unmodified) */
#define MK_C_INTERVAL_UNMOD  5     /* Time between cursor movements (unmodified) */
#define MK_W_OFFSET_UNMOD   100    /* Scroll steps per scroll action (unmodified) */
#define MK_W_INTERVAL_UNMOD 10     /* Time between scroll steps (unmodified) */

// The NUC123 USB LLD gives each logical endpoint separate IN and OUT hardware buffers,
// so raw HID can use one endpoint number for both directions.
#define USB_ENDPOINTS_ARE_REORDERABLE

// Saved overlay block, see overlay_t in hostrgb_protocol.c.
#define EECONFIG_USER_DATA_SIZE 217

// Wear-leveled EEPROM in the last 2 KB of the APROM.
#define WEAR_LEVELING_BACKING_SIZE 2048
#define WEAR_LEVELING_LOGICAL_SIZE 1024
// QMK keys the NUC123 value on QMK_MCU_FAMILY_NUC123, but this MCU builds as family NUMICRO:
// the ChibiOS EFL driver programs 32-bit words (NUC123_PAGE_SIZE).
#define BACKING_STORE_WRITE_SIZE 4
// Programmable APROM: 64 KB. The 68 KB part keeps 4 KB of data flash at 0x1F000 (CONFIG0 DFVSEN), and the
// ChibiOS EFL descriptor can report a larger size at boot (it reads CONFIG0 through ISP while ISP is off),
// which put the store at 0x10800, past the end of APROM, where every write failed silently.
// With 0x10000 the store is always the last 2 KB of APROM: 0xF800-0xFFFF (firmware must stay below 0xF800).
#define WEAR_LEVELING_EFL_FLASH_SIZE 0x10000

// Effects exposed by the protocol (keyboard.json already enables breathing, cycle_all,
// cycle_left_right, rainbow_moving_chevron and band_sat).
#define RGB_MATRIX_KEYPRESSES
#define RGB_MATRIX_FRAMEBUFFER_EFFECTS
#define ENABLE_RGB_MATRIX_GRADIENT_LEFT_RIGHT
#define ENABLE_RGB_MATRIX_HUE_WAVE
#define ENABLE_RGB_MATRIX_PIXEL_RAIN
#define ENABLE_RGB_MATRIX_DIGITAL_RAIN
#define ENABLE_RGB_MATRIX_SOLID_REACTIVE_SIMPLE
#define ENABLE_RGB_MATRIX_SPLASH
#define ENABLE_RGB_MATRIX_MULTISPLASH
#define ENABLE_RGB_MATRIX_TYPING_HEATMAP
#define RGB_MATRIX_DEFAULT_MODE RGB_MATRIX_CYCLE_LEFT_RIGHT

// #define HOSTRGB_DEBUG // enables raw HID command 0x7E (flash diagnostics), see hostrgb_protocol.c
