#include "../../include/capture/nvfbc.h"
#include "../../external/NvFBC.h"
#include "../../include/cuda.h"
#include "../../include/egl.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <X11/Xlib.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/frame.h>
#include <libavutil/version.h>
#include <libavcodec/avcodec.h>

typedef struct {
    gsr_capture_base base;
    gsr_capture_nvfbc_params params;
    void *library;

    NVFBC_SESSION_HANDLE nv_fbc_handle;
    PNVFBCCREATEINSTANCE nv_fbc_create_instance;
    NVFBC_API_FUNCTION_LIST nv_fbc_function_list;
    bool fbc_handle_created;
    bool capture_session_created;

    gsr_cuda cuda;
    CUgraphicsResource cuda_graphics_resources[2];
    CUarray mapped_arrays[2];
    CUstream cuda_stream; // TODO: asdasdsa
    NVFBC_TOGL_SETUP_PARAMS setup_params;
} gsr_capture_nvfbc;

#if defined(_WIN64) || defined(__LP64__)
typedef unsigned long long CUdeviceptr_v2;
#else
typedef unsigned int CUdeviceptr_v2;
#endif
typedef CUdeviceptr_v2 CUdeviceptr;

static int max_int(int a, int b) {
    return a > b ? a : b;
}

/* Returns 0 on failure */
static uint32_t get_output_id_from_display_name(NVFBC_RANDR_OUTPUT_INFO *outputs, uint32_t num_outputs, const char *display_name, uint32_t *width, uint32_t *height) {
    if(!outputs)
        return 0;

    for(uint32_t i = 0; i < num_outputs; ++i) {
        if(strcmp(outputs[i].name, display_name) == 0) {
            *width = outputs[i].trackedBox.w;
            *height = outputs[i].trackedBox.h;
            return outputs[i].dwId;
        }
    }

    return 0;
}

/* TODO: Test with optimus and open kernel modules */
static bool get_driver_version(int *major, int *minor) {
    *major = 0;
    *minor = 0;

    FILE *f = fopen("/proc/driver/nvidia/version", "rb");
    if(!f) {
        fprintf(stderr, "gsr warning: failed to get nvidia driver version (failed to read /proc/driver/nvidia/version)\n");
        return false;
    }

    char buffer[2048];
    size_t bytes_read = fread(buffer, 1, sizeof(buffer) - 1, f);
    buffer[bytes_read] = '\0';

    bool success = false;
    const char *p = strstr(buffer, "Kernel Module");
    if(p) {
        p += 13;
        int driver_major_version = 0, driver_minor_version = 0;
        if(sscanf(p, "%d.%d", &driver_major_version, &driver_minor_version) == 2) {
            *major = driver_major_version;
            *minor = driver_minor_version;
            success = true;
        }
    }

    if(!success)
        fprintf(stderr, "gsr warning: failed to get nvidia driver version\n");

    fclose(f);
    return success;
}

static bool version_at_least(int major, int minor, int expected_major, int expected_minor) {
    return major > expected_major || (major == expected_major && minor >= expected_minor);
}

static bool version_less_than(int major, int minor, int expected_major, int expected_minor) {
    return major < expected_major || (major == expected_major && minor < expected_minor);
}

static void set_func_ptr(void **dst, void *src) {
    *dst = src;
}

static bool gsr_capture_nvfbc_load_library(gsr_capture *cap) {
    gsr_capture_nvfbc *cap_nvfbc = cap->priv;

    dlerror(); /* clear */
    void *lib = dlopen("libnvidia-fbc.so.1", RTLD_LAZY);
    if(!lib) {
        fprintf(stderr, "gsr error: failed to load libnvidia-fbc.so.1, error: %s\n", dlerror());
        return false;
    }

    set_func_ptr((void**)&cap_nvfbc->nv_fbc_create_instance, dlsym(lib, "NvFBCCreateInstance"));
    if(!cap_nvfbc->nv_fbc_create_instance) {
        fprintf(stderr, "gsr error: unable to resolve symbol 'NvFBCCreateInstance'\n");
        dlclose(lib);
        return false;
    }

    memset(&cap_nvfbc->nv_fbc_function_list, 0, sizeof(cap_nvfbc->nv_fbc_function_list));
    cap_nvfbc->nv_fbc_function_list.dwVersion = NVFBC_VERSION;
    NVFBCSTATUS status = cap_nvfbc->nv_fbc_create_instance(&cap_nvfbc->nv_fbc_function_list);
    if(status != NVFBC_SUCCESS) {
        fprintf(stderr, "gsr error: failed to create NvFBC instance (status: %d)\n", status);
        dlclose(lib);
        return false;
    }

    cap_nvfbc->library = lib;
    return true;
}

