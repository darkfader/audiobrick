// Transport controls over the stored clips (what a media player has: play, pause, next, previous, stop).
// The playlist is the list of clips on the board in alphabetical order. Streams can be paused and stopped
// but have no next/previous.
#pragma once
#include <stdbool.h>

typedef enum { PLAYER_IDLE = 0, PLAYER_PLAYING, PLAYER_PAUSED } player_state_t;

void player_init(void);                  // starts the small task that handles auto-advance
bool player_play(void);                  // resume if paused, otherwise play the current (or first) clip
bool player_pause(void);
bool player_next(void);
bool player_prev(void);
bool player_stop(void);
bool player_play_clip(const char *name, bool loop);  // play a named clip and remember it as the current one

player_state_t player_state(void);
const char *player_current(void);        // name of the current clip ("" if none)
int player_index(void);                  // 1-based position in the playlist, 0 if unknown
int player_count(void);
void player_set_autonext(bool on);       // play the next clip when one ends by itself (default off)
bool player_autonext(void);
