#include "../../include/capture/xcomposite.h"
#include "../../include/window_texture.h"
#include "../../include/utils.h"
#include "../../include/cursor.h"
#include "../../include/color_conversion.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xdamage.h>

#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>

typedef struct {
    gsr_capture_xcomposite_params params;
    XEvent xev;

    bool should_stop;
    bool stop_is_error;
    bool window_resized;
    bool follow_focused_initialized;

    Window window;
    vec2i window_size;
    vec2i texture_size;
    double window_resize_timer;
    
    WindowTexture window_texture;

    Atom net_active_window_atom;

    gsr_cursor cursor;

    int damage_event;
    int damage_error;
    XID damage;
    bool damaged;

    bool clear_background;
} gsr_capture_xcomposite;

static void gsr_capture_xcomposite_stop(gsr_capture_xcomposite *self) {
    if(self->damage) {
        XDamageDestroy(self->params.egl->x11.dpy, self->damage);
        self->damage = None;
    }

    window_texture_deinit(&self->window_texture);
    gsr_cursor_deinit(&self->cursor);
}

static int max_int(int a, int b) {
    return a > b ? a : b;
}

static Window get_focused_window(Display *display, Atom net_active_window_atom) {
    Atom type;
    int format = 0;
    unsigned long num_items = 0;
    unsigned long bytes_after = 0;
    unsigned char *properties = NULL;
    if(XGetWindowProperty(display, DefaultRootWindow(display), net_active_window_atom, 0, 1024, False, AnyPropertyType, &type, &format, &num_items, &bytes_after, &properties) == Success && properties) {
        Window focused_window = *(unsigned long*)properties;
        XFree(properties);
        return focused_window;
    }
    return None;
}

static void gsr_capture_xcomposite_setup_damage(gsr_capture_xcomposite *self, Window window) {
    if(self->damage_event == 0)
        return;

    if(self->damage) {
        XDamageDestroy(self->params.egl->x11.dpy, self->damage);
        self->damage = None;
    }

    self->damage = XDamageCreate(self->params.egl->x11.dpy, window, XDamageReportNonEmpty);
    if(self->damage) {
        XDamageSubtract(self->params.egl->x11.dpy, self->damage, None, None);
    } else {
        fprintf(stderr, "gsr warning: gsr_capture_xcomposite_setup_damage: XDamageCreate failed\n");
    }
}

static int gsr_capture_xcomposite_start(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame *frame) {
    gsr_capture_xcomposite *self = cap->priv;

    if(self->params.follow_focused) {
        self->net_active_window_atom = XInternAtom(self->params.egl->x11.dpy, "_NET_ACTIVE_WINDOW", False);
        if(!self->net_active_window_atom) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_start failed: failed to get _NET_ACTIVE_WINDOW atom\n");
            return -1;
        }
        self->window = get_focused_window(self->params.egl->x11.dpy, self->net_active_window_atom);
    } else {
        self->window = self->params.window;
    }

    if(self->params.track_damage) {
        if(!XDamageQueryExtension(self->params.egl->x11.dpy, &self->damage_event, &self->damage_error)) {
            fprintf(stderr, "gsr warning: gsr_capture_xcomposite_start: XDamage is not supported by your X11 server\n");
            self->damage_event = 0;
            self->damage_error = 0;
        }
    } else {
        self->damage_event = 0;
        self->damage_error = 0;
    }

    self->damaged = true;
    gsr_capture_xcomposite_setup_damage(self, self->window);

    /* TODO: Do these in tick, and allow error if follow_focused */

    XWindowAttributes attr;
    if(!XGetWindowAttributes(self->params.egl->x11.dpy, self->window, &attr) && !self->params.follow_focused) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_start failed: invalid window id: %lu\n", self->window);
        return -1;
    }

    self->window_size.x = max_int(attr.width, 0);
    self->window_size.y = max_int(attr.height, 0);

    if(self->params.follow_focused)
        XSelectInput(self->params.egl->x11.dpy, DefaultRootWindow(self->params.egl->x11.dpy), PropertyChangeMask);

    // TODO: Get select and add these on top of it and then restore at the end. Also do the same in other xcomposite
    XSelectInput(self->params.egl->x11.dpy, self->window, StructureNotifyMask | ExposureMask);

    if(!self->params.egl->eglExportDMABUFImageQueryMESA) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_start: could not find eglExportDMABUFImageQueryMESA\n");
        return -1;
    }

    if(!self->params.egl->eglExportDMABUFImageMESA) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_start: could not find eglExportDMABUFImageMESA\n");
        return -1;
    }

    /* Disable vsync */
    self->params.egl->eglSwapInterval(self->params.egl->egl_display, 0);
    if(window_texture_init(&self->window_texture, self->params.egl->x11.dpy, self->window, self->params.egl) != 0 && !self->params.follow_focused) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_start: failed to get window texture for window %ld\n", self->window);
        return -1;
    }

    if(gsr_cursor_init(&self->cursor, self->params.egl, self->params.egl->x11.dpy) != 0) {
        gsr_capture_xcomposite_stop(self);
        return -1;
    }

    self->texture_size.x = 0;
    self->texture_size.y = 0;

    self->params.egl->glBindTexture(GL_TEXTURE_2D, window_texture_get_opengl_texture_id(&self->window_texture));
    self->params.egl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &self->texture_size.x);
    self->params.egl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &self->texture_size.y);
    self->params.egl->glBindTexture(GL_TEXTURE_2D, 0);

    vec2i video_size = self->texture_size;

    if(self->params.region_size.x > 0 && self->params.region_size.y > 0)
        video_size = self->params.region_size;

    video_codec_context->width = FFALIGN(video_size.x, 2);
    video_codec_context->height = FFALIGN(video_size.y, 2);

    frame->width = video_codec_context->width;
    frame->height = video_codec_context->height;

    self->window_resize_timer = clock_get_monotonic_seconds();
    return 0;
}