/* TODO: check for glx swap control extension string (GLX_EXT_swap_control, etc) */
static void set_vertical_sync_enabled(gsr_egl *egl, int enabled) {
    int result = 0;

    if(egl->glXSwapIntervalEXT) {
        egl->glXSwapIntervalEXT(egl->x11.dpy, egl->x11.window, enabled ? 1 : 0);
    } else if(egl->glXSwapIntervalMESA) {
        result = egl->glXSwapIntervalMESA(enabled ? 1 : 0);
    } else if(egl->glXSwapIntervalSGI) {
        result = egl->glXSwapIntervalSGI(enabled ? 1 : 0);
    } else {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "gsr warning: setting vertical sync not supported\n");
        }
    }

    if(result != 0)
        fprintf(stderr, "gsr warning: setting vertical sync failed\n");
}

static int gsr_capture_nvfbc_start(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame *frame) {
    gsr_capture_nvfbc *cap_nvfbc = cap->priv;

    cap_nvfbc->base.video_codec_context = video_codec_context;
    cap_nvfbc->base.egl = cap_nvfbc->params.egl;

    if(!gsr_cuda_load(&cap_nvfbc->cuda, cap_nvfbc->params.egl->x11.dpy, cap_nvfbc->params.overclock))
        return -1;

    if(!gsr_capture_nvfbc_load_library(cap)) {
        gsr_cuda_unload(&cap_nvfbc->cuda);
        return -1;
    }

    const uint32_t x = max_int(cap_nvfbc->params.pos.x, 0);
    const uint32_t y = max_int(cap_nvfbc->params.pos.y, 0);
    const uint32_t width = max_int(cap_nvfbc->params.size.x, 0);
    const uint32_t height = max_int(cap_nvfbc->params.size.y, 0);

    const bool capture_region = (x > 0 || y > 0 || width > 0 || height > 0);

    bool supports_direct_cursor = false;
    bool direct_capture = cap_nvfbc->params.direct_capture;
    int driver_major_version = 0;
    int driver_minor_version = 0;
    if(direct_capture && get_driver_version(&driver_major_version, &driver_minor_version)) {
        fprintf(stderr, "Info: detected nvidia version: %d.%d\n", driver_major_version, driver_minor_version);

        // TODO:
        if(version_at_least(driver_major_version, driver_minor_version, 515, 57) && version_less_than(driver_major_version, driver_minor_version, 520, 56)) {
            direct_capture = false;
            fprintf(stderr, "Warning: \"screen-direct\" has temporary been disabled as it causes stuttering with driver versions >= 515.57 and < 520.56. Please update your driver if possible. Capturing \"screen\" instead.\n");
        }

        // TODO:
        // Cursor capture disabled because moving the cursor doesn't update capture rate to monitor hz and instead captures at 10-30 hz
        /*
        if(direct_capture) {
            if(version_at_least(driver_major_version, driver_minor_version, 515, 57))
                supports_direct_cursor = true;
            else
                fprintf(stderr, "Info: capturing \"screen-direct\" but driver version appears to be less than 515.57. Disabling capture of cursor. Please update your driver if you want to capture your cursor or record \"screen\" instead.\n");
        }
        */
    }

    NVFBCSTATUS status;
    NVFBC_TRACKING_TYPE tracking_type;
    uint32_t output_id = 0;
    cap_nvfbc->fbc_handle_created = false;
    cap_nvfbc->capture_session_created = false;

    NVFBC_CREATE_HANDLE_PARAMS create_params;
    memset(&create_params, 0, sizeof(create_params));
    create_params.dwVersion = NVFBC_CREATE_HANDLE_PARAMS_VER;
    create_params.bExternallyManagedContext = NVFBC_TRUE;
    create_params.glxCtx = cap_nvfbc->params.egl->glx_context;
    create_params.glxFBConfig = cap_nvfbc->params.egl->glx_fb_config;

    status = cap_nvfbc->nv_fbc_function_list.nvFBCCreateHandle(&cap_nvfbc->nv_fbc_handle, &create_params);
    if(status != NVFBC_SUCCESS) {
        // Reverse engineering for interoperability
        const uint8_t enable_key[] = { 0xac, 0x10, 0xc9, 0x2e, 0xa5, 0xe6, 0x87, 0x4f, 0x8f, 0x4b, 0xf4, 0x61, 0xf8, 0x56, 0x27, 0xe9 };
        create_params.privateData = enable_key;
        create_params.privateDataSize = 16;

        status = cap_nvfbc->nv_fbc_function_list.nvFBCCreateHandle(&cap_nvfbc->nv_fbc_handle, &create_params);
        if(status != NVFBC_SUCCESS) {
            fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: %s\n", cap_nvfbc->nv_fbc_function_list.nvFBCGetLastErrorStr(cap_nvfbc->nv_fbc_handle));
            goto error_cleanup;
        }
    }
    cap_nvfbc->fbc_handle_created = true;

    NVFBC_GET_STATUS_PARAMS status_params;
    memset(&status_params, 0, sizeof(status_params));
    status_params.dwVersion = NVFBC_GET_STATUS_PARAMS_VER;

    status = cap_nvfbc->nv_fbc_function_list.nvFBCGetStatus(cap_nvfbc->nv_fbc_handle, &status_params);
    if(status != NVFBC_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: %s\n", cap_nvfbc->nv_fbc_function_list.nvFBCGetLastErrorStr(cap_nvfbc->nv_fbc_handle));
        goto error_cleanup;
    }

    if(status_params.bCanCreateNow == NVFBC_FALSE) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: it's not possible to create a capture session on this system\n");
        goto error_cleanup;
    }

    uint32_t tracking_width = XWidthOfScreen(DefaultScreenOfDisplay(cap_nvfbc->params.egl->x11.dpy));
    uint32_t tracking_height = XHeightOfScreen(DefaultScreenOfDisplay(cap_nvfbc->params.egl->x11.dpy));
    tracking_type = strcmp(cap_nvfbc->params.display_to_capture, "screen") == 0 ? NVFBC_TRACKING_SCREEN : NVFBC_TRACKING_OUTPUT;
    if(tracking_type == NVFBC_TRACKING_OUTPUT) {
        if(!status_params.bXRandRAvailable) {
            fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: the xrandr extension is not available\n");
            goto error_cleanup;
        }

        if(status_params.bInModeset) {
            fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: the x server is in modeset, unable to record\n");
            goto error_cleanup;
        }

        output_id = get_output_id_from_display_name(status_params.outputs, status_params.dwOutputNum, cap_nvfbc->params.display_to_capture, &tracking_width, &tracking_height);
        if(output_id == 0) {
            fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: display '%s' not found\n", cap_nvfbc->params.display_to_capture);
            goto error_cleanup;
        }
    }

    NVFBC_CREATE_CAPTURE_SESSION_PARAMS create_capture_params;
    memset(&create_capture_params, 0, sizeof(create_capture_params));
    create_capture_params.dwVersion = NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER;
    create_capture_params.eCaptureType = NVFBC_CAPTURE_TO_GL;
    create_capture_params.bWithCursor = (!direct_capture || supports_direct_cursor) ? NVFBC_TRUE : NVFBC_FALSE;
    if(capture_region)
        create_capture_params.captureBox = (NVFBC_BOX){ x, y, width, height };
    create_capture_params.eTrackingType = tracking_type;
    create_capture_params.dwSamplingRateMs = (uint32_t)ceilf(1000.0f / (float)cap_nvfbc->params.fps);
    create_capture_params.bAllowDirectCapture = direct_capture ? NVFBC_TRUE : NVFBC_FALSE;
    create_capture_params.bPushModel = direct_capture ? NVFBC_TRUE : NVFBC_FALSE;
    //create_capture_params.bDisableAutoModesetRecovery = true; // TODO:
    if(tracking_type == NVFBC_TRACKING_OUTPUT)
        create_capture_params.dwOutputId = output_id;

    status = cap_nvfbc->nv_fbc_function_list.nvFBCCreateCaptureSession(cap_nvfbc->nv_fbc_handle, &create_capture_params);
    if(status != NVFBC_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: %s\n", cap_nvfbc->nv_fbc_function_list.nvFBCGetLastErrorStr(cap_nvfbc->nv_fbc_handle));
        goto error_cleanup;
    }
    cap_nvfbc->capture_session_created = true;

    memset(&cap_nvfbc->setup_params, 0, sizeof(cap_nvfbc->setup_params));
    cap_nvfbc->setup_params.dwVersion = NVFBC_TOGL_SETUP_PARAMS_VER;
    cap_nvfbc->setup_params.eBufferFormat = NVFBC_BUFFER_FORMAT_BGRA;

    status = cap_nvfbc->nv_fbc_function_list.nvFBCToGLSetUp(cap_nvfbc->nv_fbc_handle, &cap_nvfbc->setup_params);
    if(status != NVFBC_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: %s\n", cap_nvfbc->nv_fbc_function_list.nvFBCGetLastErrorStr(cap_nvfbc->nv_fbc_handle));
        goto error_cleanup;
    }

    if(capture_region) {
        video_codec_context->width = width & ~1;
        video_codec_context->height = height & ~1;
    } else {
        video_codec_context->width = tracking_width & ~1;
        video_codec_context->height = tracking_height & ~1;
    }

    frame->width = video_codec_context->width;
    frame->height = video_codec_context->height;

    if(!cuda_create_codec_context(cap_nvfbc->cuda.cu_ctx, video_codec_context, video_codec_context->width, video_codec_context->height, false, &cap_nvfbc->cuda_stream))
        goto error_cleanup;

    gsr_cuda_context cuda_context = {
        .cuda = &cap_nvfbc->cuda,
        .cuda_graphics_resources = cap_nvfbc->cuda_graphics_resources,
        .mapped_arrays = cap_nvfbc->mapped_arrays
    };

    // TODO: Remove this, it creates shit we dont need
    if(!gsr_capture_base_setup_cuda_textures(&cap_nvfbc->base, frame, &cuda_context, cap_nvfbc->params.color_range, GSR_SOURCE_COLOR_BGR, cap_nvfbc->params.hdr)) {
        goto error_cleanup;
    }
    /* Disable vsync */
    set_vertical_sync_enabled(cap_nvfbc->params.egl, 0);

    return 0;

    error_cleanup:
    if(cap_nvfbc->fbc_handle_created) {
        if(cap_nvfbc->capture_session_created) {
            NVFBC_DESTROY_CAPTURE_SESSION_PARAMS destroy_capture_params;
            memset(&destroy_capture_params, 0, sizeof(destroy_capture_params));
            destroy_capture_params.dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER;
            cap_nvfbc->nv_fbc_function_list.nvFBCDestroyCaptureSession(cap_nvfbc->nv_fbc_handle, &destroy_capture_params);
            cap_nvfbc->capture_session_created = false;
        }

        NVFBC_DESTROY_HANDLE_PARAMS destroy_params;
        memset(&destroy_params, 0, sizeof(destroy_params));
        destroy_params.dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER;
        cap_nvfbc->nv_fbc_function_list.nvFBCDestroyHandle(cap_nvfbc->nv_fbc_handle, &destroy_params);
        cap_nvfbc->fbc_handle_created = false;
    }

    gsr_capture_base_stop(&cap_nvfbc->base);
    gsr_cuda_unload(&cap_nvfbc->cuda);
    return -1;
}

