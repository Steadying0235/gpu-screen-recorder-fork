#include "../../include/capture/kms_cuda.h"
#include "../../kms/client/kms_client.h"
#include "../../include/utils.h"
#include "../../include/cuda.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>

#define MAX_CONNECTOR_IDS 32

typedef struct {
    uint32_t connector_ids[MAX_CONNECTOR_IDS];
    int num_connector_ids;
} MonitorId;

typedef struct {
    gsr_capture_kms_cuda_params params;
    XEvent xev;

    bool should_stop;
    bool stop_is_error;
    bool created_hw_frame;

    gsr_cuda cuda;
    
    gsr_kms_client kms_client;
    gsr_kms_response kms_response;
    gsr_kms_response_fd wayland_kms_data;
    bool using_wayland_capture;

    vec2i capture_pos;
    vec2i capture_size;
    MonitorId monitor_id;

    CUgraphicsResource cuda_graphics_resource;
    CUarray mapped_array;
} gsr_capture_kms_cuda;

static int max_int(int a, int b) {
    return a > b ? a : b;
}

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
    hw_frame_context->sw_format = AV_PIX_FMT_BGR0;
    hw_frame_context->format = video_codec_context->pix_fmt;
    hw_frame_context->device_ref = device_ctx;
    hw_frame_context->device_ctx = (AVHWDeviceContext*)device_ctx->data;

    hw_frame_context->initial_pool_size = 1;

    if (av_hwframe_ctx_init(frame_context) < 0) {
        fprintf(stderr, "Error: Failed to initialize hardware frame context "
                        "(note: ffmpeg version needs to be > 4.0)\n");
        av_buffer_unref(&device_ctx);
        //av_buffer_unref(&frame_context);
        cap_kms->cuda.cuCtxPopCurrent_v2(&old_ctx);
        return false;
    }

    video_codec_context->hw_device_ctx = av_buffer_ref(device_ctx);
    video_codec_context->hw_frames_ctx = av_buffer_ref(frame_context);
    return true;
}

// TODO: On monitor reconfiguration, find monitor x, y, width and height again. Do the same for nvfbc.

typedef struct {
    gsr_capture_kms_cuda *cap_kms;
    const char *monitor_to_capture;
    int monitor_to_capture_len;
    int num_monitors;
} MonitorCallbackUserdata;

static void monitor_callback(const gsr_monitor *monitor, void *userdata) {
    MonitorCallbackUserdata *monitor_callback_userdata = userdata;
    ++monitor_callback_userdata->num_monitors;

    if(monitor_callback_userdata->monitor_to_capture_len != monitor->name_len || memcmp(monitor_callback_userdata->monitor_to_capture, monitor->name, monitor->name_len) != 0)
        return;

    if(monitor_callback_userdata->cap_kms->monitor_id.num_connector_ids < MAX_CONNECTOR_IDS) {
        monitor_callback_userdata->cap_kms->monitor_id.connector_ids[monitor_callback_userdata->cap_kms->monitor_id.num_connector_ids] = monitor->connector_id;
        ++monitor_callback_userdata->cap_kms->monitor_id.num_connector_ids;
    }

    if(monitor_callback_userdata->cap_kms->monitor_id.num_connector_ids == MAX_CONNECTOR_IDS)
        fprintf(stderr, "gsr warning: reached max connector ids\n");
}

static int gsr_capture_kms_cuda_start(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_kms_cuda *cap_kms = cap->priv;

    gsr_monitor monitor;
    cap_kms->monitor_id.num_connector_ids = 0;
    if(gsr_egl_start_capture(cap_kms->params.egl, cap_kms->params.display_to_capture)) {
        if(!get_monitor_by_name(cap_kms->params.egl, GSR_CONNECTION_WAYLAND, cap_kms->params.display_to_capture, &monitor)) {
            fprintf(stderr, "gsr error: gsr_capture_kms_cuda_start: failed to find monitor by name \"%s\"\n", cap_kms->params.display_to_capture);
            gsr_capture_kms_cuda_stop(cap, video_codec_context);
            return -1;
        }
        cap_kms->using_wayland_capture = true;
    } else {
        int kms_init_res = gsr_kms_client_init(&cap_kms->kms_client, cap_kms->params.card_path);
        if(kms_init_res != 0) {
            gsr_capture_kms_cuda_stop(cap, video_codec_context);
            return kms_init_res;
        }

        MonitorCallbackUserdata monitor_callback_userdata = {
            cap_kms,
            cap_kms->params.display_to_capture, strlen(cap_kms->params.display_to_capture),
            0
        };
        for_each_active_monitor_output((void*)cap_kms->params.card_path, GSR_CONNECTION_DRM, monitor_callback, &monitor_callback_userdata);

        if(!get_monitor_by_name((void*)cap_kms->params.card_path, GSR_CONNECTION_DRM, cap_kms->params.display_to_capture, &monitor)) {
            fprintf(stderr, "gsr error: gsr_capture_kms_cuda_start: failed to find monitor by name \"%s\"\n", cap_kms->params.display_to_capture);
            gsr_capture_kms_cuda_stop(cap, video_codec_context);
            return -1;
        }
    }

    cap_kms->capture_pos = monitor.pos;
    cap_kms->capture_size = monitor.size;

    video_codec_context->width = max_int(2, cap_kms->capture_size.x & ~1);
    video_codec_context->height = max_int(2, cap_kms->capture_size.y & ~1);

    /* Disable vsync */
    cap_kms->params.egl->eglSwapInterval(cap_kms->params.egl->egl_display, 0);

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

    return 0;
}

