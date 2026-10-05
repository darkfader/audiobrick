#pragma once
#include <stdbool.h>
#include "esp_http_server.h"

// Sleep timer, quiet hours and an alarm.
//
// - Sleep timer: after N minutes everything stops (clips, streams, the ambient scene) and the Bluetooth device is let go. Not saved: a restart cancels it.
// - Quiet hours: between two times of day the amp volume is held under a ceiling (for example -40 dB at night). Your own volume setting is kept and comes
//   back when the window ends. Needs the network clock (AB_FEATURE_SNTP); without it the window never opens.
// - Alarm: at a time of day on chosen weekdays a stored clip plays in a loop for a while (it stops by itself, or with Stop). It plays at the current
//   volume, so it can never be louder than what you set (and quiet hours still apply). Needs the clock and the clips feature.
//
//   GET  /schedule                 settings and state (login)
//   POST /schedule                 body lines: quiet_on=1, quiet_from=22:00, quiet_to=07:00, quiet_db=-40,
//                                  alarm_on=1, alarm_at=07:30, alarm_days=62 (bit 0 = Sunday ... bit 6 = Saturday), alarm_clip=name, alarm_s=60
//   POST /schedule/sleep?min=N     start the sleep timer (0 cancels)
//   POST /schedule/alarm_now       play the alarm clip now (test)

void schedule_init(void);
void schedule_http_register(httpd_handle_t server);
