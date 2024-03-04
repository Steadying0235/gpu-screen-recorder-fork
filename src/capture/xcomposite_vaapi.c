#include "../../include/capture/xcomposite_vaapi.h"
#include "../../include/window_texture.h"
#include "../../include/utils.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <X11/Xlib.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>
#include <va/va.h>
#include <va/va_drmcommon.h>

typedef struct {
    gsr_capture_base base;
    gsr_capture_xcomposite_vaapi_params params;
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

    VADisplay va_dpy;
    VADRMPRIMESurfaceDescriptor prime;

    Atom net_active_window_atom;
} gsr_capture_xcomposite_vaapi;

static int max_int(int a, int b) {
    return a > b ? a : b;
}

static int min_int(int a, int b) {
    return a < b ? a : b;
}

static void gsr_capture_xcomposite_vaapi_stop(gsr_capture *cap, AVCodecContext *video_codec_context);

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

static bool drm_create_codec_context(gsr_capture_xcomposite_vaapi *cap_xcomp, AVCodecContext *video_codec_context) {
    char render_path[128];
    if(!gsr_card_path_get_render_path(cap_xcomp->params.egl->card_path, render_path)) {
        fprintf(stderr, "gsr error: failed to get /dev/dri/renderDXXX file from %s\n", cap_xcomp->params.egl->card_path);
        return false;
    }

    AVBufferRef *device_ctx;
    if(av_hwdevice_ctx_create(&device_ctx, AV_HWDEVICE_TYPE_VAAPI, render_path, NULL, 0) < 0) {
        fprintf(stderr, "Error: Failed to create hardware device context\n");
        return false;
    }

    AVBufferRef *frame_context = av_hwframe_ctx_alloc(device_ctx);
    if(!frame_context) {
        fprintf(stderr, "Error: Failed to create hwframe context\n");
        av_buffer_unref(&device_ctx);
        return false;
    }

    AVHWFramesContext *hw_frame_context =
        (AVHWFramesContext *)frame_context->data;
    hw_frame_context->width = video_codec_context->width;
    hw_frame_context->height = video_codec_context->height;
    hw_frame_context->sw_format = AV_PIX_FMT_NV12;//AV_PIX_FMT_0RGB32;//AV_PIX_FMT_YUV420P;//AV_PIX_FMT_0RGB32;//AV_PIX_FMT_NV12;
    hw_frame_context->format = video_codec_context->pix_fmt;
    hw_frame_context->device_ref = device_ctx;
    hw_frame_context->device_ctx = (AVHWDeviceContext*)device_ctx->data;

    hw_frame_context->initial_pool_size = 20;

    AVVAAPIDeviceContext *vactx =((AVHWDeviceContext*)device_ctx->data)->hwctx;
    cap_xcomp->va_dpy = vactx->display;

    if (av_hwframe_ctx_init(frame_context) < 0) {
        fprintf(stderr, "Error: Failed to initialize hardware frame context "
                        "(note: ffmpeg version needs to be > 4.0)\n");
        av_buffer_unref(&device_ctx);
        //av_buffer_unref(&frame_context);
        return false;
    }

    video_codec_context->hw_device_ctx = av_buffer_ref(device_ctx);
    video_codec_context->hw_frames_ctx = av_buffer_ref(frame_context);
    return true;
}

#define DRM_FORMAT_MOD_INVALID 0xffffffffffffffULL

