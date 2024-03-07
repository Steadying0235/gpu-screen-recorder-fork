#include "../../include/capture/kms_vaapi.h"
#include "../../include/capture/kms.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavcodec/avcodec.h>
#include <va/va_drmcommon.h>

typedef struct {
    gsr_capture_base base;
    gsr_capture_kms kms;

    gsr_capture_kms_vaapi_params params;

    VADisplay va_dpy;
    VADRMPRIMESurfaceDescriptor prime;
} gsr_capture_kms_vaapi;

static void gsr_capture_kms_vaapi_stop(gsr_capture *cap, AVCodecContext *video_codec_context);

static bool drm_create_codec_context(gsr_capture_kms_vaapi *cap_kms, AVCodecContext *video_codec_context) {
    char render_path[128];
    if(!gsr_card_path_get_render_path(cap_kms->params.egl->card_path, render_path)) {
        fprintf(stderr, "gsr error: failed to get /dev/dri/renderDXXX file from %s\n", cap_kms->params.egl->card_path);
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
    hw_frame_context->sw_format = cap_kms->params.hdr ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
    hw_frame_context->format = video_codec_context->pix_fmt;
    hw_frame_context->device_ref = device_ctx;
    hw_frame_context->device_ctx = (AVHWDeviceContext*)device_ctx->data;

    //hw_frame_context->initial_pool_size = 20;

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

static int gsr_capture_kms_vaapi_start(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame *frame) {
    gsr_capture_kms_vaapi *cap_kms = cap->priv;

    int res = gsr_capture_kms_start(&cap_kms->kms, &cap_kms->base, cap_kms->params.display_to_capture, cap_kms->params.egl, video_codec_context, frame);
    if(res != 0) {
        gsr_capture_kms_vaapi_stop(cap, video_codec_context);
        return res;
    }

    if(!drm_create_codec_context(cap_kms, video_codec_context)) {
        gsr_capture_kms_vaapi_stop(cap, video_codec_context);
        return -1;
    }

    if(!gsr_capture_base_setup_vaapi_textures(&cap_kms->base, frame, cap_kms->params.egl, cap_kms->va_dpy, &cap_kms->prime, cap_kms->params.color_range)) {
        gsr_capture_kms_vaapi_stop(cap, video_codec_context);
        return -1;
    }

    return 0;
}

static bool gsr_capture_kms_vaapi_should_stop(gsr_capture *cap, bool *err) {
    gsr_capture_kms_vaapi *cap_kms = cap->priv;
    if(cap_kms->kms.should_stop) {
        if(err)
            *err = cap_kms->kms.stop_is_error;
        return true;
    }

    if(err)
        *err = false;
    return false;
}

static int gsr_capture_kms_vaapi_capture(gsr_capture *cap, AVFrame *frame) {
    gsr_capture_kms_vaapi *cap_kms = cap->priv;
    gsr_capture_kms_capture(&cap_kms->kms, &cap_kms->base, frame, cap_kms->params.egl, cap_kms->params.hdr, false, false);
    return 0;
}

static void gsr_capture_kms_vaapi_capture_end(gsr_capture *cap, AVFrame *frame) {
    (void)frame;
    gsr_capture_kms_vaapi *cap_kms = cap->priv;
    gsr_capture_kms_cleanup_kms_fds(&cap_kms->kms);
}

static void gsr_capture_kms_vaapi_stop(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_kms_vaapi *cap_kms = cap->priv;

    for(uint32_t i = 0; i < cap_kms->prime.num_objects; ++i) {
        if(cap_kms->prime.objects[i].fd > 0) {
            close(cap_kms->prime.objects[i].fd);
            cap_kms->prime.objects[i].fd = 0;
        }
    }

    if(video_codec_context->hw_device_ctx)
        av_buffer_unref(&video_codec_context->hw_device_ctx);
    if(video_codec_context->hw_frames_ctx)
        av_buffer_unref(&video_codec_context->hw_frames_ctx);

    gsr_capture_kms_stop(&cap_kms->kms);
    gsr_capture_base_stop(&cap_kms->base, cap_kms->params.egl);
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

    const char *display_to_capture = strdup(params->display_to_capture);
    if(!display_to_capture) {
        /* TODO XCloseDisplay */
        free(cap);
        free(cap_kms);
        return NULL;
    }

    cap_kms->params = *params;
    cap_kms->params.display_to_capture = display_to_capture;
    
    *cap = (gsr_capture) {
        .start = gsr_capture_kms_vaapi_start,
        .tick = NULL,
        .should_stop = gsr_capture_kms_vaapi_should_stop,
        .capture = gsr_capture_kms_vaapi_capture,
        .capture_end = gsr_capture_kms_vaapi_capture_end,
        .destroy = gsr_capture_kms_vaapi_destroy,
        .priv = cap_kms
    };

    return cap;
}
