#ifndef GSR_UTILS_H
#define GSR_UTILS_H

#include "vec2.h"
#include "../include/egl.h"
#include "../include/defs.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *name;
    int name_len;
    vec2i pos;
    vec2i size;
    uint32_t connector_id; /* Only on x11 and drm */
    gsr_monitor_rotation rotation; /* Only on x11 and wayland */
    uint32_t monitor_identifier; /* On x11 this is the crtc id */
} gsr_monitor;

typedef struct {
    const char *name;
    int name_len;
    gsr_monitor *monitor;
    bool found_monitor;
} get_monitor_by_name_userdata;

double clock_get_monotonic_seconds(void);

typedef void (*active_monitor_callback)(const gsr_monitor *monitor, void *userdata);
void for_each_active_monitor_output_x11_not_cached(Display *display, active_monitor_callback callback, void *userdata);
void for_each_active_monitor_output_x11(const gsr_egl *egl, active_monitor_callback callback, void *userdata);
void for_each_active_monitor_output(const gsr_egl *egl, gsr_connection_type connection_type, active_monitor_callback callback, void *userdata);
bool get_monitor_by_name(const gsr_egl *egl, gsr_connection_type connection_type, const char *name, gsr_monitor *monitor);
gsr_monitor_rotation drm_monitor_get_display_server_rotation(const gsr_egl *egl, const gsr_monitor *monitor);

bool gl_get_gpu_info(gsr_egl *egl, gsr_gpu_info *info);

/* |output| should be at least 128 bytes in size */
bool gsr_get_valid_card_path(gsr_egl *egl, char *output, bool is_monitor_capture);
/* |render_path| should be at least 128 bytes in size */
bool gsr_card_path_get_render_path(const char *card_path, char *render_path);

int create_directory_recursive(char *path);

/* |img_attr| needs to be at least 44 in size */
void setup_dma_buf_attrs(intptr_t *img_attr, uint32_t format, uint32_t width, uint32_t height, const int *fds, const uint32_t *offsets, const uint32_t *pitches, const uint64_t *modifiers, int num_planes, bool use_modifier);

#endif /* GSR_UTILS_H */
