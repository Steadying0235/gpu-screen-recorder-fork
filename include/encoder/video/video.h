#ifndef GSR_ENCODER_VIDEO_H
#define GSR_ENCODER_VIDEO_H

#include "../../color_conversion.h"
#include <stdbool.h>

typedef struct gsr_video_encoder gsr_video_encoder;
typedef struct AVCodecContext AVCodecContext;
typedef struct AVFrame AVFrame;

struct gsr_video_encoder {
    bool (*start)(gsr_video_encoder *encoder, AVCodecContext *video_codec_context, AVFrame *frame);
    void (*copy_textures_to_frame)(gsr_video_encoder *encoder, AVFrame *frame); /* Can be NULL */
    /* |textures| should be able to fit 2 elements */
    void (*get_textures)(gsr_video_encoder *encoder, unsigned int *textures, int *num_textures, gsr_destination_color *destination_color);
    void (*destroy)(gsr_video_encoder *encoder, AVCodecContext *video_codec_context);

    void *priv;
    bool started;
};

bool gsr_video_encoder_start(gsr_video_encoder *encoder, AVCodecContext *video_codec_context, AVFrame *frame);
void gsr_video_encoder_copy_textures_to_frame(gsr_video_encoder *encoder, AVFrame *frame);
void gsr_video_encoder_get_textures(gsr_video_encoder *encoder, unsigned int *textures, int *num_textures, gsr_destination_color *destination_color);
void gsr_video_encoder_destroy(gsr_video_encoder *encoder, AVCodecContext *video_codec_context);

#endif /* GSR_ENCODER_VIDEO_H */
