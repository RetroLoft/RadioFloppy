/*
 * RadioFloppy HTTP API, /api/v1/. Documented in docs/API.md. Also serves the
 * small web interface (web/index.html) on /, which only uses this API.
 *
 * Runs in the esp_http_server task on core 1, away from the floppy
 * interrupts and the flux stream (core 0). Requests are handled one at a
 * time; api_lock additionally serialises everything that changes the slot
 * catalog, a slot or the active disk.
 *
 * Uploads are two-step: POST /uploads (JSON options, checked before any
 * data is sent) and PUT /uploads/{id}/data (raw .ST bytes). The bytes go
 * into a PSRAM staging buffer first; only a complete, validated image is
 * written to a flash slot and/or prepared as the active disk, so an
 * interrupted or invalid upload never touches a slot or the drive.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_timer.h"

#include "api.h"
#include "disk_image.h"
#include "disk_switch.h"
#include "disk_title.h"
#include "drive_config.h"
#include "drive_emu.h"
#include "ext_flash.h"
#include "slot_store.h"
#include "st_image.h"
#include "wifi_net.h"

#define API_BODY_MAX        1024        /* JSON request bodies */
#define RECV_CHUNK          4096
#define UPLOAD_TIMEOUT_MS   60000       /* created but no data: expires */

typedef enum {
    UP_NONE,
    UP_READY,           /* created, waiting for the data */
    UP_RECEIVING,
    UP_PROCESSING,      /* validating / storing / preparing */
    UP_DONE,
    UP_FAILED,
} upload_state_t;

static const char *const state_names[] = {
    "none", "ready", "receiving", "processing", "done", "failed",
};

typedef struct {
    uint32_t id;
    upload_state_t state;
    char name[DISK_TITLE_SIZE];
    uint32_t size;
    bool to_flash;
    int slot;               /* requested slot 0..19, -1 = first free */
    bool activate;
    bool have_crc;
    uint32_t expected_crc;
    uint32_t received;
    uint32_t crc32;
    int result_slot;        /* slot written, -1 = none */
    bool activated;
    int64_t touched_us;
    const char *err_code;
    char err_msg[128];
} upload_t;

static httpd_handle_t server;
static SemaphoreHandle_t api_lock;
static upload_t up;
static uint32_t next_upload_id = 1;

/* ---- Responses ---------------------------------------------------------- */

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text ? text : "{}");
    free(text);
    return err;
}

/* {"error": {"code": ..., "message": ...}} */
static esp_err_t send_error(httpd_req_t *req, const char *status, const char *code,
                            const char *fmt, ...)
{
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    cJSON *root = cJSON_CreateObject();
    cJSON *e = cJSON_AddObjectToObject(root, "error");
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", msg);
    return send_json(req, status, root);
}

#define HTTP_400 "400 Bad Request"
#define HTTP_401 "401 Unauthorized"
#define HTTP_404 "404 Not Found"
#define HTTP_409 "409 Conflict"
#define HTTP_413 "413 Payload Too Large"
#define HTTP_422 "422 Unprocessable Entity"
#define HTTP_500 "500 Internal Server Error"
#define HTTP_503 "503 Service Unavailable"
#define HTTP_507 "507 Insufficient Storage"

/* ---- Helpers ------------------------------------------------------------ */

static void hex32(char out[9], uint32_t v)
{
    snprintf(out, 9, "%08lx", (unsigned long)v);
}

/*
 * Modifying calls. Without a configured token the API is open (trusted home
 * network). With CONFIG_RADIOFLOPPY_API_TOKEN set they need
 * "Authorization: Bearer <token>" (or X-API-Token).
 */
