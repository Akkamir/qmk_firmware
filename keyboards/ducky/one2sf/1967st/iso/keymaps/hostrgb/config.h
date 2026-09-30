// Copyright 2019 /u/KeepItUnder
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// place overrides here
#define GRAVE_ESC_GUI_OVERRIDE
#define MK_3_SPEED
#define MK_C_OFFSET_UNMOD   400    /* Cursor offset per movement (unmodified) */
#define MK_C_INTERVAL_UNMOD  5     /* Time between cursor movements (unmodified) */
#define MK_W_OFFSET_UNMOD   100    /* Scroll steps per scroll action (unmodified) */
#define MK_W_INTERVAL_UNMOD 10     /* Time between scroll steps (unmodified) */

// The NUC123 USB LLD gives each logical endpoint separate IN and OUT hardware buffers,
// so raw HID can use one endpoint number for both directions.
#define USB_ENDPOINTS_ARE_REORDERABLE
