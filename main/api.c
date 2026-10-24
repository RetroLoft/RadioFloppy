/*
 * RadioFloppy HTTP API, /api/v1/. Documented in docs/API.md. Also serves the
 * small web interface (web/index.html) on /, which only uses this API.
 *
 * Runs in the esp_http_server task on core 1, away from the floppy
 * interrupts and the flux stream (core 0). Requests are handled one at a
 * time; api_lock additionally serialises everything that changes the image
 * library or the active disk (the image store requires serialised writers).
 *
 * Uploads are two-step: POST /uploads (JSON options, checked before any
 * data is sent) and PUT /uploads/{id}/data (raw .ST bytes). The bytes go
 * into a PSRAM staging buffer first; only a complete, validated image is
 * stored in the image library and/or prepared as the active disk, so an
 * interrupted or invalid upload never touches the library or the drive.
 * Clients work with images (id, sequence); blocks are never exposed.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "esp_timer.h"

#include "api.h"
#include "disk_image.h"
#include "disk_switch.h"
#include "disk_title.h"
#include "disk_write.h"
#include "flux_monitor.h"
#include "flux_stream.h"
#include "drive_config.h"
#include "drive_emu.h"
#include "ext_flash.h"
#include "machine.h"
#include "sd_card.h"
#include "sd_path.h"
#include "settings.h"
#include "step_sound.h"
#include "hfe.h"
#include "st_format.h"
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
    bool hfe;               /* .hfe (to the SD card only) */
    bool to_sd;             /* destination "sd": a file below the root directory */
    char sd_folder[SD_REL_MAX + 1];
    char sd_name[SD_NAME_MAX + 1];
    bool overwrite;
    bool activate;
    bool have_crc;
    uint32_t expected_crc;
    uint32_t received;
    uint32_t crc32;
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

static cJSON *write_json(void)
{
    write_status_t w;
    disk_write_status(&w);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", disk_write_state_name(w.state));
    cJSON_AddBoolToObject(o, "writable", w.writable);
    cJSON_AddStringToObject(o, "reason", w.reason);
    cJSON_AddBoolToObject(o, "unsaved", w.state == WRITE_PENDING || w.state == WRITE_SAVING ||
                                        w.state == WRITE_ERROR);
    if (w.error[0]) {
        cJSON_AddStringToObject(o, "error", w.error);
    }
    if (w.note[0]) {
        cJSON_AddStringToObject(o, "note", w.note);
    }
    cJSON_AddNumberToObject(o, "sectors_written", w.sectors_written);
    cJSON_AddNumberToObject(o, "writes_rejected", w.writes_rejected);
    cJSON_AddNumberToObject(o, "saves", w.saves);
    return o;
}

static cJSON *current_json(void)
{
    disk_info_t d;
    char crc[9];

    disk_get_current(&d);
    cJSON *c = cJSON_CreateObject();
    cJSON_AddBoolToObject(c, "inserted", d.source != DISK_SRC_NONE);
    cJSON_AddStringToObject(c, "source", d.source == DISK_SRC_PSRAM ? "psram"
                                        : d.source == DISK_SRC_SD ? "sd" : "none");
    if (d.source == DISK_SRC_SD) {
        cJSON_AddStringToObject(c, "sd_path", d.sd_path);
        cJSON_AddBoolToObject(c, "sd_copy", d.sd_copy);
        cJSON_AddBoolToObject(c, "image_changed_since", d.image_changed);
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
    cJSON_AddItemToObject(c, "write", write_json());
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
    cJSON_AddStringToObject(u, "destination", up.to_sd ? "sd" : "psram");
    if (up.to_sd) {
        cJSON_AddStringToObject(u, "path", up.sd_folder);
        cJSON_AddStringToObject(u, "file", up.sd_name);
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
        cJSON_AddBoolToObject(u, "active", up.activated);
    }
    if (up.state == UP_FAILED) {
        cJSON *e = cJSON_AddObjectToObject(u, "error");
        cJSON_AddStringToObject(e, "code", up.err_code);
        cJSON_AddStringToObject(e, "message", up.err_msg);
    }
    return u;
}

/* HTTP status for an upload/activation error code. */
static const char *status_for(const char *code)
{
    static const struct { const char *code, *status; } map[] = {
        { "INVALID_IMAGE", HTTP_422 },     { "UNSUPPORTED_GEOMETRY", HTTP_422 },
        { "CHECKSUM_MISMATCH", HTTP_422 }, { "IMAGE_TOO_LARGE", HTTP_413 },
        { "NO_SPACE", HTTP_507 },          { "DRIVE_BUSY", HTTP_409 },
        { "UPLOAD_INCOMPLETE", HTTP_400 }, { "INSUFFICIENT_MEMORY", HTTP_507 },
        { "FLASH_ERROR", HTTP_500 },       { "PREPARE_FAILED", HTTP_500 },
        { "IMAGE_NOT_FOUND", HTTP_404 },   { "STORAGE_NOT_READY", HTTP_503 },
        { "MACHINE_NOT_SUPPORTED", HTTP_409 },   { "UNSAVED_CHANGES", HTTP_409 },
        { "UNSUPPORTED_FORMAT", HTTP_422 },   { "INVALID_NAME", HTTP_400 },
        { "UPLOAD_BUSY", HTTP_409 },        { "SD_ERROR", HTTP_500 },
        { "SD_NOT_PRESENT", HTTP_503 },    { "FOLDER_NOT_FOUND", HTTP_404 },
        { "FILE_EXISTS", HTTP_409 },    { "FILE_READ_ONLY", HTTP_409 },        { "FILE_NOT_FOUND", HTTP_404 },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(map[i].code, code) == 0) {
            return map[i].status;
        }
    }
    return HTTP_500;
}

/* ---- Storing and activating (with api_lock held) ------------------------ */