static bool authorized(httpd_req_t *req)
{
    const char *token = CONFIG_RADIOFLOPPY_API_TOKEN;
    char hdr[160] = "";

    if (token[0] == 0) {
        return true;
    }
    const char *given = NULL;
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
        strncmp(hdr, "Bearer ", 7) == 0) {
        given = hdr + 7;
    } else if (httpd_req_get_hdr_value_str(req, "X-API-Token", hdr, sizeof(hdr)) == ESP_OK) {
        given = hdr;
    }
    /* Compare without an early exit on the first differing byte. */
    size_t n = strlen(token);
    unsigned diff = given ? (unsigned)(strlen(given) != n) : 1;
    for (size_t i = 0; given && i < n && given[i]; i++) {
        diff |= (unsigned char)(given[i] ^ token[i]);
    }
    if (diff) {
        send_error(req, HTTP_401, "UNAUTHORIZED", "missing or wrong API token");
        return false;
    }
    return true;
}

/*
 * Modifying calls must carry the expected Content-Type. A web page on
 * another site can only send such requests after a CORS preflight, which
 * this server never grants, so it cannot change disks through a visitor's
 * browser (important when no token is configured).
 */
static bool content_type_is(httpd_req_t *req, const char *type)
{
    char ct[64] = "";

    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) == ESP_OK &&
        strncasecmp(ct, type, strlen(type)) == 0) {
        return true;
    }
    send_error(req, "415 Unsupported Media Type", "UNSUPPORTED_MEDIA_TYPE",
               "Content-Type must be %s", type);
    return false;
}

/* Read a small JSON body. NULL: an error response has been sent. */
static cJSON *read_json(httpd_req_t *req)
{
    char body[API_BODY_MAX + 1];
    int len = req->content_len;

    if (!content_type_is(req, "application/json")) {
        return NULL;
    }
    if (len <= 0 || len > API_BODY_MAX) {
        send_error(req, HTTP_400, "INVALID_REQUEST", "expected a JSON body of at most %d bytes",
                   API_BODY_MAX);
        return NULL;
    }
    for (int got = 0; got < len;) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) {
            send_error(req, HTTP_400, "INVALID_REQUEST", "incomplete request body");
            return NULL;
        }
        got += r;
    }
    body[len] = 0;
    cJSON *root = cJSON_Parse(body);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        send_error(req, HTTP_400, "INVALID_REQUEST", "body is not a JSON object");
        return NULL;
    }
    return root;
}

/* Slot number 1..20 from a path segment or JSON value; -1 if invalid. */
static int parse_slot(const char *s)
{
    char *end;
    long n = strtol(s, &end, 10);
    return (end != s && *end == 0 && n >= 1 && n <= RF_SLOT_COUNT) ? (int)n - 1 : -1;
}

static void fail_upload(const char *code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(up.err_msg, sizeof(up.err_msg), fmt, ap);
    va_end(ap);
    up.err_code = code;
    up.state = UP_FAILED;
    up.touched_us = esp_timer_get_time();
}

/* An upload that was created but never got its data expires. */
static void expire_upload(void)
{
    if (up.state == UP_READY &&
        esp_timer_get_time() - up.touched_us > UPLOAD_TIMEOUT_MS * 1000LL) {
        fail_upload("UPLOAD_EXPIRED", "no data received within %d s", UPLOAD_TIMEOUT_MS / 1000);
    }
}

static bool upload_in_progress(void)
{
    expire_upload();
    return up.state == UP_READY || up.state == UP_RECEIVING || up.state == UP_PROCESSING;
}

static const char *slot_status_name(uint8_t st)
{
    switch (st) {
    case SLOT_EMPTY:    return "empty";
    case SLOT_VALID:    return "valid";
    case SLOT_BUILDING: return "incomplete";
    case SLOT_DELETED:  return "deleted";
    default:            return "unknown";
    }
}

static cJSON *current_json(void)
{
    disk_info_t d;
    char crc[9];

    disk_get_current(&d);
    cJSON *c = cJSON_CreateObject();
    cJSON_AddBoolToObject(c, "inserted", d.source != DISK_SRC_NONE);
    cJSON_AddStringToObject(c, "source", d.source == DISK_SRC_FLASH ? "flash"
                                        : d.source == DISK_SRC_PSRAM ? "psram" : "none");
    if (d.source == DISK_SRC_FLASH && d.slot >= 0) {
        cJSON_AddNumberToObject(c, "slot", d.slot + 1);
        cJSON_AddBoolToObject(c, "slot_changed_since", d.slot_changed);
    }
    if (d.source != DISK_SRC_NONE) {
        hex32(crc, d.crc32);
        cJSON_AddStringToObject(c, "name", d.name);
        cJSON_AddNumberToObject(c, "size", d.size);
        cJSON_AddStringToObject(c, "crc32", crc);
        cJSON_AddNumberToObject(c, "sides", d.heads);
        cJSON_AddNumberToObject(c, "cylinders", d.cylinders);
        cJSON_AddNumberToObject(c, "sectors", d.sectors);
    }
    return c;
}

