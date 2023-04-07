#include "../include/utils.h"
#include <time.h>
#include <string.h>

double clock_get_monotonic_seconds(void) {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 0.000000001;
}

static const XRRModeInfo* get_mode_info(const XRRScreenResources *sr, RRMode id) {
    for(int i = 0; i < sr->nmode; ++i) {
        if(sr->modes[i].id == id)
            return &sr->modes[i];
    }    
    return NULL;
}

void for_each_active_monitor_output(Display *display, active_monitor_callback callback, void *userdata) {
    XRRScreenResources *screen_res = XRRGetScreenResources(display, DefaultRootWindow(display));
    if(!screen_res)
        return;

    for(int i = 0; i < screen_res->noutput; ++i) {
        XRROutputInfo *out_info = XRRGetOutputInfo(display, screen_res, screen_res->outputs[i]);
        if(out_info && out_info->crtc && out_info->connection == RR_Connected) {
            XRRCrtcInfo *crt_info = XRRGetCrtcInfo(display, screen_res, out_info->crtc);
            if(crt_info && crt_info->mode) {
                const XRRModeInfo *mode_info = get_mode_info(screen_res, crt_info->mode);
                if(mode_info)
                    callback(out_info, crt_info, mode_info, userdata);
            }
            if(crt_info)
                XRRFreeCrtcInfo(crt_info);
        }
        if(out_info)
            XRRFreeOutputInfo(out_info);
    }    

    XRRFreeScreenResources(screen_res);
}

static void get_monitor_by_name_callback(const XRROutputInfo *output_info, const XRRCrtcInfo *crt_info, const XRRModeInfo *mode_info, void *userdata) {
    get_monitor_by_name_userdata *data = (get_monitor_by_name_userdata*)userdata;
    if(!data->found_monitor && data->name_len == output_info->nameLen && memcmp(data->name, output_info->name, data->name_len) == 0) {
        data->monitor->pos = (vec2i){ .x = crt_info->x, .y = crt_info->y };
        data->monitor->size = (vec2i){ .x = (int)crt_info->width, .y = (int)crt_info->height };
        data->found_monitor = true;
    }
}

bool get_monitor_by_name(Display *display, const char *name, gsr_monitor *monitor) {
    get_monitor_by_name_userdata userdata;
    userdata.name = name;
    userdata.name_len = strlen(name);
    userdata.monitor = monitor;
    userdata.found_monitor = false;
    for_each_active_monitor_output(display, get_monitor_by_name_callback, &userdata);
    return userdata.found_monitor;
}
