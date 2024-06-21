#ifndef GSR_CAPTURE_XCOMPOSITE_H
#define GSR_CAPTURE_XCOMPOSITE_H

#include "capture.h"
#include "../egl.h"
#include "../vec2.h"
#include "../color_conversion.h"
#include "../window_texture.h"
#include "../cursor.h"

typedef struct {
    gsr_egl *egl;
    Window window;
    bool follow_focused; /* If this is set then |window| is ignored */
    vec2i region_size; /* This is currently only used with |follow_focused| */
    gsr_color_range color_range;
    bool record_cursor;
    bool track_damage;
} gsr_capture_xcomposite_params;

typedef struct {
    gsr_capture_base base;
    gsr_capture_xcomposite_params params;
    XEvent xev;

    bool should_stop;
    bool stop_is_error;
    bool window_resized;
    bool follow_focused_initialized;

    Window window;
    vec2i window_size;
    vec2i texture_size;
    double window_resize_timer;
    
    WindowTexture window_texture;

    Atom net_active_window_atom;

    gsr_cursor cursor;

    int damage_event;
    int damage_error;
    XID damage;
    bool damaged;
} gsr_capture_xcomposite;

void gsr_capture_xcomposite_init(gsr_capture_xcomposite *self, const gsr_capture_xcomposite_params *params);

int gsr_capture_xcomposite_start(gsr_capture_xcomposite *self, AVCodecContext *video_codec_context, AVFrame *frame);
void gsr_capture_xcomposite_stop(gsr_capture_xcomposite *self);
void gsr_capture_xcomposite_tick(gsr_capture_xcomposite *self, AVCodecContext *video_codec_context);
bool gsr_capture_xcomposite_is_damaged(gsr_capture_xcomposite *self);
void gsr_capture_xcomposite_clear_damage(gsr_capture_xcomposite *self);
bool gsr_capture_xcomposite_should_stop(gsr_capture_xcomposite *self, bool *err);
int gsr_capture_xcomposite_capture(gsr_capture_xcomposite *self, AVFrame *frame);

#endif /* GSR_CAPTURE_XCOMPOSITE_H */
