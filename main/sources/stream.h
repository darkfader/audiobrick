// TCP audio stream receiver on port 4010.
//
// Protocol: the client connects and sends one text line, then raw PCM until it closes:
//   STREAM <password> [rate] [channels]\n
// rate defaults to 48000 and may be any value from 8000 to 192000; channels is 1 or 2 (default 2).
// Samples are signed 16-bit little-endian, interleaved. The board answers "OK\n" or an error line.
#pragma once
#include <stdbool.h>
#include "sdkconfig.h"

#define STREAM_PORT 4010

#if CONFIG_AB_FEATURE_TCPSTREAM
bool stream_start(void);
#else
static inline bool stream_start(void) { return true; }
#endif
