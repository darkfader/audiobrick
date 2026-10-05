// Build-time feature switches (Kconfig menu "Audio Brick features") as plain 0/1 values.
// In the preprocessor `#if CONFIG_AB_FEATURE_X` works; in ordinary C expressions an unset option is not defined at all,
// so use these AB_HAS_* values there.
#pragma once
#include "sdkconfig.h"

#ifdef CONFIG_AB_FEATURE_EQ
#define AB_HAS_EQ 1
#else
#define AB_HAS_EQ 0
#endif
#ifdef CONFIG_AB_FEATURE_AMBIENT
#define AB_HAS_AMBIENT 1
#else
#define AB_HAS_AMBIENT 0
#endif
#ifdef CONFIG_AB_FEATURE_VBAN
#define AB_HAS_VBAN 1
#else
#define AB_HAS_VBAN 0
#endif
#ifdef CONFIG_AB_FEATURE_SCREAM
#define AB_HAS_SCREAM 1
#else
#define AB_HAS_SCREAM 0
#endif
#ifdef CONFIG_AB_FEATURE_RADIO
#define AB_HAS_RADIO 1
#else
#define AB_HAS_RADIO 0
#endif
#ifdef CONFIG_AB_FEATURE_SYNTH
#define AB_HAS_SYNTH 1
#else
#define AB_HAS_SYNTH 0
#endif
#ifdef CONFIG_AB_FEATURE_MDNS
#define AB_HAS_MDNS 1
#else
#define AB_HAS_MDNS 0
#endif
#ifdef CONFIG_AB_FEATURE_SCHEDULE
#define AB_HAS_SCHEDULE 1
#else
#define AB_HAS_SCHEDULE 0
#endif
#ifdef CONFIG_AB_FEATURE_WIFI
#define AB_HAS_WIFI 1
#else
#define AB_HAS_WIFI 0
#endif
#ifdef CONFIG_AB_FEATURE_BLUETOOTH
#define AB_HAS_BLUETOOTH 1
#else
#define AB_HAS_BLUETOOTH 0
#endif
#ifdef CONFIG_AB_FEATURE_CLIPS
#define AB_HAS_CLIPS 1
#else
#define AB_HAS_CLIPS 0
#endif
#ifdef CONFIG_AB_FEATURE_ANNOUNCE
#define AB_HAS_ANNOUNCE 1
#else
#define AB_HAS_ANNOUNCE 0
#endif
#ifdef CONFIG_AB_FEATURE_TCPSTREAM
#define AB_HAS_TCPSTREAM 1
#else
#define AB_HAS_TCPSTREAM 0
#endif
#ifdef CONFIG_AB_FEATURE_POWERSAVE
#define AB_HAS_POWERSAVE 1
#else
#define AB_HAS_POWERSAVE 0
#endif
#ifdef CONFIG_AB_FEATURE_SNTP
#define AB_HAS_SNTP 1
#else
#define AB_HAS_SNTP 0
#endif
#ifdef CONFIG_AB_FEATURE_SAFEMODE
#define AB_HAS_SAFEMODE 1
#else
#define AB_HAS_SAFEMODE 0
#endif
#ifdef CONFIG_AB_FEATURE_BOOTINFO
#define AB_HAS_BOOTINFO 1
#else
#define AB_HAS_BOOTINFO 0
#endif
