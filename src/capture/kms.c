#include "../../include/capture/kms.h"
#include "../../include/utils.h"
#include "../../include/color_conversion.h"
#include "../../kms/client/kms_client.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/mastering_display_metadata.h>

#define HDMI_STATIC_METADATA_TYPE1 0
#define HDMI_EOTF_SMPTE_ST2084 2

#define MAX_CONNECTOR_IDS 32

typedef struct {
    uint32_t connector_ids[MAX_CONNECTOR_IDS];
    int num_connector_ids;
} MonitorId;

typedef struct {
    gsr_capture_kms_params params;

    bool should_stop;
    bool stop_is_error;
    
    gsr_kms_client kms_client;
    gsr_kms_response kms_response;

    vec2i capture_pos;
    vec2i capture_size;
    MonitorId monitor_id;

    AVMasteringDisplayMetadata *mastering_display_metadata;
    AVContentLightMetadata *light_metadata;

    gsr_monitor_rotation monitor_rotation;

    unsigned int input_texture;
    unsigned int cursor_texture;
} gsr_capture_kms;

static void gsr_capture_kms_cleanup_kms_fds(gsr_capture_kms *self) {
    for(int i = 0; i < self->kms_response.num_fds; ++i) {
        if(self->kms_response.fds[i].fd > 0)
            close(self->kms_response.fds[i].fd);
        self->kms_response.fds[i].fd = 0;
    }
    self->kms_response.num_fds = 0;
}

static void gsr_capture_kms_stop(gsr_capture_kms *self) {
    if(self->input_texture) {
        self->params.egl->glDeleteTextures(1, &self->input_texture);
        self->input_texture = 0;
    }

    if(self->cursor_texture) {
        self->params.egl->glDeleteTextures(1, &self->cursor_texture);
        self->cursor_texture = 0;
    }

    gsr_capture_kms_cleanup_kms_fds(self);
    gsr_kms_client_deinit(&self->kms_client);
}

static int max_int(int a, int b) {
    return a > b ? a : b;
}

static void gsr_capture_kms_create_input_textures(gsr_capture_kms *self) {
    self->params.egl->glGenTextures(1, &self->input_texture);
    self->params.egl->glBindTexture(GL_TEXTURE_2D, self->input_texture);
    self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    self->params.egl->glBindTexture(GL_TEXTURE_2D, 0);

    self->params.egl->glGenTextures(1, &self->cursor_texture);
    self->params.egl->glBindTexture(GL_TEXTURE_2D, self->cursor_texture);
    self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    self->params.egl->glBindTexture(GL_TEXTURE_2D, 0);
}

/* TODO: On monitor reconfiguration, find monitor x, y, width and height again. Do the same for nvfbc. */

typedef struct {
    MonitorId *monitor_id;
    const char *monitor_to_capture;
    int monitor_to_capture_len;
    int num_monitors;
} MonitorCallbackUserdata;

static void monitor_callback(const gsr_monitor *monitor, void *userdata) {
    MonitorCallbackUserdata *monitor_callback_userdata = userdata;
    ++monitor_callback_userdata->num_monitors;

    if(monitor_callback_userdata->monitor_to_capture_len != monitor->name_len || memcmp(monitor_callback_userdata->monitor_to_capture, monitor->name, monitor->name_len) != 0)
        return;

    if(monitor_callback_userdata->monitor_id->num_connector_ids < MAX_CONNECTOR_IDS) {
        monitor_callback_userdata->monitor_id->connector_ids[monitor_callback_userdata->monitor_id->num_connector_ids] = monitor->connector_id;
        ++monitor_callback_userdata->monitor_id->num_connector_ids;
    }

    if(monitor_callback_userdata->monitor_id->num_connector_ids == MAX_CONNECTOR_IDS)
        fprintf(stderr, "gsr warning: reached max connector ids\n");
}

