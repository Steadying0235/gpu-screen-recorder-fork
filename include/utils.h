#ifndef GSR_UTILS_H
#define GSR_UTILS_H

#include "vec2.h"
#include <stdbool.h>
#include <X11/extensions/Xrandr.h>

typedef enum {
    GSR_GPU_VENDOR_AMD,
    GSR_GPU_VENDOR_INTEL,
    GSR_GPU_VENDOR_NVIDIA
} gsr_gpu_vendor;

typedef struct {
    gsr_gpu_vendor vendor;
    int gpu_version; /* 0 if unknown */
} gsr_gpu_info;

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

bool gl_get_gpu_info(Display *dpy, gsr_gpu_info *info);

#endif /* GSR_UTILS_H */