static int gsr_capture_xcomposite_vaapi_start(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame *frame) {
    gsr_capture_xcomposite_vaapi *cap_xcomp = cap->priv;

    cap_xcomp->base.video_codec_context = video_codec_context;

    if(cap_xcomp->params.follow_focused) {
        cap_xcomp->net_active_window_atom = XInternAtom(cap_xcomp->params.egl->x11.dpy, "_NET_ACTIVE_WINDOW", False);
        if(!cap_xcomp->net_active_window_atom) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_start failed: failed to get _NET_ACTIVE_WINDOW atom\n");
            return -1;
        }
        cap_xcomp->window = get_focused_window(cap_xcomp->params.egl->x11.dpy, cap_xcomp->net_active_window_atom);
    } else {
        cap_xcomp->window = cap_xcomp->params.window;
    }

    /* TODO: Do these in tick, and allow error if follow_focused */

    XWindowAttributes attr;
    if(!XGetWindowAttributes(cap_xcomp->params.egl->x11.dpy, cap_xcomp->params.window, &attr) && !cap_xcomp->params.follow_focused) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_start failed: invalid window id: %lu\n", cap_xcomp->params.window);
        return -1;
    }

    cap_xcomp->window_size.x = max_int(attr.width, 0);
    cap_xcomp->window_size.y = max_int(attr.height, 0);

    if(cap_xcomp->params.follow_focused)
        XSelectInput(cap_xcomp->params.egl->x11.dpy, DefaultRootWindow(cap_xcomp->params.egl->x11.dpy), PropertyChangeMask);

    // TODO: Get select and add these on top of it and then restore at the end. Also do the same in other xcomposite
    XSelectInput(cap_xcomp->params.egl->x11.dpy, cap_xcomp->params.window, StructureNotifyMask | ExposureMask);

    if(!cap_xcomp->params.egl->eglExportDMABUFImageQueryMESA) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_start: could not find eglExportDMABUFImageQueryMESA\n");
        return -1;
    }

    if(!cap_xcomp->params.egl->eglExportDMABUFImageMESA) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_start: could not find eglExportDMABUFImageMESA\n");
        return -1;
    }

    /* Disable vsync */
    cap_xcomp->params.egl->eglSwapInterval(cap_xcomp->params.egl->egl_display, 0);
    if(window_texture_init(&cap_xcomp->window_texture, cap_xcomp->params.egl->x11.dpy, cap_xcomp->params.window, cap_xcomp->params.egl) != 0 && !cap_xcomp->params.follow_focused) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_start: failed to get window texture for window %ld\n", cap_xcomp->params.window);
        return -1;
    }

    cap_xcomp->texture_size.x = 0;
    cap_xcomp->texture_size.y = 0;

    cap_xcomp->params.egl->glBindTexture(GL_TEXTURE_2D, window_texture_get_opengl_texture_id(&cap_xcomp->window_texture));
    cap_xcomp->params.egl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &cap_xcomp->texture_size.x);
    cap_xcomp->params.egl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &cap_xcomp->texture_size.y);
    cap_xcomp->params.egl->glBindTexture(GL_TEXTURE_2D, 0);

    cap_xcomp->texture_size.x = max_int(2, even_number_ceil(cap_xcomp->texture_size.x));
    cap_xcomp->texture_size.y = max_int(2, even_number_ceil(cap_xcomp->texture_size.y));

    video_codec_context->width = cap_xcomp->texture_size.x;
    video_codec_context->height = cap_xcomp->texture_size.y;

    if(cap_xcomp->params.region_size.x > 0 && cap_xcomp->params.region_size.y > 0) {
        video_codec_context->width = max_int(2, even_number_ceil(cap_xcomp->params.region_size.x));
        video_codec_context->height = max_int(2, even_number_ceil(cap_xcomp->params.region_size.y));
    }

    if(!drm_create_codec_context(cap_xcomp, video_codec_context)) {
        gsr_capture_xcomposite_vaapi_stop(cap, video_codec_context);
        return -1;
    }

    if(!gsr_capture_base_setup_vaapi_textures(&cap_xcomp->base, frame, cap_xcomp->params.egl, cap_xcomp->va_dpy, &cap_xcomp->prime, cap_xcomp->params.color_range)) {
        gsr_capture_xcomposite_vaapi_stop(cap, video_codec_context);
        return -1;
    }

    cap_xcomp->window_resize_timer = clock_get_monotonic_seconds();
    return 0;
}