static int gsr_capture_kms_start(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame *frame) {
    gsr_capture_kms *self = cap->priv;

    gsr_capture_kms_create_input_textures(self);

    gsr_monitor monitor;
    self->monitor_id.num_connector_ids = 0;

    int kms_init_res = gsr_kms_client_init(&self->kms_client, self->params.egl->card_path);
    if(kms_init_res != 0)
        return kms_init_res;

    MonitorCallbackUserdata monitor_callback_userdata = {
        &self->monitor_id,
        self->params.display_to_capture, strlen(self->params.display_to_capture),
        0,
    };
    for_each_active_monitor_output(self->params.egl, GSR_CONNECTION_DRM, monitor_callback, &monitor_callback_userdata);

    if(!get_monitor_by_name(self->params.egl, GSR_CONNECTION_DRM, self->params.display_to_capture, &monitor)) {
        fprintf(stderr, "gsr error: gsr_capture_kms_start: failed to find monitor by name \"%s\"\n", self->params.display_to_capture);
        gsr_capture_kms_stop(self);
        return -1;
    }

    monitor.name = self->params.display_to_capture;
    self->monitor_rotation = drm_monitor_get_display_server_rotation(self->params.egl, &monitor);

    self->capture_pos = monitor.pos;
    if(self->monitor_rotation == GSR_MONITOR_ROT_90 || self->monitor_rotation == GSR_MONITOR_ROT_270) {
        self->capture_size.x = monitor.size.y;
        self->capture_size.y = monitor.size.x;
    } else {
        self->capture_size = monitor.size;
    }

    /* Disable vsync */
    self->params.egl->eglSwapInterval(self->params.egl->egl_display, 0);

    // TODO: Move this and xcomposite equivalent to a common section unrelated to capture method
    if(self->params.egl->gpu_info.vendor == GSR_GPU_VENDOR_AMD && video_codec_context->codec_id == AV_CODEC_ID_HEVC) {
        // TODO: dont do this if using ffmpeg reports that this is not needed (AMD driver bug that was fixed recently)
        video_codec_context->width = FFALIGN(self->capture_size.x, 64);
        video_codec_context->height = FFALIGN(self->capture_size.y, 16);
    } else if(self->params.egl->gpu_info.vendor == GSR_GPU_VENDOR_AMD && video_codec_context->codec_id == AV_CODEC_ID_AV1) {
        // TODO: Dont do this for VCN 5 and forward which should fix this hardware bug
        video_codec_context->width = FFALIGN(self->capture_size.x, 64);
        // AMD driver has special case handling for 1080 height to set it to 1082 instead of 1088 (1080 aligned to 16).
        // TODO: Set height to 1082 in this case, but it wont work because it will be aligned to 1088.
        if(self->capture_size.y == 1080) {
            video_codec_context->height = 1080;
        } else {
            video_codec_context->height = FFALIGN(self->capture_size.y, 16);
        }
    } else {
        video_codec_context->width = FFALIGN(self->capture_size.x, 2);
        video_codec_context->height = FFALIGN(self->capture_size.y, 2);
    }

    frame->width = video_codec_context->width;
    frame->height = video_codec_context->height;
    return 0;
}