static void gsr_capture_nvfbc_destroy_session(gsr_capture *cap) {
    gsr_capture_nvfbc *cap_nvfbc = cap->priv;

    if(cap_nvfbc->fbc_handle_created) {
        if(cap_nvfbc->capture_session_created) {
            NVFBC_DESTROY_CAPTURE_SESSION_PARAMS destroy_capture_params;
            memset(&destroy_capture_params, 0, sizeof(destroy_capture_params));
            destroy_capture_params.dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER;
            cap_nvfbc->nv_fbc_function_list.nvFBCDestroyCaptureSession(cap_nvfbc->nv_fbc_handle, &destroy_capture_params);
            cap_nvfbc->capture_session_created = false;
        }

        NVFBC_DESTROY_HANDLE_PARAMS destroy_params;
        memset(&destroy_params, 0, sizeof(destroy_params));
        destroy_params.dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER;
        cap_nvfbc->nv_fbc_function_list.nvFBCDestroyHandle(cap_nvfbc->nv_fbc_handle, &destroy_params);
        cap_nvfbc->fbc_handle_created = false;
    }

    cap_nvfbc->nv_fbc_handle = 0;
}

static int gsr_capture_nvfbc_capture(gsr_capture *cap, AVFrame *frame) {
    gsr_capture_nvfbc *cap_nvfbc = cap->priv;

    NVFBC_FRAME_GRAB_INFO frame_info;
    memset(&frame_info, 0, sizeof(frame_info));

    NVFBC_TOGL_GRAB_FRAME_PARAMS grab_params;
    memset(&grab_params, 0, sizeof(grab_params));
    grab_params.dwVersion = NVFBC_TOGL_GRAB_FRAME_PARAMS_VER;
    grab_params.dwFlags = NVFBC_TOGL_GRAB_FLAGS_NOWAIT | NVFBC_TOGL_GRAB_FLAGS_FORCE_REFRESH; // TODO: Remove NVFBC_TOGL_GRAB_FLAGS_FORCE_REFRESH
    grab_params.pFrameGrabInfo = &frame_info;
    grab_params.dwTimeoutMs = 0;

    NVFBCSTATUS status = cap_nvfbc->nv_fbc_function_list.nvFBCToGLGrabFrame(cap_nvfbc->nv_fbc_handle, &grab_params);
    if(status != NVFBC_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_capture failed: %s\n", cap_nvfbc->nv_fbc_function_list.nvFBCGetLastErrorStr(cap_nvfbc->nv_fbc_handle));
        return -1;
    }

    //cap_nvfbc->params.egl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    cap_nvfbc->params.egl->glClear(0);

    gsr_color_conversion_draw(&cap_nvfbc->base.color_conversion, cap_nvfbc->setup_params.dwTextures[grab_params.dwTextureIndex],
        (vec2i){0, 0}, (vec2i){frame->width, frame->height},
        (vec2i){0, 0}, (vec2i){frame->width, frame->height},
        0.0f, false);

    cap_nvfbc->params.egl->glXSwapBuffers(cap_nvfbc->params.egl->x11.dpy, cap_nvfbc->params.egl->x11.window);

    // TODO: HDR is broken
    const int div[2] = {1, 2}; // divide UV texture size by 2 because chroma is half size
    for(int i = 0; i < 2; ++i) {
        CUDA_MEMCPY2D memcpy_struct;
        memcpy_struct.srcXInBytes = 0;
        memcpy_struct.srcY = 0;
        memcpy_struct.srcMemoryType = CU_MEMORYTYPE_ARRAY;

        memcpy_struct.dstXInBytes = 0;
        memcpy_struct.dstY = 0;
        memcpy_struct.dstMemoryType = CU_MEMORYTYPE_DEVICE;

        memcpy_struct.srcArray = cap_nvfbc->mapped_arrays[i];
        memcpy_struct.srcPitch = frame->width / div[i];
        memcpy_struct.dstDevice = (CUdeviceptr)frame->data[i];
        memcpy_struct.dstPitch = frame->linesize[i];
        memcpy_struct.WidthInBytes = frame->width * (cap_nvfbc->params.hdr ? 2 : 1);
        memcpy_struct.Height = frame->height / div[i];
        // TODO: Remove this copy if possible
        cap_nvfbc->cuda.cuMemcpy2DAsync_v2(&memcpy_struct, cap_nvfbc->cuda_stream);
    }

    // TODO: needed?
    cap_nvfbc->cuda.cuStreamSynchronize(cap_nvfbc->cuda_stream);

    return 0;
}

