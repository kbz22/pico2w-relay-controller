#ifndef RELAY_H
#define RELAY_H

#include <stdbool.h>
#include <stddef.h>

#include "pico/stdlib.h"

typedef struct {
    const char *label;
    uint gpio;
    bool on;
} relay_light_t;

void relay_init(void);
size_t relay_get_count(void);
const relay_light_t *relay_get(size_t idx);
void relay_toggle(size_t idx);
void relay_set(size_t idx, bool on);
void relay_reset_all_on(void);

#endif