/* Validate and activate a completely received temporary (PSRAM) upload. */
/* *raw: the received image; set to NULL when it is kept as the PSRAM disk. */
static void process_upload(uint8_t **rawp)
{
    const uint8_t *raw = *rawp;
    st_info_t st;

    if (hfe_signature(raw, up.size)) {
        fail_upload("INVALID_IMAGE", "this is an HFE file: upload it with the .hfe extension");
        return;
    }
    st_result_t r = st_check_image(raw, up.size, &st);
    if (r != ST_OK) {
        fail_upload(r == ST_UNSUPPORTED ? "UNSUPPORTED_GEOMETRY"
                    : r == ST_TOO_LARGE ? "IMAGE_TOO_LARGE" : "INVALID_IMAGE", "%s", st.detail);
        return;
    }
    if (st.detail[0]) {
        printf("API: note on \"%s\": %s\n", up.name, st.detail);
    }
    up.crc32 = esp_rom_crc32_le(0, raw, up.size);
    if (up.have_crc && up.crc32 != up.expected_crc) {
        fail_upload("CHECKSUM_MISMATCH", "received data has CRC-32 %08lx, expected %08lx",
                    (unsigned long)up.crc32, (unsigned long)up.expected_crc);
        return;
    }

    disk_info_t info = { .source = DISK_SRC_PSRAM, .size = up.size, .crc32 = up.crc32,
                         .cylinders = st.cylinders, .heads = st.heads, .sectors = st.sectors };
    memcpy(info.name, up.name, sizeof(info.name));
    switch_error_t e;
    if (disk_switch_psram_upload(*rawp, &info, &e) != ESP_OK) {
        fail_upload(e.code, "%s", e.msg);
        return;
    }
    *rawp = NULL;                   /* now owned by disk_switch as the PSRAM disk */
    up.activated = true;
    up.state = UP_DONE;
    up.touched_us = esp_timer_get_time();
}

/* ---- SD card: files below the root directory ------------------------------------- */

#define SD_HFE_MAX      (8u * 1024u * 1024u)    /* the track buffer fit decides */
#define SD_LIST_MAX     400
#define QUERY_MAX       (3 * SD_REL_MAX + 64)   /* %-encoded path and a few keys */

/* API error for an SD card result. */
static esp_err_t sd_error(httpd_req_t *req, sd_result_t r, bool folder)
{
    switch (r) {
    case SD_NO_CARD:
        return send_error(req, HTTP_503, "SD_NOT_PRESENT", "no SD card");
    case SD_INVALID:
        return send_error(req, HTTP_400, "INVALID_PATH", "path: names separated by '/' below the "
                          "root directory, at most %d characters; no \"..\", no '/' at the start or "
                          "end, no name starting with '.' or ' ' or ending with '.' or ' ', none of "
                          "\\ : * ? \" < > |", SD_REL_MAX);
    case SD_NOT_FOUND:
        return send_error(req, HTTP_404, folder ? "FOLDER_NOT_FOUND" : "FILE_NOT_FOUND",
                          folder ? "no such folder" : "no such file");
    case SD_EXISTS:
        return send_error(req, HTTP_409, folder ? "FOLDER_EXISTS" : "FILE_EXISTS",
                          folder ? "a folder or file of that name exists" : "the file exists");
    case SD_READ_ONLY:
        return send_error(req, HTTP_409, "FILE_READ_ONLY", "the file is read-only; make it "
                          "writable first (PUT /api/v1/sd/files {\"read_only\": false})");
    case SD_NOT_EMPTY:
        return send_error(req, HTTP_409, "FOLDER_NOT_EMPTY", "the folder is not empty; delete it "
                          "with recursive=true&confirm=<its path> to remove its contents too");
    default:
        return send_error(req, HTTP_500, "SD_ERROR", "SD card error");
    }
}

/* Query parameter key, %-decoded, into out. ESP_ERR_NOT_FOUND: missing;
 * ESP_ERR_INVALID_ARG: malformed or too long. */