static uint32_t fourcc(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (d << 24) | (c << 16) | (b << 8) | a;
}

static void gsr_capture_kms_cuda_tick(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame **frame) {
    gsr_capture_kms_cuda *cap_kms = cap->priv;

    // TODO:
    //cap_kms->params.egl->glClear(GL_COLOR_BUFFER_BIT);

    if(!cap_kms->created_hw_frame) {
        cap_kms->created_hw_frame = true;

        av_frame_free(frame);
        *frame = av_frame_alloc();
        if(!frame) {
            fprintf(stderr, "gsr error: gsr_capture_kms_cuda_tick: failed to allocate frame\n");
            cap_kms->should_stop = true;
            cap_kms->stop_is_error = true;
            return;
        }
        (*frame)->format = video_codec_context->pix_fmt;
        (*frame)->width = video_codec_context->width;
        (*frame)->height = video_codec_context->height;
        (*frame)->color_range = video_codec_context->color_range;
        (*frame)->color_primaries = video_codec_context->color_primaries;
        (*frame)->color_trc = video_codec_context->color_trc;
        (*frame)->colorspace = video_codec_context->colorspace;
        (*frame)->chroma_location = video_codec_context->chroma_sample_location;

        if(av_hwframe_get_buffer(video_codec_context->hw_frames_ctx, *frame, 0) < 0) {
            fprintf(stderr, "gsr error: gsr_capture_kms_cuda_tick: av_hwframe_get_buffer failed\n");
            cap_kms->should_stop = true;
            cap_kms->stop_is_error = true;
            return;
        }
    }
}

static bool gsr_capture_kms_cuda_should_stop(gsr_capture *cap, bool *err) {
    gsr_capture_kms_cuda *cap_kms = cap->priv;
    if(cap_kms->should_stop) {
        if(err)
            *err = cap_kms->stop_is_error;
        return true;
    }

    if(err)
        *err = false;
    return false;
}

/* Prefer non combined planes */
static gsr_kms_response_fd* find_drm_by_connector_id(gsr_kms_response *kms_response, uint32_t connector_id) {
    int index_combined = -1;
    for(int i = 0; i < kms_response->num_fds; ++i) {
        if(kms_response->fds[i].connector_id == connector_id && !kms_response->fds[i].is_cursor) {
            if(kms_response->fds[i].is_combined_plane)
                index_combined = i;
            else
                return &kms_response->fds[i];
        }
    }

    if(index_combined != -1)
        return &kms_response->fds[index_combined];
    else
        return NULL;
}

static gsr_kms_response_fd* find_first_combined_drm(gsr_kms_response *kms_response) {
    for(int i = 0; i < kms_response->num_fds; ++i) {
        if(kms_response->fds[i].is_combined_plane && !kms_response->fds[i].is_cursor)
            return &kms_response->fds[i];
    }
    return NULL;
}

static gsr_kms_response_fd* find_largest_drm(gsr_kms_response *kms_response) {
    if(kms_response->num_fds == 0)
        return NULL;

    int64_t largest_size = 0;
    gsr_kms_response_fd *largest_drm = &kms_response->fds[0];
    for(int i = 0; i < kms_response->num_fds; ++i) {
        const int64_t size = (int64_t)kms_response->fds[i].width * (int64_t)kms_response->fds[i].height;
        if(size > largest_size && !kms_response->fds[i].is_cursor) {
            largest_size = size;
            largest_drm = &kms_response->fds[i];
        }
    }
    return largest_drm;
}

