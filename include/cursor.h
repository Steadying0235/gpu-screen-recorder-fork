#ifndef GSR_CURSOR_H
#define GSR_CURSOR_H

#include "egl.h"
#include "vec2.h"

#include <X11/Xlib.h>

typedef struct {
    gsr_egl *egl;
    Display *display;
    Window window;
    int x_fixes_event_base;

    unsigned int texture_id;
    vec2i size;
    vec2i hotspot;
    vec2i position;

    bool cursor_image_set;
} gsr_cursor;

int gsr_cursor_init(gsr_cursor *self, gsr_egl *egl, Display *display);
void gsr_cursor_deinit(gsr_cursor *self);

int gsr_cursor_change_window_target(gsr_cursor *self, Window window);
void gsr_cursor_update(gsr_cursor *self, XEvent *xev);
void gsr_cursor_tick(gsr_cursor *self);

#endif /* GSR_CURSOR_H */
