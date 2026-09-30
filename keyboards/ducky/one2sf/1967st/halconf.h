// Copyright 2026 Akkamir
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#define HAL_USE_PWM TRUE // GCLK for the MBI5043 LED drivers
#define HAL_USE_GPT TRUE // LED row refresh timer

#include_next <halconf.h>
