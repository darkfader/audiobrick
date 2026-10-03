#pragma once
#include <stdbool.h>

// Starts the HTTP server on port 80 (plain HTTP; use on a trusted LAN only).
//   GET  /               web page (status for everyone; settings after login)
//   GET  /status         JSON state, no login
//   GET  /profile        speaker profile and derived limits, no login
//   GET  /session        {"logged_in":bool}
//   POST /login          body = password; sets the "sid" cookie (30 days)
//   POST /logout
//   POST /password       body = new password (login required)
//   POST /profile?...    change the speaker profile (login required)
//   POST /tone?on=1&freq=440&db=-30&vol=-30   sine test tone (login required)
//   POST /update         body = firmware .bin (login required)
// Scripts may skip the cookie and send the password in an X-Token header instead.
// The initial password is generated on first boot, stored in NVS and printed on the serial console.
bool ota_http_start(void);

// Shared with the other HTTP files and the stream port.
#include "esp_http_server.h"
bool web_authorized(httpd_req_t *req);   // valid session cookie or X-Token header
esp_err_t web_deny(httpd_req_t *req);    // sends 401
bool web_password_ok(const char *candidate);
void ota_http_register_more(httpd_handle_t server);  // registers the clip/media endpoints
void eq_http_register(httpd_handle_t server);        // registers the EQ endpoints
void ambient_http_register(httpd_handle_t server);   // registers the ambient scene and player endpoints

// Mark the running image as good so the bootloader will not roll back.
void ota_mark_valid(void);
