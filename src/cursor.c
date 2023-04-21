#include "../include/cursor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <X11/extensions/Xfixes.h>

static bool gsr_cursor_set_from_x11_cursor_image(gsr_cursor *self, XFixesCursorImage *x11_cursor_image) {
    uint8_t *cursor_data = NULL;
    uint8_t *out = NULL;

    if(!x11_cursor_image)
        goto err;
        
    if(!x11_cursor_image->pixels)
        goto err;

    self->hotspot.x = x11_cursor_image->xhot;
    self->hotspot.y = x11_cursor_image->yhot;
    self->egl->glBindTexture(GL_TEXTURE_2D, self->texture_id);

    self->size.x = x11_cursor_image->width;
    self->size.y = x11_cursor_image->height;
    const unsigned long *pixels = x11_cursor_image->pixels;
    cursor_data = malloc(self->size.x * self->size.y * 4);
    if(!cursor_data)
        goto err;
    out = cursor_data;
    /* Un-premultiply alpha */
    for(int y = 0; y < self->size.y; ++y) {
        for(int x = 0; x < self->size.x; ++x) {
            uint32_t pixel = *pixels++;
            uint8_t *in = (uint8_t*)&pixel;
            uint8_t alpha = in[3];
            if(alpha == 0)
                alpha = 1;

            *out++ = (unsigned)*in++ * 255/alpha;
            *out++ = (unsigned)*in++ * 255/alpha;
            *out++ = (unsigned)*in++ * 255/alpha;
            *out++ = *in++;
        }
    }

    self->egl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, self->size.x, self->size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, cursor_data);
    free(cursor_data);
    self->egl->glGenerateMipmap(GL_TEXTURE_2D);

    self->egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    self->egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    self->egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    self->egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);

    self->egl->glBindTexture(GL_TEXTURE_2D, 0);
    XFree(x11_cursor_image);
    return true;

    err:
    self->egl->glBindTexture(GL_TEXTURE_2D, 0);
    if(x11_cursor_image)
        XFree(x11_cursor_image);
    return false;
}

int gsr_cursor_init(gsr_cursor *self, gsr_egl *egl, Display *display) {
    assert(egl);
    assert(display);
    memset(self, 0, sizeof(*self));
    self->egl = egl;
    self->display = display;

    self->x_fixes_event_base = 0;
    int x_fixes_error_base = 0;
    if(!XFixesQueryExtension(display, &self->x_fixes_event_base, &x_fixes_error_base)) {
        fprintf(stderr, "gsr error: gsr_cursor_init: your x11 server is missing the xfixes extension. no cursor will be visible in the video\n");
        gsr_cursor_deinit(self);
        return -1;
    }

    self->egl->glGenTextures(1, &self->texture_id);
    return 0;
}

void gsr_cursor_deinit(gsr_cursor *self) {
    if(!self->egl)
        return;

    if(self->texture_id) {
        self->egl->glDeleteTextures(1, &self->texture_id);
        self->texture_id = 0;
    }

    if(self->window) {
        XFixesSelectCursorInput(self->display, self->window, 0);
        self->window = None;
    }

    self->display = NULL;
    self->egl = NULL;
}

int gsr_cursor_change_window_target(gsr_cursor *self, Window window) {
    if(self->window)
        XFixesSelectCursorInput(self->display, self->window, 0);
    
    XFixesSelectCursorInput(self->display, window, XFixesDisplayCursorNotifyMask);
    self->window = window;
    self->cursor_image_set = false;
    return 0;
}

void gsr_cursor_update(gsr_cursor *self, XEvent *xev) {
    if(xev->type == self->x_fixes_event_base + XFixesCursorNotify) {
        XFixesCursorNotifyEvent *cursor_notify_event = (XFixesCursorNotifyEvent*)&xev;
        if(cursor_notify_event->subtype == XFixesDisplayCursorNotify && cursor_notify_event->window == self->window) {
            self->cursor_image_set = true;
            gsr_cursor_set_from_x11_cursor_image(self, XFixesGetCursorImage(self->display));
        }
    }

    if(!self->cursor_image_set && self->window) {
        self->cursor_image_set = true;
        gsr_cursor_set_from_x11_cursor_image(self, XFixesGetCursorImage(self->display));
    }
}
