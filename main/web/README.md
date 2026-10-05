# web/

The HTTP side. `ota_http.*` starts the server and has login, `/status`, the speaker profile, the test tone and the firmware update; `clips_http.c`, `ambient_http.c` and
`eq_http.c` hold the endpoints of their features (feature modules in `sources/` register theirs from `ambient_http_register`). `index.html` is the whole web page; it is embedded
in the firmware at build time. Every endpoint that changes something checks `web_authorized()` first.
