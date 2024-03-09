#ifndef GSR_CAPTURE_XCOMPOSITE_CUDA_H
#define GSR_CAPTURE_XCOMPOSITE_CUDA_H

#include "capture.h"
#include "xcomposite.h"

typedef struct {
    gsr_capture_xcomposite_params base;
    bool overclock;
} gsr_capture_xcomposite_cuda_params;

gsr_capture* gsr_capture_xcomposite_cuda_create(const gsr_capture_xcomposite_cuda_params *params);

#endif /* GSR_CAPTURE_XCOMPOSITE_CUDA_H */
