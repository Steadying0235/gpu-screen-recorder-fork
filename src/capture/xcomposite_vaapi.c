#include "../../include/capture/xcomposite_vaapi.h"
#include "../../include/egl.h"
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
    gsr_capture_xcomposite_vaapi_params params;
    Display *dpy;
    XEvent xev;
    bool should_stop;
    bool stop_is_error;
    bool window_resized;
    bool created_hw_frame;
    bool follow_focused_initialized;

    Window window;
    vec2i window_size;
    vec2i texture_size;
    double window_resize_timer;
    
    WindowTexture window_texture;

    gsr_egl egl;

    int fourcc;
    int num_planes;
    uint64_t modifiers;
    int dmabuf_fd;
    int32_t pitch;
    int32_t offset;

    VADisplay va_dpy;
    VAConfigID config_id;
    VAContextID context_id;
    VASurfaceID input_surface;
    VABufferID buffer_id;
    VARectangle output_region;

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
    AVBufferRef *device_ctx;
    if(av_hwdevice_ctx_create(&device_ctx, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128", NULL, 0) < 0) {
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

    hw_frame_context->initial_pool_size = 1;

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

#define DRM_FORMAT_MOD_INVALID 72057594037927935

static int gsr_capture_xcomposite_vaapi_start(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_xcomposite_vaapi *cap_xcomp = cap->priv;

    if(cap_xcomp->params.follow_focused) {
        cap_xcomp->net_active_window_atom = XInternAtom(cap_xcomp->dpy, "_NET_ACTIVE_WINDOW", False);
        if(!cap_xcomp->net_active_window_atom) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_start failed: failed to get _NET_ACTIVE_WINDOW atom\n");
            return -1;
        }
        cap_xcomp->window = get_focused_window(cap_xcomp->dpy, cap_xcomp->net_active_window_atom);
    } else {
        cap_xcomp->window = cap_xcomp->params.window;
    }

    /* TODO: Do these in tick, and allow error if follow_focused */

    XWindowAttributes attr;
    if(!XGetWindowAttributes(cap_xcomp->dpy, cap_xcomp->params.window, &attr) && !cap_xcomp->params.follow_focused) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_start failed: invalid window id: %lu\n", cap_xcomp->params.window);
        return -1;
    }

    cap_xcomp->window_size.x = max_int(attr.width, 0);
    cap_xcomp->window_size.y = max_int(attr.height, 0);

    if(cap_xcomp->params.follow_focused)
        XSelectInput(cap_xcomp->dpy, DefaultRootWindow(cap_xcomp->dpy), PropertyChangeMask);

    // TODO: Get select and add these on top of it and then restore at the end. Also do the same in other xcomposite
    XSelectInput(cap_xcomp->dpy, cap_xcomp->params.window, StructureNotifyMask | ExposureMask);

    if(!gsr_egl_load(&cap_xcomp->egl, cap_xcomp->dpy)) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_start: failed to load opengl\n");
        return -1;
    }

    if(!cap_xcomp->egl.eglExportDMABUFImageQueryMESA) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_start: could not find eglExportDMABUFImageQueryMESA\n");
        gsr_egl_unload(&cap_xcomp->egl);
        return -1;
    }

    if(!cap_xcomp->egl.eglExportDMABUFImageMESA) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_start: could not find eglExportDMABUFImageMESA\n");
        gsr_egl_unload(&cap_xcomp->egl);
        return -1;
    }

    /* Disable vsync */
    cap_xcomp->egl.eglSwapInterval(cap_xcomp->egl.egl_display, 0);
    if(window_texture_init(&cap_xcomp->window_texture, cap_xcomp->dpy, cap_xcomp->params.window, &cap_xcomp->egl) != 0 && !cap_xcomp->params.follow_focused) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_start: failed get window texture for window %ld\n", cap_xcomp->params.window);
        gsr_egl_unload(&cap_xcomp->egl);
        return -1;
    }

    cap_xcomp->texture_size.x = 0;
    cap_xcomp->texture_size.y = 0;

    cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, window_texture_get_opengl_texture_id(&cap_xcomp->window_texture));
    cap_xcomp->texture_size.x = 0;
    cap_xcomp->texture_size.y = 0;
    cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &cap_xcomp->texture_size.x);
    cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &cap_xcomp->texture_size.y);
    cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, 0);

    cap_xcomp->texture_size.x = max_int(2, cap_xcomp->texture_size.x & ~1);
    cap_xcomp->texture_size.y = max_int(2, cap_xcomp->texture_size.y & ~1);

    video_codec_context->width = cap_xcomp->texture_size.x;
    video_codec_context->height = cap_xcomp->texture_size.y;

    if(cap_xcomp->params.region_size.x > 0 && cap_xcomp->params.region_size.y > 0) {
        video_codec_context->width = max_int(2, cap_xcomp->params.region_size.x & ~1);
        video_codec_context->height = max_int(2, cap_xcomp->params.region_size.y & ~1);
    }

    if(!drm_create_codec_context(cap_xcomp, video_codec_context)) {
        gsr_capture_xcomposite_vaapi_stop(cap, video_codec_context);
        return -1;
    }

    cap_xcomp->window_resize_timer = clock_get_monotonic_seconds();
    return 0;
}

