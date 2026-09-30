// Copyright 2026 Akkamir
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Loads the saved overlay; call from keyboard_post_init_user.
void hostrgb_init(void);

// Writes an empty overlay; call from eeconfig_init_user (first boot or EEPROM reset).
void hostrgb_reset_storage(void);
