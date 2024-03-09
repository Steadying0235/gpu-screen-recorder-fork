#include "../../include/capture/capture.h"
#include "../../include/egl.h"
#include "../../include/cuda.h"
#include "../../include/utils.h"
#include <stdio.h>
#include <stdint.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavcodec/avcodec.h>

#define FOURCC_NV12 842094158
#define FOURCC_P010 808530000

int gsr_capture_start(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame *frame) {
    if(cap->started)
        return -1;

    int res = cap->start(cap, video_codec_context, frame);
    if(res == 0)
        cap->started = true;

    return res;
}

void gsr_capture_tick(gsr_capture *cap, AVCodecContext *video_codec_context) {
    if(!cap->started) {
        fprintf(stderr, "gsr error: gsp_capture_tick failed: the gsr capture has not been started\n");
        return;
    }

    if(cap->tick)
        cap->tick(cap, video_codec_context);
}

bool gsr_capture_should_stop(gsr_capture *cap, bool *err) {
    if(!cap->started) {
        fprintf(stderr, "gsr error: gsr_capture_should_stop failed: the gsr capture has not been started\n");
        return false;
    }

    if(!cap->should_stop)
        return false;

    return cap->should_stop(cap, err);
}

int gsr_capture_capture(gsr_capture *cap, AVFrame *frame) {
    if(!cap->started) {
        fprintf(stderr, "gsr error: gsr_capture_capture failed: the gsr capture has not been started\n");
        return -1;
    }
    return cap->capture(cap, frame);
}

void gsr_capture_end(gsr_capture *cap, AVFrame *frame) {
    if(!cap->started) {
        fprintf(stderr, "gsr error: gsr_capture_end failed: the gsr capture has not been started\n");
        return;
    }

    if(!cap->capture_end)
        return;

    cap->capture_end(cap, frame);
}

void gsr_capture_destroy(gsr_capture *cap, AVCodecContext *video_codec_context) {
    cap->destroy(cap, video_codec_context);
}

static uint32_t fourcc(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (d << 24) | (c << 16) | (b << 8) | a;
}

