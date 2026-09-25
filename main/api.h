/*
 * RadioFloppy HTTP API, /api/v1/. See docs/API.md.
 */
#pragma once

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

/* Start the HTTP server (on core 1). */
esp_err_t api_start(void);

/* Shared with the setup page server (setup_mode.c). */
esp_err_t api_send_json(httpd_req_t *req, const char *status, cJSON *root);
esp_err_t api_send_error(httpd_req_t *req, const char *status, const char *code,
                         const char *message);
/* Small JSON body with Content-Type application/json; NULL: error sent. */
cJSON *api_read_json(httpd_req_t *req);