static void gsr_capture_xcomposite_vaapi_tick(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame **frame) {
    gsr_capture_xcomposite_vaapi *cap_xcomp = cap->priv;

    // TODO:
    //cap_xcomp->egl.glClear(GL_COLOR_BUFFER_BIT);

    if(!cap_xcomp->params.follow_focused && XCheckTypedWindowEvent(cap_xcomp->dpy, cap_xcomp->window, DestroyNotify, &cap_xcomp->xev)) {
        cap_xcomp->should_stop = true;
        cap_xcomp->stop_is_error = false;
    }

    if(XCheckTypedWindowEvent(cap_xcomp->dpy, cap_xcomp->window, Expose, &cap_xcomp->xev) && cap_xcomp->xev.xexpose.count == 0) {
        cap_xcomp->window_resize_timer = clock_get_monotonic_seconds();
        cap_xcomp->window_resized = true;
    }

    if(XCheckTypedWindowEvent(cap_xcomp->dpy, cap_xcomp->window, ConfigureNotify, &cap_xcomp->xev) && cap_xcomp->xev.xconfigure.window == cap_xcomp->window) {
        while(XCheckTypedWindowEvent(cap_xcomp->dpy, cap_xcomp->window, ConfigureNotify, &cap_xcomp->xev)) {}

        /* Window resize */
        if(cap_xcomp->xev.xconfigure.width != cap_xcomp->window_size.x || cap_xcomp->xev.xconfigure.height != cap_xcomp->window_size.y) {
            cap_xcomp->window_size.x = max_int(cap_xcomp->xev.xconfigure.width, 0);
            cap_xcomp->window_size.y = max_int(cap_xcomp->xev.xconfigure.height, 0);
            cap_xcomp->window_resize_timer = clock_get_monotonic_seconds();
            cap_xcomp->window_resized = true;
        }
    }

    if(cap_xcomp->params.follow_focused && (!cap_xcomp->follow_focused_initialized || (XCheckTypedWindowEvent(cap_xcomp->dpy, DefaultRootWindow(cap_xcomp->dpy), PropertyNotify, &cap_xcomp->xev) && cap_xcomp->xev.xproperty.atom == cap_xcomp->net_active_window_atom))) {
        Window focused_window = get_focused_window(cap_xcomp->dpy, cap_xcomp->net_active_window_atom);
        if(focused_window != cap_xcomp->window || !cap_xcomp->follow_focused_initialized) {
            cap_xcomp->follow_focused_initialized = true;
            XSelectInput(cap_xcomp->dpy, cap_xcomp->window, 0);
            cap_xcomp->window = focused_window;
            XSelectInput(cap_xcomp->dpy, cap_xcomp->window, StructureNotifyMask | ExposureMask);

            XWindowAttributes attr;
            attr.width = 0;
            attr.height = 0;
            if(!XGetWindowAttributes(cap_xcomp->dpy, cap_xcomp->window, &attr))
                fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_tick failed: invalid window id: %lu\n", cap_xcomp->window);

            cap_xcomp->window_size.x = max_int(attr.width, 0);
            cap_xcomp->window_size.y = max_int(attr.height, 0);
            cap_xcomp->window_resized = true;

            window_texture_deinit(&cap_xcomp->window_texture);
            window_texture_init(&cap_xcomp->window_texture, cap_xcomp->dpy, cap_xcomp->window, &cap_xcomp->egl); // TODO: Do not do the below window_texture_on_resize after this
            
            cap_xcomp->texture_size.x = 0;
            cap_xcomp->texture_size.y = 0;

            cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, window_texture_get_opengl_texture_id(&cap_xcomp->window_texture));
            cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &cap_xcomp->texture_size.x);
            cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &cap_xcomp->texture_size.y);
            cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, 0);

            cap_xcomp->texture_size.x = min_int(video_codec_context->width, max_int(2, cap_xcomp->texture_size.x & ~1));
            cap_xcomp->texture_size.y = min_int(video_codec_context->height, max_int(2, cap_xcomp->texture_size.y & ~1));
        }
    }

    const double window_resize_timeout = 1.0; // 1 second
    if(!cap_xcomp->created_hw_frame || (cap_xcomp->window_resized && clock_get_monotonic_seconds() - cap_xcomp->window_resize_timer >= window_resize_timeout)) {
        cap_xcomp->window_resized = false;

        if(window_texture_on_resize(&cap_xcomp->window_texture) != 0) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_tick: window_texture_on_resize failed\n");
            //cap_xcomp->should_stop = true;
            //cap_xcomp->stop_is_error = true;
            //return;
        }

        cap_xcomp->texture_size.x = 0;
        cap_xcomp->texture_size.y = 0;

        cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, window_texture_get_opengl_texture_id(&cap_xcomp->window_texture));
        cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &cap_xcomp->texture_size.x);
        cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &cap_xcomp->texture_size.y);
        cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, 0);

        cap_xcomp->texture_size.x = min_int(video_codec_context->width, max_int(2, cap_xcomp->texture_size.x & ~1));
        cap_xcomp->texture_size.y = min_int(video_codec_context->height, max_int(2, cap_xcomp->texture_size.y & ~1));

        if(cap_xcomp->buffer_id) {
            vaDestroyBuffer(cap_xcomp->va_dpy, cap_xcomp->buffer_id);
            cap_xcomp->buffer_id = 0;
        }

        if(cap_xcomp->context_id) {
            vaDestroyContext(cap_xcomp->va_dpy, cap_xcomp->context_id);
            cap_xcomp->context_id = 0;
        }

        if(cap_xcomp->config_id) {
            vaDestroyConfig(cap_xcomp->va_dpy, cap_xcomp->config_id);
            cap_xcomp->config_id = 0;
        }

        if(cap_xcomp->input_surface) {
            vaDestroySurfaces(cap_xcomp->va_dpy, &cap_xcomp->input_surface, 1);
            cap_xcomp->input_surface = 0;
        }

        if(cap_xcomp->dmabuf_fd) {
            close(cap_xcomp->dmabuf_fd);
            cap_xcomp->dmabuf_fd = 0;
        }

        if(!cap_xcomp->created_hw_frame) {
            cap_xcomp->created_hw_frame = true;
            av_frame_free(frame);
            *frame = av_frame_alloc();
            if(!frame) {
                fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_tick: failed to allocate frame\n");
                cap_xcomp->should_stop = true;
                cap_xcomp->stop_is_error = true;
                return;
            }
            (*frame)->format = video_codec_context->pix_fmt;
            (*frame)->width = video_codec_context->width;
            (*frame)->height = video_codec_context->height;
            (*frame)->color_range = video_codec_context->color_range;
            (*frame)->color_primaries = video_codec_context->color_primaries;
            (*frame)->color_trc = video_codec_context->color_trc;
            (*frame)->colorspace = video_codec_context->colorspace;
            (*frame)->chroma_location = video_codec_context->chroma_sample_location;

            int res = av_hwframe_get_buffer(video_codec_context->hw_frames_ctx, *frame, 0);
            if(res < 0) {
                fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_tick: av_hwframe_get_buffer failed: %d\n", res);
                cap_xcomp->should_stop = true;
                cap_xcomp->stop_is_error = true;
                return;
            }
        }

        int xx = 0, yy = 0;
        cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, window_texture_get_opengl_texture_id(&cap_xcomp->window_texture));
        cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &xx);
        cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &yy);
        cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, 0);

        const intptr_t pixmap_attrs[] = {
            EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
            EGL_NONE,
        };

        EGLImage img = cap_xcomp->egl.eglCreateImage(cap_xcomp->egl.egl_display, cap_xcomp->egl.egl_context, EGL_GL_TEXTURE_2D, (EGLClientBuffer)(uint64_t)window_texture_get_opengl_texture_id(&cap_xcomp->window_texture), pixmap_attrs);
        if(!img) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_tick: eglCreateImage failed\n");
            cap_xcomp->should_stop = true;
            cap_xcomp->stop_is_error = true;
            return;
        }

        if(!cap_xcomp->egl.eglExportDMABUFImageQueryMESA(cap_xcomp->egl.egl_display, img, &cap_xcomp->fourcc, &cap_xcomp->num_planes, &cap_xcomp->modifiers)) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_tick: eglExportDMABUFImageQueryMESA failed\n");
            cap_xcomp->should_stop = true;
            cap_xcomp->stop_is_error = true;
            cap_xcomp->egl.eglDestroyImage(cap_xcomp->egl.egl_display, img);
            return;
        }

        if(cap_xcomp->num_planes != 1) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_tick: expected 1 plane for drm buf, got %d planes\n", cap_xcomp->num_planes);
            cap_xcomp->should_stop = true;
            cap_xcomp->stop_is_error = true;
            cap_xcomp->egl.eglDestroyImage(cap_xcomp->egl.egl_display, img);
            return;
        }

        if(!cap_xcomp->egl.eglExportDMABUFImageMESA(cap_xcomp->egl.egl_display, img, &cap_xcomp->dmabuf_fd, &cap_xcomp->pitch, &cap_xcomp->offset)) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_tick: eglExportDMABUFImageMESA failed\n");
            cap_xcomp->should_stop = true;
            cap_xcomp->stop_is_error = true;
            cap_xcomp->egl.eglDestroyImage(cap_xcomp->egl.egl_display, img);
            return;
        }

        cap_xcomp->egl.eglDestroyImage(cap_xcomp->egl.egl_display, img);

        VADRMPRIMESurfaceDescriptor buf = {0};
        buf.fourcc = VA_FOURCC_XRGB;
        buf.width = xx;
        buf.height = yy;

        buf.num_objects = 0;
        buf.objects[0].fd = cap_xcomp->dmabuf_fd;
        buf.objects[0].size = yy * cap_xcomp->pitch;
        buf.objects[0].drm_format_modifier = cap_xcomp->modifiers;

        buf.num_layers = 1;
        buf.layers[0].drm_format = cap_xcomp->fourcc;
        buf.layers[0].num_planes = 1;
        buf.layers[0].object_index[0] = 0;
        buf.layers[0].offset[0] = cap_xcomp->offset;
        buf.layers[0].pitch[0] = cap_xcomp->pitch;

        VASurfaceAttrib attribs[2] = {0};
        attribs[0].type = VASurfaceAttribMemoryType;
        attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
        attribs[0].value.type = VAGenericValueTypeInteger;
        attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
        attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
        attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
        attribs[1].value.type = VAGenericValueTypePointer;
        attribs[1].value.value.p = &buf;
        
        VAStatus va_status = vaCreateSurfaces(cap_xcomp->va_dpy, VA_RT_FORMAT_RGB32, xx, yy, &cap_xcomp->input_surface, 1, attribs, 2);
        if(va_status != VA_STATUS_SUCCESS) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_tick: vaCreateSurfaces failed: %d\n", va_status);
            cap_xcomp->should_stop = true;
            cap_xcomp->stop_is_error = true;
            return;
        }

        //vaBeginPicture(cap_xcomp->va_dpy, )

        va_status = vaCreateConfig(cap_xcomp->va_dpy, VAProfileNone, VAEntrypointVideoProc, NULL, 0, &cap_xcomp->config_id);
        if(va_status != VA_STATUS_SUCCESS) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_tick: vaCreateConfig failed: %d\n", va_status);
            cap_xcomp->should_stop = true;
            cap_xcomp->stop_is_error = true;
            return;
        }

        VASurfaceID target_surface_id = (uintptr_t)(*frame)->data[3];
        va_status = vaCreateContext(cap_xcomp->va_dpy, cap_xcomp->config_id, xx, yy, VA_PROGRESSIVE, &target_surface_id, 1, &cap_xcomp->context_id);
        if(va_status != VA_STATUS_SUCCESS) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_tick: vaCreateContext failed: %d\n", va_status);
            cap_xcomp->should_stop = true;
            cap_xcomp->stop_is_error = true;
            return;
        }

        cap_xcomp->output_region = (VARectangle){
            .x = 0,
            .y = 0,
            .width = xx,
            .height = yy
        };

        // Copying a surface to another surface will automatically perform the color conversion. Thanks vaapi!
        VAProcPipelineParameterBuffer params = {0};
        params.surface = cap_xcomp->input_surface;
        params.surface_region = NULL;
        if(cap_xcomp->params.follow_focused)
            params.output_region = &cap_xcomp->output_region;
        else
            params.output_region = NULL;
        params.output_background_color = 0;
        params.filter_flags = VA_FRAME_PICTURE;
        // TODO: Colors
        params.input_color_properties.color_range = (*frame)->color_range == AVCOL_RANGE_JPEG ? VA_SOURCE_RANGE_FULL : VA_SOURCE_RANGE_REDUCED;
        params.output_color_properties.color_range = (*frame)->color_range == AVCOL_RANGE_JPEG ? VA_SOURCE_RANGE_FULL : VA_SOURCE_RANGE_REDUCED;

        va_status = vaCreateBuffer(cap_xcomp->va_dpy, cap_xcomp->context_id, VAProcPipelineParameterBufferType, sizeof(params), 1, &params, &cap_xcomp->buffer_id);
        if(va_status != VA_STATUS_SUCCESS) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_tick: vaCreateBuffer failed: %d\n", va_status);
            cap_xcomp->should_stop = true;
            cap_xcomp->stop_is_error = true;
            return;
        }

        // Clear texture with black background because the source texture (window_texture_get_opengl_texture_id(&cap_xcomp->window_texture))
        // might be smaller than cap_xcomp->target_texture_id
        // TODO:
        //cap_xcomp->egl.glClearTexImage(cap_xcomp->target_texture_id, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
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
    gsr_capture_xcomposite_vaapi *cap_xcomp = cap->priv;

    VASurfaceID target_surface_id = (uintptr_t)frame->data[3];

    VAStatus va_status = vaBeginPicture(cap_xcomp->va_dpy, cap_xcomp->context_id, target_surface_id);
    if(va_status != VA_STATUS_SUCCESS) {
        static bool error_printed = false;
        if(!error_printed) {
            error_printed = true;
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_tick: vaBeginPicture failed: %d\n", va_status);
        }
        return -1;
    }

    va_status = vaRenderPicture(cap_xcomp->va_dpy, cap_xcomp->context_id, &cap_xcomp->buffer_id, 1);
    if(va_status != VA_STATUS_SUCCESS) {
        vaEndPicture(cap_xcomp->va_dpy, cap_xcomp->context_id);
        static bool error_printed = false;
        if(!error_printed) {
            error_printed = true;
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_tick: vaRenderPicture failed: %d\n", va_status);
        }
        return -1;
    }

    va_status = vaEndPicture(cap_xcomp->va_dpy, cap_xcomp->context_id);
    if(va_status != VA_STATUS_SUCCESS) {
        static bool error_printed = false;
        if(!error_printed) {
            error_printed = true;
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_tick: vaEndPicture failed: %d\n", va_status);
        }
        return -1;
    }

    // TODO: Needed?
    //vaSyncSurface(cap_xcomp->va_dpy, target_surface_id);

    // TODO: Remove
    //cap_xcomp->egl.eglSwapBuffers(cap_xcomp->egl.egl_display, cap_xcomp->egl.egl_surface);

    return 0;
}

