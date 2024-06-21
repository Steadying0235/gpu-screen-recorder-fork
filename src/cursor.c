#include "../include/cursor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <X11/extensions/Xfixes.h>
#include <X11/extensions/XI2.h>
#include <X11/extensions/XInput2.h>

// TODO: Test cursor visibility with XFixesHideCursor

static bool gsr_cursor_set_from_x11_cursor_image(gsr_cursor *self, XFixesCursorImage *x11_cursor_image, bool *visible) {
    uint8_t *cursor_data = NULL;
    uint8_t *out = NULL;
    *visible = false;

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
            if(alpha == 0) {
                alpha = 1;
            } else {
                *visible = true;
            }

            *out++ = (unsigned)*in++ * 255/alpha;
            *out++ = (unsigned)*in++ * 255/alpha;
            *out++ = (unsigned)*in++ * 255/alpha;
            *out++ = *in++;
        }
    }

    self->egl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, self->size.x, self->size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, cursor_data);
    free(cursor_data);

    self->egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    self->egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    self->egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    self->egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    self->egl->glBindTexture(GL_TEXTURE_2D, 0);
    XFree(x11_cursor_image);
    return true;

    err:
    self->egl->glBindTexture(GL_TEXTURE_2D, 0);
    if(x11_cursor_image)
        XFree(x11_cursor_image);
    return false;
}

static bool xinput_is_supported(Display *dpy, int *xi_opcode) {
    *xi_opcode = 0;
    int query_event = 0;
    int query_error = 0;
    if(!XQueryExtension(dpy, "XInputExtension", xi_opcode, &query_event, &query_error)) {
        fprintf(stderr, "gsr error: gsr_cursor_init: X Input extension not available\n");
        return false;
    }

    int major = 2;
    int minor = 1;
    int retval = XIQueryVersion(dpy, &major, &minor);
    if (retval != Success) {
        fprintf(stderr, "gsr error: gsr_cursor_init: XInput 2.1 is not supported\n");
        return false;
    }

    return true;
}

int gsr_cursor_init(gsr_cursor *self, gsr_egl *egl, Display *display) {
    int x_fixes_error_base = 0;

    assert(egl);
    assert(display);
    memset(self, 0, sizeof(*self));
    self->egl = egl;
    self->display = display;

    self->x_fixes_event_base = 0;
    if(!XFixesQueryExtension(self->display, &self->x_fixes_event_base, &x_fixes_error_base)) {
        fprintf(stderr, "gsr error: gsr_cursor_init: your X11 server is missing the XFixes extension\n");
        gsr_cursor_deinit(self);
        return -1;
    }

    if(!xinput_is_supported(self->display, &self->xi_opcode)) {
        gsr_cursor_deinit(self);
        return -1;
    }

    unsigned char mask[XIMaskLen(XI_LASTEVENT)];
    memset(mask, 0, sizeof(mask));
    XISetMask(mask, XI_RawMotion);

    XIEventMask xi_masks;
    xi_masks.deviceid = XIAllMasterDevices;
    xi_masks.mask_len = sizeof(mask);
    xi_masks.mask = mask;
    if(XISelectEvents(self->display, DefaultRootWindow(self->display), &xi_masks, 1) != Success) {
        fprintf(stderr, "gsr error: gsr_cursor_init: XISelectEvents failed\n");
        gsr_cursor_deinit(self);
        return -1;
    }

    self->egl->glGenTextures(1, &self->texture_id);

    XFixesSelectCursorInput(self->display, DefaultRootWindow(self->display), XFixesDisplayCursorNotifyMask);
    gsr_cursor_set_from_x11_cursor_image(self, XFixesGetCursorImage(self->display), &self->visible);
    self->cursor_image_set = true;
    self->cursor_moved = true;

    return 0;
}

void gsr_cursor_deinit(gsr_cursor *self) {
    if(!self->egl)
        return;

    if(self->texture_id) {
        self->egl->glDeleteTextures(1, &self->texture_id);
        self->texture_id = 0;
    }

    XISelectEvents(self->display, DefaultRootWindow(self->display), NULL, 0);
    XFixesSelectCursorInput(self->display, DefaultRootWindow(self->display), 0);

    self->display = NULL;
    self->egl = NULL;
}

bool gsr_cursor_update(gsr_cursor *self, XEvent *xev) {
    bool updated = false;
    XGenericEventCookie *cookie = (XGenericEventCookie*)&xev->xcookie;
    const Bool got_event_data = XGetEventData(self->display, cookie);
    if(got_event_data && cookie->type == GenericEvent && cookie->extension == self->xi_opcode && cookie->evtype == XI_RawMotion) {
        updated = true;
        self->cursor_moved = true;
    }
    if(got_event_data)
        XFreeEventData(self->display, cookie);

    if(xev->type == self->x_fixes_event_base + XFixesCursorNotify) {
        XFixesCursorNotifyEvent *cursor_notify_event = (XFixesCursorNotifyEvent*)xev;
        if(cursor_notify_event->subtype == XFixesDisplayCursorNotify && cursor_notify_event->window == DefaultRootWindow(self->display)) {
            self->cursor_image_set = false;
        }
    }

    if(!self->cursor_image_set) {
        self->cursor_image_set = true;
        gsr_cursor_set_from_x11_cursor_image(self, XFixesGetCursorImage(self->display), &self->visible);
        updated = true;
    }

    return updated;
}

void gsr_cursor_tick(gsr_cursor *self, Window relative_to) {
    if(!self->cursor_moved)
        return;

    self->cursor_moved = false;

    Window dummy_window;
    int dummy_i;
    unsigned int dummy_u;
    XQueryPointer(self->display, relative_to, &dummy_window, &dummy_window, &dummy_i, &dummy_i, &self->position.x, &self->position.y, &dummy_u);
}
