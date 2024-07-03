#include "../../include/capture/xcomposite_software.h"
#include <stdio.h>
#include <stdlib.h>
#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>

typedef struct {
    gsr_capture_xcomposite xcomposite;
} gsr_capture_xcomposite_software;

static void gsr_capture_xcomposite_software_stop(gsr_capture *cap, AVCodecContext *video_codec_context);

static int gsr_capture_xcomposite_software_start(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame *frame) {
    gsr_capture_xcomposite_software *cap_xcomp = cap->priv;

    const int res = gsr_capture_xcomposite_start(&cap_xcomp->xcomposite, video_codec_context, frame);
    if(res != 0) {
        gsr_capture_xcomposite_software_stop(cap, video_codec_context);
        return res;
    }

    if(!gsr_capture_base_setup_textures(&cap_xcomp->xcomposite.base, frame, cap_xcomp->xcomposite.params.color_range, GSR_SOURCE_COLOR_RGB, false, false)) {
        gsr_capture_xcomposite_software_stop(cap, video_codec_context);
        return -1;
    }

    return 0;
}

static void gsr_capture_xcomposite_software_stop(gsr_capture *cap, AVCodecContext *video_codec_context) {
    (void)video_codec_context;
    gsr_capture_xcomposite_software *cap_xcomp = cap->priv;
    gsr_capture_xcomposite_stop(&cap_xcomp->xcomposite);
}

static void gsr_capture_xcomposite_software_tick(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_xcomposite_software *cap_xcomp = cap->priv;
    gsr_capture_xcomposite_tick(&cap_xcomp->xcomposite, video_codec_context);
}

static bool gsr_capture_xcomposite_software_is_damaged(gsr_capture *cap) {
    gsr_capture_xcomposite_software *cap_xcomp = cap->priv;
    return gsr_capture_xcomposite_is_damaged(&cap_xcomp->xcomposite);
}

static void gsr_capture_xcomposite_software_clear_damage(gsr_capture *cap) {
    gsr_capture_xcomposite_software *cap_xcomp = cap->priv;
    gsr_capture_xcomposite_clear_damage(&cap_xcomp->xcomposite);
}

static bool gsr_capture_xcomposite_software_should_stop(gsr_capture *cap, bool *err) {
    gsr_capture_xcomposite_software *cap_xcomp = cap->priv;
    return gsr_capture_xcomposite_should_stop(&cap_xcomp->xcomposite, err);
}

static int gsr_capture_xcomposite_software_capture(gsr_capture *cap, AVFrame *frame) {
    gsr_capture_xcomposite_software *cap_xcomp = cap->priv;

    gsr_capture_xcomposite_capture(&cap_xcomp->xcomposite, frame);

    const unsigned int formats[2] = { GL_RED, GL_RG };
    for(int i = 0; i < 2; ++i) {
        cap_xcomp->xcomposite.params.egl->glBindTexture(GL_TEXTURE_2D, cap_xcomp->xcomposite.base.target_textures[i]);
        cap_xcomp->xcomposite.params.egl->glGetTexImage(GL_TEXTURE_2D, 0, formats[i], GL_UNSIGNED_BYTE, frame->data[i]);
    }
    cap_xcomp->xcomposite.params.egl->glBindTexture(GL_TEXTURE_2D, 0);

    cap_xcomp->xcomposite.params.egl->eglSwapBuffers(cap_xcomp->xcomposite.params.egl->egl_display, cap_xcomp->xcomposite.params.egl->egl_surface);

    return 0;
}

static void gsr_capture_xcomposite_software_destroy(gsr_capture *cap, AVCodecContext *video_codec_context) {
    if(cap->priv) {
        gsr_capture_xcomposite_software_stop(cap, video_codec_context);
        free(cap->priv);
        cap->priv = NULL;
    }
    free(cap);
}

gsr_capture* gsr_capture_xcomposite_software_create(const gsr_capture_xcomposite_software_params *params) {
    if(!params) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_software_create params is NULL\n");
        return NULL;
    }

    gsr_capture *cap = calloc(1, sizeof(gsr_capture));
    if(!cap)
        return NULL;

    gsr_capture_xcomposite_software *cap_xcomp = calloc(1, sizeof(gsr_capture_xcomposite_software));
    if(!cap_xcomp) {
        free(cap);
        return NULL;
    }

    gsr_capture_xcomposite_init(&cap_xcomp->xcomposite, &params->base);
    
    *cap = (gsr_capture) {
        .start = gsr_capture_xcomposite_software_start,
        .tick = gsr_capture_xcomposite_software_tick,
        .is_damaged = gsr_capture_xcomposite_software_is_damaged,
        .clear_damage = gsr_capture_xcomposite_software_clear_damage,
        .should_stop = gsr_capture_xcomposite_software_should_stop,
        .capture = gsr_capture_xcomposite_software_capture,
        .capture_end = NULL,
        .destroy = gsr_capture_xcomposite_software_destroy,
        .priv = cap_xcomp
    };

    return cap;
}
