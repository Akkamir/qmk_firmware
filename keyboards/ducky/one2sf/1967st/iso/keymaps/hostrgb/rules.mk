RAW_ENABLE = yes
KEYBOARD_SHARED_EP = yes # NUC123 (AN) exposes only 2 endpoints: keyboard shares one so raw HID gets the other
KEY_OVERRIDE_ENABLE = yes
SRC += hostrgb_protocol.c
EEPROM_DRIVER = wear_leveling
WEAR_LEVELING_DRIVER = embedded_flash
