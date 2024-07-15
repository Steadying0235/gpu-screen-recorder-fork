#ifndef GSR_CAPTURE_PORTAL_H
#define GSR_CAPTURE_PORTAL_H

#include "capture.h"

typedef struct {
    gsr_egl *egl;
    gsr_color_range color_range;
    bool hdr;
    bool record_cursor;
    bool restore_portal_session;
} gsr_capture_portal_params;

gsr_capture* gsr_capture_portal_create(const gsr_capture_portal_params *params);

#endif /* GSR_CAPTURE_PORTAL_H */