static void gsr_capture_xcomposite_vaapi_tick(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_xcomposite_vaapi *cap_xcomp = cap->priv;

    //cap_xcomp->params.egl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    cap_xcomp->params.egl->glClear(0);

    bool init_new_window = false;
    while(XPending(cap_xcomp->params.egl->x11.dpy)) {
        XNextEvent(cap_xcomp->params.egl->x11.dpy, &cap_xcomp->xev);

        switch(cap_xcomp->xev.type) {
            case DestroyNotify: {
                /* Window died (when not following focused window), so we stop recording */
                if(!cap_xcomp->params.follow_focused && cap_xcomp->xev.xdestroywindow.window == cap_xcomp->window) {
                    cap_xcomp->should_stop = true;
                    cap_xcomp->stop_is_error = false;
                }
                break;
            }
            case Expose: {
                /* Requires window texture recreate */
                if(cap_xcomp->xev.xexpose.count == 0 && cap_xcomp->xev.xexpose.window == cap_xcomp->window) {
                    cap_xcomp->window_resize_timer = clock_get_monotonic_seconds();
                    cap_xcomp->window_resized = true;
                }
                break;
            }
            case ConfigureNotify: {
                /* Window resized */
                if(cap_xcomp->xev.xconfigure.window == cap_xcomp->window && (cap_xcomp->xev.xconfigure.width != cap_xcomp->window_size.x || cap_xcomp->xev.xconfigure.height != cap_xcomp->window_size.y)) {
                    cap_xcomp->window_size.x = max_int(cap_xcomp->xev.xconfigure.width, 0);
                    cap_xcomp->window_size.y = max_int(cap_xcomp->xev.xconfigure.height, 0);
                    cap_xcomp->window_resize_timer = clock_get_monotonic_seconds();
                    cap_xcomp->window_resized = true;
                }
                break;
            }
            case PropertyNotify: {
                /* Focused window changed */
                if(cap_xcomp->params.follow_focused && cap_xcomp->xev.xproperty.atom == cap_xcomp->net_active_window_atom) {
                    init_new_window = true;
                }
                break;
            }
        }
    }

    if(cap_xcomp->params.follow_focused && !cap_xcomp->follow_focused_initialized) {
        init_new_window = true;
    }

    if(init_new_window) {
        Window focused_window = get_focused_window(cap_xcomp->params.egl->x11.dpy, cap_xcomp->net_active_window_atom);
        if(focused_window != cap_xcomp->window || !cap_xcomp->follow_focused_initialized) {
            cap_xcomp->follow_focused_initialized = true;
            XSelectInput(cap_xcomp->params.egl->x11.dpy, cap_xcomp->window, 0);
            cap_xcomp->window = focused_window;
            XSelectInput(cap_xcomp->params.egl->x11.dpy, cap_xcomp->window, StructureNotifyMask | ExposureMask);

            XWindowAttributes attr;
            attr.width = 0;
            attr.height = 0;
            if(!XGetWindowAttributes(cap_xcomp->params.egl->x11.dpy, cap_xcomp->window, &attr))
                fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_tick failed: invalid window id: %lu\n", cap_xcomp->window);

            cap_xcomp->window_size.x = max_int(attr.width, 0);
            cap_xcomp->window_size.y = max_int(attr.height, 0);
            cap_xcomp->window_resized = true;

            window_texture_deinit(&cap_xcomp->window_texture);
            window_texture_init(&cap_xcomp->window_texture, cap_xcomp->params.egl->x11.dpy, cap_xcomp->window, cap_xcomp->params.egl); // TODO: Do not do the below window_texture_on_resize after this
            
            cap_xcomp->texture_size.x = 0;
            cap_xcomp->texture_size.y = 0;

            cap_xcomp->params.egl->glBindTexture(GL_TEXTURE_2D, window_texture_get_opengl_texture_id(&cap_xcomp->window_texture));
            cap_xcomp->params.egl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &cap_xcomp->texture_size.x);
            cap_xcomp->params.egl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &cap_xcomp->texture_size.y);
            cap_xcomp->params.egl->glBindTexture(GL_TEXTURE_2D, 0);

            cap_xcomp->texture_size.x = min_int(video_codec_context->width, max_int(2, even_number_ceil(cap_xcomp->texture_size.x)));
            cap_xcomp->texture_size.y = min_int(video_codec_context->height, max_int(2, even_number_ceil(cap_xcomp->texture_size.y)));
        }
    }

    const double window_resize_timeout = 1.0; // 1 second
    if(cap_xcomp->window_resized && clock_get_monotonic_seconds() - cap_xcomp->window_resize_timer >= window_resize_timeout) {
        cap_xcomp->window_resized = false;

        if(window_texture_on_resize(&cap_xcomp->window_texture) != 0) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_tick: window_texture_on_resize failed\n");
            //cap_xcomp->should_stop = true;
            //cap_xcomp->stop_is_error = true;
            return;
        }

        cap_xcomp->texture_size.x = 0;
        cap_xcomp->texture_size.y = 0;

        cap_xcomp->params.egl->glBindTexture(GL_TEXTURE_2D, window_texture_get_opengl_texture_id(&cap_xcomp->window_texture));
        cap_xcomp->params.egl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &cap_xcomp->texture_size.x);
        cap_xcomp->params.egl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &cap_xcomp->texture_size.y);
        cap_xcomp->params.egl->glBindTexture(GL_TEXTURE_2D, 0);

        cap_xcomp->texture_size.x = min_int(video_codec_context->width, max_int(2, even_number_ceil(cap_xcomp->texture_size.x)));
        cap_xcomp->texture_size.y = min_int(video_codec_context->height, max_int(2, even_number_ceil(cap_xcomp->texture_size.y)));

        gsr_color_conversion_clear(&cap_xcomp->base.color_conversion);
    }
}

