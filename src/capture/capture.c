#include "../../include/capture/capture.h"
#include <assert.h>

int gsr_capture_start(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame *frame) {
    assert(!cap->started);
    int res = cap->start(cap, video_codec_context, frame);
    if(res == 0)
        cap->started = true;

    return res;
}

void gsr_capture_tick(gsr_capture *cap, AVCodecContext *video_codec_context) {
    assert(cap->started);
    if(cap->tick)
        cap->tick(cap, video_codec_context);
}

bool gsr_capture_should_stop(gsr_capture *cap, bool *err) {
    assert(cap->started);
    if(cap->should_stop)
        return cap->should_stop(cap, err);
    else
        return false;
}

int gsr_capture_capture(gsr_capture *cap, AVFrame *frame, gsr_color_conversion *color_conversion) {
    assert(cap->started);
    return cap->capture(cap, frame, color_conversion);
}

void gsr_capture_capture_end(gsr_capture *cap, AVFrame *frame) {
    assert(cap->started);
    if(cap->capture_end)
        cap->capture_end(cap, frame);
}

gsr_source_color gsr_capture_get_source_color(gsr_capture *cap) {
    return cap->get_source_color(cap);
}

bool gsr_capture_uses_external_image(gsr_capture *cap) {
    if(cap->uses_external_image)
        return cap->uses_external_image(cap);
    else
        return false;
}

void gsr_capture_destroy(gsr_capture *cap, AVCodecContext *video_codec_context) {
    cap->destroy(cap, video_codec_context);
}
