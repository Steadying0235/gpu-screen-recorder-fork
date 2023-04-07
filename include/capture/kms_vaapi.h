#ifndef GSR_CAPTURE_KMS_VAAPI_H
#define GSR_CAPTURE_KMS_VAAPI_H

#include "capture.h"
#include "../vec2.h"
#include <X11/X.h>

typedef struct _XDisplay Display;

typedef struct {
    Display *dpy;
    const char *display_to_capture; /* if this is "screen", then the entire x11 screen is captured (all displays). A copy is made of this */
    const char *program_dir; /* ref */
} gsr_capture_kms_vaapi_params;

gsr_capture* gsr_capture_kms_vaapi_create(const gsr_capture_kms_vaapi_params *params);

#endif /* GSR_CAPTURE_KMS_VAAPI_H */
