#include "../../include/capture/kms_vaapi.h"
#include "../../kms/client/kms_client.h"
#include "../../include/egl.h"
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
    gsr_capture_kms_vaapi_params params;
    bool should_stop;
    bool stop_is_error;
    bool created_hw_frame;

    gsr_egl egl;
    
    gsr_kms_client kms_client;

    uint32_t fourcc;
    uint64_t modifiers;
    int dmabuf_fd;
    uint32_t pitch;
    uint32_t offset;
    vec2i kms_size;

    vec2i capture_pos;
    vec2i capture_size;
    bool screen_capture;

    VADisplay va_dpy;
    VAConfigID config_id;
    VAContextID context_id;
    VASurfaceID input_surface;
    VABufferID buffer_id;
    VARectangle input_region;
    bool context_created;
} gsr_capture_kms_vaapi;

static int max_int(int a, int b) {
    return a > b ? a : b;
}

static void gsr_capture_kms_vaapi_stop(gsr_capture *cap, AVCodecContext *video_codec_context);

static bool drm_create_codec_context(gsr_capture_kms_vaapi *cap_kms, AVCodecContext *video_codec_context) {
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

    hw_frame_context->initial_pool_size = 1; // TODO: (and in other places)

    AVVAAPIDeviceContext *vactx =((AVHWDeviceContext*)device_ctx->data)->hwctx;
    cap_kms->va_dpy = vactx->display;

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

// TODO: On monitor reconfiguration, find monitor x, y, width and height again. Do the same for nvfbc.

static int gsr_capture_kms_vaapi_start(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_kms_vaapi *cap_kms = cap->priv;

    // TODO: Allow specifying another card, and in other places (TODO: Use /dev/dri/renderD128?)
    if(gsr_kms_client_init(&cap_kms->kms_client, "/dev/dri/card0") != 0) {
        return -1;
    }

    // TODO: Update on monitor reconfiguration, make sure to update on window focus change (maybe for kms update each second?), needs to work on focus change to the witcher 3 on full window, fullscreen firefox, etc
    gsr_monitor monitor;
    if(strcmp(cap_kms->params.display_to_capture, "screen") == 0) {
        monitor.pos.x = 0;
        monitor.pos.y = 0;
        monitor.size.x = XWidthOfScreen(DefaultScreenOfDisplay(cap_kms->params.dpy));
        monitor.size.y = XHeightOfScreen(DefaultScreenOfDisplay(cap_kms->params.dpy));
        cap_kms->screen_capture = true;
    } else if(!get_monitor_by_name(cap_kms->params.dpy, cap_kms->params.display_to_capture, &monitor)) {
        fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_start: failed to find monitor by name \"%s\"\n", cap_kms->params.display_to_capture);
        gsr_capture_kms_vaapi_stop(cap, video_codec_context);
        return -1;
    }

    cap_kms->capture_pos = monitor.pos;
    cap_kms->capture_size = monitor.size;


    if(!gsr_egl_load(&cap_kms->egl, cap_kms->params.dpy)) {
        fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_start: failed to load opengl\n");
        return -1;
    }

    /* Disable vsync */
    cap_kms->egl.eglSwapInterval(cap_kms->egl.egl_display, 0);

    video_codec_context->width = max_int(2, cap_kms->capture_size.x & ~1);
    video_codec_context->height = max_int(2, cap_kms->capture_size.y & ~1);

    if(!drm_create_codec_context(cap_kms, video_codec_context)) {
        gsr_capture_kms_vaapi_stop(cap, video_codec_context);
        return -1;
    }

    //cap_kms->window_resize_timer = clock_get_monotonic_seconds(); // TODO:
    return 0;
}

