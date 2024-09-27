#include "../../../include/encoder/video/video.h"
#include <assert.h>

bool gsr_video_encoder_start(gsr_video_encoder *encoder, AVCodecContext *video_codec_context, AVFrame *frame) {
    assert(!encoder->started);
    bool res = encoder->start(encoder, video_codec_context, frame);
    if(res)
        encoder->started = true;
    return res;
}

void gsr_video_encoder_copy_textures_to_frame(gsr_video_encoder *encoder, AVFrame *frame, gsr_color_conversion *color_conversion) {
    assert(encoder->started);
    if(encoder->copy_textures_to_frame)
        encoder->copy_textures_to_frame(encoder, frame, color_conversion);
}

void gsr_video_encoder_get_textures(gsr_video_encoder *encoder, unsigned int *textures, int *num_textures, gsr_destination_color *destination_color) {
    assert(encoder->started);
    encoder->get_textures(encoder, textures, num_textures, destination_color);
}

void gsr_video_encoder_destroy(gsr_video_encoder *encoder, AVCodecContext *video_codec_context) {
    assert(encoder->started);
    encoder->destroy(encoder, video_codec_context);
}
