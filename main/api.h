/*
 * RadioFloppy HTTP API, /api/v1/. See docs/API.md.
 */
#pragma once

#include "esp_err.h"

/* Start the HTTP server (on core 1). */
esp_err_t api_start(void);
