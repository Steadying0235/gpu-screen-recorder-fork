#ifndef GSR_CAPTURE_XCOMPOSITE_VAAPI_H
#define GSR_CAPTURE_XCOMPOSITE_VAAPI_H

#include "capture.h"
#include "xcomposite.h"

typedef struct {
    gsr_capture_xcomposite_params base;
} gsr_capture_xcomposite_vaapi_params;

gsr_capture* gsr_capture_xcomposite_vaapi_create(const gsr_capture_xcomposite_vaapi_params *params);

#endif /* GSR_CAPTURE_XCOMPOSITE_VAAPI_H */