bool gsr_capture_base_setup_vaapi_textures(gsr_capture_base *self, AVFrame *frame, gsr_egl *egl, VADisplay va_dpy, VADRMPRIMESurfaceDescriptor *prime, gsr_color_range color_range) {
    const int res = av_hwframe_get_buffer(self->video_codec_context->hw_frames_ctx, frame, 0);
    if(res < 0) {
        fprintf(stderr, "gsr error: gsr_capture_kms_setup_vaapi_textures: av_hwframe_get_buffer failed: %d\n", res);
        return false;
    }

    VASurfaceID target_surface_id = (uintptr_t)frame->data[3];

    VAStatus va_status = vaExportSurfaceHandle(va_dpy, target_surface_id, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_WRITE_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, prime);
    if(va_status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_capture_kms_setup_vaapi_textures: vaExportSurfaceHandle failed, error: %d\n", va_status);
        return false;
    }
    vaSyncSurface(va_dpy, target_surface_id);

    egl->glGenTextures(1, &self->input_texture);
    egl->glBindTexture(GL_TEXTURE_2D, self->input_texture);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    egl->glBindTexture(GL_TEXTURE_2D, 0);

    egl->glGenTextures(1, &self->cursor_texture);
    egl->glBindTexture(GL_TEXTURE_2D, self->cursor_texture);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    egl->glBindTexture(GL_TEXTURE_2D, 0);

    const uint32_t formats_nv12[2] = { fourcc('R', '8', ' ', ' '), fourcc('G', 'R', '8', '8') };
    const uint32_t formats_p010[2] = { fourcc('R', '1', '6', ' '), fourcc('G', 'R', '3', '2') };

    if(prime->fourcc == FOURCC_NV12 || prime->fourcc == FOURCC_P010) {
        const uint32_t *formats = prime->fourcc == FOURCC_NV12 ? formats_nv12 : formats_p010;
        const int div[2] = {1, 2}; // divide UV texture size by 2 because chroma is half size

        egl->glGenTextures(2, self->target_textures);
        for(int i = 0; i < 2; ++i) {
            const int layer = i;
            const int plane = 0;

            //const uint64_t modifier = prime->objects[prime->layers[layer].object_index[plane]].drm_format_modifier;

            const intptr_t img_attr[] = {
                EGL_LINUX_DRM_FOURCC_EXT,       formats[i],
                EGL_WIDTH,                      prime->width / div[i],
                EGL_HEIGHT,                     prime->height / div[i],
                EGL_DMA_BUF_PLANE0_FD_EXT,      prime->objects[prime->layers[layer].object_index[plane]].fd,
                EGL_DMA_BUF_PLANE0_OFFSET_EXT,  prime->layers[layer].offset[plane],
                EGL_DMA_BUF_PLANE0_PITCH_EXT,   prime->layers[layer].pitch[plane],
                // TODO:
                //EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, modifier & 0xFFFFFFFFULL,
                //EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, modifier >> 32ULL,
                EGL_NONE
            };

            while(egl->eglGetError() != EGL_SUCCESS){}
            EGLImage image = egl->eglCreateImage(egl->egl_display, 0, EGL_LINUX_DMA_BUF_EXT, NULL, img_attr);
            if(!image) {
                fprintf(stderr, "gsr error: gsr_capture_kms_setup_vaapi_textures: failed to create egl image from drm fd for output drm fd, error: %d\n", egl->eglGetError());
                return false;
            }

            egl->glBindTexture(GL_TEXTURE_2D, self->target_textures[i]);
            egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

            while(egl->glGetError()) {}
            while(egl->eglGetError() != EGL_SUCCESS){}
            egl->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
            if(egl->glGetError() != 0 || egl->eglGetError() != EGL_SUCCESS) {
                // TODO: Get the error properly
                fprintf(stderr, "gsr error: gsr_capture_kms_setup_vaapi_textures: failed to bind egl image to gl texture, error: %d\n", egl->eglGetError());
                egl->eglDestroyImage(egl->egl_display, image);
                egl->glBindTexture(GL_TEXTURE_2D, 0);
                return false;
            }

            egl->eglDestroyImage(egl->egl_display, image);
            egl->glBindTexture(GL_TEXTURE_2D, 0);
        }

        gsr_color_conversion_params color_conversion_params = {0};
        color_conversion_params.color_range = color_range;
        color_conversion_params.egl = egl;
        color_conversion_params.source_color = GSR_SOURCE_COLOR_RGB;
        if(prime->fourcc == FOURCC_NV12)
            color_conversion_params.destination_color = GSR_DESTINATION_COLOR_NV12;
        else
            color_conversion_params.destination_color = GSR_DESTINATION_COLOR_P010;

        color_conversion_params.destination_textures[0] = self->target_textures[0];
        color_conversion_params.destination_textures[1] = self->target_textures[1];
        color_conversion_params.num_destination_textures = 2;

        if(gsr_color_conversion_init(&self->color_conversion, &color_conversion_params) != 0) {
            fprintf(stderr, "gsr error: gsr_capture_kms_setup_vaapi_textures: failed to create color conversion\n");
            return false;
        }

        return true;
    } else {
        fprintf(stderr, "gsr error: gsr_capture_kms_setup_vaapi_textures: unexpected fourcc %u for output drm fd, expected nv12 or p010\n", prime->fourcc);
        return false;
    }
}

static unsigned int gl_create_texture(gsr_egl *egl, int width, int height, int internal_format, unsigned int format) {
    unsigned int texture_id = 0;
    egl->glGenTextures(1, &texture_id);
    egl->glBindTexture(GL_TEXTURE_2D, texture_id);
    egl->glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, GL_UNSIGNED_BYTE, NULL);

    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

    egl->glBindTexture(GL_TEXTURE_2D, 0);
    return texture_id;
}