static void gsr_capture_nvfbc_destroy(gsr_capture *cap, AVCodecContext *video_codec_context) {
    (void)video_codec_context;
    gsr_capture_nvfbc *cap_nvfbc = cap->priv;
    gsr_capture_nvfbc_destroy_session(cap);
    if(cap_nvfbc) {
        gsr_capture_base_stop(&cap_nvfbc->base);
        gsr_cuda_unload(&cap_nvfbc->cuda);
        dlclose(cap_nvfbc->library);
        free((void*)cap_nvfbc->params.display_to_capture);
        cap_nvfbc->params.display_to_capture = NULL;
        free(cap->priv);
        cap->priv = NULL;
    }
    free(cap);
}

gsr_capture* gsr_capture_nvfbc_create(const gsr_capture_nvfbc_params *params) {
    if(!params) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_create params is NULL\n");
        return NULL;
    }

    if(!params->display_to_capture) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_create params.display_to_capture is NULL\n");
        return NULL;
    }

    gsr_capture *cap = calloc(1, sizeof(gsr_capture));
    if(!cap)
        return NULL;

    gsr_capture_nvfbc *cap_nvfbc = calloc(1, sizeof(gsr_capture_nvfbc));
    if(!cap_nvfbc) {
        free(cap);
        return NULL;
    }

    const char *display_to_capture = strdup(params->display_to_capture);
    if(!display_to_capture) {
        free(cap);
        free(cap_nvfbc);
        return NULL;
    }

    cap_nvfbc->params = *params;
    cap_nvfbc->params.display_to_capture = display_to_capture;
    cap_nvfbc->params.fps = max_int(cap_nvfbc->params.fps, 1);
    
    *cap = (gsr_capture) {
        .start = gsr_capture_nvfbc_start,
        .tick = NULL,
        .should_stop = NULL,
        .capture = gsr_capture_nvfbc_capture,
        .capture_end = NULL,
        .destroy = gsr_capture_nvfbc_destroy,
        .priv = cap_nvfbc
    };

    return cap;
}
