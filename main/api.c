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
#include "esp_system.h"
#include "esp_http_server.h"
#include "esp_timer.h"

#include "api.h"
#include "disk_image.h"
#include "disk_switch.h"
#include "disk_title.h"
#include "disk_write.h"
#include "drive_config.h"
#include "drive_emu.h"
#include "ext_flash.h"
#include "machine.h"
#include "settings.h"
#include "step_sound.h"
#include "hfe.h"
#include "hfe_import.h"
#include "image_store.h"
#include "img_codec.h"
#include "st_format.h"
#include "st_image.h"
#include "wifi_net.h"

#define API_BODY_MAX        1024        /* JSON request bodies */
#define RECV_CHUNK          4096
#define UPLOAD_TIMEOUT_MS   60000       /* created but no data: expires */

/* HFE uploads are checked and stored as a stream; they are kept (made an
 * image) only once the HFE player exists, so no image is listed that
 * cannot be inserted. 0: everything runs, then the result is discarded. */
#ifndef HFE_PLAYBACK
#define HFE_PLAYBACK        0
#endif

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
    uint16_t replace_id;    /* flash: image to replace, 0 = new image */
    bool hfe;               /* .hfe: stored as a stream (hfe_import) */
    bool activate;
    bool have_crc;
    uint32_t expected_crc;
    uint32_t received;
    uint32_t crc32;
    uint16_t result_id;     /* image stored, 0 = none */
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

/* Image id 1..65535 from a path segment; 0 if invalid. */
static uint16_t parse_image_id(const char *s)
{
    char *end;
    long n = strtol(s, &end, 10);
    return (end != s && *end == 0 && n >= 1 && n <= 65535) ? (uint16_t)n : 0;
}

/* Image id from a JSON number; 0 if missing or invalid. */
static uint16_t json_image_id(const cJSON *j)
{
    return cJSON_IsNumber(j) && j->valuedouble == (int)j->valuedouble && j->valueint >= 1 &&
           j->valueint <= 65535 ? (uint16_t)j->valueint : 0;
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

static cJSON *storage_json(void)
{
    const rf_geometry_t *g = image_store_geometry();
    store_usage_t u;
    char id[8];

    image_store_usage(&u);
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "state", image_store_state_name(image_store_state()));
    cJSON_AddBoolToObject(s, "flash_detected", ext_flash_ready());
    snprintf(id, sizeof(id), "%06lx", (unsigned long)ext_flash_jedec_id());
    cJSON_AddStringToObject(s, "jedec_id", id);
    cJSON_AddNumberToObject(s, "capacity", ext_flash_size());
    cJSON_AddNumberToObject(s, "block_size", g->block_size);
    cJSON_AddNumberToObject(s, "blocks_total", u.blocks_total);
    cJSON_AddNumberToObject(s, "blocks_used", u.blocks_used);
    cJSON_AddNumberToObject(s, "blocks_free", u.blocks_free);
    cJSON_AddNumberToObject(s, "used_percent", u.used_percent);
    cJSON_AddNumberToObject(s, "images", u.images);
    cJSON_AddNumberToObject(s, "max_image_size", RF_MAX_IMAGE_SIZE);
    cJSON_AddNumberToObject(s, "free_bytes", (double)u.blocks_free * g->block_size);
    return s;
}

static cJSON *image_json(const image_record_t *r, const disk_info_t *cur)
{
    char crc[9], title[IMG_TITLE_SIZE];
    cJSON *o = cJSON_CreateObject();

    image_store_title(r, title);
    cJSON_AddNumberToObject(o, "id", r->id);
    cJSON_AddNumberToObject(o, "sequence", r->sequence);
    cJSON_AddStringToObject(o, "status", r->status == IMG_VALID ? "valid" : "incomplete");
    cJSON_AddStringToObject(o, "name", title);
    if (r->status == IMG_VALID) {
        hex32(crc, r->crc32);
        cJSON_AddStringToObject(o, "format", image_format_name(image_format(r)));
        cJSON_AddNumberToObject(o, "size", r->original_size);
        cJSON_AddNumberToObject(o, "stored_size", r->stored_size);
        cJSON_AddStringToObject(o, "storage_format",
                                r->storage_format == IMG_STORE_DEFLATE ? "deflate" : "raw");
        cJSON_AddStringToObject(o, "crc32", crc);
        cJSON_AddNumberToObject(o, "blocks_used", r->block_count);
        cJSON_AddStringToObject(o, "access", image_read_write(r) ? "READ_WRITE" : "READ_ONLY");
        cJSON_AddBoolToObject(o, "write_supported", image_format_writable(image_format(r)));
    }
    cJSON_AddBoolToObject(o, "active", cur->source == DISK_SRC_FLASH && cur->image_id == r->id &&
                                       !cur->image_changed);
    return o;
}

