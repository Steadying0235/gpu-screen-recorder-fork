#ifndef GSR_PIPEWIRE_H
#define GSR_PIPEWIRE_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include <spa/utils/hook.h>
#include <spa/param/video/format.h>

typedef struct gsr_egl gsr_egl;

typedef struct {
    int major;
    int minor;
    int micro;
} gsr_pipewire_data_version;

typedef struct {
    uint32_t fps_num;
    uint32_t fps_den;
} gsr_pipewire_video_info;

typedef struct {
    int fd;
    uint32_t offset;
    int32_t stride;
} gsr_pipewire_dmabuf_data;

typedef struct {
    int x, y;
    int width, height;
} gsr_pipewire_region;

typedef struct {
    gsr_egl *egl;
    int fd;
    uint32_t node;
    pthread_mutex_t mutex;
    bool mutex_initialized;

    struct pw_thread_loop *thread_loop;
    struct pw_context *context;
    struct pw_core *core;
    struct spa_hook core_listener;
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    struct spa_source *reneg;
    struct spa_video_info format;
    int server_version_sync;
    bool negotiated;

    struct {
        bool visible;
        bool valid;
        uint8_t *data;
        int x, y;
        int hotspot_x, hotspot_y;
        int width, height;
    } cursor;

    struct {
        bool valid;
        int x, y;
        uint32_t width, height;
    } crop;

    gsr_pipewire_data_version server_version;
    gsr_pipewire_video_info video_info;
    gsr_pipewire_dmabuf_data dmabuf_data;
} gsr_pipewire;

/*
    |capture_cursor| only applies to when capturing a window or region.
    In other cases |pipewire_node|'s setup will determine if the cursor is included.
    Note that the cursor is not guaranteed to be shown even if set to true, it depends on the wayland compositor.
*/
bool gsr_pipewire_init(gsr_pipewire *self, int pipewire_fd, uint32_t pipewire_node, int fps, bool capture_cursor, gsr_egl *egl);
void gsr_pipewire_deinit(gsr_pipewire *self);

bool gsr_pipewire_map_texture(gsr_pipewire *self, unsigned int texture_id, unsigned int cursor_texture_id, gsr_pipewire_region *region, gsr_pipewire_region *cursor_region, int *plane_fd);

#endif /* GSR_PIPEWIRE_H */