static float monitor_rotation_to_radians(gsr_monitor_rotation rot) {
    switch(rot) {
        case GSR_MONITOR_ROT_0:   return 0.0f;
        case GSR_MONITOR_ROT_90:  return M_PI_2;
        case GSR_MONITOR_ROT_180: return M_PI;
        case GSR_MONITOR_ROT_270: return M_PI + M_PI_2;
    }
    return 0.0f;
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

static gsr_kms_response_fd* find_cursor_drm(gsr_kms_response *kms_response) {
    for(int i = 0; i < kms_response->num_fds; ++i) {
        if(kms_response->fds[i].is_cursor)
            return &kms_response->fds[i];
    }
    return NULL;
}

static bool hdr_metadata_is_supported_format(const struct hdr_output_metadata *hdr_metadata) {
    return hdr_metadata->metadata_type == HDMI_STATIC_METADATA_TYPE1 &&
        hdr_metadata->hdmi_metadata_type1.metadata_type == HDMI_STATIC_METADATA_TYPE1 &&
        hdr_metadata->hdmi_metadata_type1.eotf == HDMI_EOTF_SMPTE_ST2084;
}

static void gsr_kms_set_hdr_metadata(gsr_capture_kms *self, AVFrame *frame, gsr_kms_response_fd *drm_fd) {
    if(!self->mastering_display_metadata)
        self->mastering_display_metadata = av_mastering_display_metadata_create_side_data(frame);

    if(!self->light_metadata)
        self->light_metadata = av_content_light_metadata_create_side_data(frame);

    if(self->mastering_display_metadata) {
        for(int i = 0; i < 3; ++i) {
            self->mastering_display_metadata->display_primaries[i][0] = av_make_q(drm_fd->hdr_metadata.hdmi_metadata_type1.display_primaries[i].x, 50000);
            self->mastering_display_metadata->display_primaries[i][1] = av_make_q(drm_fd->hdr_metadata.hdmi_metadata_type1.display_primaries[i].y, 50000);
        }

        self->mastering_display_metadata->white_point[0] = av_make_q(drm_fd->hdr_metadata.hdmi_metadata_type1.white_point.x, 50000);
        self->mastering_display_metadata->white_point[1] = av_make_q(drm_fd->hdr_metadata.hdmi_metadata_type1.white_point.y, 50000);

        self->mastering_display_metadata->min_luminance = av_make_q(drm_fd->hdr_metadata.hdmi_metadata_type1.min_display_mastering_luminance, 10000);
        self->mastering_display_metadata->max_luminance = av_make_q(drm_fd->hdr_metadata.hdmi_metadata_type1.max_display_mastering_luminance, 1);

        self->mastering_display_metadata->has_primaries = self->mastering_display_metadata->display_primaries[0][0].num > 0;
        self->mastering_display_metadata->has_luminance = self->mastering_display_metadata->max_luminance.num > 0;
    }

    if(self->light_metadata) {
        self->light_metadata->MaxCLL = drm_fd->hdr_metadata.hdmi_metadata_type1.max_cll;
        self->light_metadata->MaxFALL = drm_fd->hdr_metadata.hdmi_metadata_type1.max_fall;
    }
}

static vec2i swap_vec2i(vec2i value) {
    int tmp = value.x;
    value.x = value.y;
    value.y = tmp;
    return value;
}

static int gsr_capture_kms_capture(gsr_capture *cap, AVFrame *frame, gsr_color_conversion *color_conversion) {
    gsr_capture_kms *self = cap->priv;
    const bool screen_plane_use_modifiers = self->params.egl->gpu_info.vendor != GSR_GPU_VENDOR_AMD;
    const bool cursor_texture_is_external = self->params.egl->gpu_info.vendor == GSR_GPU_VENDOR_NVIDIA;

    //egl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    self->params.egl->glClear(0);

    gsr_capture_kms_cleanup_kms_fds(self);

    gsr_kms_response_fd *drm_fd = NULL;
    gsr_kms_response_fd *cursor_drm_fd = NULL;
    bool capture_is_combined_plane = false;

    if(gsr_kms_client_get_kms(&self->kms_client, &self->kms_response) != 0) {
        fprintf(stderr, "gsr error: gsr_capture_kms_capture: failed to get kms, error: %d (%s)\n", self->kms_response.result, self->kms_response.err_msg);
        return -1;
    }

    if(self->kms_response.num_fds == 0) {
        static bool error_shown = false;
        if(!error_shown) {
            error_shown = true;
            fprintf(stderr, "gsr error: no drm found, capture will fail\n");
        }
        return -1;
    }

    for(int i = 0; i < self->monitor_id.num_connector_ids; ++i) {
        drm_fd = find_drm_by_connector_id(&self->kms_response, self->monitor_id.connector_ids[i]);
        if(drm_fd)
            break;
    }

    // Will never happen on wayland unless the target monitor has been disconnected
    if(!drm_fd) {
        drm_fd = find_first_combined_drm(&self->kms_response);
        if(!drm_fd)
            drm_fd = find_largest_drm(&self->kms_response);
        capture_is_combined_plane = true;
    }

    cursor_drm_fd = find_cursor_drm(&self->kms_response);

    if(!drm_fd)
        return -1;

    if(!capture_is_combined_plane && cursor_drm_fd && cursor_drm_fd->connector_id != drm_fd->connector_id)
        cursor_drm_fd = NULL;

    if(drm_fd->has_hdr_metadata && self->params.hdr && hdr_metadata_is_supported_format(&drm_fd->hdr_metadata))
        gsr_kms_set_hdr_metadata(self, frame, drm_fd);

    // TODO: This causes a crash sometimes on steam deck, why? is it a driver bug? a vaapi pure version doesn't cause a crash.
    // Even ffmpeg kmsgrab causes this crash. The error is:
    // amdgpu: Failed to allocate a buffer:
    // amdgpu:    size      : 28508160 bytes
    // amdgpu:    alignment : 2097152 bytes
    // amdgpu:    domains   : 4
    // amdgpu:    flags   : 4
    // amdgpu: Failed to allocate a buffer:
    // amdgpu:    size      : 28508160 bytes
    // amdgpu:    alignment : 2097152 bytes
    // amdgpu:    domains   : 4
    // amdgpu:    flags   : 4
    // EE ../jupiter-mesa/src/gallium/drivers/radeonsi/radeon_vcn_enc.c:516 radeon_create_encoder UVD - Can't create CPB buffer.
    // [hevc_vaapi @ 0x55ea72b09840] Failed to upload encode parameters: 2 (resource allocation failed).
    // [hevc_vaapi @ 0x55ea72b09840] Encode failed: -5.
    // Error: avcodec_send_frame failed, error: Input/output error
    // Assertion pic->display_order == pic->encode_order failed at libavcodec/vaapi_encode_h265.c:765
    // kms server info: kms client shutdown, shutting down the server
    intptr_t img_attr[18] = {
        EGL_LINUX_DRM_FOURCC_EXT,       drm_fd->pixel_format,
        EGL_WIDTH,                      drm_fd->width,
        EGL_HEIGHT,                     drm_fd->height,
        EGL_DMA_BUF_PLANE0_FD_EXT,      drm_fd->fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,  drm_fd->offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,   drm_fd->pitch,
    };

    if(screen_plane_use_modifiers) {
        img_attr[12] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        img_attr[13] = drm_fd->modifier & 0xFFFFFFFFULL;

        img_attr[14] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        img_attr[15] = drm_fd->modifier >> 32ULL;

        img_attr[16] = EGL_NONE;
        img_attr[17] = EGL_NONE;
    } else {
        img_attr[12] = EGL_NONE;
        img_attr[13] = EGL_NONE;
    }

    EGLImage image = self->params.egl->eglCreateImage(self->params.egl->egl_display, 0, EGL_LINUX_DMA_BUF_EXT, NULL, img_attr);
    self->params.egl->glBindTexture(GL_TEXTURE_2D, self->input_texture);
    self->params.egl->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    self->params.egl->eglDestroyImage(self->params.egl->egl_display, image);
    self->params.egl->glBindTexture(GL_TEXTURE_2D, 0);

    vec2i capture_pos = self->capture_pos;
    if(!capture_is_combined_plane)
        capture_pos = (vec2i){drm_fd->x, drm_fd->y};

    const float texture_rotation = monitor_rotation_to_radians(self->monitor_rotation);

    const int target_x = max_int(0, frame->width / 2 - self->capture_size.x / 2);
    const int target_y = max_int(0, frame->height / 2 - self->capture_size.y / 2);

    gsr_color_conversion_draw(color_conversion, self->input_texture,
        (vec2i){target_x, target_y}, self->capture_size,
        capture_pos, self->capture_size,
        texture_rotation, false);

    if(self->params.record_cursor && cursor_drm_fd) {
        const vec2i cursor_size = {cursor_drm_fd->width, cursor_drm_fd->height};
        vec2i cursor_pos = {cursor_drm_fd->x, cursor_drm_fd->y};
        switch(self->monitor_rotation) {
            case GSR_MONITOR_ROT_0:
                break;
            case GSR_MONITOR_ROT_90:
                cursor_pos = swap_vec2i(cursor_pos);
                cursor_pos.x = self->capture_size.x - cursor_pos.x;
                // TODO: Remove this horrible hack
                cursor_pos.x -= cursor_size.x;
                break;
            case GSR_MONITOR_ROT_180:
                cursor_pos.x = self->capture_size.x - cursor_pos.x;
                cursor_pos.y = self->capture_size.y - cursor_pos.y;
                // TODO: Remove this horrible hack
                cursor_pos.x -= cursor_size.x;
                cursor_pos.y -= cursor_size.y;
                break;
            case GSR_MONITOR_ROT_270:
                cursor_pos = swap_vec2i(cursor_pos);
                cursor_pos.y = self->capture_size.y - cursor_pos.y;
                // TODO: Remove this horrible hack
                cursor_pos.y -= cursor_size.y;
                break;
        }

        cursor_pos.x += target_x;
        cursor_pos.y += target_y;

        const intptr_t img_attr_cursor[] = {
            EGL_LINUX_DRM_FOURCC_EXT,       cursor_drm_fd->pixel_format,
            EGL_WIDTH,                      cursor_drm_fd->width,
            EGL_HEIGHT,                     cursor_drm_fd->height,
            EGL_DMA_BUF_PLANE0_FD_EXT,      cursor_drm_fd->fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT,  cursor_drm_fd->offset,
            EGL_DMA_BUF_PLANE0_PITCH_EXT,   cursor_drm_fd->pitch,
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, cursor_drm_fd->modifier & 0xFFFFFFFFULL,
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, cursor_drm_fd->modifier >> 32ULL,
            EGL_NONE
        };

        EGLImage cursor_image = self->params.egl->eglCreateImage(self->params.egl->egl_display, 0, EGL_LINUX_DMA_BUF_EXT, NULL, img_attr_cursor);
        const int target = cursor_texture_is_external ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;
        self->params.egl->glBindTexture(target, self->cursor_texture);
        self->params.egl->glEGLImageTargetTexture2DOES(target, cursor_image);
        self->params.egl->eglDestroyImage(self->params.egl->egl_display, cursor_image);
        self->params.egl->glBindTexture(target, 0);

        self->params.egl->glEnable(GL_SCISSOR_TEST);
        self->params.egl->glScissor(target_x, target_y, self->capture_size.x, self->capture_size.y);

        gsr_color_conversion_draw(color_conversion, self->cursor_texture,
            cursor_pos, cursor_size,
            (vec2i){0, 0}, cursor_size,
            texture_rotation, cursor_texture_is_external);

        self->params.egl->glDisable(GL_SCISSOR_TEST);
    }

    self->params.egl->eglSwapBuffers(self->params.egl->egl_display, self->params.egl->egl_surface);
    
    // TODO: Do software specific video encoder conversion here

    //self->params.egl->glFlush();
    //self->params.egl->glFinish();

    return 0;
}

static bool gsr_capture_kms_should_stop(gsr_capture *cap, bool *err) {
    gsr_capture_kms *cap_kms = cap->priv;
    if(cap_kms->should_stop) {
        if(err)
            *err = cap_kms->stop_is_error;
        return true;
    }

    if(err)
        *err = false;
    return false;
}

static void gsr_capture_kms_capture_end(gsr_capture *cap, AVFrame *frame) {
    (void)frame;
    gsr_capture_kms_cleanup_kms_fds(cap->priv);
}

static gsr_source_color gsr_capture_kms_get_source_color(gsr_capture *cap) {
    (void)cap;
    return GSR_SOURCE_COLOR_RGB;
}

static bool gsr_capture_kms_uses_external_image(gsr_capture *cap) {
    gsr_capture_kms *cap_kms = cap->priv;
    return cap_kms->params.egl->gpu_info.vendor == GSR_GPU_VENDOR_NVIDIA;
}

static void gsr_capture_kms_destroy(gsr_capture *cap, AVCodecContext *video_codec_context) {
    (void)video_codec_context;
    gsr_capture_kms *cap_kms = cap->priv;
    if(cap->priv) {
        gsr_capture_kms_stop(cap_kms);
        free((void*)cap_kms->params.display_to_capture);
        cap_kms->params.display_to_capture = NULL;
        free(cap->priv);
        cap->priv = NULL;
    }
    free(cap);
}

gsr_capture* gsr_capture_kms_create(const gsr_capture_kms_params *params) {
    if(!params) {
        fprintf(stderr, "gsr error: gsr_capture_kms_create params is NULL\n");
        return NULL;
    }

    gsr_capture *cap = calloc(1, sizeof(gsr_capture));
    if(!cap)
        return NULL;

    gsr_capture_kms *cap_kms = calloc(1, sizeof(gsr_capture_kms));
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
        .start = gsr_capture_kms_start,
        .tick = NULL,
        .should_stop = gsr_capture_kms_should_stop,
        .capture = gsr_capture_kms_capture,
        .capture_end = gsr_capture_kms_capture_end,
        .get_source_color = gsr_capture_kms_get_source_color,
        .uses_external_image = gsr_capture_kms_uses_external_image,
        .destroy = gsr_capture_kms_destroy,
        .priv = cap_kms
    };

    return cap;
}