static cJSON *upload_json(void)
{
    char crc[9];
    cJSON *u = cJSON_CreateObject();
    char id[12];

    snprintf(id, sizeof(id), "%lu", (unsigned long)up.id);
    cJSON_AddStringToObject(u, "upload_id", id);
    cJSON_AddStringToObject(u, "state", state_names[up.state]);
    cJSON_AddStringToObject(u, "name", up.name);
    cJSON_AddNumberToObject(u, "size", up.size);
    cJSON_AddNumberToObject(u, "received", up.received);
    cJSON_AddStringToObject(u, "destination", up.to_flash ? "flash" : "psram");
    if (up.to_flash && up.slot >= 0) {
        cJSON_AddNumberToObject(u, "slot", up.slot + 1);
    }
    cJSON_AddBoolToObject(u, "activate", up.activate);
    if (up.state == UP_READY) {
        char url[48];
        snprintf(url, sizeof(url), "/api/v1/uploads/%lu/data", (unsigned long)up.id);
        cJSON_AddStringToObject(u, "upload_url", url);
    }
    if (up.state == UP_DONE) {
        hex32(crc, up.crc32);
        cJSON_AddStringToObject(u, "crc32", crc);
        if (up.result_slot >= 0) {
            cJSON_AddNumberToObject(u, "stored_in_slot", up.result_slot + 1);
        }
        cJSON_AddBoolToObject(u, "active", up.activated);
    }
    if (up.state == UP_FAILED) {
        cJSON *e = cJSON_AddObjectToObject(u, "error");
        cJSON_AddStringToObject(e, "code", up.err_code);
        cJSON_AddStringToObject(e, "message", up.err_msg);
        if (up.result_slot >= 0) {
            cJSON_AddNumberToObject(u, "stored_in_slot", up.result_slot + 1);
        }
    }
    return u;
}

/* HTTP status for an upload/activation error code. */
static const char *status_for(const char *code)
{
    static const struct { const char *code, *status; } map[] = {
        { "INVALID_IMAGE", HTTP_422 },     { "UNSUPPORTED_GEOMETRY", HTTP_422 },
        { "CHECKSUM_MISMATCH", HTTP_422 }, { "IMAGE_TOO_LARGE", HTTP_413 },
        { "NO_FREE_SLOT", HTTP_409 },      { "DRIVE_BUSY", HTTP_409 },
        { "UPLOAD_INCOMPLETE", HTTP_400 }, { "INSUFFICIENT_MEMORY", HTTP_507 },
        { "FLASH_ERROR", HTTP_500 },       { "PREPARE_FAILED", HTTP_500 },
        { "SLOT_EMPTY", HTTP_404 },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(map[i].code, code) == 0) {
            return map[i].status;
        }
    }
    return HTTP_500;
}

/* ---- Storing and activating (with api_lock held) ------------------------ */

