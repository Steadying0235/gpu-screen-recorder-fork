#ifndef GSR_CAPTURE_XCOMPOSITE_SOFTWARE_H
#define GSR_CAPTURE_XCOMPOSITE_SOFTWARE_H

#include "capture.h"
#include "xcomposite.h"

typedef struct {
    gsr_capture_xcomposite_params base;
} gsr_capture_xcomposite_software_params;

gsr_capture* gsr_capture_xcomposite_software_create(const gsr_capture_xcomposite_software_params *params);

#endif /* GSR_CAPTURE_XCOMPOSITE_SOFTWARE_H */
