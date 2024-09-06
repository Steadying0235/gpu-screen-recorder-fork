#ifndef GSR_DAMAGE_H
#define GSR_DAMAGE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct _XDisplay Display;
typedef union _XEvent XEvent;

typedef struct {
    Display *display;
    int damage_event;
    int damage_error;
    uint64_t damage;
    bool damaged;
} gsr_damage;

bool gsr_damage_init(gsr_damage *self, Display *display);
void gsr_damage_deinit(gsr_damage *self);

bool gsr_damage_set_target_window(gsr_damage *self, uint64_t window);
void gsr_damage_update(gsr_damage *self, XEvent *xev);
/* Also returns true if damage tracking is not available */
bool gsr_damage_is_damaged(gsr_damage *self);
void gsr_damage_clear(gsr_damage *self);

#endif /* GSR_DAMAGE_H */