/* Validate and store/activate a completely received upload. */
/* *raw: the received image; set to NULL when it is kept as the PSRAM disk. */
static void process_upload(uint8_t **rawp)
{
    const uint8_t *raw = *rawp;
    st_info_t st;
    st_result_t r = st_check_image(raw, up.size, &st);
    if (r != ST_OK) {
        fail_upload(r == ST_UNSUPPORTED ? "UNSUPPORTED_GEOMETRY"
                    : r == ST_TOO_LARGE ? "IMAGE_TOO_LARGE" : "INVALID_IMAGE", "%s", st.detail);
        return;
    }
    up.crc32 = slot_store_crc32(0, raw, up.size);
    if (up.have_crc && up.crc32 != up.expected_crc) {
        fail_upload("CHECKSUM_MISMATCH", "received data has CRC-32 %08lx, expected %08lx",
                    (unsigned long)up.crc32, (unsigned long)up.expected_crc);
        return;
    }

    disk_info_t info = { .source = up.to_flash ? DISK_SRC_FLASH : DISK_SRC_PSRAM,
                         .slot = -1, .size = up.size, .crc32 = up.crc32,
                         .cylinders = st.cylinders, .heads = st.heads, .sectors = st.sectors };
    memcpy(info.name, up.name, sizeof(info.name));

    if (up.to_flash) {
        int slot = up.slot;
        if (slot < 0) {
            slot = slot_store_find_free();
            if (slot < 0) {
                fail_upload("NO_FREE_SLOT", "all %d slots are in use", RF_SLOT_COUNT);
                return;
            }
        } else if (slot_store_is_valid(slot) || (slot_store_record(slot) &&
                   slot_store_record(slot)->status == SLOT_BUILDING)) {
            /* Explicit slot: overwrite on purpose; the old image is gone
             * from here on, also if this upload fails. */
            if (slot_store_delete(slot) != ESP_OK) {
                fail_upload("FLASH_ERROR", "could not free slot %d", slot + 1);
                return;
            }
            disk_note_slot_changed(slot);
        }
        esp_err_t err = slot_store_prepare(slot, up.name, SLOT_FMT_ST, up.size, up.crc32);
        if (err == ESP_OK) {
            err = slot_store_write(slot, 0, raw, up.size);
        }
        if (err == ESP_OK) {
            err = slot_store_commit(slot);      /* reads back, checks the CRC */
        }
        if (err != ESP_OK) {
            fail_upload("FLASH_ERROR", "writing slot %d failed (%s); slot left incomplete",
                        slot + 1, esp_err_to_name(err));
            return;
        }
        disk_note_slot_changed(slot);
        up.result_slot = slot;
        info.slot = slot;
        printf("API: \"%s\" stored in slot %d (%lu bytes, CRC32 %08lx)\n", up.name, slot + 1,
               (unsigned long)up.size, (unsigned long)up.crc32);
    }

    if (up.activate) {
        switch_error_t e;
        esp_err_t err = up.to_flash ? disk_switch_raw(raw, &info, &e)
                                    : disk_switch_psram_upload(*rawp, &info, &e);
        if (err != ESP_OK) {
            fail_upload(e.code, "%s%s", e.msg, up.to_flash ? " (image is stored in the slot)" : "");
            return;
        }
        if (!up.to_flash) {
            *rawp = NULL;           /* now owned by disk_switch as the PSRAM disk */
        }
        up.activated = true;
    }
    up.state = UP_DONE;
    up.touched_us = esp_timer_get_time();
}

/* ---- Handlers ----------------------------------------------------------- */