static bool cuda_register_opengl_texture(gsr_cuda *cuda, CUgraphicsResource *cuda_graphics_resource, CUarray *mapped_array, unsigned int texture_id) {
    CUresult res;
    CUcontext old_ctx;
    res = cuda->cuCtxPushCurrent_v2(cuda->cu_ctx);
    res = cuda->cuGraphicsGLRegisterImage(cuda_graphics_resource, texture_id, GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_NONE);
    if (res != CUDA_SUCCESS) {
        const char *err_str = "unknown";
        cuda->cuGetErrorString(res, &err_str);
        fprintf(stderr, "gsr error: cuda_register_opengl_texture: cuGraphicsGLRegisterImage failed, error: %s, texture " "id: %u\n", err_str, texture_id);
        res = cuda->cuCtxPopCurrent_v2(&old_ctx);
        return false;
    }

    res = cuda->cuGraphicsResourceSetMapFlags(*cuda_graphics_resource, CU_GRAPHICS_MAP_RESOURCE_FLAGS_NONE);
    res = cuda->cuGraphicsMapResources(1, cuda_graphics_resource, 0);

    res = cuda->cuGraphicsSubResourceGetMappedArray(mapped_array, *cuda_graphics_resource, 0, 0);
    res = cuda->cuCtxPopCurrent_v2(&old_ctx);
    return true;
}

bool gsr_capture_base_setup_cuda_textures(gsr_capture_base *base, AVFrame *frame, gsr_cuda_context *cuda_context, gsr_egl *egl, gsr_color_range color_range, gsr_source_color source_color, bool hdr) {
    // TODO:
    const int res = av_hwframe_get_buffer(base->video_codec_context->hw_frames_ctx, frame, 0);
    if(res < 0) {
        fprintf(stderr, "gsr error: gsr_capture_kms_setup_cuda_textures: av_hwframe_get_buffer failed: %d\n", res);
        return false;
    }

    egl->glGenTextures(1, &base->input_texture);
    egl->glBindTexture(GL_TEXTURE_2D, base->input_texture);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    egl->glBindTexture(GL_TEXTURE_2D, 0);

    egl->glGenTextures(1, &base->cursor_texture);
    egl->glBindTexture(GL_TEXTURE_EXTERNAL_OES, base->cursor_texture);
    egl->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    egl->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    egl->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    egl->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    egl->glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    const unsigned int internal_formats_nv12[2] = { GL_R8, GL_RG8 };
    const unsigned int internal_formats_p010[2] = { GL_R16, GL_RG16 };
    const unsigned int formats[2] = { GL_RED, GL_RG };
    const int div[2] = {1, 2}; // divide UV texture size by 2 because chroma is half size

    for(int i = 0; i < 2; ++i) {
        base->target_textures[i] = gl_create_texture(egl, base->video_codec_context->width / div[i], base->video_codec_context->height / div[i], !hdr ? internal_formats_nv12[i] : internal_formats_p010[i], formats[i]);
        if(base->target_textures[i] == 0) {
            fprintf(stderr, "gsr error: gsr_capture_kms_setup_cuda_textures: failed to create opengl texture\n");
            return false;
        }

        if(!cuda_register_opengl_texture(cuda_context->cuda, &cuda_context->cuda_graphics_resources[i], &cuda_context->mapped_arrays[i], base->target_textures[i])) {
            return false;
        }
    }

    gsr_color_conversion_params color_conversion_params = {0};
    color_conversion_params.color_range = color_range;
    color_conversion_params.egl = egl;
    color_conversion_params.source_color = source_color;
    if(!hdr)
        color_conversion_params.destination_color = GSR_DESTINATION_COLOR_NV12;
    else
        color_conversion_params.destination_color = GSR_DESTINATION_COLOR_P010;

    color_conversion_params.destination_textures[0] = base->target_textures[0];
    color_conversion_params.destination_textures[1] = base->target_textures[1];
    color_conversion_params.num_destination_textures = 2;

    if(gsr_color_conversion_init(&base->color_conversion, &color_conversion_params) != 0) {
        fprintf(stderr, "gsr error: gsr_capture_kms_setup_cuda_textures: failed to create color conversion\n");
        return false;
    }

    return true;
}

void gsr_capture_base_stop(gsr_capture_base *self, gsr_egl *egl) {
    gsr_color_conversion_deinit(&self->color_conversion);

    if(egl->egl_context) {
        if(self->input_texture) {
            egl->glDeleteTextures(1, &self->input_texture);
            self->input_texture = 0;
        }

        if(self->cursor_texture) {
            egl->glDeleteTextures(1, &self->cursor_texture);
            self->cursor_texture = 0;
        }

        egl->glDeleteTextures(2, self->target_textures);
        self->target_textures[0] = 0;
        self->target_textures[1] = 0;
    }
}

