# app/

Startup and housekeeping. `main.c` is the place to start reading: it shows the order in which everything comes up (flash, diagnostics, LED, audio engine, sources, network).
`board.h` has the pins of the ESP32 board (the ESP32-S3 board uses different ones), `features.h` the build-time feature switches, `bootinfo.*` why the board last restarted,
`safemode.*` the crash-loop guard. See `docs/architecture.md`.