static void gsr_capture_xcomposite_tick(gsr_capture *cap, AVCodecContext *video_codec_context) {
    (void)video_codec_context;
    gsr_capture_xcomposite *self = cap->priv;

    bool init_new_window = false;
    while(XPending(self->params.egl->x11.dpy)) {
        XNextEvent(self->params.egl->x11.dpy, &self->xev);

        switch(self->xev.type) {
            case DestroyNotify: {
                /* Window died (when not following focused window), so we stop recording */
                if(!self->params.follow_focused && self->xev.xdestroywindow.window == self->window) {
                    self->should_stop = true;
                    self->stop_is_error = false;
                }
                break;
            }
            case Expose: {
                /* Requires window texture recreate */
                if(self->xev.xexpose.count == 0 && self->xev.xexpose.window == self->window) {
                    self->window_resize_timer = clock_get_monotonic_seconds();
                    self->window_resized = true;
                }
                break;
            }
            case ConfigureNotify: {
                /* Window resized */
                if(self->xev.xconfigure.window == self->window && (self->xev.xconfigure.width != self->window_size.x || self->xev.xconfigure.height != self->window_size.y)) {
                    self->window_size.x = max_int(self->xev.xconfigure.width, 0);
                    self->window_size.y = max_int(self->xev.xconfigure.height, 0);
                    self->window_resize_timer = clock_get_monotonic_seconds();
                    self->window_resized = true;
                }
                break;
            }
            case PropertyNotify: {
                /* Focused window changed */
                if(self->params.follow_focused && self->xev.xproperty.atom == self->net_active_window_atom) {
                    init_new_window = true;
                }
                break;
            }
        }

        if(self->damage_event && self->xev.type == self->damage_event + XDamageNotify) {
            XDamageNotifyEvent *de = (XDamageNotifyEvent*)&self->xev;
            XserverRegion region = XFixesCreateRegion(self->params.egl->x11.dpy, NULL, 0);
            // Subtract all the damage, repairing the window
            XDamageSubtract(self->params.egl->x11.dpy, de->damage, None, region);
            XFixesDestroyRegion(self->params.egl->x11.dpy, region);
            self->damaged = true;
        }

        if(gsr_cursor_update(&self->cursor, &self->xev)) {
            if(self->params.record_cursor && self->cursor.visible) {
                self->damaged = true;
            }
        }
    }

    if(self->params.follow_focused && !self->follow_focused_initialized) {
        init_new_window = true;
    }

    if(init_new_window) {
        Window focused_window = get_focused_window(self->params.egl->x11.dpy, self->net_active_window_atom);
        if(focused_window != self->window || !self->follow_focused_initialized) {
            self->follow_focused_initialized = true;
            XSelectInput(self->params.egl->x11.dpy, self->window, 0);
            self->window = focused_window;
            XSelectInput(self->params.egl->x11.dpy, self->window, StructureNotifyMask | ExposureMask);

            XWindowAttributes attr;
            attr.width = 0;
            attr.height = 0;
            if(!XGetWindowAttributes(self->params.egl->x11.dpy, self->window, &attr))
                fprintf(stderr, "gsr error: gsr_capture_xcomposite_tick failed: invalid window id: %lu\n", self->window);

            self->window_size.x = max_int(attr.width, 0);
            self->window_size.y = max_int(attr.height, 0);
            self->window_resized = true;

            window_texture_deinit(&self->window_texture);
            window_texture_init(&self->window_texture, self->params.egl->x11.dpy, self->window, self->params.egl); // TODO: Do not do the below window_texture_on_resize after this
            gsr_capture_xcomposite_setup_damage(self, self->window);
        }
    }

    const double window_resize_timeout = 1.0; // 1 second
    if(self->window_resized && clock_get_monotonic_seconds() - self->window_resize_timer >= window_resize_timeout) {
        self->window_resized = false;

        if(window_texture_on_resize(&self->window_texture) != 0) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_tick: window_texture_on_resize failed\n");
            //self->should_stop = true;
            //self->stop_is_error = true;
            return;
        }

        self->texture_size.x = 0;
        self->texture_size.y = 0;

        self->params.egl->glBindTexture(GL_TEXTURE_2D, window_texture_get_opengl_texture_id(&self->window_texture));
        self->params.egl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &self->texture_size.x);
        self->params.egl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &self->texture_size.y);
        self->params.egl->glBindTexture(GL_TEXTURE_2D, 0);

        self->clear_background = true;
        gsr_capture_xcomposite_setup_damage(self, self->window);
    }
}

