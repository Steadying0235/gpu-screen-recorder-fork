#ifndef GSR_CAPTURE_CAPTURE_H
#define GSR_CAPTURE_CAPTURE_H

#include "../color_conversion.h"
#include <stdbool.h>

typedef struct AVCodecContext AVCodecContext;
typedef struct AVFrame AVFrame;
typedef void* VADisplay;
typedef struct _VADRMPRIMESurfaceDescriptor VADRMPRIMESurfaceDescriptor;
typedef struct gsr_cuda gsr_cuda;
typedef struct AVFrame AVFrame;
typedef struct CUgraphicsResource_st *CUgraphicsResource;
typedef struct CUarray_st *CUarray;
typedef struct CUctx_st *CUcontext;
typedef struct CUstream_st *CUstream;

typedef struct gsr_capture gsr_capture;

struct gsr_capture {
    /* These methods should not be called manually. Call gsr_capture_* instead */
    int (*start)(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame *frame);
    void (*tick)(gsr_capture *cap, AVCodecContext *video_codec_context); /* can be NULL */
    bool (*should_stop)(gsr_capture *cap, bool *err); /* can be NULL */
    int (*capture)(gsr_capture *cap, AVFrame *frame);
    void (*capture_end)(gsr_capture *cap, AVFrame *frame); /* can be NULL */
    void (*destroy)(gsr_capture *cap, AVCodecContext *video_codec_context);

    void *priv; /* can be NULL */
    bool started;
};

typedef struct gsr_capture_base gsr_capture_base;

struct gsr_capture_base {
    gsr_egl *egl;

    unsigned int input_texture;
    unsigned int target_textures[2];
    unsigned int cursor_texture;

    gsr_color_conversion color_conversion;

    AVCodecContext *video_codec_context;
    vec2i video_alignment_padding;
};

typedef struct {
    gsr_cuda *cuda;
    CUgraphicsResource *cuda_graphics_resources;
    CUarray *mapped_arrays;
} gsr_cuda_context;

int gsr_capture_start(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame *frame);
void gsr_capture_tick(gsr_capture *cap, AVCodecContext *video_codec_context);
bool gsr_capture_should_stop(gsr_capture *cap, bool *err);
int gsr_capture_capture(gsr_capture *cap, AVFrame *frame);
void gsr_capture_end(gsr_capture *cap, AVFrame *frame);
/* Calls |gsr_capture_stop| as well */
void gsr_capture_destroy(gsr_capture *cap, AVCodecContext *video_codec_context);

bool gsr_capture_base_setup_vaapi_textures(gsr_capture_base *self, AVFrame *frame, VADisplay va_dpy, VADRMPRIMESurfaceDescriptor *prime, gsr_color_range color_range);
bool gsr_capture_base_setup_cuda_textures(gsr_capture_base *self, AVFrame *frame, gsr_cuda_context *cuda_context, gsr_color_range color_range, gsr_source_color source_color, bool hdr);
void gsr_capture_base_stop(gsr_capture_base *self);

bool drm_create_codec_context(const char *card_path, AVCodecContext *video_codec_context, int width, int height, bool hdr, VADisplay *va_dpy);
bool cuda_create_codec_context(CUcontext cu_ctx, AVCodecContext *video_codec_context, int width, int height, bool hdr, CUstream *cuda_stream);

#endif /* GSR_CAPTURE_CAPTURE_H */
