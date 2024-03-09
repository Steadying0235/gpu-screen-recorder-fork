#include "../../include/capture/xcomposite_cuda.h"
#include "../../include/cuda.h"
#include <stdio.h>
#include <stdlib.h>
#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>

typedef struct {
    gsr_capture_xcomposite xcomposite;
    bool overclock;

    gsr_cuda cuda;
    CUgraphicsResource cuda_graphics_resources[2];
    CUarray mapped_arrays[2];
    CUstream cuda_stream;
} gsr_capture_xcomposite_cuda;

static void gsr_capture_xcomposite_cuda_stop(gsr_capture *cap, AVCodecContext *video_codec_context);

static int gsr_capture_xcomposite_cuda_start(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame *frame) {
    gsr_capture_xcomposite_cuda *cap_xcomp = cap->priv;

    const int res = gsr_capture_xcomposite_start(&cap_xcomp->xcomposite, video_codec_context, frame);
    if(res != 0) {
        gsr_capture_xcomposite_cuda_stop(cap, video_codec_context);
        return res;
    }

    // TODO: overclocking is not supported on wayland...
    if(!gsr_cuda_load(&cap_xcomp->cuda, NULL, false)) {
        fprintf(stderr, "gsr error: gsr_capture_kms_cuda_start: failed to load cuda\n");
        gsr_capture_xcomposite_cuda_stop(cap, video_codec_context);
        return -1;
    }

    if(!cuda_create_codec_context(cap_xcomp->cuda.cu_ctx, video_codec_context, &cap_xcomp->cuda_stream)) {
        gsr_capture_xcomposite_cuda_stop(cap, video_codec_context);
        return -1;
    }

    gsr_cuda_context cuda_context = {
        .cuda = &cap_xcomp->cuda,
        .cuda_graphics_resources = cap_xcomp->cuda_graphics_resources,
        .mapped_arrays = cap_xcomp->mapped_arrays
    };

    if(!gsr_capture_base_setup_cuda_textures(&cap_xcomp->xcomposite.base, frame, &cuda_context, cap_xcomp->xcomposite.params.egl, cap_xcomp->xcomposite.params.color_range, GSR_SOURCE_COLOR_RGB, false)) {
        gsr_capture_xcomposite_cuda_stop(cap, video_codec_context);
        return -1;
    }

    return 0;
}

static void gsr_capture_xcomposite_unload_cuda_graphics(gsr_capture_xcomposite_cuda *cap_xcomp) {
    if(cap_xcomp->cuda.cu_ctx) {
        CUcontext old_ctx;
        cap_xcomp->cuda.cuCtxPushCurrent_v2(cap_xcomp->cuda.cu_ctx);

        for(int i = 0; i < 2; ++i) {
            if(cap_xcomp->cuda_graphics_resources[i]) {
                cap_xcomp->cuda.cuGraphicsUnmapResources(1, &cap_xcomp->cuda_graphics_resources[i], 0);
                cap_xcomp->cuda.cuGraphicsUnregisterResource(cap_xcomp->cuda_graphics_resources[i]);
                cap_xcomp->cuda_graphics_resources[i] = 0;
            }
        }

        cap_xcomp->cuda.cuCtxPopCurrent_v2(&old_ctx);
    }
}

static void gsr_capture_xcomposite_cuda_stop(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_xcomposite_cuda *cap_xcomp = cap->priv;

    if(video_codec_context->hw_device_ctx)
        av_buffer_unref(&video_codec_context->hw_device_ctx);
    if(video_codec_context->hw_frames_ctx)
        av_buffer_unref(&video_codec_context->hw_frames_ctx);

    gsr_capture_base_stop(&cap_xcomp->xcomposite.base, cap_xcomp->xcomposite.params.egl);
    gsr_capture_xcomposite_stop(&cap_xcomp->xcomposite, video_codec_context);
    gsr_capture_xcomposite_unload_cuda_graphics(cap_xcomp);
    gsr_cuda_unload(&cap_xcomp->cuda);
}

static void gsr_capture_xcomposite_cuda_tick(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_xcomposite_cuda *cap_xcomp = cap->priv;
    gsr_capture_xcomposite_tick(&cap_xcomp->xcomposite, video_codec_context);
}

static bool gsr_capture_xcomposite_cuda_should_stop(gsr_capture *cap, bool *err) {
    gsr_capture_xcomposite_cuda *cap_xcomp = cap->priv;
    return gsr_capture_xcomposite_should_stop(&cap_xcomp->xcomposite, err);
}

static int gsr_capture_xcomposite_cuda_capture(gsr_capture *cap, AVFrame *frame) {
    gsr_capture_xcomposite_cuda *cap_xcomp = cap->priv;

    gsr_capture_xcomposite_capture(&cap_xcomp->xcomposite, frame);

    const int div[2] = {1, 2}; // divide UV texture size by 2 because chroma is half size
    for(int i = 0; i < 2; ++i) {
        CUDA_MEMCPY2D memcpy_struct;
        memcpy_struct.srcXInBytes = 0;
        memcpy_struct.srcY = 0;
        memcpy_struct.srcMemoryType = CU_MEMORYTYPE_ARRAY;

        memcpy_struct.dstXInBytes = 0;
        memcpy_struct.dstY = 0;
        memcpy_struct.dstMemoryType = CU_MEMORYTYPE_DEVICE;

        memcpy_struct.srcArray = cap_xcomp->mapped_arrays[i];
        memcpy_struct.srcPitch = frame->width / div[i];
        memcpy_struct.dstDevice = (CUdeviceptr)frame->data[i];
        memcpy_struct.dstPitch = frame->linesize[i];
        memcpy_struct.WidthInBytes = frame->width;
        memcpy_struct.Height = frame->height / div[i];
        // TODO: Remove this copy if possible
        cap_xcomp->cuda.cuMemcpy2DAsync_v2(&memcpy_struct, cap_xcomp->cuda_stream);
    }

    // TODO: needed?
    cap_xcomp->cuda.cuStreamSynchronize(cap_xcomp->cuda_stream);

    return 0;
}

static void gsr_capture_xcomposite_cuda_destroy(gsr_capture *cap, AVCodecContext *video_codec_context) {
    if(cap->priv) {
        gsr_capture_xcomposite_cuda_stop(cap, video_codec_context);
        free(cap->priv);
        cap->priv = NULL;
    }
    free(cap);
}

gsr_capture* gsr_capture_xcomposite_cuda_create(const gsr_capture_xcomposite_cuda_params *params) {
    if(!params) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_cuda_create params is NULL\n");
        return NULL;
    }

    gsr_capture *cap = calloc(1, sizeof(gsr_capture));
    if(!cap)
        return NULL;

    gsr_capture_xcomposite_cuda *cap_xcomp = calloc(1, sizeof(gsr_capture_xcomposite_cuda));
    if(!cap_xcomp) {
        free(cap);
        return NULL;
    }

    gsr_capture_xcomposite_init(&cap_xcomp->xcomposite, &params->base);
    cap_xcomp->overclock = params->overclock;
    
    *cap = (gsr_capture) {
        .start = gsr_capture_xcomposite_cuda_start,
        .tick = gsr_capture_xcomposite_cuda_tick,
        .should_stop = gsr_capture_xcomposite_cuda_should_stop,
        .capture = gsr_capture_xcomposite_cuda_capture,
        .capture_end = NULL,
        .destroy = gsr_capture_xcomposite_cuda_destroy,
        .priv = cap_xcomp
    };

    return cap;
}
