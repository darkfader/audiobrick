"""Constants for the Esparagus Audio Brick integration."""

DOMAIN = "audiobrick"

CONF_HOST = "host"
CONF_PASSWORD = "password"

DEFAULT_HOST = "audiobrick.local"
SCAN_INTERVAL_SECONDS = 5

# The announcement endpoint accepts at most 400 KB (an MP3 or a 16-bit WAV).
ANNOUNCE_MAX_BYTES = 400 * 1024