static esp_err_t query_param(httpd_req_t *req, const char *key, char *out, size_t size)
{
    char q[QUERY_MAX];
    size_t n = httpd_req_get_url_query_len(req);

    if (n == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (n >= sizeof(q) || httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = httpd_query_key_value(q, key, out, size);
    if (err == ESP_ERR_NOT_FOUND) {
        return err;
    }
    return err == ESP_OK && sd_url_decode(out) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static void list_entry(void *ctx, const sd_entry_t *e)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", e->name);
    cJSON_AddStringToObject(o, "type", e->is_dir ? "folder" : "file");
    if (!e->is_dir) {
        cJSON_AddNumberToObject(o, "size", e->size);
        cJSON_AddBoolToObject(o, "read_only", e->read_only);
    }
    cJSON_AddItemToArray(ctx, o);
}

/* GET /api/v1/sd/files?path=<folder>: the contents of a folder. */
static esp_err_t get_sd_files(httpd_req_t *req)
{
    char path[SD_REL_MAX + 1] = "";
    esp_err_t err = query_param(req, "path", path, sizeof(path));
    if (err == ESP_ERR_INVALID_ARG) {
        return sd_error(req, SD_INVALID, true);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *list = cJSON_CreateArray();
    bool more = false;
    sd_result_t r = sd_card_list(path, SD_LIST_MAX, list_entry, list, &more);
    if (r != SD_OK) {
        cJSON_Delete(list);
        cJSON_Delete(root);
        return sd_error(req, r, true);
    }
    settings_t cfg;
    settings_get(&cfg);
    cJSON_AddStringToObject(root, "root_dir", cfg.root_dir);
    cJSON_AddStringToObject(root, "path", path);
    cJSON_AddItemToObject(root, "entries", list);
    cJSON_AddBoolToObject(root, "truncated", more);
    return send_json(req, "200 OK", root);
}

/* DELETE /api/v1/sd/files?path=<file> */
static esp_err_t delete_sd_file(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    char path[SD_REL_MAX + 1];
    if (query_param(req, "path", path, sizeof(path)) != ESP_OK) {
        return sd_error(req, SD_INVALID, false);
    }
    xSemaphoreTake(api_lock, portMAX_DELAY);
    sd_result_t r = sd_card_remove(path);
    xSemaphoreGive(api_lock);
    if (r == SD_OK) {
        disk_note_sd_changed(path, false);
        disk_write_refresh();
    }
    if (r != SD_OK) {
        return sd_error(req, r, false);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "path", path);
    cJSON_AddStringToObject(root, "status", "deleted");
    return send_json(req, "200 OK", root);
}

/* PUT /api/v1/sd/files?path=<file> {"read_only": true|false}: the FAT
 * read-only attribute, i.e. the write-protect tab of that disk. */
static esp_err_t put_sd_file(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    char path[SD_REL_MAX + 1];
    if (query_param(req, "path", path, sizeof(path)) != ESP_OK || !path[0]) {
        return sd_error(req, SD_INVALID, false);
    }
    cJSON *body = read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const cJSON *jro = cJSON_GetObjectItem(body, "read_only");
    bool ro = cJSON_IsTrue(jro);
    bool valid = cJSON_IsBool(jro);
    cJSON_Delete(body);
    if (!valid) {
        return send_error(req, HTTP_400, "INVALID_REQUEST", "\"read_only\" (true/false) is required");
    }
    disk_info_t d;
    disk_get_current(&d);
    if (ro && d.source == DISK_SRC_SD && strcmp(d.sd_path, path) == 0 && disk_write_unsaved()) {
        return send_error(req, HTTP_409, "UNSAVED_CHANGES", "the computer wrote to this disk and "
                          "the changes are not saved yet; try again in a few seconds");
    }
    xSemaphoreTake(api_lock, portMAX_DELAY);
    sd_result_t r = sd_card_set_read_only(path, ro);
    xSemaphoreGive(api_lock);
    if (r != SD_OK) {
        return sd_error(req, r, false);
    }
    disk_note_sd_read_only(path, ro);
    disk_write_refresh();               /* write protection of the inserted disk */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "path", path);
    cJSON_AddBoolToObject(root, "read_only", ro);
    return send_json(req, "200 OK", root);
}

/* POST /api/v1/sd/folders {"path": "<folder>"}: create it (and the ones on the way). */
static esp_err_t post_sd_folder(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const cJSON *jp = cJSON_GetObjectItem(body, "path");
    char path[SD_REL_MAX + 1];
    if (!cJSON_IsString(jp) || strlen(jp->valuestring) >= sizeof(path) || !jp->valuestring[0]) {
        cJSON_Delete(body);
        return sd_error(req, SD_INVALID, true);
    }
    snprintf(path, sizeof(path), "%s", jp->valuestring);
    cJSON_Delete(body);
    xSemaphoreTake(api_lock, portMAX_DELAY);
    sd_result_t r = sd_card_mkdir(path);
    xSemaphoreGive(api_lock);
    if (r != SD_OK) {
        return sd_error(req, r, true);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "path", path);
    cJSON_AddStringToObject(root, "status", "created");
    return send_json(req, "201 Created", root);
}

/* DELETE /api/v1/sd/folders?path=<folder>[&recursive=true&confirm=<folder>] */
static esp_err_t delete_sd_folder(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    char path[SD_REL_MAX + 1], rec[8] = "", confirm[SD_REL_MAX + 1] = "";
    if (query_param(req, "path", path, sizeof(path)) != ESP_OK || !path[0]) {
        return sd_error(req, SD_INVALID, true);
    }
    query_param(req, "recursive", rec, sizeof(rec));
    query_param(req, "confirm", confirm, sizeof(confirm));
    bool recursive = strcmp(rec, "true") == 0;
    if (recursive && strcmp(confirm, path) != 0) {
        return send_error(req, HTTP_400, "CONFIRMATION_REQUIRED", "deleting a folder with its "
                          "contents needs confirm=<the same path>: everything in it is lost");
    }
    xSemaphoreTake(api_lock, portMAX_DELAY);
    sd_result_t r = sd_card_rmdir(path, recursive);
    xSemaphoreGive(api_lock);
    if (r == SD_OK) {
        disk_note_sd_changed(path, true);
        disk_write_refresh();
    }
    if (r != SD_OK) {
        return sd_error(req, r, true);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "path", path);
    cJSON_AddStringToObject(root, "status", "deleted");
    return send_json(req, "200 OK", root);
}

/* POST /api/v1/uploads with "destination": "sd" (api_lock held, no upload running). */
static esp_err_t post_upload_sd(httpd_req_t *req, const cJSON *body)
{
    const cJSON *jfn = cJSON_GetObjectItem(body, "filename");
    const cJSON *jsize = cJSON_GetObjectItem(body, "size");
    const cJSON *jpath = cJSON_GetObjectItem(body, "path");
    const cJSON *jover = cJSON_GetObjectItem(body, "overwrite");
    const cJSON *jact = cJSON_GetObjectItem(body, "activate");
    const cJSON *jcrc = cJSON_GetObjectItem(body, "crc32");

    if (!cJSON_IsString(jfn) || !cJSON_IsNumber(jsize) || jsize->valuedouble < 0 ||
        jsize->valuedouble > 0xffffffffu || (jpath && !cJSON_IsString(jpath)) ||
        (jover && !cJSON_IsBool(jover))) {
        return send_error(req, HTTP_400, "INVALID_REQUEST", "\"filename\" (string) and \"size\" "
                          "(number) are required; \"path\" (string) and \"overwrite\" (true/false) "
                          "are optional");
    }
    if (cJSON_IsTrue(jact)) {
        return send_error(req, HTTP_400, "INVALID_REQUEST", "an upload to the SD card is not "
                          "inserted: \"activate\" must be false; insert it with PUT /api/v1/current");
    }
    const char *name = jfn->valuestring, *folder = jpath ? jpath->valuestring : "";
    if (!sd_name_valid(name)) {
        return send_error(req, HTTP_422, "INVALID_NAME", "file name: at most %d characters, none "
                          "of \\ / : * ? \" < > |, not starting with '.' or ' ', not ending with "
                          "'.' or ' '", SD_NAME_MAX);
    }
    if (!sd_relpath_valid(folder) || strlen(folder) > SD_REL_MAX) {
        return sd_error(req, SD_INVALID, true);
    }
    size_t fl = strlen(name);
    bool hfe = fl >= 4 && strcasecmp(name + fl - 4, ".hfe") == 0;
    bool st = fl >= 3 && strcasecmp(name + fl - 3, ".st") == 0;
    if (!hfe && !st) {
        return send_error(req, HTTP_422, "UNSUPPORTED_FORMAT", "only .ST and .HFE images can be "
                          "stored on the SD card");
    }
    uint32_t size = (uint32_t)jsize->valuedouble;
    st_info_t sti;
    if (hfe && (size < 512 || size > SD_HFE_MAX)) {
        return send_error(req, status_for("IMAGE_TOO_LARGE"), "IMAGE_TOO_LARGE",
                          "%lu bytes, an HFE image on the SD card may have at most %u",
                          (unsigned long)size, SD_HFE_MAX);
    }
    if (st) {
        st_result_t r = st_check_size(size, &sti);
        if (r != ST_OK) {
            const char *code = r == ST_TOO_LARGE ? "IMAGE_TOO_LARGE"
                               : r == ST_UNSUPPORTED ? "UNSUPPORTED_GEOMETRY" : "INVALID_IMAGE";
            return send_error(req, status_for(code), code, "%s", sti.detail);
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
            return send_error(req, HTTP_400, "INVALID_REQUEST",
                              "\"crc32\" must be a hex string, e.g. \"42ce7eed\"");
        }
        have_crc = true;
    }
    /* The folder must exist; the file only with "overwrite". */
    char rel[SD_REL_MAX + SD_NAME_MAX + 2];
    bool is_dir = false;
    uint32_t old_size;
    sd_result_t r = sd_card_stat(folder, &is_dir, &old_size);
    if (r == SD_OK && !is_dir) {
        r = SD_NOT_FOUND;
    }
    if (r != SD_OK) {
        return sd_error(req, r, true);
    }
    snprintf(rel, sizeof(rel), "%s%s%s", folder, folder[0] ? "/" : "", name);
    if (strlen(rel) <= SD_REL_MAX && sd_card_stat(rel, &is_dir, &old_size) == SD_OK &&
        (is_dir || !cJSON_IsTrue(jover))) {
        return sd_error(req, SD_EXISTS, is_dir);
    }

    memset(&up, 0, sizeof(up));
    up.id = next_upload_id++;
    up.state = UP_READY;
    disk_title_from_filename(name, up.name);
    up.size = size;
    up.to_sd = true;
    up.hfe = hfe;
    snprintf(up.sd_folder, sizeof(up.sd_folder), "%s", folder);
    snprintf(up.sd_name, sizeof(up.sd_name), "%s", name);
    up.overwrite = cJSON_IsTrue(jover);
    up.have_crc = have_crc;
    up.expected_crc = crc;
    up.touched_us = esp_timer_get_time();
    return send_json(req, "201 Created", upload_json());
}

static void fail_sd(sd_result_t r)
{
    switch (r) {
    case SD_NO_CARD:   fail_upload("SD_NOT_PRESENT", "no SD card"); break;
    case SD_NOT_FOUND: fail_upload("FOLDER_NOT_FOUND", "the folder does not exist (any more)"); break;
    case SD_EXISTS:    fail_upload("FILE_EXISTS", "the file exists"); break;
    case SD_READ_ONLY: fail_upload("FILE_READ_ONLY", "the file is read-only; nothing stored"); break;
    case SD_INVALID:   fail_upload("INVALID_NAME", "invalid file or folder name"); break;
    default:           fail_upload("SD_ERROR", "writing to the SD card failed; nothing stored"); break;
    }
}

/*
 * PUT data of an upload to the SD card: written straight into a hidden
 * temporary file while it arrives (CRC, and for HFE the stream check);
 * checked as an image afterwards; only then, after reading it back, the
 * file gets its name.
 */
static esp_err_t receive_sd(httpd_req_t *req)
{
    uint8_t *chunk = heap_caps_malloc(RECV_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t head[512];
    uint32_t head_n = 0, crc = 0;
    hfe_stream_t *hs = NULL;
    uint8_t *region = NULL;
    hfe_pack_t pack;
    hfe_result_t hr = HFE_OK;
    sd_writer_t *w = NULL;
    int64_t t0 = esp_timer_get_time();

    if (up.hfe) {
        hs = heap_caps_malloc(sizeof(*hs), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        region = heap_caps_malloc(HFE_REGION_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    sd_result_t r = SD_IO;
    if (!chunk || (up.hfe && (!hs || !region))) {
        fail_upload("INSUFFICIENT_MEMORY", "no memory for the upload");
    } else if ((r = sd_card_writer_open(up.sd_folder, up.sd_name, up.overwrite, &w)) != SD_OK) {
        fail_sd(r);
    } else {
        up.state = UP_RECEIVING;
        if (hs) {
            hfe_pack_init(&pack, NULL, DISK_TRACKS_BYTES);
            hfe_stream_init(hs, up.size, region, &pack);
        }
    }
    up.touched_us = esp_timer_get_time();
    while (up.received < up.size && chunk) {
        uint32_t want = up.size - up.received < RECV_CHUNK ? up.size - up.received : RECV_CHUNK;
        int n = httpd_req_recv(req, (char *)chunk, want);
        if (n <= 0) {
            break;      /* closed or timed out (recv_wait_timeout) */
        }
        up.received += n;
        up.touched_us = esp_timer_get_time();
        if (up.state != UP_RECEIVING) {
            continue;   /* refused early: drain the rest */
        }
        crc = esp_rom_crc32_le(crc, chunk, n);
        if (head_n < sizeof(head)) {
            uint32_t k = sizeof(head) - head_n < (uint32_t)n ? sizeof(head) - head_n : (uint32_t)n;
            memcpy(head + head_n, chunk, k);
            head_n += k;
        }
        if (hs && (hr = hfe_stream_feed(hs, chunk, n)) != HFE_OK) {
            fail_upload(hr == HFE_V2 || hr == HFE_UNSUPPORTED || hr == HFE_BAD_LAYOUT ? "UNSUPPORTED_FORMAT"
                        : hr == HFE_TOO_LARGE ? "IMAGE_TOO_LARGE" : "INVALID_IMAGE",
                        "%s", hfe_stream_info(hs)->detail);
        } else if ((r = sd_card_writer_write(w, chunk, n)) != SD_OK) {
            fail_sd(r);
        }
    }
    free(chunk);

    if (up.state == UP_RECEIVING && up.received != up.size) {
        fail_upload("UPLOAD_INCOMPLETE", "received %lu of %lu bytes",
                    (unsigned long)up.received, (unsigned long)up.size);
    }
    if (up.state == UP_RECEIVING) {
        up.state = UP_PROCESSING;
        up.crc32 = crc;
        st_info_t st;
        if (hs && (hr = hfe_stream_end(hs)) != HFE_OK) {
            fail_upload(hr == HFE_TOO_LARGE ? "IMAGE_TOO_LARGE" : "INVALID_IMAGE", "%s",
                        hfe_stream_info(hs)->detail);
        } else if (!hs && hfe_signature(head, head_n)) {
            fail_upload("INVALID_IMAGE", "this is an HFE file: upload it with the .hfe extension");
        } else if (!hs && st_check_image(head, up.size, &st) != ST_OK) {
            fail_upload("INVALID_IMAGE", "%s", st.detail);
        } else if (up.have_crc && crc != up.expected_crc) {
            fail_upload("CHECKSUM_MISMATCH", "received data has CRC-32 %08lx, expected %08lx",
                        (unsigned long)crc, (unsigned long)up.expected_crc);
        } else {
            if (!hs && st.detail[0]) {
                printf("API: note on \"%s\": %s\n", up.sd_name, st.detail);
            }
            r = sd_card_writer_commit(w, crc);
            w = NULL;                   /* freed by the commit */
            if (r != SD_OK) {
                fail_sd(r);
            } else {
                if (up.overwrite) {
                    char rel[SD_REL_MAX + SD_NAME_MAX + 2];
                    snprintf(rel, sizeof(rel), "%s%s%s", up.sd_folder, up.sd_folder[0] ? "/" : "",
                             up.sd_name);
                    disk_note_sd_changed(rel, false);
                    disk_write_refresh();
                }
                up.state = UP_DONE;
                up.touched_us = esp_timer_get_time();
                printf("API: \"%s\" stored on the SD card in %s%s (%lu bytes, CRC32 %08lx, %lld ms)\n",
                       up.sd_name, up.sd_folder[0] ? up.sd_folder : "the root directory",
                       up.overwrite ? ", replacing" : "", (unsigned long)up.size,
                       (unsigned long)crc, (esp_timer_get_time() - t0) / 1000);
            }
        }
    }
    sd_card_writer_abort(w);
    free(hs);
    free(region);
    if (up.state == UP_FAILED) {
        printf("API: upload %lu failed: %s - %s\n", (unsigned long)up.id, up.err_code, up.err_msg);
    }
    return send_json(req, up.state == UP_DONE ? "200 OK" : status_for(up.err_code), upload_json());
}

/* ---- Handlers ----------------------------------------------------------- */

static esp_err_t get_status(httpd_req_t *req)
{
    wifi_net_status_t w;
    drive_status_t d;
    wifi_net_get_status(&w);
    drive_peek(&d);                 /* read only: an API call never writes outputs */

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device", "RadioFloppy");
    cJSON_AddStringToObject(root, "api", "v1");
    cJSON_AddStringToObject(root, "firmware", esp_app_get_description()->version);
    cJSON_AddStringToObject(root, "hostname", w.hostname);

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(wifi, "connected", w.connected);
    cJSON_AddStringToObject(wifi, "ssid", w.ssid);
    cJSON_AddStringToObject(wifi, "ip", w.ip);
    cJSON_AddNumberToObject(wifi, "rssi", w.rssi);

    sd_status_t sd;
    sd_card_status(&sd);
    cJSON *jsd = cJSON_AddObjectToObject(root, "sd");
    cJSON_AddBoolToObject(jsd, "present", sd.present);
    if (sd.present) {
        cJSON_AddNumberToObject(jsd, "size_mb", sd.size_mb);
        cJSON_AddBoolToObject(jsd, "root_exists", sd.root_exists);
    }
    const machine_profile_t *mp = machine_profile(machine_active());
    cJSON *mach = cJSON_AddObjectToObject(root, "machine");
    cJSON_AddStringToObject(mach, "id", mp->id);
    cJSON_AddStringToObject(mach, "name", mp->name);
    cJSON_AddBoolToObject(mach, "supported", mp->supported);
    settings_t cfg;
    settings_get(&cfg);
    cJSON_AddStringToObject(mach, "configured", machine_profile(cfg.machine)->id);

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

/*
 * POST /api/v1/sd/disks {"path": "<folder>", "name": "<name>"}: a new,
 * empty, formatted 720 KiB .ST disk as <name>.st in that folder of the
 * SD card (Atari only).
 */
static esp_err_t post_sd_disk(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const cJSON *jp = cJSON_GetObjectItem(body, "path");
    const cJSON *jn = cJSON_GetObjectItem(body, "name");
    char folder[SD_REL_MAX + 1] = "", file[SD_NAME_MAX + 1] = "";
    bool ok = (!jp || (cJSON_IsString(jp) && strlen(jp->valuestring) <= SD_REL_MAX)) &&
              cJSON_IsString(jn);
    if (ok) {
        snprintf(folder, sizeof(folder), "%s", jp ? jp->valuestring : "");
        /* The name without spaces around it; ".st" added unless it has it. */
        const char *n = jn->valuestring, *e;
        while (*n == ' ') {
            n++;
        }
        e = n + strlen(n);
        while (e > n && e[-1] == ' ') {
            e--;
        }
        size_t len = (size_t)(e - n);
        bool has_ext = len >= 3 && strncasecmp(e - 3, ".st", 3) == 0;
        ok = len > 0 && len + (has_ext ? 0 : 3) <= SD_NAME_MAX;
        if (ok) {
            snprintf(file, sizeof(file), "%.*s%s", (int)len, n, has_ext ? "" : ".st");
        }
    }
    cJSON_Delete(body);
    if (!ok || !sd_name_valid(file)) {
        return send_error(req, HTTP_422, "INVALID_NAME", "\"name\": a file name (at most %d "
                          "characters, none of \\ / : * ? \" < > |, not starting with '.'); "
                          "\".st\" is added", SD_NAME_MAX - 3);
    }
    if (!sd_relpath_valid(folder)) {
        return sd_error(req, SD_INVALID, true);
    }
    settings_t cfg;
    settings_get(&cfg);
    if (cfg.machine != MACHINE_ATARI) {
        return send_error(req, HTTP_409, "MACHINE_NOT_SUPPORTED", "creating a new disk is not "
                          "supported yet for %s", machine_profile(cfg.machine)->name);
    }

    xSemaphoreTake(api_lock, portMAX_DELAY);
    esp_err_t ret;
    sd_writer_t *w = NULL;
    uint8_t *raw = heap_caps_malloc(ST_BLANK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    sd_result_t r;
    if (!raw) {
        ret = send_error(req, HTTP_507, "INSUFFICIENT_MEMORY", "no PSRAM for the new disk");
    } else if ((r = sd_card_writer_open(folder, file, false, &w)) != SD_OK) {
        ret = sd_error(req, r, r != SD_EXISTS);
    } else {
        st_format_blank(raw, esp_random() & 0xffffff);
        uint32_t crc = esp_rom_crc32_le(0, raw, ST_BLANK_SIZE);
        r = sd_card_writer_write(w, raw, ST_BLANK_SIZE);
        if (r == SD_OK) {
            r = sd_card_writer_commit(w, crc);
        } else {
            sd_card_writer_abort(w);
        }
        if (r != SD_OK) {
            ret = sd_error(req, r, false);
        } else {
            char rel[SD_REL_MAX + SD_NAME_MAX + 2];
            snprintf(rel, sizeof(rel), "%s%s%s", folder, folder[0] ? "/" : "", file);
            printf("API: new disk %s created on the SD card (720 KiB .ST)\n", rel);
            cJSON *root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "path", rel);
            cJSON_AddNumberToObject(root, "size", ST_BLANK_SIZE);
            ret = send_json(req, "201 Created", root);
        }
    }
    free(raw);
    xSemaphoreGive(api_lock);
    return ret;
}

static esp_err_t get_current(httpd_req_t *req)
{
    return send_json(req, "200 OK", current_json());
}

/* PUT /api/v1/current {"sd_path": "..."}: insert a file from the SD card. */
static esp_err_t put_current(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const cJSON *jsd = cJSON_GetObjectItem(body, "sd_path");
    char sd_path[SD_REL_MAX + 1] = "";
    if (jsd) {
        if (!cJSON_IsString(jsd) || strlen(jsd->valuestring) > SD_REL_MAX ||
            !jsd->valuestring[0] || !sd_relpath_valid(jsd->valuestring)) {
            cJSON_Delete(body);
            return sd_error(req, SD_INVALID, false);
        }
        snprintf(sd_path, sizeof(sd_path), "%s", jsd->valuestring);
    }
    cJSON_Delete(body);
    if (!sd_path[0]) {
        return send_error(req, HTTP_400, "INVALID_REQUEST", "\"sd_path\" (a file on the SD "
                          "card, below the root directory) is required");
    }

    xSemaphoreTake(api_lock, portMAX_DELAY);
    switch_error_t e;
    esp_err_t ret;
    if (disk_switch_sd(sd_path, &e) == ESP_OK) {
        ret = send_json(req, "200 OK", current_json());
    } else {
        ret = send_error(req, status_for(e.code), e.code, "%s", e.msg);
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
    if (strcmp(jdest->valuestring, "sd") == 0) {
        ret = post_upload_sd(req, body);
        goto out;
    }
    if (strcmp(jdest->valuestring, "psram") != 0) {
        ret = send_error(req, HTTP_400, "INVALID_REQUEST",
                         "\"destination\" must be \"psram\" (temporary) or \"sd\" (the SD card)");
        goto out;
    }
    if (jact && !cJSON_IsTrue(jact)) {
        ret = send_error(req, HTTP_400, "INVALID_REQUEST",
                         "a PSRAM upload is always activated; \"activate\": false is not supported");
        goto out;
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
    size_t fl = strlen(jfn->valuestring);
    if (fl >= 4 && strcasecmp(jfn->valuestring + fl - 4, ".hfe") == 0) {
        ret = send_error(req, status_for("UNSUPPORTED_FORMAT"), "UNSUPPORTED_FORMAT",
                         "HFE images cannot be loaded temporarily: store them on the SD card "
                         "(\"destination\": \"sd\") and insert them from there");
        goto out;
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
    up.activate = true;
    up.have_crc = have_crc;
    up.expected_crc = crc;
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

    if (up.to_sd) {
        ret = receive_sd(req);
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

/* ---- Settings and system ---------------------------------------------------- */

static bool restart_pending;

static void restart_cb(void *arg)
{
    esp_restart();
}

static cJSON *settings_json(void)
{
    settings_t s;
    wifi_net_status_t w;

    settings_get(&s);
    wifi_net_get_status(&w);
    cJSON *root = cJSON_CreateObject();
    int running_ds = EMU_SELECT_LINE == EMU_DS0 ? 0 : 1;
    cJSON_AddStringToObject(root, "hostname", s.hostname);
    cJSON_AddStringToObject(root, "drive_select", s.drive_select == 0 ? "DS0" : "DS1");
    cJSON_AddStringToObject(root, "drive_select_active", running_ds == 0 ? "DS0" : "DS1");
    cJSON_AddBoolToObject(root, "buzzer", s.buzzer);
    cJSON_AddNumberToObject(root, "leds", s.leds);
    cJSON_AddStringToObject(root, "root_dir", s.root_dir);
    sd_status_t sd;
    sd_card_status(&sd);
    cJSON *jsd = cJSON_AddObjectToObject(root, "sd");
    cJSON_AddBoolToObject(jsd, "present", sd.present);
    if (sd.present) {
        cJSON_AddNumberToObject(jsd, "size_mb", sd.size_mb);
        cJSON_AddBoolToObject(jsd, "root_exists", sd.root_exists);
    }
    cJSON_AddStringToObject(root, "machine", machine_profile(s.machine)->id);
    cJSON_AddStringToObject(root, "machine_active", machine_profile(machine_active())->id);
    cJSON_AddBoolToObject(root, "machine_supported", machine_profile(s.machine)->supported);
    cJSON *ms = cJSON_AddArrayToObject(root, "machines");
    for (int i = 0; i < MACHINE_COUNT; i++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "id", machine_profile(i)->id);
        cJSON_AddStringToObject(m, "name", machine_profile(i)->name);
        cJSON_AddBoolToObject(m, "supported", machine_profile(i)->supported);
        cJSON_AddItemToArray(ms, m);
    }
    cJSON_AddStringToObject(root, "last_sd_path", s.last_sd);
    cJSON_AddBoolToObject(root, "restart_required", strcmp(s.hostname, w.hostname) != 0 ||
                                                   s.drive_select != running_ds ||
                                                   s.machine != machine_active() ||
                                                   restart_pending);
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi, "ssid", s.wifi_ssid);
    cJSON_AddStringToObject(wifi, "security", settings_security_name(s.wifi_security));
    cJSON_AddBoolToObject(wifi, "connected", w.connected);
    cJSON_AddStringToObject(wifi, "ip", w.ip);
    cJSON_AddNumberToObject(wifi, "rssi", w.rssi);
    uint8_t mac[6];
    char mac_str[18] = "";
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    cJSON_AddStringToObject(wifi, "mac", mac_str);
    cJSON *fw = cJSON_AddObjectToObject(root, "firmware");
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(fw, "version", app->version);
    cJSON_AddStringToObject(fw, "idf", app->idf_ver);
    return root;
}

static esp_err_t get_settings(httpd_req_t *req)
{
    return send_json(req, "200 OK", settings_json());
}

static esp_err_t put_settings(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    if (!body) {
        return ESP_OK;
    }
    settings_t s;
    settings_get(&s);
    const cJSON *h = cJSON_GetObjectItem(body, "hostname");
    if (h) {
        if (!cJSON_IsString(h) || !settings_hostname_valid(h->valuestring)) {
            cJSON_Delete(body);
            return send_error(req, HTTP_422, "INVALID_HOSTNAME",
                              "host name: 1 to 32 letters, digits or '-', not starting or "
                              "ending with '-'");
        }
        snprintf(s.hostname, sizeof(s.hostname), "%s", h->valuestring);
    }
    const cJSON *ds = cJSON_GetObjectItem(body, "drive_select");
    if (ds) {
        bool ok = cJSON_IsString(ds) && (strcmp(ds->valuestring, "DS0") == 0 ||
                                         strcmp(ds->valuestring, "DS1") == 0);
        if (!ok) {
            cJSON_Delete(body);
            return send_error(req, HTTP_422, "INVALID_DRIVE_SELECT",
                              "drive_select: \"DS0\" (drive A:) or \"DS1\" (drive B:)");
        }
        s.drive_select = ds->valuestring[2] == '0' ? 0 : 1;
    }
    const cJSON *jm = cJSON_GetObjectItem(body, "machine");
    if (jm) {
        int m = cJSON_IsString(jm) ? machine_from_id(jm->valuestring) : -1;
        if (m < 0) {
            cJSON_Delete(body);
            return send_error(req, HTTP_422, "INVALID_MACHINE",
                              "machine: \"ATARI\", \"AMIGA\" or \"DOS\"");
        }
        s.machine = (uint8_t)m;
    }
    const cJSON *jr = cJSON_GetObjectItem(body, "root_dir");
    if (jr) {
        if (!cJSON_IsString(jr) || !settings_root_valid(jr->valuestring)) {
            cJSON_Delete(body);
            return send_error(req, HTTP_422, "INVALID_ROOT_DIR",
                              "root_dir: folder names separated by '/', at most %d characters; "
                              "letters, digits, space and - _ . ( ) & + , ! only; no \"..\", no '/' "
                              "at the start or end, no name starting with '.' or ' ' or ending "
                              "with '.' or ' '", SETTINGS_ROOT_MAX);
        }
        settings_t cur;
        settings_get(&cur);
        if (strcmp(cur.root_dir, jr->valuestring) != 0 && disk_write_unsaved()) {
            cJSON_Delete(body);
            return send_error(req, HTTP_409, "UNSAVED_CHANGES", "the computer wrote to the "
                              "inserted disk and the changes are not saved yet; try again in a "
                              "few seconds");
        }
        /* Create it on the card now; without a card it is created once one is in. */
        esp_err_t err = sd_card_make_root(jr->valuestring);
        if (err == ESP_FAIL) {
            cJSON_Delete(body);
            return send_error(req, HTTP_500, "SD_ERROR", "the folder could not be created on the "
                              "SD card; the setting was not changed");
        }
        snprintf(s.root_dir, sizeof(s.root_dir), "%s", jr->valuestring);
    }
    const cJSON *jl = cJSON_GetObjectItem(body, "leds");
    if (jl) {
        if (!cJSON_IsNumber(jl) || (jl->valuedouble != 1 && jl->valuedouble != 2)) {
            cJSON_Delete(body);
            return send_error(req, HTTP_400, "INVALID_REQUEST", "\"leds\" must be 1 or 2");
        }
        s.leds = (uint8_t)jl->valuedouble;
    }
    const cJSON *bz = cJSON_GetObjectItem(body, "buzzer");
    if (bz) {
        if (!cJSON_IsBool(bz)) {
            cJSON_Delete(body);
            return send_error(req, HTTP_400, "INVALID_REQUEST", "\"buzzer\" must be true or false");
        }
        s.buzzer = cJSON_IsTrue(bz);
    }
    cJSON_Delete(body);

    settings_t old;
    settings_get(&old);
    if (memcmp(&s, &old, sizeof(s)) != 0) {
        esp_err_t err = settings_save(&s);
        if (err != ESP_OK) {
            return send_error(req, HTTP_500, "FLASH_ERROR", "settings could not be saved (%s)",
                              esp_err_to_name(err));
        }
        if (strcmp(s.root_dir, old.root_dir) != 0) {
            disk_note_root_changed();   /* its path now names another file */
            disk_write_refresh();
        }
        printf("API: settings saved (host name %s, drive %s, buzzer %s, machine %s, %u LED(s), "
               "root \"%s\")\n", s.hostname, s.drive_select == 0 ? "A: / DS0" : "B: / DS1",
               s.buzzer ? "on" : "off", machine_profile(s.machine)->id, s.leds, s.root_dir);
    }
    step_sound_set_enabled(s.buzzer);   /* takes effect at once */
    return send_json(req, "200 OK", settings_json());
}

static esp_err_t post_restart(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    if (!body) {
        return ESP_OK;
    }
    cJSON_Delete(body);

    /* Sectors written by the computer are saved before the restart. */
    if (disk_write_flush() != ESP_OK) {
        return send_error(req, HTTP_409, "UNSAVED_CHANGES", "written sectors could not be saved; "
                          "not restarting (see the disk status)");
    }
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t t = { .callback = restart_cb, .name = "restart" };
    if (!timer && esp_timer_create(&t, &timer) != ESP_OK) {
        return send_error(req, HTTP_500, "RESTART_FAILED", "restart could not be scheduled");
    }
    restart_pending = true;
    esp_timer_stop(timer);
    esp_timer_start_once(timer, 1000 * 1000);   /* after this answer went out */
    printf("API: restart requested\n");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "restarting", true);
    return send_json(req, "202 Accepted", root);
}

/* Placeholder: online updates are not implemented yet. */
/* ---- Debug: flux monitor (flux_monitor.h) -------------------------------------- */

static cJSON *flux_monitor_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", flux_monitor_enabled());
    cJSON_AddStringToObject(root, "mode", flux_stream_mode() == FLUX_MODE_HFE ? "hfe" : "st");
    flux_hfe_stats_t hs;
    flux_hfe_stats(&hs);
    cJSON *h = cJSON_AddObjectToObject(root, "hfe");
    cJSON_AddNumberToObject(h, "late_pulses", hs.late_pulses);
    cJSON_AddNumberToObject(h, "late_max_us", hs.late_max_us);
    cJSON *list = cJSON_AddArrayToObject(root, "tracks");
    for (int c = 0; c < DISK_MAX_CYLS; c++) {
        for (int sd = 0; sd < 2; sd++) {
            mon_track_t t;
            if (!flux_monitor_track(c, sd, &t)) {
                continue;
            }
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "cyl", c);
            cJSON_AddNumberToObject(o, "side", sd);
            cJSON_AddNumberToObject(o, "revs", t.revs);
            cJSON_AddNumberToObject(o, "expected_us", t.expected_ns / 1000.0);
            cJSON_AddNumberToObject(o, "period_min_us", t.period_min_ns / 1000.0);
            cJSON_AddNumberToObject(o, "period_max_us", t.period_max_ns / 1000.0);
            cJSON_AddNumberToObject(o, "pulse_min_us", t.pulse_min_ns / 1000.0);
            cJSON_AddNumberToObject(o, "pulse_max_us", t.pulse_max_ns / 1000.0);
            cJSON_AddNumberToObject(o, "checks", t.checks);
            cJSON_AddNumberToObject(o, "clean", t.clean);
            cJSON_AddNumberToObject(o, "unaligned", t.unaligned);
            cJSON_AddNumberToObject(o, "intervals", t.intervals);
            cJSON_AddNumberToObject(o, "bad", t.bad);
            cJSON_AddNumberToObject(o, "align_min_us", t.align_min_ns / 1000.0);
            cJSON_AddNumberToObject(o, "align_max_us", t.align_max_ns / 1000.0);
            cJSON_AddItemToArray(list, o);
        }
    }
    mon_failure_t f[MON_FAILURES];
    int n = flux_monitor_failures(f, MON_FAILURES);
    cJSON *fl = cJSON_AddArrayToObject(root, "failures");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "cyl", f[i].cyl);
        cJSON_AddNumberToObject(o, "side", f[i].side);
        cJSON_AddNumberToObject(o, "first_at", f[i].at);
        cJSON_AddNumberToObject(o, "cell", f[i].cell);
        cJSON_AddNumberToObject(o, "expected_ns", f[i].expected_ns);
        cJSON_AddNumberToObject(o, "got_ns", f[i].got_ns);
        cJSON_AddNumberToObject(o, "bad", f[i].bad);
        cJSON_AddNumberToObject(o, "intervals", f[i].intervals);
        cJSON_AddItemToArray(fl, o);
    }
    return root;
}

/* GET /api/v1/debug/flux: what the monitor measured. */
static esp_err_t get_debug_flux(httpd_req_t *req)
{
    return send_json(req, "200 OK", flux_monitor_json());
}

/* PUT /api/v1/debug/flux {"enabled": bool, "reset": bool} */
static esp_err_t put_debug_flux(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const cJSON *je = cJSON_GetObjectItem(body, "enabled");
    const cJSON *jr = cJSON_GetObjectItem(body, "reset");
    bool valid = (!je || cJSON_IsBool(je)) && (!jr || cJSON_IsBool(jr)) && (je || jr);
    bool on = cJSON_IsTrue(je), reset = cJSON_IsTrue(jr), set = je != NULL;
    cJSON_Delete(body);
    if (!valid) {
        return send_error(req, HTTP_400, "INVALID_REQUEST", "\"enabled\" and/or \"reset\" (true/false)");
    }
    if (reset) {
        flux_monitor_reset();
    }
    if (set && flux_monitor_enable(on) != ESP_OK) {
        return send_error(req, HTTP_500, "MONITOR_FAILED", "the flux monitor could not be started "
                          "(see the device log)");
    }
    return send_json(req, "200 OK", flux_monitor_json());
}

static esp_err_t get_update(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "current", esp_app_get_description()->version);
    cJSON_AddStringToObject(root, "status", "not_available");
    cJSON_AddBoolToObject(root, "update_available", false);
    cJSON_AddStringToObject(root, "message", "Online update checks are not available yet.");
    return send_json(req, "200 OK", root);
}

/* ---- Shared with the setup page server --------------------------------------- */

esp_err_t api_send_json(httpd_req_t *req, const char *status, cJSON *root)
{
    return send_json(req, status, root);
}

esp_err_t api_send_error(httpd_req_t *req, const char *status, const char *code,
                         const char *message)
{
    return send_error(req, status, code, "%s", message);
}

cJSON *api_read_json(httpd_req_t *req)
{
    return read_json(req);
}

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
    cfg.max_uri_handlers = 28;
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
        { .uri = "/api/v1/sd/disks",  .method = HTTP_POST,   .handler = post_sd_disk },
        { .uri = "/api/v1/current",   .method = HTTP_GET,    .handler = get_current },
        { .uri = "/api/v1/current",   .method = HTTP_PUT,    .handler = put_current },
        { .uri = "/api/v1/uploads",   .method = HTTP_POST,   .handler = post_upload },
        { .uri = "/api/v1/uploads/*", .method = HTTP_GET,    .handler = get_upload },
        { .uri = "/api/v1/uploads/*", .method = HTTP_PUT,    .handler = put_upload_data },
        { .uri = "/api/v1/sd/files",  .method = HTTP_GET,    .handler = get_sd_files },
        { .uri = "/api/v1/sd/files",  .method = HTTP_DELETE, .handler = delete_sd_file },
        { .uri = "/api/v1/sd/files",  .method = HTTP_PUT,    .handler = put_sd_file },
        { .uri = "/api/v1/sd/folders", .method = HTTP_POST,  .handler = post_sd_folder },
        { .uri = "/api/v1/sd/folders", .method = HTTP_DELETE, .handler = delete_sd_folder },
        { .uri = "/api/v1/settings",  .method = HTTP_GET,    .handler = get_settings },
        { .uri = "/api/v1/settings",  .method = HTTP_PUT,    .handler = put_settings },
        { .uri = "/api/v1/system/restart", .method = HTTP_POST, .handler = post_restart },
        { .uri = "/api/v1/system/update",  .method = HTTP_GET,  .handler = get_update },
        { .uri = "/api/v1/debug/flux", .method = HTTP_GET, .handler = get_debug_flux },
        { .uri = "/api/v1/debug/flux", .method = HTTP_PUT, .handler = put_debug_flux },
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
