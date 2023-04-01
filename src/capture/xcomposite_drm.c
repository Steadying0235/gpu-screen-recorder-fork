#include "../../include/capture/xcomposite_drm.h"
#include "../../include/egl.h"
#include "../../include/window_texture.h"
#include "../../include/time.h"
#include <stdlib.h>
#include <stdio.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>
//#include <drm_fourcc.h>
#include <assert.h>
/* TODO: Proper error checks and cleanups */

typedef struct {
    gsr_capture_xcomposite_drm_params params;
    Display *dpy;
    XEvent xev;
    bool created_hw_frame;

    vec2i window_pos;
    vec2i window_size;
    vec2i texture_size;
    double window_resize_timer;
    
    WindowTexture window_texture;

    gsr_egl egl;

    int fourcc;
    int num_planes;
    uint64_t modifiers;
    int dmabuf_fd;
    int32_t pitch;
    int32_t offset;

    unsigned int target_textures[2];

    VADisplay va_dpy;
    VAConfigID config_id;
    VAContextID context_id;
    VASurfaceID input_surface;
} gsr_capture_xcomposite_drm;

static int max_int(int a, int b) {
    return a > b ? a : b;
}

static bool drm_create_codec_context(gsr_capture_xcomposite_drm *cap_xcomp, AVCodecContext *video_codec_context) {
    AVBufferRef *device_ctx;
    if(av_hwdevice_ctx_create(&device_ctx, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128", NULL, 0) < 0) {
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
    hw_frame_context->sw_format = AV_PIX_FMT_NV12;//AV_PIX_FMT_0RGB32;//AV_PIX_FMT_YUV420P;//AV_PIX_FMT_0RGB32;//AV_PIX_FMT_NV12;
    hw_frame_context->format = video_codec_context->pix_fmt;
    hw_frame_context->device_ref = device_ctx;
    hw_frame_context->device_ctx = (AVHWDeviceContext*)device_ctx->data;

    hw_frame_context->initial_pool_size = 1;

    AVVAAPIDeviceContext *vactx =((AVHWDeviceContext*)device_ctx->data)->hwctx;
    cap_xcomp->va_dpy = vactx->display;

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

#define DRM_FORMAT_MOD_INVALID 72057594037927935

static int gsr_capture_xcomposite_drm_start(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_xcomposite_drm *cap_xcomp = cap->priv;

    XWindowAttributes attr;
    if(!XGetWindowAttributes(cap_xcomp->dpy, cap_xcomp->params.window, &attr)) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_drm_start failed: invalid window id: %lu\n", cap_xcomp->params.window);
        return -1;
    }

    cap_xcomp->window_size.x = max_int(attr.width, 0);
    cap_xcomp->window_size.y = max_int(attr.height, 0);
    Window c;
    XTranslateCoordinates(cap_xcomp->dpy, cap_xcomp->params.window, DefaultRootWindow(cap_xcomp->dpy), 0, 0, &cap_xcomp->window_pos.x, &cap_xcomp->window_pos.y, &c);

    // TODO: Get select and add these on top of it and then restore at the end. Also do the same in other xcomposite
    XSelectInput(cap_xcomp->dpy, cap_xcomp->params.window, StructureNotifyMask | ExposureMask);

    if(!gsr_egl_load(&cap_xcomp->egl, cap_xcomp->dpy)) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_drm_start: failed to load opengl\n");
        return -1;
    }

    if(!cap_xcomp->egl.eglExportDMABUFImageQueryMESA) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_drm_start: could not find eglExportDMABUFImageQueryMESA\n");
        gsr_egl_unload(&cap_xcomp->egl);
        return -1;
    }

    if(!cap_xcomp->egl.eglExportDMABUFImageMESA) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_drm_start: could not find eglExportDMABUFImageMESA\n");
        gsr_egl_unload(&cap_xcomp->egl);
        return -1;
    }

    /* Disable vsync */
    cap_xcomp->egl.eglSwapInterval(cap_xcomp->egl.egl_display, 0);
