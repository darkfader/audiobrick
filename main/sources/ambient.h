// Ambient scene: a looping background (rain, a PC fan ...) plus events that play at random intervals
// (a meow every 5-15 minutes, typing, a door). Uses the background and event channels of the mixer, so it
// plays underneath whatever else is going on, and pauses itself while a stream, a clip or the test tone
// is playing on the main channel. The amp volume cap and the EQ apply as to every other sound.
#pragma once
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>

#define AMBIENT_MAX_RULES 8

typedef struct {
    bool on;
    uint8_t type;        // 0 = event (random interval), 1 = background (loops continuously)
    char clip[32];
    uint32_t min_s;      // events: shortest wait between plays (5 .. 86400)
    uint32_t max_s;      // events: longest wait (>= min_s)
    float gain_db;       // level relative to the amp volume, -30 .. 0
} ambient_rule_t;

typedef struct {
    bool enabled;
    ambient_rule_t rule[AMBIENT_MAX_RULES];
} ambient_config_t;

#if CONFIG_AB_FEATURE_AMBIENT
void ambient_init(void);                 // loads the stored scene and starts the scheduler task
ambient_config_t ambient_get(void);
bool ambient_set(const ambient_config_t *cfg);   // validates, stores, applies
bool ambient_running(void);              // true while the scene is actually producing sound
// Seconds until the event rule i plays next, or -1 if it is not scheduled.
int ambient_next_in_s(int i);
#else  // not built
static inline void ambient_init(void) {}
#endif