static void gsr_capture_kms_vaapi_tick(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame **frame) {
    gsr_capture_kms_vaapi *cap_kms = cap->priv;

    // TODO:
    //cap_kms->egl.glClear(GL_COLOR_BUFFER_BIT);

    //const double window_resize_timeout = 1.0; // 1 second
    if(!cap_kms->created_hw_frame) {
        cap_kms->created_hw_frame = true;

        av_frame_free(frame);
        *frame = av_frame_alloc();
        if(!frame) {
            fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_tick: failed to allocate frame\n");
            cap_kms->should_stop = true;
            cap_kms->stop_is_error = true;
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
            fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_tick: av_hwframe_get_buffer failed: %d\n", res);
            cap_kms->should_stop = true;
            cap_kms->stop_is_error = true;
            return;
        }
    }
}

static bool gsr_capture_kms_vaapi_should_stop(gsr_capture *cap, bool *err) {
    gsr_capture_kms_vaapi *cap_kms = cap->priv;
    if(cap_kms->should_stop) {
        if(err)
            *err = cap_kms->stop_is_error;
        return true;
    }

    if(err)
        *err = false;
    return false;
}

static int gsr_capture_kms_vaapi_capture(gsr_capture *cap, AVFrame *frame) {
    gsr_capture_kms_vaapi *cap_kms = cap->priv;

    VASurfaceID target_surface_id = (uintptr_t)frame->data[3];

    if(cap_kms->dmabuf_fd > 0) {
        close(cap_kms->dmabuf_fd);
        cap_kms->dmabuf_fd = 0;
    }

    gsr_kms_response kms_response;
    if(gsr_kms_client_get_kms(&cap_kms->kms_client, &kms_response) != 0) {
        fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_capture: failed to get kms, error: %d (%s)\n", kms_response.result, kms_response.data.err_msg);
        return -1;
    }

    cap_kms->dmabuf_fd = kms_response.data.fd.fd;
    cap_kms->pitch = kms_response.data.fd.pitch;
    cap_kms->offset = kms_response.data.fd.offset;
    cap_kms->fourcc = kms_response.data.fd.pixel_format;
    cap_kms->modifiers = kms_response.data.fd.modifier;
    cap_kms->kms_size.x = kms_response.data.fd.width;
    cap_kms->kms_size.y = kms_response.data.fd.height;

    if(!cap_kms->context_created) {
        cap_kms->context_created = true;

        VAStatus va_status = vaCreateConfig(cap_kms->va_dpy, VAProfileNone, VAEntrypointVideoProc, NULL, 0, &cap_kms->config_id);
        if(va_status != VA_STATUS_SUCCESS) {
            fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_capture: vaCreateConfig failed: %d\n", va_status);
            cap_kms->should_stop = true;
            cap_kms->stop_is_error = true;
            return -1;
        }

        va_status = vaCreateContext(cap_kms->va_dpy, cap_kms->config_id, cap_kms->kms_size.x, cap_kms->kms_size.y, VA_PROGRESSIVE, &target_surface_id, 1, &cap_kms->context_id);
        if(va_status != VA_STATUS_SUCCESS) {
            fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_capture: vaCreateContext failed: %d\n", va_status);
            cap_kms->should_stop = true;
            cap_kms->stop_is_error = true;
            return -1;
        }
    }

    if(cap_kms->buffer_id) {
        vaDestroyBuffer(cap_kms->va_dpy, cap_kms->buffer_id);
        cap_kms->buffer_id = 0;
    }

    if(cap_kms->input_surface) {
        vaDestroySurfaces(cap_kms->va_dpy, &cap_kms->input_surface, 1);
        cap_kms->input_surface = 0;
    }

    uintptr_t dmabuf = cap_kms->dmabuf_fd;

    VASurfaceAttribExternalBuffers buf = {0};
    buf.pixel_format = VA_FOURCC_BGRX;
    buf.width = cap_kms->kms_size.x;
    buf.height = cap_kms->kms_size.y;
    buf.data_size = cap_kms->kms_size.y * cap_kms->pitch;
    buf.num_planes = 1;
    buf.pitches[0] = cap_kms->pitch;
    buf.offsets[0] = cap_kms->offset;
    buf.buffers = &dmabuf;
    buf.num_buffers = 1;
    buf.flags = 0;
    buf.private_data = 0;

    #define VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME        0x20000000

    VASurfaceAttrib attribs[3] = {0};
    attribs[0].type = VASurfaceAttribMemoryType;
    attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[0].value.type = VAGenericValueTypeInteger;
    attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
    attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[1].value.type = VAGenericValueTypePointer;
    attribs[1].value.value.p = &buf;
    
    // TODO: Do we really need to create a new surface every frame?
    VAStatus va_status = vaCreateSurfaces(cap_kms->va_dpy, VA_RT_FORMAT_RGB32, cap_kms->kms_size.x, cap_kms->kms_size.y, &cap_kms->input_surface, 1, attribs, 2);
    if(va_status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_capture: vaCreateSurfaces failed: %d\n", va_status);
        cap_kms->should_stop = true;
        cap_kms->stop_is_error = true;
        return -1;
    }

    cap_kms->input_region = (VARectangle) {
        .x = cap_kms->capture_pos.x,
        .y = cap_kms->capture_pos.y,
        .width = cap_kms->capture_size.x,
        .height = cap_kms->capture_size.y
    };

    // Copying a surface to another surface will automatically perform the color conversion. Thanks vaapi!
    VAProcPipelineParameterBuffer params = {0};
    params.surface = cap_kms->input_surface;
    if(cap_kms->screen_capture)
        params.surface_region = NULL;
    else
        params.surface_region = &cap_kms->input_region;
    params.output_region = NULL;
    params.output_background_color = 0;
    params.filter_flags = VA_FRAME_PICTURE;
    // TODO: Colors
    params.input_color_properties.color_range = frame->color_range == AVCOL_RANGE_JPEG ? VA_SOURCE_RANGE_FULL : VA_SOURCE_RANGE_REDUCED;
    params.output_color_properties.color_range = frame->color_range == AVCOL_RANGE_JPEG ? VA_SOURCE_RANGE_FULL : VA_SOURCE_RANGE_REDUCED;

    // Clear texture with black background because the source texture (window_texture_get_opengl_texture_id(&cap_kms->window_texture))
    // might be smaller than cap_kms->target_texture_id
    // TODO:
    //cap_kms->egl.glClearTexImage(cap_kms->target_texture_id, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    va_status = vaBeginPicture(cap_kms->va_dpy, cap_kms->context_id, target_surface_id);
    if(va_status != VA_STATUS_SUCCESS) {
        static bool error_printed = false;
        if(!error_printed) {
            error_printed = true;
            fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_capture: vaBeginPicture failed: %d\n", va_status);
        }
        return -1;
    }

    va_status = vaCreateBuffer(cap_kms->va_dpy, cap_kms->context_id, VAProcPipelineParameterBufferType, sizeof(params), 1, &params, &cap_kms->buffer_id);
    if(va_status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_capture: vaCreateBuffer failed: %d\n", va_status);
        cap_kms->should_stop = true;
        cap_kms->stop_is_error = true;
        return -1;
    }

    va_status = vaRenderPicture(cap_kms->va_dpy, cap_kms->context_id, &cap_kms->buffer_id, 1);
    if(va_status != VA_STATUS_SUCCESS) {
        vaEndPicture(cap_kms->va_dpy, cap_kms->context_id);
        static bool error_printed = false;
        if(!error_printed) {
            error_printed = true;
            fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_capture: vaRenderPicture failed: %d\n", va_status);
        }
        return -1;
    }

    va_status = vaEndPicture(cap_kms->va_dpy, cap_kms->context_id);
    if(va_status != VA_STATUS_SUCCESS) {
        static bool error_printed = false;
        if(!error_printed) {
            error_printed = true;
            fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_capture: vaEndPicture failed: %d\n", va_status);
        }
        return -1;
    }

    // TODO: Needed?
    vaSyncSurface(cap_kms->va_dpy, cap_kms->input_surface);
    vaSyncSurface(cap_kms->va_dpy, target_surface_id);

    if(cap_kms->buffer_id) {
        vaDestroyBuffer(cap_kms->va_dpy, cap_kms->buffer_id);
        cap_kms->buffer_id = 0;
    }

    if(cap_kms->input_surface) {
        vaDestroySurfaces(cap_kms->va_dpy, &cap_kms->input_surface, 1);
        cap_kms->input_surface = 0;
    }

    if(cap_kms->dmabuf_fd > 0) {
        close(cap_kms->dmabuf_fd);
        cap_kms->dmabuf_fd = 0;
    }

    // TODO: Remove
    //cap_kms->egl.eglSwapBuffers(cap_kms->egl.egl_display, cap_kms->egl.egl_surface);

    return 0;
}