static esp_err_t get_status(httpd_req_t *req)
{
    wifi_net_status_t w;
    drive_status_t d;
    char id[8];

    wifi_net_get_status(&w);
    drive_refresh(&d);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device", "RadioFloppy");
    cJSON_AddStringToObject(root, "api", "v1");

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(wifi, "connected", w.connected);
    cJSON_AddStringToObject(wifi, "ssid", w.ssid);
    cJSON_AddStringToObject(wifi, "ip", w.ip);
    cJSON_AddNumberToObject(wifi, "rssi", w.rssi);

    cJSON *fl = cJSON_AddObjectToObject(root, "external_flash");
    cJSON_AddBoolToObject(fl, "detected", ext_flash_ready());
    snprintf(id, sizeof(id), "%06lx", (unsigned long)ext_flash_jedec_id());
    cJSON_AddStringToObject(fl, "jedec_id", id);
    cJSON_AddNumberToObject(fl, "size", ext_flash_size());
    slot_store_state_t ss = slot_store_state();
    cJSON_AddStringToObject(fl, "slot_store", ss == SLOT_STORE_VALID ? "valid"
                                             : ss == SLOT_STORE_BLANK ? "blank" : "invalid");
    int used = 0;
    for (int i = 0; i < RF_SLOT_COUNT; i++) {
        used += slot_store_is_valid(i);
    }
    cJSON_AddNumberToObject(fl, "slots_used", used);
    cJSON_AddNumberToObject(fl, "slots_total", RF_SLOT_COUNT);

    cJSON *drv = cJSON_AddObjectToObject(root, "drive");
    cJSON_AddStringToObject(drv, "select_line", EMU_SELECT_NAME);
    cJSON_AddBoolToObject(drv, "armed", d.armed);
    cJSON_AddBoolToObject(drv, "selected", d.selected);
    cJSON_AddBoolToObject(drv, "motor", d.motor);
    cJSON_AddNumberToObject(drv, "cylinder", d.cyl);

    cJSON_AddItemToObject(root, "current", current_json());
    disk_info_t pi;
    if (disk_switch_psram_info(&pi)) {
        char crc[9];
        hex32(crc, pi.crc32);
        cJSON *p = cJSON_AddObjectToObject(root, "psram_image");
        cJSON_AddStringToObject(p, "name", pi.name);
        cJSON_AddNumberToObject(p, "size", pi.size);
        cJSON_AddStringToObject(p, "crc32", crc);
        cJSON_AddNumberToObject(p, "sides", pi.heads);
        cJSON_AddNumberToObject(p, "cylinders", pi.cylinders);
        cJSON_AddNumberToObject(p, "sectors", pi.sectors);
    }
    cJSON_AddNumberToObject(root, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddBoolToObject(root, "auth_required", CONFIG_RADIOFLOPPY_API_TOKEN[0] != 0);

    xSemaphoreTake(api_lock, portMAX_DELAY);
    if (upload_in_progress() || up.state != UP_NONE) {
        cJSON_AddItemToObject(root, "upload", upload_json());
    }
    xSemaphoreGive(api_lock);
    return send_json(req, "200 OK", root);
}

static esp_err_t get_slots(httpd_req_t *req)
{
    disk_info_t d;
    char crc[9];

    disk_get_current(&d);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "slots");
    for (int i = 0; i < RF_SLOT_COUNT; i++) {
        const slot_record_t *r = slot_store_record(i);
        cJSON *s = cJSON_CreateObject();
        cJSON_AddNumberToObject(s, "slot", i + 1);
        uint8_t status = r ? r->status : SLOT_EMPTY;
        cJSON_AddStringToObject(s, "status", status == SLOT_VALID && !slot_store_is_valid(i)
                                             ? "invalid" : slot_status_name(status));
        if (status == SLOT_VALID) {
            char title[SLOT_TITLE_SIZE];
            slot_record_title(r, title);
            hex32(crc, r->crc32);
            cJSON_AddStringToObject(s, "name", title);
            cJSON_AddNumberToObject(s, "size", r->size);
            cJSON_AddStringToObject(s, "crc32", crc);
            cJSON_AddStringToObject(s, "format", "st");
        }
        cJSON_AddBoolToObject(s, "active", d.source == DISK_SRC_FLASH && d.slot == i &&
                                           !d.slot_changed);
        cJSON_AddItemToArray(arr, s);
    }
    return send_json(req, "200 OK", root);
}

static esp_err_t get_current(httpd_req_t *req)
{
    return send_json(req, "200 OK", current_json());
}

/* PUT /api/v1/current {"slot": n}: activate a stored slot. */
static esp_err_t put_current(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    if (!body) {
        return ESP_OK;
    }
    cJSON *js = cJSON_GetObjectItem(body, "slot");
    int slot = cJSON_IsNumber(js) && js->valuedouble == (int)js->valuedouble
               && js->valueint >= 1 && js->valueint <= RF_SLOT_COUNT ? js->valueint - 1 : -1;
    cJSON_Delete(body);
    if (slot < 0) {
        return send_error(req, HTTP_400, "INVALID_SLOT", "\"slot\" must be a number 1..%d",
                          RF_SLOT_COUNT);
    }

    xSemaphoreTake(api_lock, portMAX_DELAY);
    switch_error_t e;
    esp_err_t ret;
    if (disk_switch_slot(slot, &e) == ESP_OK) {
        ret = send_json(req, "200 OK", current_json());
    } else {
        ret = send_error(req, status_for(e.code), e.code, "%s", e.msg);
    }
    xSemaphoreGive(api_lock);
    return ret;
}

