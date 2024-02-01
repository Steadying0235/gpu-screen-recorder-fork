#ifndef GSR_CAPTURE_XCOMPOSITE_VAAPI_H
#define GSR_CAPTURE_XCOMPOSITE_VAAPI_H

#include "capture.h"
#include "../egl.h"
#include "../vec2.h"
#include "../color_conversion.h"
#include <X11/X.h>

typedef struct _XDisplay Display;

typedef struct {
    gsr_egl *egl;
    Display *dpy;
    Window window;
    bool follow_focused; /* If this is set then |window| is ignored */
    vec2i region_size; /* This is currently only used with |follow_focused| */
    const char *card_path; /* reference */
    gsr_color_range color_range;
} gsr_capture_xcomposite_vaapi_params;

gsr_capture* gsr_capture_xcomposite_vaapi_create(const gsr_capture_xcomposite_vaapi_params *params);

#endif /* GSR_CAPTURE_XCOMPOSITE_VAAPI_H */
