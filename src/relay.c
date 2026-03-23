#include "relay.h"

static relay_light_t lights[] = {
    {"LIGHT 1", 15, true},
    {"LIGHT 2", 14, true},
    {"LIGHT 3", 13, true},
};

static const size_t light_count = sizeof(lights) / sizeof(lights[0]);

static void relay_apply_hw(size_t idx) {
    // Active-low relays: HIGH = OFF, LOW = ON.
    gpio_put(lights[idx].gpio, lights[idx].on ? 0 : 1);
}

void relay_init(void) {
    for (size_t i = 0; i < light_count; ++i) {
        gpio_init(lights[i].gpio);
        gpio_set_dir(lights[i].gpio, GPIO_OUT);
        relay_apply_hw(i);
    }
}

size_t relay_get_count(void) {
    return light_count;
}

const relay_light_t *relay_get(size_t idx) {
    if (idx >= light_count) {
        return NULL;
    }
    return &lights[idx];
}

void relay_set(size_t idx, bool on) {
    if (idx >= light_count) {
        return;
    }
    lights[idx].on = on;
    relay_apply_hw(idx);
}

void relay_toggle(size_t idx) {
    if (idx >= light_count) {
        return;
    }
    lights[idx].on = !lights[idx].on;
    relay_apply_hw(idx);
}

void relay_reset_all_on(void) {
    for (size_t i = 0; i < light_count; ++i) {
        lights[i].on = true;
        relay_apply_hw(i);
    }
}
