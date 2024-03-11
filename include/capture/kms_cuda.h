#ifndef GSR_CAPTURE_KMS_CUDA_H
#define GSR_CAPTURE_KMS_CUDA_H

#include "../vec2.h"
#include "../utils.h"
#include "../color_conversion.h"
#include "capture.h"

typedef struct {
    gsr_egl *egl;
    const char *display_to_capture; /* if this is "screen", then the first monitor is captured. A copy is made of this */
    gsr_gpu_info gpu_inf;
    bool hdr;
    gsr_color_range color_range;
    bool record_cursor;
} gsr_capture_kms_cuda_params;

gsr_capture* gsr_capture_kms_cuda_create(const gsr_capture_kms_cuda_params *params);

#endif /* GSR_CAPTURE_KMS_CUDA_H */
