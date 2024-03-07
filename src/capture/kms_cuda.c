#include "../../include/capture/kms_cuda.h"
#include "../../include/capture/kms.h"
#include "../../include/cuda.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavcodec/avcodec.h>

typedef struct {
    gsr_capture_base base;
    gsr_capture_kms kms;

    gsr_capture_kms_cuda_params params;

    gsr_cuda cuda;
    CUgraphicsResource cuda_graphics_resources[2];
    CUarray mapped_arrays[2];
    CUstream cuda_stream;
} gsr_capture_kms_cuda;

static void gsr_capture_kms_cuda_stop(gsr_capture *cap, AVCodecContext *video_codec_context);

static bool cuda_create_codec_context(gsr_capture_kms_cuda *cap_kms, AVCodecContext *video_codec_context) {
    CUcontext old_ctx;
    cap_kms->cuda.cuCtxPushCurrent_v2(cap_kms->cuda.cu_ctx);

    AVBufferRef *device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA);
    if(!device_ctx) {
        fprintf(stderr, "Error: Failed to create hardware device context\n");
        cap_kms->cuda.cuCtxPopCurrent_v2(&old_ctx);
        return false;
    }

    AVHWDeviceContext *hw_device_context = (AVHWDeviceContext*)device_ctx->data;
    AVCUDADeviceContext *cuda_device_context = (AVCUDADeviceContext*)hw_device_context->hwctx;
    cuda_device_context->cuda_ctx = cap_kms->cuda.cu_ctx;
    if(av_hwdevice_ctx_init(device_ctx) < 0) {
        fprintf(stderr, "Error: Failed to create hardware device context\n");
        av_buffer_unref(&device_ctx);
        cap_kms->cuda.cuCtxPopCurrent_v2(&old_ctx);
        return false;
    }

    AVBufferRef *frame_context = av_hwframe_ctx_alloc(device_ctx);
    if(!frame_context) {
        fprintf(stderr, "Error: Failed to create hwframe context\n");
        av_buffer_unref(&device_ctx);
        cap_kms->cuda.cuCtxPopCurrent_v2(&old_ctx);
        return false;
    }

    AVHWFramesContext *hw_frame_context =
        (AVHWFramesContext *)frame_context->data;
    hw_frame_context->width = video_codec_context->width;
    hw_frame_context->height = video_codec_context->height;
    hw_frame_context->sw_format = cap_kms->params.hdr ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
    hw_frame_context->format = video_codec_context->pix_fmt;
    hw_frame_context->device_ref = device_ctx;
    hw_frame_context->device_ctx = (AVHWDeviceContext*)device_ctx->data;

    if (av_hwframe_ctx_init(frame_context) < 0) {
        fprintf(stderr, "Error: Failed to initialize hardware frame context "
                        "(note: ffmpeg version needs to be > 4.0)\n");
        av_buffer_unref(&device_ctx);
        //av_buffer_unref(&frame_context);
        cap_kms->cuda.cuCtxPopCurrent_v2(&old_ctx);
        return false;
    }

    cap_kms->cuda_stream = cuda_device_context->stream;
    video_codec_context->hw_device_ctx = av_buffer_ref(device_ctx);
    video_codec_context->hw_frames_ctx = av_buffer_ref(frame_context);
    return true;
}

static int gsr_capture_kms_cuda_start(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame *frame) {
    gsr_capture_kms_cuda *cap_kms = cap->priv;

    const int res = gsr_capture_kms_start(&cap_kms->kms, &cap_kms->base, cap_kms->params.display_to_capture, cap_kms->params.egl, video_codec_context, frame);
    if(res != 0) {
        gsr_capture_kms_cuda_stop(cap, video_codec_context);
        return res;
    }

    // TODO: overclocking is not supported on wayland...
    if(!gsr_cuda_load(&cap_kms->cuda, NULL, false)) {
        fprintf(stderr, "gsr error: gsr_capture_kms_cuda_start: failed to load cuda\n");
        gsr_capture_kms_cuda_stop(cap, video_codec_context);
        return -1;
    }

    if(!cuda_create_codec_context(cap_kms, video_codec_context)) {
        gsr_capture_kms_cuda_stop(cap, video_codec_context);
        return -1;
    }

    gsr_cuda_context cuda_context = {
        .cuda = &cap_kms->cuda,
        .cuda_graphics_resources = cap_kms->cuda_graphics_resources,
        .mapped_arrays = cap_kms->mapped_arrays
    };
    if(!gsr_capture_base_setup_cuda_textures(&cap_kms->base, frame, &cuda_context, cap_kms->params.egl, cap_kms->params.color_range, GSR_SOURCE_COLOR_RGB, cap_kms->params.hdr)) {
        gsr_capture_kms_cuda_stop(cap, video_codec_context);
        return -1;
    }

    return 0;
}

static bool gsr_capture_kms_cuda_should_stop(gsr_capture *cap, bool *err) {
    gsr_capture_kms_cuda *cap_kms = cap->priv;
    if(cap_kms->kms.should_stop) {
        if(err)
            *err = cap_kms->kms.stop_is_error;
        return true;
    }

    if(err)
        *err = false;
    return false;
}

