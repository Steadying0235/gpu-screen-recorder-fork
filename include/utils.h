#ifndef GSR_UTILS_H
#define GSR_UTILS_H

#include "vec2.h"
#include <stdbool.h>
#include <X11/extensions/Xrandr.h>

typedef struct {
    vec2i pos;
    vec2i size;
} gsr_monitor;

typedef struct {
    const char *name;
    int name_len;
    gsr_monitor *monitor;
    bool found_monitor;
} get_monitor_by_name_userdata;

double clock_get_monotonic_seconds(void);

typedef void (*active_monitor_callback)(const XRROutputInfo *output_info, const XRRCrtcInfo *crt_info, const XRRModeInfo *mode_info, void *userdata);
void for_each_active_monitor_output(Display *display, active_monitor_callback callback, void *userdata);
bool get_monitor_by_name(Display *display, const char *name, gsr_monitor *monitor);

#endif /* GSR_UTILS_H */
