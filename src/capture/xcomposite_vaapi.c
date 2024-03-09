#include "../../include/capture/xcomposite_vaapi.h"
#include "../../include/capture/xcomposite.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <libavcodec/avcodec.h>

typedef struct {
    gsr_capture_xcomposite xcomposite;

    VADisplay va_dpy;
    VADRMPRIMESurfaceDescriptor prime;
} gsr_capture_xcomposite_vaapi;

static void gsr_capture_xcomposite_vaapi_stop(gsr_capture *cap, AVCodecContext *video_codec_context);

static int gsr_capture_xcomposite_vaapi_start(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame *frame) {
    gsr_capture_xcomposite_vaapi *cap_xcomp = cap->priv;

    const int res = gsr_capture_xcomposite_start(&cap_xcomp->xcomposite, video_codec_context, frame);
    if(res != 0) {
        gsr_capture_xcomposite_vaapi_stop(cap, video_codec_context);
        return res;
    }

    if(!drm_create_codec_context(cap_xcomp->xcomposite.params.egl->card_path, video_codec_context, video_codec_context->width, video_codec_context->height, false, &cap_xcomp->va_dpy)) {
        gsr_capture_xcomposite_vaapi_stop(cap, video_codec_context);
        return -1;
    }

    if(!gsr_capture_base_setup_vaapi_textures(&cap_xcomp->xcomposite.base, frame, cap_xcomp->va_dpy, &cap_xcomp->prime, cap_xcomp->xcomposite.params.color_range)) {
        gsr_capture_xcomposite_vaapi_stop(cap, video_codec_context);
        return -1;
    }

    return 0;
}

static void gsr_capture_xcomposite_vaapi_tick(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_xcomposite_vaapi *cap_xcomp = cap->priv;
    gsr_capture_xcomposite_tick(&cap_xcomp->xcomposite, video_codec_context);
}

static bool gsr_capture_xcomposite_vaapi_should_stop(gsr_capture *cap, bool *err) {
    gsr_capture_xcomposite_vaapi *cap_xcomp = cap->priv;
    return gsr_capture_xcomposite_should_stop(&cap_xcomp->xcomposite, err);
}

static int gsr_capture_xcomposite_vaapi_capture(gsr_capture *cap, AVFrame *frame) {
    gsr_capture_xcomposite_vaapi *cap_xcomp = cap->priv;
    return gsr_capture_xcomposite_capture(&cap_xcomp->xcomposite, frame);
}

static void gsr_capture_xcomposite_vaapi_stop(gsr_capture *cap, AVCodecContext *video_codec_context) {
    (void)video_codec_context;
    gsr_capture_xcomposite_vaapi *cap_xcomp = cap->priv;

    for(uint32_t i = 0; i < cap_xcomp->prime.num_objects; ++i) {
        if(cap_xcomp->prime.objects[i].fd > 0) {
            close(cap_xcomp->prime.objects[i].fd);
            cap_xcomp->prime.objects[i].fd = 0;
        }
    }

    gsr_capture_xcomposite_stop(&cap_xcomp->xcomposite);
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

    gsr_capture_xcomposite_init(&cap_xcomp->xcomposite, &params->base);
    
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