static void gsr_capture_kms_vaapi_stop(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_kms_vaapi *cap_kms = cap->priv;

    if(cap_kms->buffer_id) {
        vaDestroyBuffer(cap_kms->va_dpy, cap_kms->buffer_id);
        cap_kms->buffer_id = 0;
    }

    if(cap_kms->context_id) {
        vaDestroyContext(cap_kms->va_dpy, cap_kms->context_id);
        cap_kms->context_id = 0;
    }

    if(cap_kms->config_id) {
        vaDestroyConfig(cap_kms->va_dpy, cap_kms->config_id);
        cap_kms->config_id = 0;
    }

    if(cap_kms->input_surface) {
        vaDestroySurfaces(cap_kms->va_dpy, &cap_kms->input_surface, 1);
        cap_kms->input_surface = 0;
    }

    if(cap_kms->dmabuf_fd > 0) {
        close(cap_kms->dmabuf_fd);
        cap_kms->dmabuf_fd = 0;
    }

    if(video_codec_context->hw_device_ctx)
        av_buffer_unref(&video_codec_context->hw_device_ctx);
    if(video_codec_context->hw_frames_ctx)
        av_buffer_unref(&video_codec_context->hw_frames_ctx);

    gsr_egl_unload(&cap_kms->egl);
    gsr_kms_client_deinit(&cap_kms->kms_client);
}