#if 0
    // TODO: Fallback to composite window
    if(window_texture_init(&cap_xcomp->window_texture, cap_xcomp->dpy, cap_xcomp->params.window, &cap_xcomp->gl) != 0) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_start: failed get window texture for window %ld\n", cap_xcomp->params.window);
        gsr_egl_unload(&cap_xcomp->egl);
        return -1;
    }

    cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, window_texture_get_opengl_texture_id(&cap_xcomp->window_texture));
    cap_xcomp->texture_size.x = 0;
    cap_xcomp->texture_size.y = 0;
    cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &cap_xcomp->texture_size.x);
    cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &cap_xcomp->texture_size.y);
    cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, 0);

    cap_xcomp->texture_size.x = max_int(2, cap_xcomp->texture_size.x & ~1);
    cap_xcomp->texture_size.y = max_int(2, cap_xcomp->texture_size.y & ~1);

    cap_xcomp->target_texture_id = gl_create_texture(cap_xcomp, cap_xcomp->texture_size.x, cap_xcomp->texture_size.y);
    if(cap_xcomp->target_texture_id == 0) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_start: failed to create opengl texture\n");
        gsr_capture_xcomposite_stop(cap, video_codec_context);
        return -1;
    }

    video_codec_context->width = cap_xcomp->texture_size.x;
    video_codec_context->height = cap_xcomp->texture_size.y;

    cap_xcomp->window_resize_timer = clock_get_monotonic_seconds();
    return 0;
#else
    // TODO: Fallback to composite window
    if(window_texture_init(&cap_xcomp->window_texture, cap_xcomp->dpy, cap_xcomp->params.window, &cap_xcomp->egl) != 0) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_drm_start: failed get window texture for window %ld\n", cap_xcomp->params.window);
        gsr_egl_unload(&cap_xcomp->egl);
        return -1;
    }

    cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, window_texture_get_opengl_texture_id(&cap_xcomp->window_texture));
    cap_xcomp->texture_size.x = 0;
    cap_xcomp->texture_size.y = 0;
    cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &cap_xcomp->texture_size.x);
    cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &cap_xcomp->texture_size.y);
    cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, 0);

    cap_xcomp->texture_size.x = max_int(2, cap_xcomp->texture_size.x & ~1);
    cap_xcomp->texture_size.y = max_int(2, cap_xcomp->texture_size.y & ~1);

    video_codec_context->width = cap_xcomp->texture_size.x;
    video_codec_context->height = cap_xcomp->texture_size.y;

    {
        const intptr_t pixmap_attrs[] = {
            EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
            EGL_NONE,
        };

        EGLImage img = cap_xcomp->egl.eglCreateImage(cap_xcomp->egl.egl_display, cap_xcomp->egl.egl_context, EGL_GL_TEXTURE_2D, (EGLClientBuffer)(uint64_t)window_texture_get_opengl_texture_id(&cap_xcomp->window_texture), pixmap_attrs);
        if(!img) {
            fprintf(stderr, "eglCreateImage failed\n");
            return -1;
        }

        if(!cap_xcomp->egl.eglExportDMABUFImageQueryMESA(cap_xcomp->egl.egl_display, img, &cap_xcomp->fourcc, &cap_xcomp->num_planes, &cap_xcomp->modifiers)) {
            fprintf(stderr, "eglExportDMABUFImageQueryMESA failed\n"); 
            return -1;
        }

        if(cap_xcomp->num_planes != 1) {
            // TODO: FAIL!
            fprintf(stderr, "Blablalba\n");
            return -1;
        }

        if(!cap_xcomp->egl.eglExportDMABUFImageMESA(cap_xcomp->egl.egl_display, img, &cap_xcomp->dmabuf_fd, &cap_xcomp->pitch, &cap_xcomp->offset)) {
            fprintf(stderr, "eglExportDMABUFImageMESA failed\n");
            return -1;
        }

        fprintf(stderr, "texture: %u, dmabuf: %d, pitch: %d, offset: %d\n", window_texture_get_opengl_texture_id(&cap_xcomp->window_texture), cap_xcomp->dmabuf_fd, cap_xcomp->pitch, cap_xcomp->offset);
        fprintf(stderr, "fourcc: %d, num planes: %d, modifiers: %zu\n", cap_xcomp->fourcc, cap_xcomp->num_planes, cap_xcomp->modifiers);
    }

    if(!drm_create_codec_context(cap_xcomp, video_codec_context)) {
        fprintf(stderr, "failed to create hw codec context\n");
        gsr_egl_unload(&cap_xcomp->egl);
        return -1;
    }

    //fprintf(stderr, "sneed: %u\n", cap_xcomp->FramebufferName);
    return 0;
