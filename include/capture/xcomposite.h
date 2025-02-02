#ifndef GSR_CAPTURE_XCOMPOSITE_H
#define GSR_CAPTURE_XCOMPOSITE_H

#include "capture.h"
#include "../vec2.h"

typedef struct {
    gsr_egl *egl;
    unsigned long window;
    bool follow_focused; /* If this is set then |window| is ignored */
    gsr_color_range color_range;
    bool record_cursor;
    gsr_color_depth color_depth;
    vec2i output_resolution;
} gsr_capture_xcomposite_params;

gsr_capture* gsr_capture_xcomposite_create(const gsr_capture_xcomposite_params *params);

#endif /* GSR_CAPTURE_XCOMPOSITE_H */
