#ifndef GSR_CAPTURE_KMS_SOFTWARE_H
#define GSR_CAPTURE_KMS_SOFTWARE_H

#include "../vec2.h"
#include "../utils.h"
#include "../color_conversion.h"
#include "capture.h"

typedef struct {
    gsr_egl *egl;
    const char *display_to_capture; /* if this is "screen", then the first monitor is captured. A copy is made of this */
    bool hdr;
    gsr_color_range color_range;
    bool record_cursor;
} gsr_capture_kms_software_params;

gsr_capture* gsr_capture_kms_software_create(const gsr_capture_kms_software_params *params);

#endif /* GSR_CAPTURE_KMS_SOFTWARE_H */