static void gsr_capture_kms_vaapi_destroy(gsr_capture *cap, AVCodecContext *video_codec_context) {
    (void)video_codec_context;
    gsr_capture_kms_vaapi *cap_kms = cap->priv;
    if(cap->priv) {
        gsr_capture_kms_vaapi_stop(cap, video_codec_context);
        free((void*)cap_kms->params.display_to_capture);
        cap_kms->params.display_to_capture = NULL;
        free(cap->priv);
        cap->priv = NULL;
    }
    free(cap);
}

gsr_capture* gsr_capture_kms_vaapi_create(const gsr_capture_kms_vaapi_params *params) {
    if(!params) {
        fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_create params is NULL\n");
        return NULL;
    }

    gsr_capture *cap = calloc(1, sizeof(gsr_capture));
    if(!cap)
        return NULL;

    gsr_capture_kms_vaapi *cap_kms = calloc(1, sizeof(gsr_capture_kms_vaapi));
    if(!cap_kms) {
        free(cap);
        return NULL;
    }

    Display *display = XOpenDisplay(NULL);
    if(!display) {
        fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_create failed: XOpenDisplay failed\n");
        free(cap);
        free(cap_kms);
        return NULL;
    }

    const char *display_to_capture = strdup(params->display_to_capture);
    if(!display_to_capture) {
        free(cap);
        free(cap_kms);
        return NULL;
    }

    cap_kms->params = *params;
    cap_kms->params.display_to_capture = display_to_capture;
    
    *cap = (gsr_capture) {
        .start = gsr_capture_kms_vaapi_start,
        .tick = gsr_capture_kms_vaapi_tick,
        .should_stop = gsr_capture_kms_vaapi_should_stop,
        .capture = gsr_capture_kms_vaapi_capture,
        .destroy = gsr_capture_kms_vaapi_destroy,
        .priv = cap_kms
    };

    return cap;
}
