#ifndef GSR_CODEC_QUERY_H
#define GSR_CODEC_QUERY_H

#include <stdbool.h>

typedef struct {
    bool h264;
    bool hevc;
    bool hevc_hdr;
    bool hevc_10bit;
    bool av1;
    bool av1_hdr;
    bool av1_10bit;
    bool vp8;
    bool vp9;
} gsr_supported_video_codecs;

#endif /* GSR_CODEC_QUERY_H */
