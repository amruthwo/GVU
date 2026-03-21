#include "platform.h"
#include <stdio.h>
#include <string.h>

static Platform g_platform = PLATFORM_UNKNOWN;
static int      g_detected  = 0;

Platform detect_platform(void) {
    if (g_detected) return g_platform;

    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) {
        g_platform = PLATFORM_UNKNOWN;
        g_detected = 1;
        return g_platform;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        /* Miyoo A30 — Allwinner H700 */
        if (strstr(line, "sun8i")) {
            g_platform = PLATFORM_A30;
            break;
        }
        /* Miyoo Flip V1/V2, GKD Pixel2 — RK3566 */
        if (strstr(line, "0xd05")) {
            /* Both report 0xd05; treat as FLIP for now.
               If Pixel2 needs separate handling, distinguish via /proc/cpuinfo model name. */
            g_platform = PLATFORM_FLIP;
            break;
        }
        /* TrimUI Brick / Hammer */
        if (strstr(line, "TG3040")) {
            g_platform = PLATFORM_BRICK;
            break;
        }
        /* TrimUI Smart Pro */
        if (strstr(line, "TG5040")) {
            g_platform = PLATFORM_SMART_PRO;
            break;
        }
        /* TrimUI Smart Pro S */
        if (strstr(line, "TG5050")) {
            g_platform = PLATFORM_SMART_PRO_S;
            break;
        }
        /* Miyoo Mini / Mini Flip — fallback; appears in Hardware field */
        if (strstr(line, "Allwinner") || strstr(line, "V3s")) {
            g_platform = PLATFORM_MIYOO_MINI;
            break;
        }
    }

    fclose(f);
    g_detected = 1;
    return g_platform;
}

Platform get_platform(void) {
    return detect_platform();
}

const char *platform_name(Platform p) {
    switch (p) {
        case PLATFORM_A30:         return "Miyoo A30";
        case PLATFORM_FLIP:        return "Miyoo Flip / GKD Pixel2";
        case PLATFORM_MIYOO_MINI:  return "Miyoo Mini / Mini Flip";
        case PLATFORM_BRICK:       return "TrimUI Brick";
        case PLATFORM_SMART_PRO:   return "TrimUI Smart Pro";
        case PLATFORM_SMART_PRO_S: return "TrimUI Smart Pro S";
        default:                   return "Unknown";
    }
}