#endif
}

static void gsr_capture_xcomposite_drm_tick(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame **frame) {
    gsr_capture_xcomposite_drm *cap_xcomp = cap->priv;

    cap_xcomp->egl.glClear(GL_COLOR_BUFFER_BIT);

    if(!cap_xcomp->created_hw_frame) {
        cap_xcomp->created_hw_frame = true;

        av_frame_free(frame);
        *frame = av_frame_alloc();
        if(!frame) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_tick: failed to allocate frame\n");
            return;
        }
        (*frame)->format = video_codec_context->pix_fmt;
        (*frame)->width = video_codec_context->width;
        (*frame)->height = video_codec_context->height;
        (*frame)->color_range = AVCOL_RANGE_JPEG;

        int res = av_hwframe_get_buffer(video_codec_context->hw_frames_ctx, *frame, 0);
        if(res < 0) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_tick: av_hwframe_get_buffer failed 1: %d\n", res);
            return;
        }

        fprintf(stderr, "fourcc: %u\n", cap_xcomp->fourcc);
        fprintf(stderr, "va surface id: %u\n", (VASurfaceID)(uintptr_t)(*frame)->data[3]);

        int xx = 0, yy = 0;
        cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, window_texture_get_opengl_texture_id(&cap_xcomp->window_texture));
        cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &xx);
        cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &yy);
        cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, 0);

        uintptr_t dmabuf = cap_xcomp->dmabuf_fd;

        VASurfaceAttribExternalBuffers buf = {0};
        buf.pixel_format = VA_FOURCC_BGRX; // TODO: VA_FOURCC_XRGB?
        buf.width = xx;
        buf.height = yy;
        buf.data_size = yy * cap_xcomp->pitch;
        buf.num_planes = 1;
        buf.pitches[0] = cap_xcomp->pitch;
        buf.offsets[0] = cap_xcomp->offset;
        buf.buffers = &dmabuf;
        buf.num_buffers = 1;
        buf.flags = 0;
        buf.private_data = 0;

        #define VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME        0x20000000

        VASurfaceAttrib attribs[2] = {0};
        attribs[0].type = VASurfaceAttribMemoryType;
        attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
        attribs[0].value.type = VAGenericValueTypeInteger;
        attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME; // TODO: prime1 instead?
        attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
        attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
        attribs[1].value.type = VAGenericValueTypePointer;
        attribs[1].value.value.p = &buf;
        
        VAStatus va_status = vaCreateSurfaces(cap_xcomp->va_dpy, VA_RT_FORMAT_RGB32, xx, yy, &cap_xcomp->input_surface, 1, attribs, 2);
        if(va_status != VA_STATUS_SUCCESS) {
            fprintf(stderr, "failed to create surface: %d\n", va_status);
            abort();
            return;
        }

        //vaBeginPicture(cap_xcomp->va_dpy, )

        va_status = vaCreateConfig(cap_xcomp->va_dpy, VAProfileNone, VAEntrypointVideoProc, NULL, 0, &cap_xcomp->config_id);
        if(va_status != VA_STATUS_SUCCESS) {
            fprintf(stderr, "vaCreateConfig failed: %d\n", va_status);
            abort();
            return;
        }

        VASurfaceID target_surface_id = (uintptr_t)(*frame)->data[3];
        va_status = vaCreateContext(cap_xcomp->va_dpy, cap_xcomp->config_id, xx, yy, VA_PROGRESSIVE, &target_surface_id, 1, &cap_xcomp->context_id);
        if(va_status != VA_STATUS_SUCCESS) {
            fprintf(stderr, "vaCreateContext failed: %d\n", va_status);
            abort();
            return;
        }

        // Clear texture with black background because the source texture (window_texture_get_opengl_texture_id(&cap_xcomp->window_texture))
        // might be smaller than cap_xcomp->target_texture_id
        // TODO:
        //cap_xcomp->egl.glClearTexImage(cap_xcomp->target_texture_id, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
}