/* Storage must be usable for changes; else an error has been sent. */
static bool storage_ready(httpd_req_t *req)
{
    if (image_store_state() == STORE_VALID) {
        return true;
    }
    send_error(req, HTTP_503, "STORAGE_NOT_READY", "image library not usable (%s)%s",
               image_store_state_name(image_store_state()),
               image_store_state() == STORE_OLD_FORMAT ? " - initialise it with "
               "POST /api/v1/storage/format" : "");
    return false;
}

/* The inserted disk is image id and has written sectors not saved yet. */
static bool active_unsaved(uint16_t id)
{
    disk_info_t d;
    disk_get_current(&d);
    return disk_write_unsaved() && d.source == DISK_SRC_FLASH && d.image_id == id;
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
    cJSON_AddStringToObject(c, "source", d.source == DISK_SRC_FLASH ? "flash"
                                        : d.source == DISK_SRC_PSRAM ? "psram" : "none");
    if (d.source == DISK_SRC_FLASH && d.image_id) {
        cJSON_AddNumberToObject(c, "image_id", d.image_id);
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
    cJSON_AddStringToObject(u, "destination", up.to_flash ? "flash" : "psram");
    if (up.to_flash && up.replace_id) {
        cJSON_AddNumberToObject(u, "replace", up.replace_id);
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
        if (up.result_id) {
            cJSON_AddNumberToObject(u, "image_id", up.result_id);
        }
        cJSON_AddBoolToObject(u, "active", up.activated);
    }
    if (up.state == UP_FAILED) {
        cJSON *e = cJSON_AddObjectToObject(u, "error");
        cJSON_AddStringToObject(e, "code", up.err_code);
        cJSON_AddStringToObject(e, "message", up.err_msg);
        if (up.result_id) {
            cJSON_AddNumberToObject(u, "image_id", up.result_id);
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
        { "NO_SPACE", HTTP_507 },          { "DRIVE_BUSY", HTTP_409 },
        { "UPLOAD_INCOMPLETE", HTTP_400 }, { "INSUFFICIENT_MEMORY", HTTP_507 },
        { "FLASH_ERROR", HTTP_500 },       { "PREPARE_FAILED", HTTP_500 },
        { "IMAGE_NOT_FOUND", HTTP_404 },   { "STORAGE_NOT_READY", HTTP_503 },
        { "MACHINE_NOT_SUPPORTED", HTTP_409 },   { "UNSAVED_CHANGES", HTTP_409 },
        { "UNSUPPORTED_FORMAT", HTTP_422 },   { "INVALID_NAME", HTTP_400 },
        { "UPLOAD_BUSY", HTTP_409 },
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
/* HFE files (.hfe) do not come here: receive_hfe stores them as a stream. */
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
    up.crc32 = image_store_crc32(0, raw, up.size);
    if (up.have_crc && up.crc32 != up.expected_crc) {
        fail_upload("CHECKSUM_MISMATCH", "received data has CRC-32 %08lx, expected %08lx",
                    (unsigned long)up.crc32, (unsigned long)up.expected_crc);
        return;
    }

    disk_info_t info = { .source = up.to_flash ? DISK_SRC_FLASH : DISK_SRC_PSRAM,
                         .size = up.size, .crc32 = up.crc32,
                         .cylinders = st.cylinders, .heads = st.heads, .sectors = st.sectors };
    memcpy(info.name, up.name, sizeof(info.name));

    if (up.to_flash) {
        uint16_t id = 0;
        settings_t cfg;
        settings_get(&cfg);
        if (cfg.compress) {
            disk_verify_cancel();
        }
        int64_t t0 = esp_timer_get_time();
        esp_err_t err = image_store_save_ex(up.replace_id, up.name, IMG_FMT_ST, raw, up.size,
                                            cfg.compress, &id);
        int64_t store_ms = (esp_timer_get_time() - t0) / 1000;
        if (up.replace_id) {
            disk_note_image_changed(up.replace_id);     /* old image gone, also on failure */
        }
        if (err != ESP_OK) {
            if (err == ESP_ERR_NO_MEM) {
                fail_upload("NO_SPACE", "not enough free storage: %lu blocks needed",
                            (unsigned long)image_store_blocks_needed(up.size));
            } else if (err == ESP_ERR_NOT_FOUND) {
                fail_upload("IMAGE_NOT_FOUND", "image %u to replace does not exist", up.replace_id);
            } else {
                fail_upload("FLASH_ERROR", "storing the image failed (%s)%s", esp_err_to_name(err),
                            up.replace_id ? "; the replaced image is lost" : "; nothing stored");
            }
            return;
        }
        up.result_id = id;
        info.image_id = id;
        image_record_t rec;
        image_store_get(id, &rec);
        printf("API: \"%s\" %s image %u (%lu bytes, stored %s %lu bytes = %lu%%, %u block(s), "
               "CRC32 %08lx, %lld ms)\n", up.name, up.replace_id ? "replaced" : "stored as", id,
               (unsigned long)up.size, rec.storage_format == IMG_STORE_DEFLATE ? "compressed" : "raw",
               (unsigned long)rec.stored_size, (unsigned long)(rec.stored_size * 100ull / up.size),
               rec.block_count, (unsigned long)up.crc32, store_ms);
    }

    if (up.activate) {
        switch_error_t e;
        esp_err_t err = up.to_flash ? disk_switch_raw(raw, &info, &e)
                                    : disk_switch_psram_upload(*rawp, &info, &e);
        if (err != ESP_OK) {
            fail_upload(e.code, "%s%s", e.msg, up.to_flash ? " (the image is stored)" : "");
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

/* ---- HFE uploads: a stream into the library ---------------------------------- */

/* API error for a failed HFE import. */
static void fail_hfe(hfe_import_error_t e, const hfe_import_t *imp)
{
    const hfe_info_t *hi = imp ? hfe_import_info(imp) : NULL;
    hfe_result_t hr = imp ? hfe_import_result(imp) : HFE_OK;

    switch (e) {
    case HFE_IMPORT_FORMAT:
        fail_upload(hr == HFE_V2 || hr == HFE_UNSUPPORTED || hr == HFE_BAD_LAYOUT ? "UNSUPPORTED_FORMAT"
                    : hr == HFE_TOO_LARGE ? "IMAGE_TOO_LARGE" : "INVALID_IMAGE",
                    "%s", hi && hi->detail[0] ? hi->detail : "not a valid HFE file");
        break;
    case HFE_IMPORT_STORED_TOO_LARGE:
        fail_upload("IMAGE_TOO_LARGE", "compressed, it needs more than the %u KiB one image may "
                    "occupy", (unsigned)(RF_MAX_IMAGE_SIZE / 1024));
        break;
    case HFE_IMPORT_NO_SPACE:
        fail_upload("NO_SPACE", "not enough free storage for the compressed image");
        break;
    case HFE_IMPORT_NO_MEMORY:
        fail_upload("INSUFFICIENT_MEMORY", "no memory to store the image");
        break;
    case HFE_IMPORT_BUSY:
        fail_upload("UPLOAD_BUSY", "another image is being written");
        break;
    default:
        fail_upload("FLASH_ERROR", "storing the image failed (%s); nothing stored",
                    hfe_import_error_name(e));
        break;
    }
}

/*
 * PUT data of an .hfe upload: received in pieces, compressed into the
 * library while it arrives and verified afterwards (hfe_import). A bad
 * file is recognised early; the rest of its data is then only drained.
 */
static esp_err_t receive_hfe(httpd_req_t *req)
{
    hfe_import_t *imp = NULL;
    hfe_import_error_t e;
    uint8_t *chunk = heap_caps_malloc(RECV_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int64_t t0 = esp_timer_get_time(), t_recv = 0;

    disk_verify_cancel();               /* its copy of the active disk: PSRAM */
    e = chunk ? hfe_import_begin(up.size, DISK_TRACKS_BYTES, &imp) : HFE_IMPORT_NO_MEMORY;
    if (e != HFE_IMPORT_OK) {
        fail_hfe(e, NULL);
    } else {
        up.state = UP_RECEIVING;
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
        if (up.state == UP_RECEIVING && (e = hfe_import_write(imp, chunk, n)) != HFE_IMPORT_OK) {
            fail_hfe(e, imp);           /* refused early: drain the rest */
            hfe_import_abort(imp);
            imp = NULL;
        }
    }
    free(chunk);
    t_recv = esp_timer_get_time();

    uint32_t crc = 0;
    if (up.state == UP_RECEIVING && up.received != up.size) {
        fail_upload("UPLOAD_INCOMPLETE", "received %lu of %lu bytes",
                    (unsigned long)up.received, (unsigned long)up.size);
    }
    if (up.state == UP_RECEIVING) {
        up.state = UP_PROCESSING;
        e = hfe_import_end(imp, &crc);
        up.crc32 = crc;
        if (e != HFE_IMPORT_OK) {
            fail_hfe(e, imp);
        } else if (up.have_crc && crc != up.expected_crc) {
            fail_upload("CHECKSUM_MISMATCH", "received data has CRC-32 %08lx, expected %08lx",
                        (unsigned long)crc, (unsigned long)up.expected_crc);
        } else if (!HFE_PLAYBACK) {
            const hfe_info_t *hi = hfe_import_info(imp);
            printf("API: HFE \"%s\" checked and stored in %lld + %lld ms (%lu -> %lu bytes), "
                   "then discarded: playback not built in\n", up.name, (t_recv - t0) / 1000,
                   (esp_timer_get_time() - t_recv) / 1000, (unsigned long)up.size,
                   (unsigned long)hfe_import_stored(imp));
            fail_upload("UNSUPPORTED_FORMAT", "%s: HFE playback is still in development", hi->detail);
        } else {
            hfe_info_t hi = *hfe_import_info(imp);
            uint32_t stored = hfe_import_stored(imp);
            uint16_t id = 0;
            e = hfe_import_commit(imp, up.replace_id, up.name, &id);
            imp = NULL;
            if (e != HFE_IMPORT_OK) {
                fail_hfe(e, NULL);
            } else {
                if (up.replace_id) {
                    disk_note_image_changed(up.replace_id);
                }
                up.result_id = id;
                printf("API: \"%s\" %s image %u (%s, %lu bytes, stored compressed %lu bytes = %lu%%, "
                       "CRC32 %08lx, receive %lld ms, verify %lld ms)\n", up.name,
                       up.replace_id ? "replaced" : "stored as", id, hi.detail,
                       (unsigned long)up.size, (unsigned long)stored,
                       (unsigned long)(stored * 100ull / up.size), (unsigned long)crc,
                       (t_recv - t0) / 1000, (esp_timer_get_time() - t_recv) / 1000);
                switch_error_t se;
                if (up.activate && disk_switch_image(id, &se) != ESP_OK) {
                    fail_upload(se.code, "%s (the image is stored)", se.msg);
                } else {
                    up.activated = up.activate;
                    up.state = UP_DONE;
                    up.touched_us = esp_timer_get_time();
                }
            }
        }
    }
    hfe_import_abort(imp);
    const char *status = up.state == UP_DONE ? "200 OK" : status_for(up.err_code);
    if (up.state == UP_FAILED) {
        printf("API: upload %lu failed: %s - %s\n", (unsigned long)up.id, up.err_code, up.err_msg);
    }
    return send_json(req, status, upload_json());
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

    cJSON_AddItemToObject(root, "storage", storage_json());
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

static image_record_t *list_buf(void)
{
    static image_record_t *buf;         /* 24 KiB, PSRAM; httpd handles one request at a time */
    if (!buf) {
        buf = heap_caps_malloc(IMG_MAX_RECORDS * sizeof(*buf), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return buf;
}

/* GET /api/v1/images: all images in sequence order, plus the storage use. */
static esp_err_t get_images(httpd_req_t *req)
{
    disk_info_t d;
    image_record_t *recs = list_buf();

    if (!recs) {
        return send_error(req, HTTP_507, "INSUFFICIENT_MEMORY", "no memory for the list");
    }
    disk_get_current(&d);
    int n = image_store_list(recs, IMG_MAX_RECORDS);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "images");
    for (int i = 0; i < n; i++) {
        cJSON_AddItemToArray(arr, image_json(&recs[i], &d));
    }
    cJSON_AddItemToObject(root, "storage", storage_json());
    return send_json(req, "200 OK", root);
}

static esp_err_t get_storage(httpd_req_t *req)
{
    return send_json(req, "200 OK", storage_json());
}

/* ---- Downloads: the image streamed from the flash ------------------------------ */

typedef struct {
    const image_record_t *r;
    httpd_req_t *req;           /* NULL: only compute the CRC */
    uint32_t crc;
    uint32_t len;
} download_t;

static esp_err_t download_out(void *ctx, const uint8_t *data, size_t len)
{
    download_t *dl = ctx;
    if (len > dl->r->original_size - dl->len) {
        return ESP_ERR_INVALID_SIZE;
    }
    dl->crc = image_store_crc32(dl->crc, data, len);
    dl->len += (uint32_t)len;
    return dl->req ? httpd_resp_send_chunk(dl->req, (const char *)data, len) : ESP_OK;
}

static esp_err_t download_read(void *ctx, uint32_t off, uint8_t *buf, size_t len)
{
    return image_store_read(ctx, off, buf, (uint32_t)len);
}

/* The image once through out (decompressed if needed); OK only if the
 * length and CRC-32 are those of the record. */
static esp_err_t download_pass(const image_record_t *r, httpd_req_t *req)
{
    download_t dl = { .r = r, .req = req };
    esp_err_t err;

    if (r->storage_format == IMG_STORE_DEFLATE) {
        err = img_inflate_stream(r->stored_size, download_read, (void *)r, download_out, &dl, NULL);
    } else {
        uint8_t *buf = heap_caps_malloc(RECV_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        err = buf ? ESP_OK : ESP_ERR_NO_MEM;
        for (uint32_t off = 0; err == ESP_OK && off < r->original_size; off += RECV_CHUNK) {
            uint32_t n = r->original_size - off < RECV_CHUNK ? r->original_size - off : RECV_CHUNK;
            if ((err = image_store_read(r, off, buf, n)) == ESP_OK) {
                err = download_out(&dl, buf, n);
            }
        }
        free(buf);
    }
    if (err == ESP_OK && (dl.len != r->original_size || dl.crc != r->crc32)) {
        err = ESP_ERR_INVALID_CRC;
    }
    return err;
}

/*
 * GET /api/v1/images/{id}/data: the image itself (uncompressed, as it was
 * uploaded or as the computer wrote it), as a file download. Streamed:
 * first checked once without sending (so a damaged image gets an error
 * status), then sent. Never whole in memory.
 */
static esp_err_t get_image_data(httpd_req_t *req, uint16_t id)
{
    image_record_t r;
    char title[IMG_TITLE_SIZE], fname[IMG_TITLE_SIZE + 8], hdr[IMG_TITLE_SIZE + 40];

    if (!id || !image_store_get(id, &r) || !image_store_is_valid(&r)) {
        return send_error(req, HTTP_404, "IMAGE_NOT_FOUND", "no valid image with this id");
    }
    if (active_unsaved(id)) {
        return send_error(req, HTTP_409, "UNSAVED_CHANGES", "the computer wrote to this disk and "
                          "the changes are not saved yet; download it once they are");
    }
    esp_err_t err = download_pass(&r, NULL);
    if (err == ESP_ERR_NO_MEM) {
        return send_error(req, HTTP_507, "INSUFFICIENT_MEMORY", "no memory for the download");
    }
    if (err != ESP_OK) {
        return send_error(req, HTTP_500, "FLASH_ERROR", "reading the image failed (%s)",
                          esp_err_to_name(err));
    }
    /* File name: the title, characters a file system dislikes replaced. */
    image_store_title(&r, title);
    size_t n = 0;
    for (const char *p = title; *p && n < sizeof(title) - 1; p++) {
        fname[n++] = strchr("\\/:*?\"<>|", *p) ? '_' : *p;
    }
    if (!n) {
        fname[n++] = 'd';
    }
    /* Extension from the format ("st", "hfe"; HFEv3 is also .hfe). */
    uint8_t fmt = image_format(&r);
    snprintf(fname + n, sizeof(fname) - n, ".%s",
             fmt == IMG_FMT_HFE3 ? "hfe" : image_format_name(fmt));
    snprintf(hdr, sizeof(hdr), "attachment; filename=\"%s\"", fname);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", hdr);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    /* Checked again while sending: if the data changed meanwhile, the
     * download is cut off instead of completed with wrong data. */
    if ((err = download_pass(&r, req)) != ESP_OK) {
        printf("API: download of image %u stopped (%s)\n", id, esp_err_to_name(err));
        return ESP_FAIL;                /* closes the connection: incomplete */
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* GET /api/v1/images/{id} and GET /api/v1/images/{id}/data */
static esp_err_t get_image(httpd_req_t *req)
{
    disk_info_t d;
    image_record_t r;
    char seg[16];
    const char *p = req->uri + strlen("/api/v1/images/");
    const char *slash = strchr(p, '/');

    if (slash && strcmp(slash, "/data") == 0 && (size_t)(slash - p) < sizeof(seg)) {
        memcpy(seg, p, slash - p);
        seg[slash - p] = 0;
        return get_image_data(req, parse_image_id(seg));
    }
    uint16_t id = parse_image_id(p);
    if (!id || !image_store_get(id, &r)) {
        return send_error(req, HTTP_404, "IMAGE_NOT_FOUND", "no image with this id");
    }
    disk_get_current(&d);
    return send_json(req, "200 OK", image_json(&r, &d));
}

/*
 * PUT /api/v1/images/{id}: change {"sequence": n} (move to position n,
 * 1 = first) and/or {"access": "READ_ONLY" | "READ_WRITE"} (metadata only:
 * the drive stays write-protected for the computer).
 */
static esp_err_t put_image(httpd_req_t *req)
{
    if (!authorized(req) || !storage_ready(req)) {
        return ESP_OK;
    }
    uint16_t id = parse_image_id(req->uri + strlen("/api/v1/images/"));
    cJSON *body = read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const cJSON *js = cJSON_GetObjectItem(body, "sequence");
    const cJSON *ja = cJSON_GetObjectItem(body, "access");
    int pos = cJSON_IsNumber(js) && js->valuedouble == (int)js->valuedouble ? js->valueint : 0;
    int rw = !cJSON_IsString(ja) ? -1 : strcmp(ja->valuestring, "READ_WRITE") == 0 ? 1
             : strcmp(ja->valuestring, "READ_ONLY") == 0 ? 0 : -2;
    cJSON_Delete(body);
    if ((!js && !ja) || (js && pos < 1) || (ja && rw < 0)) {
        return send_error(req, HTTP_400, "INVALID_REQUEST",
                          "send \"sequence\" (a number >= 1) and/or \"access\" "
                          "(\"READ_ONLY\" or \"READ_WRITE\")");
    }

    xSemaphoreTake(api_lock, portMAX_DELAY);
    esp_err_t ret;
    image_record_t r;
    esp_err_t err = id && image_store_get(id, &r) ? ESP_OK : ESP_ERR_NOT_FOUND;
    if (err == ESP_OK && ja) {
        err = image_store_set_read_write(id, rw == 1);
        disk_write_refresh();           /* write protection of the inserted disk */
    }
    if (err == ESP_OK && js) {
        err = image_store_set_position(id, pos);
    }
    if (err == ESP_ERR_NOT_FOUND) {
        ret = send_error(req, HTTP_404, "IMAGE_NOT_FOUND", "no valid image with this id");
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        ret = send_error(req, HTTP_422, "FORMAT_NOT_WRITABLE",
                         "this image format does not support writing");

    } else if (err != ESP_OK || !image_store_get(id, &r)) {
        ret = send_error(req, HTTP_500, "FLASH_ERROR", "catalog update failed");
    } else {
        if (ja) {
            printf("API: image %u set to %s\n", id, rw ? "READ_WRITE" : "READ_ONLY");
        }
        disk_info_t d;
        disk_get_current(&d);
        ret = send_json(req, "200 OK", image_json(&r, &d));
    }
    xSemaphoreGive(api_lock);
    return ret;
}

/*
 * POST /api/v1/images {"name": "..."}: create a new, empty, formatted disk
 * in the library for the configured computer (READ_WRITE). Atari: .ST,
 * 720 KiB (80 x 2 x 9). Other computers: not supported yet.
 */
static esp_err_t post_image(httpd_req_t *req)
{
    if (!authorized(req) || !storage_ready(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const cJSON *jn = cJSON_GetObjectItem(body, "name");
    char name[IMG_TITLE_SIZE] = "";
    bool name_ok = cJSON_IsString(jn);
    if (name_ok) {
        const char *s = jn->valuestring, *e;
        while (*s == ' ') {
            s++;
        }
        e = s + strlen(s);
        while (e > s && e[-1] == ' ') {
            e--;
        }
        name_ok = e > s && e - s <= IMG_NAME_LEN;
        for (const char *p = s; name_ok && p < e; p++) {
            name_ok = *p >= 0x20 && *p <= 0x7e;
        }
        if (name_ok) {
            memcpy(name, s, e - s);
            name[e - s] = 0;
        }
    }
    cJSON_Delete(body);
    if (!name_ok) {
        return send_error(req, HTTP_400, "INVALID_NAME",
                          "\"name\": 1 to %d printable characters", IMG_NAME_LEN);
    }

    settings_t cfg;
    settings_get(&cfg);
    const machine_profile_t *mp = machine_profile(cfg.machine);
    if (cfg.machine != MACHINE_ATARI) {
        return send_error(req, HTTP_409, "MACHINE_NOT_SUPPORTED",
                          "creating a new disk is not supported yet for %s", mp->name);
    }

    xSemaphoreTake(api_lock, portMAX_DELAY);
    esp_err_t ret;
    uint8_t *raw = NULL;
    if (upload_in_progress()) {
        ret = send_error(req, HTTP_409, "UPLOAD_BUSY", "an upload is in progress");
    } else if (!image_store_fits(ST_BLANK_SIZE, 0)) {
        ret = send_error(req, HTTP_507, "NO_SPACE", "not enough free storage: %lu blocks needed",
                         (unsigned long)image_store_blocks_needed(ST_BLANK_SIZE));
    } else if (!(raw = heap_caps_malloc(ST_BLANK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))) {
        ret = send_error(req, HTTP_507, "INSUFFICIENT_MEMORY", "no PSRAM for the new disk");
    } else {
        st_format_blank(raw, esp_random() & 0xffffff);
        uint16_t id = 0;
        if (cfg.compress) {
            disk_verify_cancel();
        }
        esp_err_t err = image_store_save_ex(0, name, IMG_FMT_ST | IMG_FLAG_READ_WRITE, raw,
                                            ST_BLANK_SIZE, cfg.compress, &id);
        image_record_t r;
        if (err == ESP_ERR_NO_MEM) {
            ret = send_error(req, HTTP_507, "NO_SPACE", "not enough free storage");
        } else if (err != ESP_OK || !image_store_get(id, &r)) {
            ret = send_error(req, HTTP_500, "FLASH_ERROR", "storing the new disk failed (%s)",
                             esp_err_to_name(err));
        } else {
            printf("API: new disk \"%s\" created as image %u (720 KiB .ST, read-write)\n",
                   name, id);
            disk_info_t d;
            disk_get_current(&d);
            ret = send_json(req, "201 Created", image_json(&r, &d));
        }
    }
    free(raw);
    xSemaphoreGive(api_lock);
    return ret;
}

/* DELETE /api/v1/images/{id} */
static esp_err_t delete_image(httpd_req_t *req)
{
    if (!authorized(req) || !storage_ready(req)) {
        return ESP_OK;
    }
    uint16_t id = parse_image_id(req->uri + strlen("/api/v1/images/"));

    xSemaphoreTake(api_lock, portMAX_DELAY);
    esp_err_t ret;
    image_record_t r;
    if (upload_in_progress() && up.to_flash && up.replace_id == id) {
        ret = send_error(req, HTTP_409, "UPLOAD_BUSY", "an upload replacing image %u is in progress",
                         id);
    } else if (!id || !image_store_get(id, &r)) {
        ret = send_error(req, HTTP_404, "IMAGE_NOT_FOUND", "no image with this id");
    } else if (active_unsaved(id)) {
        ret = send_error(req, HTTP_409, "UNSAVED_CHANGES",
                         "the computer wrote to this disk and the changes are not saved yet");
    } else if (image_store_delete(id) != ESP_OK) {
        ret = send_error(req, HTTP_500, "FLASH_ERROR", "catalog update failed");
    } else {
        disk_note_image_changed(id);
        disk_write_refresh();
        printf("API: image %u deleted (%u block(s) free again)\n", id, r.block_count);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "id", id);
        cJSON_AddStringToObject(root, "status", "deleted");
        cJSON_AddItemToObject(root, "storage", storage_json());
        ret = send_json(req, "200 OK", root);
    }
    xSemaphoreGive(api_lock);
    return ret;
}

/* POST /api/v1/storage/format {"confirm": "ERASE ALL IMAGES"} */
static esp_err_t post_format(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const cJSON *jc = cJSON_GetObjectItem(body, "confirm");
    bool ok = cJSON_IsString(jc) && strcmp(jc->valuestring, "ERASE ALL IMAGES") == 0;
    cJSON_Delete(body);
    if (!ok) {
        return send_error(req, HTTP_400, "CONFIRMATION_REQUIRED",
                          "send {\"confirm\": \"ERASE ALL IMAGES\"}: every stored image is lost");
    }

    xSemaphoreTake(api_lock, portMAX_DELAY);
    esp_err_t ret;
    if (upload_in_progress()) {
        ret = send_error(req, HTTP_409, "UPLOAD_BUSY", "an upload is in progress");
    } else if (disk_write_unsaved()) {
        ret = send_error(req, HTTP_409, "UNSAVED_CHANGES", "written sectors are not saved yet");
    } else if (!ext_flash_ready() || image_store_state() == STORE_NO_FLASH) {
        ret = send_error(req, HTTP_503, "STORAGE_NOT_READY", "no external flash");
    } else if (image_store_format() != ESP_OK) {
        ret = send_error(req, HTTP_500, "FLASH_ERROR", "initialising the image library failed");
    } else {
        disk_info_t d;
        disk_get_current(&d);
        if (d.source == DISK_SRC_FLASH) {
            disk_note_image_changed(d.image_id);
        }
        printf("API: image library initialised (all images erased)\n");
        ret = send_json(req, "200 OK", storage_json());
    }
    xSemaphoreGive(api_lock);
    return ret;
}

static esp_err_t get_current(httpd_req_t *req)
{
    return send_json(req, "200 OK", current_json());
}

/* PUT /api/v1/current {"image_id": n}: activate a stored image. */
static esp_err_t put_current(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json(req);
    if (!body) {
        return ESP_OK;
    }
    uint16_t id = json_image_id(cJSON_GetObjectItem(body, "image_id"));
    cJSON_Delete(body);
    if (!id) {
        return send_error(req, HTTP_400, "INVALID_REQUEST", "\"image_id\" must be an image id");
    }

    xSemaphoreTake(api_lock, portMAX_DELAY);
    switch_error_t e;
    esp_err_t ret;
    if (disk_switch_image(id, &e) == ESP_OK) {
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
    const cJSON *jrepl = cJSON_GetObjectItem(body, "replace");
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
    uint16_t replace_id = 0;
    if (jrepl) {
        replace_id = json_image_id(jrepl);
        if (!to_flash || !replace_id) {
            ret = send_error(req, HTTP_400, "INVALID_REQUEST",
                             "\"replace\" must be an image id, with \"destination\": \"flash\"");
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
    /* The name only decides which size check comes first; the format is
     * recognised from the content once it has arrived. */
    size_t fl = strlen(jfn->valuestring);
    bool maybe_hfe = fl >= 4 && strcasecmp(jfn->valuestring + fl - 4, ".hfe") == 0;
    st_result_t r = maybe_hfe ? (size > RF_MAX_LOGICAL_SIZE ? ST_TOO_LARGE : ST_OK)
                              : st_check_size(size, &st);
    if (maybe_hfe && r != ST_OK) {
        snprintf(st.detail, sizeof(st.detail), "%lu bytes, an HFE image may have at most %u",
                 (unsigned long)size, RF_MAX_LOGICAL_SIZE);
    }
    if (r != ST_OK) {
        const char *code = r == ST_TOO_LARGE ? "IMAGE_TOO_LARGE"
                           : r == ST_UNSUPPORTED ? "UNSUPPORTED_GEOMETRY" : "INVALID_IMAGE";
        ret = send_error(req, status_for(code), code, "%s", st.detail);
        goto out;
    }
    if (to_flash) {
        image_record_t old;
        if (image_store_state() != STORE_VALID) {
            ret = send_error(req, HTTP_503, "STORAGE_NOT_READY", "image library not usable (%s)",
                             image_store_state_name(image_store_state()));
            goto out;
        }
        if (replace_id && !image_store_get(replace_id, &old)) {
            ret = send_error(req, HTTP_404, "IMAGE_NOT_FOUND", "image %u does not exist",
                             replace_id);
            goto out;
        }
        if (replace_id && active_unsaved(replace_id)) {
            ret = send_error(req, HTTP_409, "UNSAVED_CHANGES",
                             "the computer wrote to this disk and the changes are not saved yet");
            goto out;
        }
        settings_t cfg;
        settings_get(&cfg);
        /* With compression the stored size is known only after compressing:
         * then the final check is done when storing (507 NO_SPACE). */
        if (!cfg.compress && !maybe_hfe && !image_store_fits(size, replace_id)) {
            store_usage_t u;
            image_store_usage(&u);
            ret = send_error(req, HTTP_507, "NO_SPACE",
                             "not enough free storage: %lu blocks needed, %u free%s",
                             (unsigned long)image_store_blocks_needed(size), u.blocks_free,
                             replace_id ? " (plus those of the replaced image)" : "");
            goto out;
        }
    }
    if (maybe_hfe && !to_flash) {
        ret = send_error(req, status_for("UNSUPPORTED_FORMAT"), "UNSUPPORTED_FORMAT",
                         "HFE images are stored in the library (\"destination\": \"flash\"); "
                         "the temporary PSRAM disk is for .ST images only");
        goto out;
    }
    /* HFE is received as a stream: no buffer for the whole file. */
    if (!maybe_hfe && heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < size) {
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
    up.replace_id = replace_id;
    up.hfe = maybe_hfe;
    up.activate = activate;
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

    if (up.hfe) {
        ret = receive_hfe(req);
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
    cJSON_AddBoolToObject(root, "compression", s.compress);
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
    cJSON_AddNumberToObject(root, "last_image_id", s.last_image_id);
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
    const cJSON *jc = cJSON_GetObjectItem(body, "compression");
    if (jc) {
        if (!cJSON_IsBool(jc)) {
            cJSON_Delete(body);
            return send_error(req, HTTP_400, "INVALID_REQUEST", "\"compression\" must be true or false");
        }
        s.compress = cJSON_IsTrue(jc);
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
        printf("API: settings saved (host name %s, drive %s, buzzer %s, machine %s)\n",
               s.hostname, s.drive_select == 0 ? "A: / DS0" : "B: / DS1",
               s.buzzer ? "on" : "off", machine_profile(s.machine)->id);
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
    cfg.max_uri_handlers = 24;
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
        { .uri = "/api/v1/images",    .method = HTTP_GET,    .handler = get_images },
        { .uri = "/api/v1/images",    .method = HTTP_POST,   .handler = post_image },
        { .uri = "/api/v1/images/*",  .method = HTTP_GET,    .handler = get_image },
        { .uri = "/api/v1/images/*",  .method = HTTP_PUT,    .handler = put_image },
        { .uri = "/api/v1/images/*",  .method = HTTP_DELETE, .handler = delete_image },
        { .uri = "/api/v1/storage",   .method = HTTP_GET,    .handler = get_storage },
        { .uri = "/api/v1/storage/format", .method = HTTP_POST, .handler = post_format },
        { .uri = "/api/v1/current",   .method = HTTP_GET,    .handler = get_current },
        { .uri = "/api/v1/current",   .method = HTTP_PUT,    .handler = put_current },
        { .uri = "/api/v1/uploads",   .method = HTTP_POST,   .handler = post_upload },
        { .uri = "/api/v1/uploads/*", .method = HTTP_GET,    .handler = get_upload },
        { .uri = "/api/v1/uploads/*", .method = HTTP_PUT,    .handler = put_upload_data },
        { .uri = "/api/v1/settings",  .method = HTTP_GET,    .handler = get_settings },
        { .uri = "/api/v1/settings",  .method = HTTP_PUT,    .handler = put_settings },
        { .uri = "/api/v1/system/restart", .method = HTTP_POST, .handler = post_restart },
        { .uri = "/api/v1/system/update",  .method = HTTP_GET,  .handler = get_update },
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