/* DELETE /api/v1/slots/{n} */
static esp_err_t delete_slot(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    int slot = parse_slot(req->uri + strlen("/api/v1/slots/"));
    if (slot < 0) {
        return send_error(req, HTTP_400, "INVALID_SLOT", "slot must be 1..%d", RF_SLOT_COUNT);
    }

    xSemaphoreTake(api_lock, portMAX_DELAY);
    esp_err_t ret;
    const slot_record_t *r = slot_store_record(slot);
    if (upload_in_progress() && up.to_flash && up.slot == slot) {
        ret = send_error(req, HTTP_409, "UPLOAD_BUSY", "an upload to slot %d is in progress",
                         slot + 1);
    } else if (!r || r->status == SLOT_EMPTY || r->status == SLOT_DELETED) {
        ret = send_error(req, HTTP_404, "SLOT_EMPTY", "slot %d is already free", slot + 1);
    } else if (slot_store_delete(slot) != ESP_OK) {
        ret = send_error(req, HTTP_500, "FLASH_ERROR", "catalog update failed");
    } else {
        disk_note_slot_changed(slot);
        printf("API: slot %d freed\n", slot + 1);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "slot", slot + 1);
        cJSON_AddStringToObject(root, "status", "deleted");
        ret = send_json(req, "200 OK", root);
    }
    xSemaphoreGive(api_lock);
    return ret;
}

