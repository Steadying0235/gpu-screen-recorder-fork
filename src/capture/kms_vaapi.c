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
    gsr_capture_kms kms;

    gsr_capture_kms_vaapi_params params;

    VADisplay va_dpy;
    VADRMPRIMESurfaceDescriptor prime;
} gsr_capture_kms_vaapi;

static void gsr_capture_kms_vaapi_stop(gsr_capture *cap, AVCodecContext *video_codec_context);

static int gsr_capture_kms_vaapi_start(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame *frame) {
    gsr_capture_kms_vaapi *cap_kms = cap->priv;

    int res = gsr_capture_kms_start(&cap_kms->kms, cap_kms->params.display_to_capture, cap_kms->params.egl, video_codec_context, frame);
    if(res != 0) {
        gsr_capture_kms_vaapi_stop(cap, video_codec_context);
        return res;
    }

    if(!vaapi_create_codec_context(cap_kms->params.egl->card_path, video_codec_context, video_codec_context->width, video_codec_context->height, cap_kms->params.hdr, &cap_kms->va_dpy)) {
        gsr_capture_kms_vaapi_stop(cap, video_codec_context);
        return -1;
    }

    if(!gsr_capture_base_setup_vaapi_textures(&cap_kms->kms.base, frame, cap_kms->va_dpy, &cap_kms->prime, cap_kms->params.color_range)) {
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
    cap_kms->kms.base.egl->glClear(0);
    gsr_capture_kms_capture(&cap_kms->kms, frame, cap_kms->params.hdr, cap_kms->params.egl->gpu_info.vendor == GSR_GPU_VENDOR_INTEL, false, cap_kms->params.record_cursor);
    cap_kms->kms.base.egl->eglSwapBuffers(cap_kms->kms.base.egl->egl_display, cap_kms->kms.base.egl->egl_surface);
    return 0;
}

static void gsr_capture_kms_vaapi_capture_end(gsr_capture *cap, AVFrame *frame) {
    (void)frame;
    gsr_capture_kms_vaapi *cap_kms = cap->priv;
    gsr_capture_kms_cleanup_kms_fds(&cap_kms->kms);
}

static void gsr_capture_kms_vaapi_stop(gsr_capture *cap, AVCodecContext *video_codec_context) {
    (void)video_codec_context;
    gsr_capture_kms_vaapi *cap_kms = cap->priv;

    for(uint32_t i = 0; i < cap_kms->prime.num_objects; ++i) {
        if(cap_kms->prime.objects[i].fd > 0) {
            close(cap_kms->prime.objects[i].fd);
            cap_kms->prime.objects[i].fd = 0;
        }
    }

    gsr_capture_kms_stop(&cap_kms->kms);
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