static bool gsr_capture_xcomposite_vaapi_should_stop(gsr_capture *cap, bool *err) {
    gsr_capture_xcomposite_vaapi *cap_xcomp = cap->priv;
    if(cap_xcomp->should_stop) {
        if(err)
            *err = cap_xcomp->stop_is_error;
        return true;
    }

    if(err)
        *err = false;
    return false;
}

static int gsr_capture_xcomposite_vaapi_capture(gsr_capture *cap, AVFrame *frame) {
    (void)frame;
    gsr_capture_xcomposite_vaapi *cap_xcomp = cap->priv;

    const int target_x = max_int(0, frame->width / 2 - cap_xcomp->texture_size.x / 2);
    const int target_y = max_int(0, frame->height / 2 - cap_xcomp->texture_size.y / 2);

    gsr_color_conversion_draw(&cap_xcomp->base.color_conversion, window_texture_get_opengl_texture_id(&cap_xcomp->window_texture),
        (vec2i){target_x, target_y}, cap_xcomp->texture_size,
        (vec2i){0, 0}, cap_xcomp->texture_size,
        0.0f, false);

    cap_xcomp->params.egl->eglSwapBuffers(cap_xcomp->params.egl->egl_display, cap_xcomp->params.egl->egl_surface);
    //cap_xcomp->params.egl->glFlush();
    //cap_xcomp->params.egl->glFinish();

    return 0;
}

static void gsr_capture_xcomposite_vaapi_stop(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_xcomposite_vaapi *cap_xcomp = cap->priv;

    for(uint32_t i = 0; i < cap_xcomp->prime.num_objects; ++i) {
        if(cap_xcomp->prime.objects[i].fd > 0) {
            close(cap_xcomp->prime.objects[i].fd);
            cap_xcomp->prime.objects[i].fd = 0;
        }
    }

    window_texture_deinit(&cap_xcomp->window_texture);

    if(video_codec_context->hw_device_ctx)
        av_buffer_unref(&video_codec_context->hw_device_ctx);
    if(video_codec_context->hw_frames_ctx)
        av_buffer_unref(&video_codec_context->hw_frames_ctx);

    gsr_capture_base_stop(&cap_xcomp->base, cap_xcomp->params.egl);
}

static void gsr_capture_xcomposite_vaapi_destroy(gsr_capture *cap, AVCodecContext *video_codec_context) {
    (void)video_codec_context;
    if(cap->priv) {
        gsr_capture_xcomposite_vaapi_stop(cap, video_codec_context);
        free(cap->priv);
        cap->priv = NULL;
    }
    free(cap);
}

gsr_capture* gsr_capture_xcomposite_vaapi_create(const gsr_capture_xcomposite_vaapi_params *params) {
    if(!params) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_create params is NULL\n");
        return NULL;
    }

    gsr_capture *cap = calloc(1, sizeof(gsr_capture));
    if(!cap)
        return NULL;

    gsr_capture_xcomposite_vaapi *cap_xcomp = calloc(1, sizeof(gsr_capture_xcomposite_vaapi));
    if(!cap_xcomp) {
        free(cap);
        return NULL;
    }

    cap_xcomp->params = *params;
    
    *cap = (gsr_capture) {
        .start = gsr_capture_xcomposite_vaapi_start,
        .tick = gsr_capture_xcomposite_vaapi_tick,
        .should_stop = gsr_capture_xcomposite_vaapi_should_stop,
        .capture = gsr_capture_xcomposite_vaapi_capture,
        .capture_end = NULL,
        .destroy = gsr_capture_xcomposite_vaapi_destroy,
        .priv = cap_xcomp
    };

    return cap;
}
