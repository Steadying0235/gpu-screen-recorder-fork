#include "../include/damage.h"

#include <stdio.h>
#include <string.h>
#include <X11/extensions/Xdamage.h>

bool gsr_damage_init(gsr_damage *self, Display *display) {
    memset(self, 0, sizeof(*self));
    self->display = display;

    if(!XDamageQueryExtension(self->display, &self->damage_event, &self->damage_error)) {
        fprintf(stderr, "gsr warning: gsr_damage_init: XDamage is not supported by your X11 server\n");
        self->damage_event = 0;
        self->damage_error = 0;
        return false;
    }

    self->damaged = true;
    return true;
}

void gsr_damage_deinit(gsr_damage *self) {
    if(self->damage) {
        XDamageDestroy(self->display, self->damage);
        self->damage = None;
    }
}

bool gsr_damage_set_target_window(gsr_damage *self, uint64_t window) {
    if(self->damage_event == 0)
        return false;

    if(self->damage) {
        XDamageDestroy(self->display, self->damage);
        self->damage = None;
    }

    self->damage = XDamageCreate(self->display, window, XDamageReportNonEmpty);
    if(self->damage) {
        XDamageSubtract(self->display, self->damage, None, None);
        self->damaged = true;
        return true;
    } else {
        fprintf(stderr, "gsr warning: gsr_damage_set_target_window: XDamageCreate failed\n");
        return false;
    }
}

void gsr_damage_update(gsr_damage *self, XEvent *xev) {
    if(self->damage_event == 0 || !self->damage)
        return;

    if(self->damage_event && xev->type == self->damage_event + XDamageNotify) {
        XDamageNotifyEvent *de = (XDamageNotifyEvent*)xev;
        XserverRegion region = XFixesCreateRegion(self->display, NULL, 0);
        /* Subtract all the damage, repairing the window */
        XDamageSubtract(self->display, de->damage, None, region);
        XFixesDestroyRegion(self->display, region);
        XFlush(self->display);
        self->damaged = true;
    }
}

bool gsr_damage_is_damaged(gsr_damage *self) {
    return self->damage_event == 0 || !self->damage || self->damaged;
}

void gsr_damage_clear(gsr_damage *self) {
    self->damaged = false;
}