static void gsr_capture_xcomposite_vaapi_stop(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_xcomposite_vaapi *cap_xcomp = cap->priv;

    if(cap_xcomp->buffer_id) {
        vaDestroyBuffer(cap_xcomp->va_dpy, cap_xcomp->buffer_id);
        cap_xcomp->buffer_id = 0;
    }

    if(cap_xcomp->context_id) {
        vaDestroyContext(cap_xcomp->va_dpy, cap_xcomp->context_id);
        cap_xcomp->context_id = 0;
    }

    if(cap_xcomp->config_id) {
        vaDestroyConfig(cap_xcomp->va_dpy, cap_xcomp->config_id);
        cap_xcomp->config_id = 0;
    }

    if(cap_xcomp->input_surface) {
        vaDestroySurfaces(cap_xcomp->va_dpy, &cap_xcomp->input_surface, 1);
        cap_xcomp->input_surface = 0;
    }

    if(cap_xcomp->dmabuf_fd) {
        close(cap_xcomp->dmabuf_fd);
        cap_xcomp->dmabuf_fd = 0;
    }

    window_texture_deinit(&cap_xcomp->window_texture);

    if(video_codec_context->hw_device_ctx)
        av_buffer_unref(&video_codec_context->hw_device_ctx);
    if(video_codec_context->hw_frames_ctx)
        av_buffer_unref(&video_codec_context->hw_frames_ctx);

    gsr_egl_unload(&cap_xcomp->egl);
    if(cap_xcomp->dpy) {
        // TODO: This causes a crash, why? maybe some other library dlclose xlib and that also happened to unload this???
        //XCloseDisplay(cap_xcomp->dpy);
        cap_xcomp->dpy = NULL;
    }
}