static bool gsr_capture_xcomposite_is_damaged(gsr_capture *cap) {
    gsr_capture_xcomposite *self = cap->priv;
    return self->damage_event ? self->damaged : true;
}

static void gsr_capture_xcomposite_clear_damage(gsr_capture *cap) {
    gsr_capture_xcomposite *self = cap->priv;
    self->damaged = false;
}

static bool gsr_capture_xcomposite_should_stop(gsr_capture *cap, bool *err) {
    gsr_capture_xcomposite *self = cap->priv;
    if(self->should_stop) {
        if(err)
            *err = self->stop_is_error;
        return true;
    }

    if(err)
        *err = false;
    return false;
}

static int gsr_capture_xcomposite_capture(gsr_capture *cap, AVFrame *frame, gsr_color_conversion *color_conversion) {
    gsr_capture_xcomposite *self = cap->priv;
    (void)frame;

    //self->params.egl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    self->params.egl->glClear(0);

    if(self->clear_background) {
        self->clear_background = false;
        gsr_color_conversion_clear(color_conversion);
    }

    const int target_x = max_int(0, frame->width / 2 - self->texture_size.x / 2);
    const int target_y = max_int(0, frame->height / 2 - self->texture_size.y / 2);

    const vec2i cursor_pos = {
        target_x + self->cursor.position.x - self->cursor.hotspot.x,
        target_y + self->cursor.position.y - self->cursor.hotspot.y
    };

    gsr_color_conversion_draw(color_conversion, window_texture_get_opengl_texture_id(&self->window_texture),
        (vec2i){target_x, target_y}, self->texture_size,
        (vec2i){0, 0}, self->texture_size,
        0.0f, false);

    if(self->params.record_cursor && self->cursor.visible) {
        gsr_cursor_tick(&self->cursor, self->window);

        const bool cursor_inside_window =
            cursor_pos.x + self->cursor.size.x >= target_x &&
            cursor_pos.x <= target_x + self->texture_size.x &&
            cursor_pos.y + self->cursor.size.y >= target_y &&
            cursor_pos.y <= target_y + self->texture_size.y;

        if(cursor_inside_window) {
            self->params.egl->glEnable(GL_SCISSOR_TEST);
            self->params.egl->glScissor(target_x, target_y, self->texture_size.x, self->texture_size.y);

            gsr_color_conversion_draw(color_conversion, self->cursor.texture_id,
                cursor_pos, self->cursor.size,
                (vec2i){0, 0}, self->cursor.size,
                0.0f, false);

            self->params.egl->glDisable(GL_SCISSOR_TEST);
        }
    }

    self->params.egl->eglSwapBuffers(self->params.egl->egl_display, self->params.egl->egl_surface);

    // TODO: Do video encoder specific conversion here

    //self->params.egl->glFlush();
    //self->params.egl->glFinish();

    return 0;
}

static gsr_source_color gsr_capture_xcomposite_get_source_color(gsr_capture *cap) {
    (void)cap;
    return GSR_SOURCE_COLOR_RGB;
}

static void gsr_capture_xcomposite_destroy(gsr_capture *cap, AVCodecContext *video_codec_context) {
    (void)video_codec_context;
    if(cap->priv) {
        gsr_capture_xcomposite_stop(cap->priv);
        free(cap->priv);
        cap->priv = NULL;
    }
    free(cap);
}

gsr_capture* gsr_capture_xcomposite_create(const gsr_capture_xcomposite_params *params) {
    if(!params) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_create params is NULL\n");
        return NULL;
    }

    gsr_capture *cap = calloc(1, sizeof(gsr_capture));
    if(!cap)
        return NULL;

    gsr_capture_xcomposite *cap_xcomp = calloc(1, sizeof(gsr_capture_xcomposite));
    if(!cap_xcomp) {
        free(cap);
        return NULL;
    }

    cap_xcomp->params = *params;
    
    *cap = (gsr_capture) {
        .start = gsr_capture_xcomposite_start,
        .tick = gsr_capture_xcomposite_tick,
        .is_damaged = gsr_capture_xcomposite_is_damaged,
        .clear_damage = gsr_capture_xcomposite_clear_damage,
        .should_stop = gsr_capture_xcomposite_should_stop,
        .capture = gsr_capture_xcomposite_capture,
        .capture_end = NULL,
        .get_source_color = gsr_capture_xcomposite_get_source_color,
        .uses_external_image = NULL,
        .destroy = gsr_capture_xcomposite_destroy,
        .priv = cap_xcomp
    };

    return cap;
}
