#ifndef GSR_CAPTURE_KMS_H
#define GSR_CAPTURE_KMS_H

#include "capture.h"
#include "../../kms/client/kms_client.h"
#include "../color_conversion.h"
#include "../vec2.h"
#include "../defs.h"
#include <stdbool.h>

typedef struct AVCodecContext AVCodecContext;
typedef struct AVMasteringDisplayMetadata AVMasteringDisplayMetadata;
typedef struct AVContentLightMetadata AVContentLightMetadata;
typedef struct gsr_capture_kms gsr_capture_kms;
typedef struct gsr_egl gsr_egl;
typedef struct AVFrame AVFrame;

#define MAX_CONNECTOR_IDS 32

typedef struct {
    uint32_t connector_ids[MAX_CONNECTOR_IDS];
    int num_connector_ids;
} MonitorId;

struct gsr_capture_kms {
    gsr_capture_base base;

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
};

/* Returns 0 on success */
int gsr_capture_kms_start(gsr_capture_kms *self, const char *display_to_capture, gsr_egl *egl, AVCodecContext *video_codec_context, AVFrame *frame);
void gsr_capture_kms_stop(gsr_capture_kms *self);
bool gsr_capture_kms_capture(gsr_capture_kms *self, AVFrame *frame, bool hdr, bool screen_plane_use_modifiers, bool cursor_texture_is_external);
void gsr_capture_kms_cleanup_kms_fds(gsr_capture_kms *self);

#endif /* GSR_CAPTURE_KMS_H */