static bool gsr_capture_kms_register_egl_image_in_cuda(gsr_capture_kms_cuda *cap_kms, EGLImage image) {
    CUcontext old_ctx;
    CUresult res = cap_kms->cuda.cuCtxPushCurrent_v2(cap_kms->cuda.cu_ctx);
    res = cap_kms->cuda.cuGraphicsEGLRegisterImage(&cap_kms->cuda_graphics_resource, image, CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY);
    if(res != CUDA_SUCCESS) {
        const char *err_str = "unknown";
        cap_kms->cuda.cuGetErrorString(res, &err_str);
        fprintf(stderr, "gsr error: cuda_register_egl_image: cuGraphicsEGLRegisterImage failed, error: %s (%d), egl image %p\n",
                err_str, res, image);
        res = cap_kms->cuda.cuCtxPopCurrent_v2(&old_ctx);
        return false;
    }

    res = cap_kms->cuda.cuGraphicsResourceSetMapFlags(cap_kms->cuda_graphics_resource, CU_GRAPHICS_MAP_RESOURCE_FLAGS_READ_ONLY);
    res = cap_kms->cuda.cuGraphicsSubResourceGetMappedArray(&cap_kms->mapped_array, cap_kms->cuda_graphics_resource, 0, 0);
    res = cap_kms->cuda.cuCtxPopCurrent_v2(&old_ctx);
    return true;
}

static void gsr_capture_kms_unload_cuda_graphics(gsr_capture_kms_cuda *cap_kms) {
    if(cap_kms->cuda.cu_ctx) {
        CUcontext old_ctx;
        cap_kms->cuda.cuCtxPushCurrent_v2(cap_kms->cuda.cu_ctx);

        if(cap_kms->cuda_graphics_resource) {
            cap_kms->cuda.cuGraphicsUnmapResources(1, &cap_kms->cuda_graphics_resource, 0);
            cap_kms->cuda.cuGraphicsUnregisterResource(cap_kms->cuda_graphics_resource);
            cap_kms->cuda_graphics_resource = 0;
        }

        cap_kms->cuda.cuCtxPopCurrent_v2(&old_ctx);
    }
}

static int gsr_capture_kms_cuda_capture(gsr_capture *cap, AVFrame *frame) {
    (void)frame;
    gsr_capture_kms_cuda *cap_kms = cap->priv;

    for(int i = 0; i < cap_kms->kms_response.num_fds; ++i) {
        if(cap_kms->kms_response.fds[i].fd > 0)
            close(cap_kms->kms_response.fds[i].fd);
        cap_kms->kms_response.fds[i].fd = 0;
    }
    cap_kms->kms_response.num_fds = 0;

    gsr_kms_response_fd *drm_fd = NULL;
    if(cap_kms->using_wayland_capture) {
        gsr_egl_update(cap_kms->params.egl);
        cap_kms->wayland_kms_data.fd = cap_kms->params.egl->fd;
        cap_kms->wayland_kms_data.width = cap_kms->params.egl->width;
        cap_kms->wayland_kms_data.height = cap_kms->params.egl->height;
        cap_kms->wayland_kms_data.pitch = cap_kms->params.egl->pitch;
        cap_kms->wayland_kms_data.offset = cap_kms->params.egl->offset;
        cap_kms->wayland_kms_data.pixel_format = cap_kms->params.egl->pixel_format;
        cap_kms->wayland_kms_data.modifier = cap_kms->params.egl->modifier;
        cap_kms->wayland_kms_data.connector_id = 0;
        cap_kms->wayland_kms_data.is_combined_plane = false;
        cap_kms->wayland_kms_data.is_cursor = false;
        cap_kms->wayland_kms_data.x = cap_kms->wayland_kms_data.x; // TODO: Use these
        cap_kms->wayland_kms_data.y = cap_kms->wayland_kms_data.y;
        cap_kms->wayland_kms_data.src_w = cap_kms->wayland_kms_data.width;
        cap_kms->wayland_kms_data.src_h = cap_kms->wayland_kms_data.height;

        if(cap_kms->wayland_kms_data.fd <= 0)
            return -1;

        drm_fd = &cap_kms->wayland_kms_data;
    } else {
        if(gsr_kms_client_get_kms(&cap_kms->kms_client, &cap_kms->kms_response) != 0) {
            fprintf(stderr, "gsr error: gsr_capture_kms_cuda_capture: failed to get kms, error: %d (%s)\n", cap_kms->kms_response.result, cap_kms->kms_response.err_msg);
            return -1;
        }

        if(cap_kms->kms_response.num_fds == 0) {
            static bool error_shown = false;
            if(!error_shown) {
                error_shown = true;
                fprintf(stderr, "gsr error: no drm found, capture will fail\n");
            }
            return -1;
        }

        for(int i = 0; i < cap_kms->monitor_id.num_connector_ids; ++i) {
            drm_fd = find_drm_by_connector_id(&cap_kms->kms_response, cap_kms->monitor_id.connector_ids[i]);
            if(drm_fd)
                break;
        }

        if(!drm_fd) {
            drm_fd = find_first_combined_drm(&cap_kms->kms_response);
            if(!drm_fd)
                drm_fd = find_largest_drm(&cap_kms->kms_response);
        }
    }

    // TODO: Use capture pos and capture size. Right now they are not used here and doesn't really need to be used on wayland
    // and kms_cuda is only used on wayland right now so maybe it can be ignored.

    if(!drm_fd)
        return -1;

    const intptr_t img_attr[] = {
        //EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
        EGL_LINUX_DRM_FOURCC_EXT,       fourcc('A', 'R', '2', '4'),//cap_kms->params.egl->pixel_format, ARGB8888
        EGL_WIDTH,                      drm_fd->width,//cap_kms->params.egl->width,
        EGL_HEIGHT,                     drm_fd->height,//cap_kms->params.egl->height,
        EGL_DMA_BUF_PLANE0_FD_EXT,      drm_fd->fd,//cap_kms->params.egl->fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,  drm_fd->offset,//cap_kms->params.egl->offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,   drm_fd->pitch,//cap_kms->params.egl->pitch,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, drm_fd->modifier & 0xFFFFFFFFULL,//cap_kms->params.egl->modifier & 0xFFFFFFFFULL,
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, drm_fd->modifier >> 32ULL,//cap_kms->params.egl->modifier >> 32ULL,
        EGL_NONE
    };

    while(cap_kms->params.egl->glGetError()) {}
    while(cap_kms->params.egl->eglGetError() != EGL_SUCCESS){}
    EGLImage image = cap_kms->params.egl->eglCreateImage(cap_kms->params.egl->egl_display, 0, EGL_LINUX_DMA_BUF_EXT, NULL, img_attr);
    if(cap_kms->params.egl->glGetError() != 0 || cap_kms->params.egl->eglGetError() != EGL_SUCCESS) {
        fprintf(stderr, "egl error!\n");
    }

    gsr_capture_kms_register_egl_image_in_cuda(cap_kms, image);
    cap_kms->params.egl->eglDestroyImage(cap_kms->params.egl->egl_display, image);

    //cap_kms->params.egl->eglSwapBuffers(cap_kms->params.egl->egl_display, cap_kms->params.egl->egl_surface);

    frame->linesize[0] = frame->width * 4;

    CUDA_MEMCPY2D memcpy_struct;
    memcpy_struct.srcXInBytes = 0;
    memcpy_struct.srcY = 0;
    memcpy_struct.srcMemoryType = CU_MEMORYTYPE_ARRAY;

    memcpy_struct.dstXInBytes = 0;
    memcpy_struct.dstY = 0;
    memcpy_struct.dstMemoryType = CU_MEMORYTYPE_DEVICE;

    memcpy_struct.srcArray = cap_kms->mapped_array;
    memcpy_struct.srcPitch = frame->linesize[0];
    memcpy_struct.dstDevice = (CUdeviceptr)frame->data[0];
    memcpy_struct.dstPitch = frame->linesize[0];
    memcpy_struct.WidthInBytes = frame->width * 4;
    memcpy_struct.Height = frame->height;
    cap_kms->cuda.cuMemcpy2D_v2(&memcpy_struct);

    gsr_capture_kms_unload_cuda_graphics(cap_kms);

    return 0;
}