static void gsr_capture_xcomposite_vaapi_destroy(gsr_capture *cap, AVCodecContext *video_codec_context) {
    (void)video_codec_context;
    gsr_capture_xcomposite_vaapi *cap_xcomp = cap->priv;
    if(cap->priv) {
        gsr_capture_xcomposite_vaapi_stop(cap, video_codec_context);
        free(cap->priv);
        cap->priv = NULL;
    }
    if(cap_xcomp->dpy) {
        // TODO: This causes a crash, why? maybe some other library dlclose xlib and that also happened to unload this???
        //XCloseDisplay(cap_xcomp->dpy);
        cap_xcomp->dpy = NULL;
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

    Display *display = XOpenDisplay(NULL);
    if(!display) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_create failed: XOpenDisplay failed\n");
        free(cap);
        free(cap_xcomp);
        return NULL;
    }

    cap_xcomp->dpy = display;
    cap_xcomp->params = *params;
    
    *cap = (gsr_capture) {
        .start = gsr_capture_xcomposite_vaapi_start,
        .tick = gsr_capture_xcomposite_vaapi_tick,
        .should_stop = gsr_capture_xcomposite_vaapi_should_stop,
        .capture = gsr_capture_xcomposite_vaapi_capture,
        .destroy = gsr_capture_xcomposite_vaapi_destroy,
        .priv = cap_xcomp
    };

    return cap;
}