bool drm_create_codec_context(const char *card_path, AVCodecContext *video_codec_context, bool hdr, VADisplay *va_dpy) {
    char render_path[128];
    if(!gsr_card_path_get_render_path(card_path, render_path)) {
        fprintf(stderr, "gsr error: failed to get /dev/dri/renderDXXX file from %s\n", card_path);
        return false;
    }

    AVBufferRef *device_ctx;
    if(av_hwdevice_ctx_create(&device_ctx, AV_HWDEVICE_TYPE_VAAPI, render_path, NULL, 0) < 0) {
        fprintf(stderr, "Error: Failed to create hardware device context\n");
        return false;
    }

    AVBufferRef *frame_context = av_hwframe_ctx_alloc(device_ctx);
    if(!frame_context) {
        fprintf(stderr, "Error: Failed to create hwframe context\n");
        av_buffer_unref(&device_ctx);
        return false;
    }

    AVHWFramesContext *hw_frame_context =
        (AVHWFramesContext *)frame_context->data;
    hw_frame_context->width = video_codec_context->width;
    hw_frame_context->height = video_codec_context->height;
    hw_frame_context->sw_format = hdr ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
    hw_frame_context->format = video_codec_context->pix_fmt;
    hw_frame_context->device_ref = device_ctx;
    hw_frame_context->device_ctx = (AVHWDeviceContext*)device_ctx->data;

    //hw_frame_context->initial_pool_size = 20;

    AVVAAPIDeviceContext *vactx =((AVHWDeviceContext*)device_ctx->data)->hwctx;
    *va_dpy = vactx->display;

    if (av_hwframe_ctx_init(frame_context) < 0) {
        fprintf(stderr, "Error: Failed to initialize hardware frame context "
                        "(note: ffmpeg version needs to be > 4.0)\n");
        av_buffer_unref(&device_ctx);
        //av_buffer_unref(&frame_context);
        return false;
    }

    video_codec_context->hw_device_ctx = av_buffer_ref(device_ctx);
    video_codec_context->hw_frames_ctx = av_buffer_ref(frame_context);
    return true;
}

bool cuda_create_codec_context(CUcontext cu_ctx, AVCodecContext *video_codec_context, CUstream *cuda_stream) {
    AVBufferRef *device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA);
    if(!device_ctx) {
        fprintf(stderr, "gsr error: cuda_create_codec_context failed: failed to create hardware device context\n");
        return false;
    }

    AVHWDeviceContext *hw_device_context = (AVHWDeviceContext*)device_ctx->data;
    AVCUDADeviceContext *cuda_device_context = (AVCUDADeviceContext*)hw_device_context->hwctx;
    cuda_device_context->cuda_ctx = cu_ctx;
    if(av_hwdevice_ctx_init(device_ctx) < 0) {
        fprintf(stderr, "gsr error: cuda_create_codec_context failed: failed to create hardware device context\n");
        av_buffer_unref(&device_ctx);
        return false;
    }

    AVBufferRef *frame_context = av_hwframe_ctx_alloc(device_ctx);
    if(!frame_context) {
        fprintf(stderr, "gsr error: cuda_create_codec_context failed: failed to create hwframe context\n");
        av_buffer_unref(&device_ctx);
        return false;
    }

    AVHWFramesContext *hw_frame_context = (AVHWFramesContext*)frame_context->data;
    hw_frame_context->width = video_codec_context->width;
    hw_frame_context->height = video_codec_context->height;
    hw_frame_context->sw_format = AV_PIX_FMT_NV12;
    hw_frame_context->format = video_codec_context->pix_fmt;
    hw_frame_context->device_ref = device_ctx;
    hw_frame_context->device_ctx = (AVHWDeviceContext*)device_ctx->data;

    if (av_hwframe_ctx_init(frame_context) < 0) {
        fprintf(stderr, "gsr error: cuda_create_codec_context failed: failed to initialize hardware frame context "
                        "(note: ffmpeg version needs to be > 4.0)\n");
        av_buffer_unref(&device_ctx);
        //av_buffer_unref(&frame_context);
        return false;
    }

    *cuda_stream = cuda_device_context->stream;
    video_codec_context->hw_device_ctx = av_buffer_ref(device_ctx);
    video_codec_context->hw_frames_ctx = av_buffer_ref(frame_context);
    return true;
}
