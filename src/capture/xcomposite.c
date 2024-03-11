#include "../../include/capture/xcomposite.h"
#include "../../include/window_texture.h"
#include "../../include/utils.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <X11/Xlib.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext.h>
#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>
#include <va/va.h>
#include <va/va_drmcommon.h>

static int max_int(int a, int b) {
    return a > b ? a : b;
}

static int min_int(int a, int b) {
    return a < b ? a : b;
}

void gsr_capture_xcomposite_init(gsr_capture_xcomposite *self, const gsr_capture_xcomposite_params *params) {
    memset(self, 0, sizeof(*self));
    self->params = *params;
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

int gsr_capture_xcomposite_start(gsr_capture_xcomposite *self, AVCodecContext *video_codec_context, AVFrame *frame) {
    self->base.video_codec_context = video_codec_context;
    self->base.egl = self->params.egl;

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

    self->texture_size.x = max_int(2, even_number_ceil(self->texture_size.x));
    self->texture_size.y = max_int(2, even_number_ceil(self->texture_size.y));

    video_codec_context->width = self->texture_size.x;
    video_codec_context->height = self->texture_size.y;

    if(self->params.region_size.x > 0 && self->params.region_size.y > 0) {
        video_codec_context->width = max_int(2, even_number_ceil(self->params.region_size.x));
        video_codec_context->height = max_int(2, even_number_ceil(self->params.region_size.y));
    }

    frame->width = video_codec_context->width;
    frame->height = video_codec_context->height;

    self->window_resize_timer = clock_get_monotonic_seconds();
    self->clear_next_frame = true;
    return 0;
}

void gsr_capture_xcomposite_stop(gsr_capture_xcomposite *self) {
    window_texture_deinit(&self->window_texture);
    gsr_cursor_deinit(&self->cursor);
    gsr_capture_base_stop(&self->base);
}

void gsr_capture_xcomposite_tick(gsr_capture_xcomposite *self, AVCodecContext *video_codec_context) {
    //self->params.egl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    self->params.egl->glClear(0);

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

        gsr_cursor_update(&self->cursor, &self->xev);
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

        self->texture_size.x = min_int(video_codec_context->width, max_int(2, even_number_ceil(self->texture_size.x)));
        self->texture_size.y = min_int(video_codec_context->height, max_int(2, even_number_ceil(self->texture_size.y)));

        gsr_color_conversion_clear(&self->base.color_conversion);
    }
}

bool gsr_capture_xcomposite_should_stop(gsr_capture_xcomposite *self, bool *err) {
    if(self->should_stop) {
        if(err)
            *err = self->stop_is_error;
        return true;
    }

    if(err)
        *err = false;
    return false;
}

int gsr_capture_xcomposite_capture(gsr_capture_xcomposite *self, AVFrame *frame) {
    (void)frame;

    const int target_x = max_int(0, frame->width / 2 - self->texture_size.x / 2);
    const int target_y = max_int(0, frame->height / 2 - self->texture_size.y / 2);

    // TODO: Can we do this a better way than to call it every capture?
    if(self->params.record_cursor)
        gsr_cursor_tick(&self->cursor, self->window);

    const vec2i cursor_pos = {
        target_x + self->cursor.position.x - self->cursor.hotspot.x,
        target_y + self->cursor.position.y - self->cursor.hotspot.y
    };

    const bool cursor_completely_inside_window =
        cursor_pos.x >= target_x &&
        cursor_pos.x <= target_x + self->texture_size.x &&
        cursor_pos.y >= target_y &&
        cursor_pos.y <= target_y + self->texture_size.x;

    const bool cursor_inside_window =
        cursor_pos.x + self->cursor.size.x >= target_x &&
        cursor_pos.x <= target_x + self->texture_size.x &&
        cursor_pos.y + self->cursor.size.y >= target_y &&
        cursor_pos.y <= target_y + self->texture_size.x;

    if(self->clear_next_frame) {
        self->clear_next_frame = false;
        gsr_color_conversion_clear(&self->base.color_conversion);
    }

    /*
        We dont draw the cursor if it's outside the window but if it's partially inside the window then the cursor area that is outside the window
        will not get overdrawn the next frame causing a cursor trail to be visible since we dont clear the background.
        To fix this we detect if the cursor is partially inside the window and clear the background only in that case.
    */
    if(!cursor_completely_inside_window && cursor_inside_window && self->params.record_cursor)
        self->clear_next_frame = true;

    gsr_color_conversion_draw(&self->base.color_conversion, window_texture_get_opengl_texture_id(&self->window_texture),
        (vec2i){target_x, target_y}, self->texture_size,
        (vec2i){0, 0}, self->texture_size,
        0.0f, false);

    if(cursor_inside_window && self->params.record_cursor) {
        gsr_color_conversion_draw(&self->base.color_conversion, self->cursor.texture_id,
            cursor_pos, self->cursor.size,
            (vec2i){0, 0}, self->cursor.size,
            0.0f, false);
    }

    self->params.egl->eglSwapBuffers(self->params.egl->egl_display, self->params.egl->egl_surface);
    //self->params.egl->glFlush();
    //self->params.egl->glFinish();

    return 0;
}
