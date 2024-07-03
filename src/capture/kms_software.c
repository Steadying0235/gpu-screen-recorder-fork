#include "../../include/capture/kms_software.h"
#include "../../include/capture/kms.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <libavcodec/avcodec.h>

typedef struct {
    gsr_capture_kms kms;
    gsr_capture_kms_software_params params;
} gsr_capture_kms_software;

static void gsr_capture_kms_software_stop(gsr_capture *cap, AVCodecContext *video_codec_context);

#define GL_DYNAMIC_READ                   0x88E9

static int gsr_capture_kms_software_start(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame *frame) {
    gsr_capture_kms_software *cap_kms = cap->priv;

    const int res = gsr_capture_kms_start(&cap_kms->kms, cap_kms->params.display_to_capture, cap_kms->params.egl, video_codec_context, frame);
    if(res != 0) {
        gsr_capture_kms_software_stop(cap, video_codec_context);
        return res;
    }

    if(!gsr_capture_base_setup_textures(&cap_kms->kms.base, frame, cap_kms->params.color_range, GSR_SOURCE_COLOR_RGB, cap_kms->params.hdr, cap_kms->params.egl->gpu_info.vendor == GSR_GPU_VENDOR_NVIDIA)) {
        gsr_capture_kms_software_stop(cap, video_codec_context);
        return -1;
    }

    return 0;
}

static bool gsr_capture_kms_software_should_stop(gsr_capture *cap, bool *err) {
    gsr_capture_kms_software *cap_kms = cap->priv;
    if(cap_kms->kms.should_stop) {
        if(err)
            *err = cap_kms->kms.stop_is_error;
        return true;
    }

    if(err)
        *err = false;
    return false;
}

static int gsr_capture_kms_software_capture(gsr_capture *cap, AVFrame *frame) {
    gsr_capture_kms_software *cap_kms = cap->priv;

    cap_kms->kms.base.egl->glClear(0);
    gsr_capture_kms_capture(&cap_kms->kms, frame, cap_kms->params.hdr, cap_kms->params.egl->gpu_info.vendor != GSR_GPU_VENDOR_AMD, cap_kms->params.egl->gpu_info.vendor == GSR_GPU_VENDOR_NVIDIA, cap_kms->params.record_cursor);

    // TODO: hdr support
    const unsigned int formats[2] = { GL_RED, GL_RG };
    for(int i = 0; i < 2; ++i) {
        cap_kms->params.egl->glBindTexture(GL_TEXTURE_2D, cap_kms->kms.base.target_textures[i]);
        cap_kms->params.egl->glGetTexImage(GL_TEXTURE_2D, 0, formats[i], GL_UNSIGNED_BYTE, frame->data[i]);
    }
    cap_kms->params.egl->glBindTexture(GL_TEXTURE_2D, 0);

    cap_kms->kms.base.egl->eglSwapBuffers(cap_kms->kms.base.egl->egl_display, cap_kms->kms.base.egl->egl_surface);

    return 0;
}

static void gsr_capture_kms_software_capture_end(gsr_capture *cap, AVFrame *frame) {
    (void)frame;
    gsr_capture_kms_software *cap_kms = cap->priv;
    gsr_capture_kms_cleanup_kms_fds(&cap_kms->kms);
}

static void gsr_capture_kms_software_stop(gsr_capture *cap, AVCodecContext *video_codec_context) {
    (void)video_codec_context;
    gsr_capture_kms_software *cap_kms = cap->priv;
    gsr_capture_kms_stop(&cap_kms->kms);
}

static void gsr_capture_kms_software_destroy(gsr_capture *cap, AVCodecContext *video_codec_context) {
    (void)video_codec_context;
    gsr_capture_kms_software *cap_kms = cap->priv;
    if(cap->priv) {
        gsr_capture_kms_software_stop(cap, video_codec_context);
        free((void*)cap_kms->params.display_to_capture);
        cap_kms->params.display_to_capture = NULL;
        free(cap->priv);
        cap->priv = NULL;
    }
    free(cap);
}

gsr_capture* gsr_capture_kms_software_create(const gsr_capture_kms_software_params *params) {
    if(!params) {
        fprintf(stderr, "gsr error: gsr_capture_kms_software_create params is NULL\n");
        return NULL;
    }

    gsr_capture *cap = calloc(1, sizeof(gsr_capture));
    if(!cap)
        return NULL;

    gsr_capture_kms_software *cap_kms = calloc(1, sizeof(gsr_capture_kms_software));
    if(!cap_kms) {
        free(cap);
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
        .start = gsr_capture_kms_software_start,
        .tick = NULL,
        .should_stop = gsr_capture_kms_software_should_stop,
        .capture = gsr_capture_kms_software_capture,
        .capture_end = gsr_capture_kms_software_capture_end,
        .destroy = gsr_capture_kms_software_destroy,
        .priv = cap_kms
    };

    return cap;
}
