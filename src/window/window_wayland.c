#include "../../include/window/window_wayland.h"

#include "../../include/vec2.h"
#include "../../include/defs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#define GSR_MAX_OUTPUTS 32

typedef struct {
    uint32_t wl_name;
    void *output;
    vec2i pos;
    vec2i size;
    int32_t transform;
    char *name;
} gsr_wayland_output;

typedef struct {
    void *display;
    void *window;
    void *registry;
    void *surface;
    void *compositor;
    gsr_wayland_output outputs[GSR_MAX_OUTPUTS];
    int num_outputs;
} gsr_window_wayland;

static void output_handle_geometry(void *data, struct wl_output *wl_output,
        int32_t x, int32_t y, int32_t phys_width, int32_t phys_height,
        int32_t subpixel, const char *make, const char *model,
        int32_t transform) {
    (void)wl_output;
    (void)phys_width;
    (void)phys_height;
    (void)subpixel;
    (void)make;
    (void)model;
    gsr_wayland_output *gsr_output = data;
    gsr_output->pos.x = x;
    gsr_output->pos.y = y;
    gsr_output->transform = transform;
}

static void output_handle_mode(void *data, struct wl_output *wl_output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    (void)wl_output;
    (void)flags;
    (void)refresh;
    gsr_wayland_output *gsr_output = data;
    gsr_output->size.x = width;
    gsr_output->size.y = height;
}

static void output_handle_done(void *data, struct wl_output *wl_output) {
    (void)data;
    (void)wl_output;
}

static void output_handle_scale(void* data, struct wl_output *wl_output, int32_t factor) {
    (void)data;
    (void)wl_output;
    (void)factor;
}

static void output_handle_name(void *data, struct wl_output *wl_output, const char *name) {
    (void)wl_output;
    gsr_wayland_output *gsr_output = data;
    if(gsr_output->name) {
        free(gsr_output->name);
        gsr_output->name = NULL;
    }
    gsr_output->name = strdup(name);
}

static void output_handle_description(void *data, struct wl_output *wl_output, const char *description) {
    (void)data;
    (void)wl_output;
    (void)description;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_handle_geometry,
    .mode = output_handle_mode,
    .done = output_handle_done,
    .scale = output_handle_scale,
    .name = output_handle_name,
    .description = output_handle_description,
};

static void registry_add_object(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    (void)version;
    gsr_window_wayland *window_wayland = data;
    if (strcmp(interface, "wl_compositor") == 0) {
        if(window_wayland->compositor) {
            wl_compositor_destroy(window_wayland->compositor);
            window_wayland->compositor = NULL;
        }
        window_wayland->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    } else if(strcmp(interface, wl_output_interface.name) == 0) {
        if(version < 4) {
            fprintf(stderr, "gsr warning: wl output interface version is < 4, expected >= 4 to capture a monitor. Using KMS capture instead\n");
            return;
        }

        if(window_wayland->num_outputs == GSR_MAX_OUTPUTS) {
            fprintf(stderr, "gsr warning: reached maximum outputs (%d), ignoring output %u\n", GSR_MAX_OUTPUTS, name);
            return;
        }

        gsr_wayland_output *gsr_output = &window_wayland->outputs[window_wayland->num_outputs];
        window_wayland->num_outputs++;
        *gsr_output = (gsr_wayland_output) {
            .wl_name = name,
            .output = wl_registry_bind(registry, name, &wl_output_interface, 4),
            .pos = { .x = 0, .y = 0 },
            .size = { .x = 0, .y = 0 },
            .transform = 0,
            .name = NULL,
        };
        wl_output_add_listener(gsr_output->output, &output_listener, gsr_output);
    }
}