static void gsr_capture_kms_cuda_capture_end(gsr_capture *cap, AVFrame *frame) {
    (void)frame;
    gsr_capture_kms_cuda *cap_kms = cap->priv;

    gsr_egl_cleanup_frame(cap_kms->params.egl);

    for(int i = 0; i < cap_kms->kms_response.num_fds; ++i) {
        if(cap_kms->kms_response.fds[i].fd > 0)
            close(cap_kms->kms_response.fds[i].fd);
        cap_kms->kms_response.fds[i].fd = 0;
    }
    cap_kms->kms_response.num_fds = 0;
}

static void gsr_capture_kms_cuda_stop(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_kms_cuda *cap_kms = cap->priv;

    gsr_capture_kms_unload_cuda_graphics(cap_kms);

    for(int i = 0; i < cap_kms->kms_response.num_fds; ++i) {
        if(cap_kms->kms_response.fds[i].fd > 0)
            close(cap_kms->kms_response.fds[i].fd);
        cap_kms->kms_response.fds[i].fd = 0;
    }
    cap_kms->kms_response.num_fds = 0;

    if(video_codec_context->hw_device_ctx)
        av_buffer_unref(&video_codec_context->hw_device_ctx);
    if(video_codec_context->hw_frames_ctx)
        av_buffer_unref(&video_codec_context->hw_frames_ctx);

    gsr_cuda_unload(&cap_kms->cuda);
    gsr_kms_client_deinit(&cap_kms->kms_client);
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
        .tick = gsr_capture_kms_cuda_tick,
        .should_stop = gsr_capture_kms_cuda_should_stop,
        .capture = gsr_capture_kms_cuda_capture,
        .capture_end = gsr_capture_kms_cuda_capture_end,
        .destroy = gsr_capture_kms_cuda_destroy,
        .priv = cap_kms
    };

    return cap;
}