/* POST /api/v1/uploads */
static esp_err_t post_upload(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    if (!body) {
        return ESP_OK;
    }

    const cJSON *jfn = cJSON_GetObjectItem(body, "filename");
    const cJSON *jsize = cJSON_GetObjectItem(body, "size");
    const cJSON *jdest = cJSON_GetObjectItem(body, "destination");
    const cJSON *jslot = cJSON_GetObjectItem(body, "slot");
    const cJSON *jact = cJSON_GetObjectItem(body, "activate");
    const cJSON *jcrc = cJSON_GetObjectItem(body, "crc32");
    esp_err_t ret;

    xSemaphoreTake(api_lock, portMAX_DELAY);
    if (upload_in_progress()) {
        ret = send_error(req, HTTP_409, "UPLOAD_BUSY", "upload %lu is still in progress",
                         (unsigned long)up.id);
        goto out;
    }
    if (!cJSON_IsString(jfn) || !cJSON_IsNumber(jsize) || jsize->valuedouble < 0 ||
        jsize->valuedouble > 0xffffffffu || !cJSON_IsString(jdest)) {
        ret = send_error(req, HTTP_400, "INVALID_REQUEST",
                         "\"filename\" (string), \"size\" (number) and \"destination\" are required");
        goto out;
    }
    bool to_flash = strcmp(jdest->valuestring, "flash") == 0;
    if (!to_flash && strcmp(jdest->valuestring, "psram") != 0) {
        ret = send_error(req, HTTP_400, "INVALID_REQUEST",
                         "\"destination\" must be \"psram\" or \"flash\"");
        goto out;
    }
    if (jact && !cJSON_IsBool(jact)) {
        ret = send_error(req, HTTP_400, "INVALID_REQUEST", "\"activate\" must be true or false");
        goto out;
    }
    bool activate = jact ? cJSON_IsTrue(jact) : !to_flash;
    if (!to_flash && !activate) {
        ret = send_error(req, HTTP_400, "INVALID_REQUEST",
                         "a PSRAM upload is always activated; \"activate\": false is not supported");
        goto out;
    }
    int slot = -1;
    if (jslot) {
        if (!to_flash) {
            ret = send_error(req, HTTP_400, "INVALID_SLOT", "\"slot\" only applies to \"flash\"");
            goto out;
        }
        slot = cJSON_IsNumber(jslot) && jslot->valuedouble == (int)jslot->valuedouble &&
               jslot->valueint >= 1 && jslot->valueint <= RF_SLOT_COUNT ? jslot->valueint - 1 : -1;
        if (slot < 0) {
            ret = send_error(req, HTTP_400, "INVALID_SLOT", "\"slot\" must be a number 1..%d",
                             RF_SLOT_COUNT);
            goto out;
        }
    }
    bool have_crc = false;
    uint32_t crc = 0;
    if (jcrc) {
        char *end = NULL;
        if (cJSON_IsString(jcrc)) {
            crc = strtoul(jcrc->valuestring, &end, 16);
        }
        if (!end || *end || end == jcrc->valuestring) {
            ret = send_error(req, HTTP_400, "INVALID_REQUEST",
                             "\"crc32\" must be a hex string, e.g. \"42ce7eed\"");
            goto out;
        }
        have_crc = true;
    }

    uint32_t size = (uint32_t)jsize->valuedouble;
    st_info_t st;
    st_result_t r = st_check_size(size, &st);
    if (r != ST_OK) {
        const char *code = r == ST_TOO_LARGE ? "IMAGE_TOO_LARGE"
                           : r == ST_UNSUPPORTED ? "UNSUPPORTED_GEOMETRY" : "INVALID_IMAGE";
        ret = send_error(req, status_for(code), code, "%s", st.detail);
        goto out;
    }
    if (to_flash) {
        if (!ext_flash_ready() || slot_store_state() == SLOT_STORE_INVALID) {
            ret = send_error(req, HTTP_503, "FLASH_ERROR", "external flash / slot store not usable");
            goto out;
        }
        if (slot < 0 && slot_store_find_free() < 0) {
            ret = send_error(req, HTTP_409, "NO_FREE_SLOT", "all %d slots are in use",
                             RF_SLOT_COUNT);
            goto out;
        }
    }
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < size) {
        ret = send_error(req, HTTP_507, "INSUFFICIENT_MEMORY", "no PSRAM for a %lu byte upload",
                         (unsigned long)size);
        goto out;
    }

    memset(&up, 0, sizeof(up));
    up.id = next_upload_id++;
    up.state = UP_READY;
    disk_title_from_filename(jfn->valuestring, up.name);
    up.size = size;
    up.to_flash = to_flash;
    up.slot = slot;
    up.activate = activate;
    up.have_crc = have_crc;
    up.expected_crc = crc;
    up.result_slot = -1;
    up.touched_us = esp_timer_get_time();
    ret = send_json(req, "201 Created", upload_json());
out:
    xSemaphoreGive(api_lock);
    cJSON_Delete(body);
    return ret;
}

/* Upload id from /api/v1/uploads/{id}[/data]; *data: path ends in /data. */
static uint32_t upload_id_from_uri(const char *uri, bool *data)
{
    const char *p = uri + strlen("/api/v1/uploads/");
    char *end;
    unsigned long id = strtoul(p, &end, 10);

    *data = strcmp(end, "/data") == 0;
    return (end != p && (*end == 0 || *data)) ? (uint32_t)id : 0;
}

/* GET /api/v1/uploads/{id} */
static esp_err_t get_upload(httpd_req_t *req)
{
    bool data;
    uint32_t id = upload_id_from_uri(req->uri, &data);
    esp_err_t ret;

    xSemaphoreTake(api_lock, portMAX_DELAY);
    expire_upload();
    if (data || id == 0 || id != up.id || up.state == UP_NONE) {
        ret = send_error(req, HTTP_404, "UPLOAD_NOT_FOUND", "no such upload");
    } else {
        ret = send_json(req, "200 OK", upload_json());
    }
    xSemaphoreGive(api_lock);
    return ret;
}

