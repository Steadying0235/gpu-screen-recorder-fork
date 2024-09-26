#include "../../../include/encoder/video/vulkan.h"
#include "../../../include/utils.h"
#include "../../../include/egl.h"

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_vulkan.h>

typedef struct {
    gsr_video_encoder_vulkan_params params;
    unsigned int target_textures[2];
    AVBufferRef *device_ctx;
} gsr_video_encoder_vulkan;

static bool gsr_video_encoder_vulkan_setup_context(gsr_video_encoder_vulkan *self, AVCodecContext *video_codec_context) {
    // TODO: Use correct device
    if(av_hwdevice_ctx_create(&self->device_ctx, AV_HWDEVICE_TYPE_VULKAN, NULL, NULL, 0) < 0) {
        fprintf(stderr, "gsr error: gsr_video_encoder_vulkan_setup_context: failed to create hardware device context\n");
        return false;
    }

    AVBufferRef *frame_context = av_hwframe_ctx_alloc(self->device_ctx);
    if(!frame_context) {
        fprintf(stderr, "gsr error: gsr_video_encoder_vulkan_setup_context: failed to create hwframe context\n");
        av_buffer_unref(&self->device_ctx);
        return false;
    }

    AVHWFramesContext *hw_frame_context = (AVHWFramesContext*)frame_context->data;
    hw_frame_context->width = video_codec_context->width;
    hw_frame_context->height = video_codec_context->height;
    hw_frame_context->sw_format = self->params.color_depth == GSR_COLOR_DEPTH_10_BITS ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
    hw_frame_context->format = video_codec_context->pix_fmt;
    hw_frame_context->device_ctx = (AVHWDeviceContext*)self->device_ctx->data;

    //hw_frame_context->initial_pool_size = 20;

    if (av_hwframe_ctx_init(frame_context) < 0) {
        fprintf(stderr, "gsr error: gsr_video_encoder_vulkan_setup_context: failed to initialize hardware frame context "
                        "(note: ffmpeg version needs to be > 4.0)\n");
        av_buffer_unref(&self->device_ctx);
        //av_buffer_unref(&frame_context);
        return false;
    }

    video_codec_context->hw_frames_ctx = av_buffer_ref(frame_context);
    av_buffer_unref(&frame_context);
    return true;
}

static bool gsr_video_encoder_vulkan_setup_textures(gsr_video_encoder_vulkan *self, AVCodecContext *video_codec_context, AVFrame *frame) {
    const int res = av_hwframe_get_buffer(video_codec_context->hw_frames_ctx, frame, 0);
    if(res < 0) {
        fprintf(stderr, "gsr error: gsr_video_encoder_vulkan_setup_textures: av_hwframe_get_buffer failed: %d\n", res);
        return false;
    }
    return true;
}

static void gsr_video_encoder_vulkan_stop(gsr_video_encoder_vulkan *self, AVCodecContext *video_codec_context);

static bool gsr_video_encoder_vulkan_start(gsr_video_encoder *encoder, AVCodecContext *video_codec_context, AVFrame *frame) {
    gsr_video_encoder_vulkan *self = encoder->priv;

    if(!gsr_video_encoder_vulkan_setup_context(self, video_codec_context)) {
        gsr_video_encoder_vulkan_stop(self, video_codec_context);
        return false;
    }

    if(!gsr_video_encoder_vulkan_setup_textures(self, video_codec_context, frame)) {
        gsr_video_encoder_vulkan_stop(self, video_codec_context);
        return false;
    }

    return true;
}

void gsr_video_encoder_vulkan_stop(gsr_video_encoder_vulkan *self, AVCodecContext *video_codec_context) {
    self->params.egl->glDeleteTextures(2, self->target_textures);
    self->target_textures[0] = 0;
    self->target_textures[1] = 0;

    if(video_codec_context->hw_frames_ctx)
        av_buffer_unref(&video_codec_context->hw_frames_ctx);
    if(self->device_ctx)
        av_buffer_unref(&self->device_ctx);
}

static void gsr_video_encoder_vulkan_get_textures(gsr_video_encoder *encoder, unsigned int *textures, int *num_textures, gsr_destination_color *destination_color) {
    gsr_video_encoder_vulkan *encoder_vaapi = encoder->priv;
    textures[0] = encoder_vaapi->target_textures[0];
    textures[1] = encoder_vaapi->target_textures[1];
    *num_textures = 2;
    *destination_color = encoder_vaapi->params.color_depth == GSR_COLOR_DEPTH_10_BITS ? GSR_DESTINATION_COLOR_P010 : GSR_DESTINATION_COLOR_NV12;
}

static void gsr_video_encoder_vulkan_destroy(gsr_video_encoder *encoder, AVCodecContext *video_codec_context) {
    gsr_video_encoder_vulkan_stop(encoder->priv, video_codec_context);
    free(encoder->priv);
    free(encoder);
}

gsr_video_encoder* gsr_video_encoder_vulkan_create(const gsr_video_encoder_vulkan_params *params) {
    gsr_video_encoder *encoder = calloc(1, sizeof(gsr_video_encoder));
    if(!encoder)
        return NULL;

    gsr_video_encoder_vulkan *encoder_vaapi = calloc(1, sizeof(gsr_video_encoder_vulkan));
    if(!encoder_vaapi) {
        free(encoder);
        return NULL;
    }

    encoder_vaapi->params = *params;

    *encoder = (gsr_video_encoder) {
        .start = gsr_video_encoder_vulkan_start,
        .copy_textures_to_frame = NULL,
        .get_textures = gsr_video_encoder_vulkan_get_textures,
        .destroy = gsr_video_encoder_vulkan_destroy,
        .priv = encoder_vaapi
    };

    return encoder;
}