static bool gsr_capture_xcomposite_drm_should_stop(gsr_capture *cap, bool *err) {
    return false;
}

#define GL_FLOAT				0x1406
#define GL_FALSE				0
#define GL_TRUE					1
#define GL_TRIANGLES				0x0004

static int gsr_capture_xcomposite_drm_capture(gsr_capture *cap, AVFrame *frame) {
    gsr_capture_xcomposite_drm *cap_xcomp = cap->priv;

    VASurfaceID target_surface_id = (uintptr_t)frame->data[3];

    VAStatus va_status = vaBeginPicture(cap_xcomp->va_dpy, cap_xcomp->context_id, target_surface_id);
    if(va_status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaBeginPicture failed: %d\n", va_status);
        abort();
        return 1;
    }

    VAProcPipelineParameterBuffer params = {0};
    params.surface = cap_xcomp->input_surface;
    params.surface_region = NULL;
    params.output_background_color = 0xFF000000;
    params.filter_flags = VA_FRAME_PICTURE;
    // TODO: Colors

    VABufferID buffer_id = 0;
    va_status = vaCreateBuffer(cap_xcomp->va_dpy, cap_xcomp->context_id, VAProcPipelineParameterBufferType, sizeof(params), 1, &params, &buffer_id);
    if(va_status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaCreateBuffer failed: %d\n", va_status);
        return 1;
    }

    va_status = vaRenderPicture(cap_xcomp->va_dpy, cap_xcomp->context_id, &buffer_id, 1);
    if(va_status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaRenderPicture failed: %d\n", va_status);
        return 1;
    }

    va_status = vaEndPicture(cap_xcomp->va_dpy, cap_xcomp->context_id);
    if(va_status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaEndPicture failed: %d\n", va_status);
        return 1;
    }

    // TODO: Needed?
    //vaSyncSurface(cap_xcomp->va_dpy, target_surface_id);

    // TODO: Remove
    //cap_xcomp->egl.eglSwapBuffers(cap_xcomp->egl.egl_display, cap_xcomp->egl.egl_surface);

    return 0;
}

static void gsr_capture_xcomposite_drm_destroy(gsr_capture *cap, AVCodecContext *video_codec_context) {
    (void)video_codec_context;
    gsr_capture_xcomposite_drm *cap_xcomp = cap->priv;
    if(cap->priv) {
        free(cap->priv);
        cap->priv = NULL;
    }
    if(cap_xcomp->dpy) {
        // TODO: This causes a crash, why? maybe some other library dlclose xlib and that also happened to unload this???
        //XCloseDisplay(cap_xcomp->dpy);
        cap_xcomp->dpy = NULL;
    }
    free(cap);
}

gsr_capture* gsr_capture_xcomposite_drm_create(const gsr_capture_xcomposite_drm_params *params) {
    if(!params) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_drm_create params is NULL\n");
        return NULL;
    }

    gsr_capture *cap = calloc(1, sizeof(gsr_capture));
    if(!cap)
        return NULL;

    gsr_capture_xcomposite_drm *cap_xcomp = calloc(1, sizeof(gsr_capture_xcomposite_drm));
    if(!cap_xcomp) {
        free(cap);
        return NULL;
    }

    Display *display = XOpenDisplay(NULL);
    if(!display) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_drm_create failed: XOpenDisplay failed\n");
        free(cap);
        free(cap_xcomp);
        return NULL;
    }

    cap_xcomp->dpy = display;
    cap_xcomp->params = *params;
    
    *cap = (gsr_capture) {
        .start = gsr_capture_xcomposite_drm_start,
        .tick = gsr_capture_xcomposite_drm_tick,
        .should_stop = gsr_capture_xcomposite_drm_should_stop,
        .capture = gsr_capture_xcomposite_drm_capture,
        .destroy = gsr_capture_xcomposite_drm_destroy,
        .priv = cap_xcomp
    };

    return cap;
}