/* PUT /api/v1/uploads/{id}/data: the raw image bytes. */
static esp_err_t put_upload_data(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    bool data;
    uint32_t id = upload_id_from_uri(req->uri, &data);
    esp_err_t ret;

    xSemaphoreTake(api_lock, portMAX_DELAY);
    expire_upload();
    if (!data || id == 0 || id != up.id || up.state == UP_NONE) {
        ret = send_error(req, HTTP_404, "UPLOAD_NOT_FOUND", "no such upload");
        goto out;
    }
    if (up.state != UP_READY) {
        ret = send_error(req, HTTP_409, "UPLOAD_STATE", "upload is %s, data can only be sent once",
                         state_names[up.state]);
        goto out;
    }
    if (!content_type_is(req, "application/octet-stream")) {
        ret = ESP_OK;               /* error response already sent */
        goto out;
    }
    if (req->content_len != up.size) {
        fail_upload("UPLOAD_INCOMPLETE", "Content-Length %u does not match the announced size %lu",
                    (unsigned)req->content_len, (unsigned long)up.size);
        ret = send_json(req, HTTP_400, upload_json());
        goto out;
    }

    uint8_t *raw = heap_caps_malloc(up.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *chunk = heap_caps_malloc(RECV_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!raw || !chunk) {
        free(raw);
        free(chunk);
        fail_upload("INSUFFICIENT_MEMORY", "no memory for the upload buffer");
        ret = send_json(req, HTTP_507, upload_json());
        goto out;
    }

    up.state = UP_RECEIVING;
    up.touched_us = esp_timer_get_time();
    while (up.received < up.size) {
        uint32_t want = up.size - up.received < RECV_CHUNK ? up.size - up.received : RECV_CHUNK;
        int n = httpd_req_recv(req, (char *)chunk, want);
        if (n <= 0) {
            break;      /* closed or timed out (recv_wait_timeout) */
        }
        memcpy(raw + up.received, chunk, n);
        up.received += n;
        up.touched_us = esp_timer_get_time();
    }
    free(chunk);

    const char *status = "200 OK";
    if (up.received != up.size) {
        fail_upload("UPLOAD_INCOMPLETE", "received %lu of %lu bytes",
                    (unsigned long)up.received, (unsigned long)up.size);
        status = HTTP_400;
    } else {
        up.state = UP_PROCESSING;
        process_upload(&raw);
        if (up.state == UP_FAILED) {
            status = status_for(up.err_code);
        }
    }
    free(raw);
    if (up.state == UP_FAILED) {
        printf("API: upload %lu failed: %s - %s\n", (unsigned long)up.id, up.err_code, up.err_msg);
    }
    ret = send_json(req, status, upload_json());
out:
    xSemaphoreGive(api_lock);
    return ret;
}

/* ---- Web interface ------------------------------------------------------ */

/* web/index.html, embedded in the firmware (EMBED_TXTFILES, NUL-terminated). */
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static esp_err_t get_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t get_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ---- Server ------------------------------------------------------------- */

esp_err_t api_start(void)
{
    api_lock = xSemaphoreCreateMutex();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.core_id = 1;                    /* away from the floppy ISRs */
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 12;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        printf("API: HTTP server failed to start (%s)\n", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/api/v1/status",    .method = HTTP_GET,    .handler = get_status },
        { .uri = "/api/v1/slots",     .method = HTTP_GET,    .handler = get_slots },
        { .uri = "/api/v1/slots/*",   .method = HTTP_DELETE, .handler = delete_slot },
        { .uri = "/api/v1/current",   .method = HTTP_GET,    .handler = get_current },
        { .uri = "/api/v1/current",   .method = HTTP_PUT,    .handler = put_current },
        { .uri = "/api/v1/uploads",   .method = HTTP_POST,   .handler = post_upload },
        { .uri = "/api/v1/uploads/*", .method = HTTP_GET,    .handler = get_upload },
        { .uri = "/api/v1/uploads/*", .method = HTTP_PUT,    .handler = put_upload_data },
        { .uri = "/",                 .method = HTTP_GET,    .handler = get_index },
        { .uri = "/favicon.ico",      .method = HTTP_GET,    .handler = get_favicon },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    printf("API: HTTP server on port %d, web interface on /, API on /api/v1/ (%s)\n", cfg.server_port,
           CONFIG_RADIOFLOPPY_API_TOKEN[0] ? "token required" : "open, no token");
    return ESP_OK;
}