static void registry_remove_object(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static struct wl_registry_listener registry_listener = {
    .global = registry_add_object,
    .global_remove = registry_remove_object,
};

static void gsr_window_wayland_deinit(gsr_window_wayland *self) {
    if(self->window) {
        wl_egl_window_destroy(self->window);
        self->window = NULL;
    }

    if(self->surface) {
        wl_surface_destroy(self->surface);
        self->surface = NULL;
    }

    for(int i = 0; i < self->num_outputs; ++i) {
        if(self->outputs[i].output) {
            wl_output_destroy(self->outputs[i].output);
            self->outputs[i].output = NULL;
        }

        if(self->outputs[i].name) {
            free(self->outputs[i].name);
            self->outputs[i].name = NULL;
        }
    }
    self->num_outputs = 0;

    if(self->compositor) {
        wl_compositor_destroy(self->compositor);
        self->compositor = NULL;
    }

    if(self->registry) {
        wl_registry_destroy(self->registry);
        self->registry = NULL;
    }

    if(self->display) {
        wl_display_disconnect(self->display);
        self->display = NULL;
    }
}

static bool gsr_window_wayland_init(gsr_window_wayland *self) {
    self->display = wl_display_connect(NULL);
    if(!self->display) {
        fprintf(stderr, "gsr error: gsr_window_wayland_init failed: failed to connect to the Wayland server\n");
        goto fail;
    }

    self->registry = wl_display_get_registry(self->display); // TODO: Error checking
    wl_registry_add_listener(self->registry, &registry_listener, self); // TODO: Error checking

    // Fetch globals
    wl_display_roundtrip(self->display);

    // Fetch wl_output
    wl_display_roundtrip(self->display);

    if(!self->compositor) {
        fprintf(stderr, "gsr error: gsr_window_wayland_init failed: failed to find compositor\n");
        goto fail;
    }

    self->surface = wl_compositor_create_surface(self->compositor);
    if(!self->surface) {
        fprintf(stderr, "gsr error: gsr_window_wayland_init failed: failed to create surface\n");
        goto fail;
    }

    self->window = wl_egl_window_create(self->surface, 16, 16);
    if(!self->window) {
        fprintf(stderr, "gsr error: gsr_window_wayland_init failed: failed to create window\n");
        goto fail;
    }

    fail:
    gsr_window_wayland_deinit(self);
    return false;
}

static void gsr_window_wayland_destroy(gsr_window *window) {
    gsr_window_wayland *self = window->priv;
    gsr_window_wayland_deinit(self);
    free(self);
    free(window);
}

static bool gsr_window_wayland_process_event(gsr_window *window) {
    gsr_window_wayland *self = window->priv;
    // TODO: pselect on wl_display_get_fd before doing dispatch
    const bool events_available = wl_display_dispatch_pending(self->display) > 0;
    wl_display_flush(self->display);
    return events_available;
}

static gsr_display_server gsr_wayland_get_display_server(void) {
    return GSR_DISPLAY_SERVER_WAYLAND;
}

static void* gsr_window_wayland_get_display(gsr_window *window) {
    gsr_window_wayland *self = window->priv;
    return self->display;
}

static void* gsr_window_wayland_get_window(gsr_window *window) {
    gsr_window_wayland *self = window->priv;
    return self->window;
}

static gsr_monitor_rotation wayland_transform_to_gsr_rotation(int32_t rot) {
    switch(rot) {
        case 0: return GSR_MONITOR_ROT_0;
        case 1: return GSR_MONITOR_ROT_90;
        case 2: return GSR_MONITOR_ROT_180;
        case 3: return GSR_MONITOR_ROT_270;
    }
    return GSR_MONITOR_ROT_0;
}

static void gsr_window_wayland_for_each_active_monitor_output_cached(const gsr_window *window, active_monitor_callback callback, void *userdata) {
    const gsr_window_wayland *self = window->priv;
    drm_connector_type_count type_counts[CONNECTOR_TYPE_COUNTS];
    int num_type_counts = 0;

    for(int i = 0; i < self->num_outputs; ++i) {
        const gsr_wayland_output *output = &self->outputs[i];
        if(!output->name)
            continue;

        const int connector_type_index = get_connector_type_by_name(output->name);
        drm_connector_type_count *connector_type = NULL;
        if(connector_type_index != -1)
            connector_type = drm_connector_types_get_index(type_counts, &num_type_counts, connector_type_index);
        
        if(connector_type) {
            ++connector_type->count;
            ++connector_type->count_active;
        }

        const gsr_monitor monitor = {
            .name = output->name,
            .name_len = strlen(output->name),
            .pos = { .x = output->pos.x, .y = output->pos.y },
            .size = { .x = output->size.x, .y = output->size.y },
            .connector_id = 0,
            .rotation = wayland_transform_to_gsr_rotation(output->transform),
            .monitor_identifier = connector_type ? monitor_identifier_from_type_and_count(connector_type_index, connector_type->count_active) : 0
        };
        callback(&monitor, userdata);
    }
}

gsr_window* gsr_window_wayland_create(void) {
    gsr_window *window = calloc(1, sizeof(gsr_window));
    if(!window)
        return window;

    gsr_window_wayland *window_wayland = calloc(1, sizeof(gsr_window_wayland));
    if(!window_wayland) {
        free(window);
        return NULL;
    }

    if(!gsr_window_wayland_init(window_wayland)) {
        free(window_wayland);
        free(window);
        return NULL;
    }

    *window = (gsr_window) {
        .destroy = gsr_window_wayland_destroy,
        .process_event = gsr_window_wayland_process_event,
        .get_event_data = NULL,
        .get_display_server = gsr_wayland_get_display_server,
        .get_display = gsr_window_wayland_get_display,
        .get_window = gsr_window_wayland_get_window,
        .for_each_active_monitor_output_cached = gsr_window_wayland_for_each_active_monitor_output_cached,
        .priv = window_wayland
    };

    return window;
}