static void gsr_capture_kms_unload_cuda_graphics(gsr_capture_kms_cuda *cap_kms) {
    if(cap_kms->cuda.cu_ctx) {
        CUcontext old_ctx;
        cap_kms->cuda.cuCtxPushCurrent_v2(cap_kms->cuda.cu_ctx);

        for(int i = 0; i < 2; ++i) {
            if(cap_kms->cuda_graphics_resources[i]) {
                cap_kms->cuda.cuGraphicsUnmapResources(1, &cap_kms->cuda_graphics_resources[i], 0);
                cap_kms->cuda.cuGraphicsUnregisterResource(cap_kms->cuda_graphics_resources[i]);
                cap_kms->cuda_graphics_resources[i] = 0;
            }
        }

        cap_kms->cuda.cuCtxPopCurrent_v2(&old_ctx);
    }
}

static int gsr_capture_kms_cuda_capture(gsr_capture *cap, AVFrame *frame) {
    gsr_capture_kms_cuda *cap_kms = cap->priv;

    gsr_capture_kms_capture(&cap_kms->kms, &cap_kms->base, frame, cap_kms->params.egl, cap_kms->params.hdr, true, true);

    const int div[2] = {1, 2}; // divide UV texture size by 2 because chroma is half size
    for(int i = 0; i < 2; ++i) {
        CUDA_MEMCPY2D memcpy_struct;
        memcpy_struct.srcXInBytes = 0;
        memcpy_struct.srcY = 0;
        memcpy_struct.srcMemoryType = CU_MEMORYTYPE_ARRAY;

        memcpy_struct.dstXInBytes = 0;
        memcpy_struct.dstY = 0;
        memcpy_struct.dstMemoryType = CU_MEMORYTYPE_DEVICE;

        memcpy_struct.srcArray = cap_kms->mapped_arrays[i];
        memcpy_struct.srcPitch = frame->width / div[i];
        memcpy_struct.dstDevice = (CUdeviceptr)frame->data[i];
        memcpy_struct.dstPitch = frame->linesize[i];
        memcpy_struct.WidthInBytes = frame->width * (cap_kms->params.hdr ? 2 : 1);
        memcpy_struct.Height = frame->height / div[i];
        // TODO: Remove this copy if possible
        cap_kms->cuda.cuMemcpy2DAsync_v2(&memcpy_struct, cap_kms->cuda_stream);
    }

    // TODO: needed?
    cap_kms->cuda.cuStreamSynchronize(cap_kms->cuda_stream);

    return 0;
}

static void gsr_capture_kms_cuda_capture_end(gsr_capture *cap, AVFrame *frame) {
    (void)frame;
    gsr_capture_kms_cuda *cap_kms = cap->priv;
    gsr_capture_kms_cleanup_kms_fds(&cap_kms->kms);
}

static void gsr_capture_kms_cuda_stop(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_kms_cuda *cap_kms = cap->priv;

    gsr_capture_kms_unload_cuda_graphics(cap_kms);

    if(video_codec_context->hw_device_ctx)
        av_buffer_unref(&video_codec_context->hw_device_ctx);
    if(video_codec_context->hw_frames_ctx)
        av_buffer_unref(&video_codec_context->hw_frames_ctx);

    gsr_cuda_unload(&cap_kms->cuda);
    gsr_capture_kms_stop(&cap_kms->kms);
    gsr_capture_base_stop(&cap_kms->base, cap_kms->params.egl);
}

static void gsr_capture_kms_cuda_destroy(gsr_capture *cap, AVCodecContext *video_codec_context) {
    (void)video_codec_context;
    gsr_capture_kms_cuda *cap_kms = cap->priv;
    if(cap->priv) {
        gsr_capture_kms_cuda_stop(cap, video_codec_context);
        free((void*)cap_kms->params.display_to_capture);
        cap_kms->params.display_to_capture = NULL;
        free(cap->priv);
        cap->priv = NULL;
    }
    free(cap);
}

gsr_capture* gsr_capture_kms_cuda_create(const gsr_capture_kms_cuda_params *params) {
    if(!params) {
        fprintf(stderr, "gsr error: gsr_capture_kms_cuda_create params is NULL\n");
        return NULL;
    }

    gsr_capture *cap = calloc(1, sizeof(gsr_capture));
    if(!cap)
        return NULL;

    gsr_capture_kms_cuda *cap_kms = calloc(1, sizeof(gsr_capture_kms_cuda));
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
        .start = gsr_capture_kms_cuda_start,
        .tick = NULL,
        .should_stop = gsr_capture_kms_cuda_should_stop,
        .capture = gsr_capture_kms_cuda_capture,
        .capture_end = gsr_capture_kms_cuda_capture_end,
        .destroy = gsr_capture_kms_cuda_destroy,
        .priv = cap_kms
    };

    return cap;
}
