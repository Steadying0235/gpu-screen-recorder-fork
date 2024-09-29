#include "../../../include/encoder/video/vulkan.h"
#include "../../../include/utils.h"
#include "../../../include/egl.h"

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_vulkan.h>

//#include <vulkan/vulkan_core.h>

#define GL_TEXTURE_TILING_EXT             0x9580
#define GL_OPTIMAL_TILING_EXT             0x9584
#define GL_LINEAR_TILING_EXT              0x9585

#define GL_PIXEL_PACK_BUFFER              0x88EB
#define GL_PIXEL_UNPACK_BUFFER            0x88EC
#define GL_STREAM_READ                    0x88E1
#define GL_STREAM_DRAW                    0x88E0
#define GL_READ_ONLY                      0x88B8
#define GL_WRITE_ONLY                     0x88B9
#define GL_READ_FRAMEBUFFER               0x8CA8

typedef struct {
    gsr_video_encoder_vulkan_params params;
    unsigned int target_textures[2];
    AVBufferRef *device_ctx;
    AVVulkanDeviceContext* vv;
    unsigned int pbo_y[2];
    unsigned int pbo_uv[2];
    AVFrame *sw_frame;
} gsr_video_encoder_vulkan;

static bool gsr_video_encoder_vulkan_setup_context(gsr_video_encoder_vulkan *self, AVCodecContext *video_codec_context) {
    AVDictionary *options = NULL;
    //av_dict_set(&options, "linear_images", "1", 0);
    //av_dict_set(&options, "disable_multiplane", "1", 0);

    // TODO: Use correct device
    if(av_hwdevice_ctx_create(&self->device_ctx, AV_HWDEVICE_TYPE_VULKAN, NULL, options, 0) < 0) {
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

    //AVVulkanFramesContext *vk_frame_ctx = (AVVulkanFramesContext*)hw_frame_context->hwctx;
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

static unsigned int gl_create_texture(gsr_egl *egl, int width, int height, int internal_format, unsigned int format) {
    unsigned int texture_id = 0;
    egl->glGenTextures(1, &texture_id);
    egl->glBindTexture(GL_TEXTURE_2D, texture_id);
    //egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
    egl->glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, GL_UNSIGNED_BYTE, NULL);

    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

    egl->glBindTexture(GL_TEXTURE_2D, 0);
    return texture_id;
}

static AVVulkanDeviceContext* video_codec_context_get_vulkan_data(AVCodecContext *video_codec_context) {
    AVBufferRef *hw_frames_ctx = video_codec_context->hw_frames_ctx;
    if(!hw_frames_ctx)
        return NULL;

    AVHWFramesContext *hw_frame_context = (AVHWFramesContext*)hw_frames_ctx->data;
    AVHWDeviceContext *device_context = (AVHWDeviceContext*)hw_frame_context->device_ctx;
    if(device_context->type != AV_HWDEVICE_TYPE_VULKAN)
        return NULL;

    return (AVVulkanDeviceContext*)device_context->hwctx;
}

static bool gsr_video_encoder_vulkan_setup_textures(gsr_video_encoder_vulkan *self, AVCodecContext *video_codec_context, AVFrame *frame) {
    const int res = av_hwframe_get_buffer(video_codec_context->hw_frames_ctx, frame, 0);
    if(res < 0) {
        fprintf(stderr, "gsr error: gsr_video_encoder_vulkan_setup_textures: av_hwframe_get_buffer failed: %d\n", res);
        return false;
    }

    //AVVkFrame *target_surface_id = (AVVkFrame*)frame->data[0];
    self->vv = video_codec_context_get_vulkan_data(video_codec_context);

    const unsigned int internal_formats_nv12[2] = { GL_RGBA8, GL_RGBA8 };
    const unsigned int internal_formats_p010[2] = { GL_R16, GL_RG16 };
    const unsigned int formats[2] = { GL_RED, GL_RG };
    const int div[2] = {1, 2}; // divide UV texture size by 2 because chroma is half size

    for(int i = 0; i < 2; ++i) {
        self->target_textures[i] = gl_create_texture(self->params.egl, video_codec_context->width / div[i], video_codec_context->height / div[i], self->params.color_depth == GSR_COLOR_DEPTH_8_BITS ? internal_formats_nv12[i] : internal_formats_p010[i], formats[i]);
        if(self->target_textures[i] == 0) {
            fprintf(stderr, "gsr error: gsr_video_encoder_cuda_setup_textures: failed to create opengl texture\n");
            return false;
        }
    }

    self->params.egl->glGenBuffers(2, self->pbo_y);

    self->params.egl->glBindBuffer(GL_PIXEL_PACK_BUFFER, self->pbo_y[0]);
    self->params.egl->glBufferData(GL_PIXEL_PACK_BUFFER, 3840 * 2160, 0, GL_STREAM_READ);

    self->params.egl->glBindBuffer(GL_PIXEL_PACK_BUFFER, self->pbo_y[1]);
    self->params.egl->glBufferData(GL_PIXEL_PACK_BUFFER, 3840 * 2160, 0, GL_STREAM_READ);

    self->params.egl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    self->params.egl->glGenBuffers(2, self->pbo_uv);

    self->params.egl->glBindBuffer(GL_PIXEL_PACK_BUFFER, self->pbo_uv[0]);
    self->params.egl->glBufferData(GL_PIXEL_PACK_BUFFER, 1920 * 1080 * 2, 0, GL_STREAM_READ);

    self->params.egl->glBindBuffer(GL_PIXEL_PACK_BUFFER, self->pbo_uv[1]);
    self->params.egl->glBufferData(GL_PIXEL_PACK_BUFFER, 1920 * 1080 * 2, 0, GL_STREAM_READ);

    self->params.egl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    self->sw_frame = av_frame_alloc();
    self->sw_frame->format = AV_PIX_FMT_NV12;
    self->sw_frame->width = frame->width;
    self->sw_frame->height = frame->height;

    // TODO: Remove
    if(av_frame_get_buffer(self->sw_frame, 0) < 0) {
        fprintf(stderr, "failed to allocate sw frame\n");
    }

    // TODO: Remove
    if(av_frame_make_writable(self->sw_frame) < 0) {
        fprintf(stderr, "failed to make writable\n");
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

static void nop_free(void *opaque, uint8_t *data) {

}

static void gsr_video_encoder_vulkan_copy_textures_to_frame(gsr_video_encoder *encoder, AVFrame *frame, gsr_color_conversion *color_conversion) {
    gsr_video_encoder_vulkan *self = encoder->priv;

    static int counter = 0;
    ++counter;

    // AVBufferRef *av_buffer_create(uint8_t *data, size_t size,
    //                           void (*free)(void *opaque, uint8_t *data),
    //                           void *opaque, int flags);

    while(self->params.egl->glGetError()){}
    self->params.egl->glBindFramebuffer(GL_READ_FRAMEBUFFER, color_conversion->framebuffers[0]);
    //fprintf(stderr, "1 gl err: %d\n", self->params.egl->glGetError());
    self->params.egl->glBindBuffer(GL_PIXEL_PACK_BUFFER, self->pbo_y[counter % 2]);
    self->params.egl->glBufferData(GL_PIXEL_PACK_BUFFER, 3840 * 2160, 0, GL_STREAM_READ);
    self->params.egl->glReadPixels(0, 0, 3840, 2160, GL_RED, GL_UNSIGNED_BYTE, 0);
    //fprintf(stderr, "2 gl err: %d\n", self->params.egl->glGetError());

    const int next_pbo_y = (counter + 1) % 2;
    self->params.egl->glBindBuffer(GL_PIXEL_PACK_BUFFER, self->pbo_y[next_pbo_y]);
    self->params.egl->glBufferData(GL_PIXEL_PACK_BUFFER, 3840 * 2160, 0, GL_STREAM_READ);
    //fprintf(stderr, "3 gl err: %d\n", self->params.egl->glGetError());
    uint8_t *ptr_y = (uint8_t*)self->params.egl->glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    //fprintf(stderr, "4 gl err: %d\n", self->params.egl->glGetError());
    if(!ptr_y) {
        fprintf(stderr, "failed to map buffer y!\n");
    }

    while(self->params.egl->glGetError()){}
    self->params.egl->glBindFramebuffer(GL_READ_FRAMEBUFFER, color_conversion->framebuffers[1]);
    //fprintf(stderr, "5 gl err: %d\n", self->params.egl->glGetError());
    self->params.egl->glBindBuffer(GL_PIXEL_PACK_BUFFER, self->pbo_uv[counter % 2]);
    self->params.egl->glBufferData(GL_PIXEL_PACK_BUFFER, 1920 * 1080 * 2, 0, GL_STREAM_READ);
    //fprintf(stderr, "5.5 gl err: %d\n", self->params.egl->glGetError());
    self->params.egl->glReadPixels(0, 0, 1920, 1080, GL_RG, GL_UNSIGNED_BYTE, 0);
    //fprintf(stderr, "6 gl err: %d\n", self->params.egl->glGetError());

    const int next_pbo_uv = (counter + 1) % 2;
    self->params.egl->glBindBuffer(GL_PIXEL_PACK_BUFFER, self->pbo_uv[next_pbo_uv]);
    self->params.egl->glBufferData(GL_PIXEL_PACK_BUFFER, 1920 * 1080 * 2, 0, GL_STREAM_READ);
    //fprintf(stderr, "7 gl err: %d\n", self->params.egl->glGetError());
    uint8_t *ptr_uv = (uint8_t*)self->params.egl->glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    //fprintf(stderr, "8 gl err: %d\n", self->params.egl->glGetError());
    if(!ptr_uv) {
        fprintf(stderr, "failed to map buffer uv!\n");
    }

    //self->sw_frame->buf[0] = av_buffer_create(ptr_y, 3840 * 2160, nop_free, NULL, 0);
    //self->sw_frame->buf[1] = av_buffer_create(ptr_uv, 1920 * 1080 * 2, nop_free, NULL, 0);
    //self->sw_frame->data[0] = self->sw_frame->buf[0]->data;
    //self->sw_frame->data[1] = self->sw_frame->buf[1]->data;
    //self->sw_frame->extended_data[0] = self->sw_frame->data[0];
    //self->sw_frame->extended_data[1] = self->sw_frame->data[1];

    self->sw_frame->data[0] = ptr_y;
    self->sw_frame->data[1] = ptr_uv;

    self->params.egl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    self->params.egl->glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    //self->params.egl->glBindTexture(GL_TEXTURE_2D, self->target_textures[1]);
    //self->params.egl->glGetTexImage(GL_TEXTURE_2D, 0, GL_RG, GL_UNSIGNED_BYTE, sw_frame->data[1]);

    //self->params.egl->glBindTexture(GL_TEXTURE_2D, 0);

    int ret = av_hwframe_transfer_data(frame, self->sw_frame, 0);
    if(ret < 0) {
        fprintf(stderr, "transfer data failed, error: %s\n", av_err2str(ret));
    }

    //av_buffer_unref(&self->sw_frame->buf[0]);
    //av_buffer_unref(&self->sw_frame->buf[1]);

    //av_frame_free(&sw_frame);
    self->params.egl->glBindBuffer(GL_PIXEL_PACK_BUFFER, self->pbo_y[next_pbo_y]);
    self->params.egl->glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    self->params.egl->glBindBuffer(GL_PIXEL_PACK_BUFFER, self->pbo_y[next_pbo_uv]);
    self->params.egl->glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    self->params.egl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

static void gsr_video_encoder_vulkan_get_textures(gsr_video_encoder *encoder, unsigned int *textures, int *num_textures, gsr_destination_color *destination_color) {
    gsr_video_encoder_vulkan *self = encoder->priv;
    textures[0] = self->target_textures[0];
    textures[1] = self->target_textures[1];
    *num_textures = 2;
    *destination_color = self->params.color_depth == GSR_COLOR_DEPTH_10_BITS ? GSR_DESTINATION_COLOR_P010 : GSR_DESTINATION_COLOR_NV12;
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

    gsr_video_encoder_vulkan *encoder_vulkan = calloc(1, sizeof(gsr_video_encoder_vulkan));
    if(!encoder_vulkan) {
        free(encoder);
        return NULL;
    }

    encoder_vulkan->params = *params;

    *encoder = (gsr_video_encoder) {
        .start = gsr_video_encoder_vulkan_start,
        .copy_textures_to_frame = gsr_video_encoder_vulkan_copy_textures_to_frame,
        .get_textures = gsr_video_encoder_vulkan_get_textures,
        .destroy = gsr_video_encoder_vulkan_destroy,
        .priv = encoder_vulkan
    };

    return encoder;
}
