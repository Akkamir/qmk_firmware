// Copyright 2026 Akkamir
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Loads the saved overlay; call from keyboard_post_init_user.
void hostrgb_init(void);

// Writes an empty overlay; call from eeconfig_init_user (first boot or EEPROM reset).
void hostrgb_reset_storage(void);

// Tells the host that the agent key `slot` (0 = Delete, 1 = Page Up, 2 = Page Down) was pressed with Fn.
void hostrgb_agent_key(uint8_t slot);
