#include "../../include/capture/xcomposite.h"
#include "../../include/window_texture.h"
#include "../../include/utils.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xdamage.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext.h>
#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>
#include <va/va.h>
#include <va/va_drmcommon.h>

static int max_int(int a, int b) {
    return a > b ? a : b;
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

    if(self->params.egl->gpu_info.vendor == GSR_GPU_VENDOR_AMD && video_codec_context->codec_id == AV_CODEC_ID_HEVC) {
        // TODO: dont do this if using ffmpeg reports that this is not needed (AMD driver bug that was fixed recently)
        video_codec_context->width = FFALIGN(video_size.x, 64);
        video_codec_context->height = FFALIGN(video_size.y, 16);
    } else if(self->params.egl->gpu_info.vendor == GSR_GPU_VENDOR_AMD && video_codec_context->codec_id == AV_CODEC_ID_AV1) {
        // TODO: Dont do this for VCN 5 and forward which should fix this hardware bug
        video_codec_context->width = FFALIGN(video_size.x, 64);
        // AMD driver has special case handling for 1080 height to set it to 1082 instead of 1088 (1080 aligned to 16).
        // TODO: Set height to 1082 in this case, but it wont work because it will be aligned to 1088.
        if(video_size.y == 1080) {
            video_codec_context->height = 1080;
        } else {
            video_codec_context->height = FFALIGN(video_size.y, 16);
        }
    } else {
        video_codec_context->width = FFALIGN(video_size.x, 2);
        video_codec_context->height = FFALIGN(video_size.y, 2);
    }

    frame->width = video_codec_context->width;
    frame->height = video_codec_context->height;

    self->window_resize_timer = clock_get_monotonic_seconds();
    return 0;
}

void gsr_capture_xcomposite_stop(gsr_capture_xcomposite *self) {
    if(self->damage) {
        XDamageDestroy(self->params.egl->x11.dpy, self->damage);
        self->damage = None;
    }

    window_texture_deinit(&self->window_texture);
    gsr_cursor_deinit(&self->cursor);
    gsr_capture_base_stop(&self->base);
}

void gsr_capture_xcomposite_tick(gsr_capture_xcomposite *self, AVCodecContext *video_codec_context) {
    (void)video_codec_context;
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

        gsr_color_conversion_clear(&self->base.color_conversion);
        gsr_capture_xcomposite_setup_damage(self, self->window);
    }
}

bool gsr_capture_xcomposite_is_damaged(gsr_capture_xcomposite *self) {
    return self->damage_event ? self->damaged : true;
}

void gsr_capture_xcomposite_clear_damage(gsr_capture_xcomposite *self) {
    self->damaged = false;
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

    const vec2i cursor_pos = {
        target_x + self->cursor.position.x - self->cursor.hotspot.x,
        target_y + self->cursor.position.y - self->cursor.hotspot.y
    };

    gsr_color_conversion_draw(&self->base.color_conversion, window_texture_get_opengl_texture_id(&self->window_texture),
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
            self->base.egl->glEnable(GL_SCISSOR_TEST);
            self->base.egl->glScissor(target_x, target_y, self->texture_size.x, self->texture_size.y);

            gsr_color_conversion_draw(&self->base.color_conversion, self->cursor.texture_id,
                cursor_pos, self->cursor.size,
                (vec2i){0, 0}, self->cursor.size,
                0.0f, false);

            self->base.egl->glDisable(GL_SCISSOR_TEST);
        }
    }

    self->params.egl->eglSwapBuffers(self->params.egl->egl_display, self->params.egl->egl_surface);
    //self->params.egl->glFlush();
    //self->params.egl->glFinish();

    return 0;
}
