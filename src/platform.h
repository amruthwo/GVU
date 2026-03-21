#pragma once

typedef enum {
    PLATFORM_A30,
    PLATFORM_FLIP,
    PLATFORM_MIYOO_MINI,
    PLATFORM_BRICK,
    PLATFORM_SMART_PRO,
    PLATFORM_SMART_PRO_S,
    PLATFORM_UNKNOWN
} Platform;

Platform detect_platform(void);
Platform get_platform(void);
const char *platform_name(Platform p);
