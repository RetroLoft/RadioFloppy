/*
 * Optional SSD1306 OLED. See oled.h.
 *
 * Uses the ESP-IDF I2C master driver at 400 kHz. The display is written
 * only on request (at start-up for now), from task context; it never
 * touches the floppy interrupts or the flux stream. Every error just
 * disables the display.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_app_desc.h"

#include "disk_image.h"
#include "drive_emu.h"
#include "wifi_net.h"

#include "oled.h"

#define I2C_TIMEOUT_MS  100

static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t dev;
static bool ready;

static esp_err_t send_cmds(const uint8_t *cmds, size_t n)
{
    uint8_t buf[32];

    buf[0] = 0x00;                      /* control byte: command stream */
    memcpy(buf + 1, cmds, n);
    return i2c_master_transmit(dev, buf, n + 1, I2C_TIMEOUT_MS);
}

bool oled_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,                 /* any free port */
        .sda_io_num = OLED_SDA_GPIO,
        .scl_io_num = OLED_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   /* modules usually have their own */
    };
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        printf("OLED: I2C bus init failed, continuing without display\n");
        return false;
    }
    if (i2c_master_probe(bus, OLED_I2C_ADDR, I2C_TIMEOUT_MS) != ESP_OK) {
        printf("OLED: not detected, continuing without display\n");
        i2c_del_master_bus(bus);
        bus = NULL;
        return false;
    }
    printf("OLED: SSD1306 detected at 0x%02X\n", OLED_I2C_ADDR);

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
        printf("OLED: device setup failed, continuing without display\n");
        return false;
    }

    const uint8_t init[] = {
        0xAE,                           /* display off */
        0xD5, 0x80,                     /* clock divide / oscillator */
        0xA8, OLED_HEIGHT - 1,          /* multiplex ratio */
        0xD3, 0x00,                     /* display offset */
        0x40,                           /* start line 0 */
        0x8D, 0x14,                     /* charge pump on (3.3 V supply) */
        0x20, 0x00,                     /* horizontal addressing */
        0xA1,                           /* segment remap: column 127 = SEG0 */
        0xC8,                           /* COM scan descending */
        0xDA, OLED_HEIGHT == 32 ? 0x02 : 0x12,  /* COM pins configuration */
        0x81, 0x8F,                     /* contrast */
        0xD9, 0xF1,                     /* pre-charge */
        0xDB, 0x40,                     /* VCOMH deselect level */
        0x2E,                           /* no scrolling */
        0xA4,                           /* display follows RAM */
        0xA6,                           /* normal, not inverted */
    };
    if (send_cmds(init, sizeof(init)) != ESP_OK) {
        printf("OLED: init failed, continuing without display\n");
        return false;
    }

    ready = true;
    oled_gfx_clear();
    if (oled_flush() != ESP_OK || send_cmds((const uint8_t[]){ 0xAF }, 1) != ESP_OK) {
        ready = false;
        printf("OLED: init failed, continuing without display\n");
        return false;
    }
    printf("OLED: %dx%d initialized\n", OLED_WIDTH, OLED_HEIGHT);
    return true;
}

esp_err_t oled_flush(void)
{
    static uint8_t buf[1 + sizeof(oled_fb)];

    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t window[] = {
        0x21, 0, OLED_WIDTH - 1,        /* column range */
        0x22, 0, OLED_PAGES - 1,        /* page range */
    };
    esp_err_t err = send_cmds(window, sizeof(window));
    if (err == ESP_OK) {
        buf[0] = 0x40;                  /* control byte: data stream */
        memcpy(buf + 1, oled_fb, sizeof(oled_fb));
        err = i2c_master_transmit(dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
    }
    if (err != ESP_OK) {
        printf("OLED: I2C error (%s), display disabled\n", esp_err_to_name(err));
        ready = false;
    }
    return err;
}

void oled_show_test(void)
{
    if (!ready) {
        return;
    }
    oled_gfx_clear();
    oled_gfx_text(0, 2, "RadioFloppy");
    oled_gfx_text(0, 18, "OLED test OK");
    oled_flush();
}

/* ---- Title of the active disk -------------------------------------------- */

#define TITLE_HEIGHT    11      /* glyph rows: about 1.5x the 7-row font */
#define TITLE_PITCH     16      /* line spacing: 2 lines on 32 px */

static TaskHandle_t title_task;

/* ---- WiFi signal icon (right end of line 2) ------------------------------ */

#define WIFI_ICON_W     11
#define WIFI_ICON_X     (OLED_WIDTH - WIFI_ICON_W)
#define WIFI_ICON_Y     (TITLE_PITCH + (TITLE_PITCH - TITLE_HEIGHT) / 2)
#define WIFI_ICON_COLS  (OLED_COLS - 2)     /* text left of the icon */
#define WIFI_POLL_MS    2000
#define WIFI_HYST_DB    3

/* -1 = WiFi not configured (no icon), 0 = not connected, 1..3 = bars. */
static int wifi_level_for(int rssi)
{
    return rssi >= -60 ? 3 : rssi >= -72 ? 2 : 1;
}

static int wifi_level(int prev)
{
    wifi_net_status_t w;

    wifi_net_get_status(&w);
    if (!w.configured) {
        return -1;
    }
    if (!w.connected || w.rssi == 0) {
        return 0;
    }
    /* Hysteresis: go up at the threshold, down only WIFI_HYST_DB below it. */
    int up = wifi_level_for(w.rssi);
    int down = wifi_level_for(w.rssi + WIFI_HYST_DB);
    if (prev <= 0 || up > prev) {
        return up;
    }
    return down < prev ? down : prev;
}

/* Three bars 3 px wide, 4/7/11 px high: filled up to `level`, else outline. */
static void draw_wifi_icon(int level)
{
    static const int bar_h[3] = { 4, 7, 11 };

    if (level < 0) {
        return;
    }
    for (int b = 0; b < 3; b++) {
        int x0 = WIFI_ICON_X + b * 4;
        int y0 = WIFI_ICON_Y + TITLE_HEIGHT - bar_h[b];
        for (int y = y0; y < WIFI_ICON_Y + TITLE_HEIGHT; y++) {
            for (int x = x0; x < x0 + 3; x++) {
                bool edge = x == x0 || x == x0 + 2 || y == y0 ||
                            y == WIFI_ICON_Y + TITLE_HEIGHT - 1;
                oled_gfx_pixel(x, y, b < level || edge);
            }
        }
    }
}

static void draw_disk_title(int wifi)
{
    disk_info_t d;

    disk_get_current(&d);
    oled_gfx_clear();
    /* Two lines of 16 px: 5x7 glyphs stretched to 11 rows, centred in the line. */
    oled_gfx_text_wrapped_ex((TITLE_PITCH - TITLE_HEIGHT) / 2, OLED_HEIGHT / TITLE_PITCH,
                             TITLE_HEIGHT, TITLE_PITCH,
                             wifi < 0 ? OLED_COLS : WIFI_ICON_COLS,
                             d.source == DISK_SRC_NONE ? "(no disk)" : d.name);
    draw_wifi_icon(wifi);
    oled_flush();
}

#define EV_TITLE        (1u << 0)   /* active disk changed */
#define EV_NETWORK      (1u << 1)   /* show network info */
#define EV_MESSAGE      (1u << 2)   /* fixed message until restart */

static char message[2][OLED_COLS + 1];

static void draw_message(void)
{
    oled_gfx_clear();
    oled_gfx_text_wrapped((TITLE_PITCH - TITLE_HEIGHT) / 2, 1, TITLE_HEIGHT, TITLE_PITCH,
                          message[0]);
    oled_gfx_text_wrapped(TITLE_PITCH + (TITLE_PITCH - TITLE_HEIGHT) / 2, 1, TITLE_HEIGHT,
                          TITLE_PITCH, message[1]);
    oled_flush();
}

static void draw_network(void)
{
    wifi_net_status_t w;

    wifi_net_get_status(&w);
    oled_gfx_clear();
    oled_gfx_text_wrapped((TITLE_PITCH - TITLE_HEIGHT) / 2, 1, TITLE_HEIGHT, TITLE_PITCH,
                          w.hostname);
    oled_gfx_text_wrapped(TITLE_PITCH + (TITLE_PITCH - TITLE_HEIGHT) / 2, 1, TITLE_HEIGHT,
                          TITLE_PITCH, !w.configured ? "WiFi off"
                                       : w.connected ? w.ip : "No connection");
    oled_flush();
}

/* Disk change (any task): wake the display task, never wait for I2C. */
static void disk_changed(void)
{
    if (title_task) {
        xTaskNotify(title_task, EV_TITLE, eSetBits);
    }
}

void oled_show_network(void)
{
    if (title_task) {
        xTaskNotify(title_task, EV_NETWORK, eSetBits);
    }
}

/* While our drive reads: title on line 1, "Track 02 Side 1" on line 2. */
static void draw_track(int cyl, int side, int wifi)
{
    disk_info_t d;
    char pos[24];

    disk_get_current(&d);
    snprintf(pos, sizeof(pos), "Track %02d Side %d", cyl, side);
    oled_gfx_clear();
    oled_gfx_text_wrapped((TITLE_PITCH - TITLE_HEIGHT) / 2, 1, TITLE_HEIGHT, TITLE_PITCH,
                          d.source == DISK_SRC_NONE ? "(no disk)" : d.name);
    oled_gfx_text_wrapped(TITLE_PITCH + (TITLE_PITCH - TITLE_HEIGHT) / 2, 1, TITLE_HEIGHT,
                          TITLE_PITCH, pos);
    draw_wifi_icon(wifi);
    oled_flush();
}

/*
 * One task owns the display. It redraws on events (disk change, network
 * info request) and polls the drive state every POLL_MS: the track view
 * starts when our drive is selected with the motor on and ends when the
 * motor goes off. Only changes are sent over I2C.
 */
#define POLL_MS         100

/* Boot screen: product name and firmware version, centred on two lines. */
#define SPLASH_MS       2000

static void draw_centred(int line, const char *s)
{
    int x = (OLED_WIDTH - (int)strlen(s) * OLED_CHAR_W) / 2;

    oled_gfx_text_tall(x < 0 ? 0 : x, line * TITLE_PITCH + (TITLE_PITCH - TITLE_HEIGHT) / 2,
                       TITLE_HEIGHT, s);
}

static void draw_splash(void)
{
    char ver[40];

    snprintf(ver, sizeof(ver), "v%s", esp_app_get_description()->version);
    oled_gfx_clear();
    draw_centred(0, "RadioFloppy");
    draw_centred(1, ver);
    oled_flush();
}

static void title_task_fn(void *arg)
{
    draw_splash();
    vTaskDelay(pdMS_TO_TICKS(SPLASH_MS));

    bool track_view = false;
    int shown_cyl = -1, shown_side = -1;
    TickType_t network_until = 0;
    bool network = false;
    bool redraw = true;
    int wifi = wifi_level(0);
    TickType_t wifi_checked = xTaskGetTickCount();

    while (ready) {
        uint32_t ev = 0;
        xTaskNotifyWait(0, UINT32_MAX, &ev, pdMS_TO_TICKS(POLL_MS));
        TickType_t now = xTaskGetTickCount();

        if (ev & EV_MESSAGE) {
            draw_message();
            while (ready) {                 /* until the restart */
                xTaskNotifyWait(0, UINT32_MAX, NULL, portMAX_DELAY);
            }
            break;
        }
        if (ev & EV_NETWORK) {
            network = true;
            network_until = now + pdMS_TO_TICKS(OLED_INFO_MS);
            draw_network();
        }
        if (network && ((ev & EV_TITLE) || (int32_t)(now - network_until) >= 0)) {
            network = false;                /* back to the normal view */
            redraw = true;
        }
        if (ev & EV_TITLE) {
            redraw = true;
        }
        if ((TickType_t)(now - wifi_checked) >= pdMS_TO_TICKS(WIFI_POLL_MS)) {
            wifi_checked = now;
            int level = wifi_level(wifi);
            if (level != wifi) {
                wifi = level;
                redraw = true;
            }
        }

        drive_status_t st;
        drive_peek(&st);
        if (!track_view && st.armed && st.selected && st.motor) {
            track_view = true;
            redraw = true;
        } else if (track_view && !st.motor) {
            track_view = false;
            redraw = true;
        }
        if (track_view && (st.cyl != shown_cyl || st.side != shown_side)) {
            redraw = true;
        }

        if (redraw && !network) {
            redraw = false;
            if (track_view) {
                shown_cyl = st.cyl;
                shown_side = st.side;
                draw_track(st.cyl, st.side, wifi);
            } else {
                shown_cyl = shown_side = -1;
                draw_disk_title(wifi);
            }
        }
    }
    title_task = NULL;
    vTaskDelete(NULL);
}

void oled_start_disk_title(void)
{
    if (!ready) {
        return;
    }
    disk_set_change_callback(disk_changed);
    /* Low priority, on core 1 with WiFi: away from the floppy ISRs. */
    xTaskCreatePinnedToCore(title_task_fn, "oled", 3072, NULL, 2, &title_task, 1);
}

void oled_show_message(const char *line1, const char *line2)
{
    if (!ready) {
        return;
    }
    snprintf(message[0], sizeof(message[0]), "%s", line1);
    snprintf(message[1], sizeof(message[1]), "%s", line2);
    if (title_task) {
        xTaskNotify(title_task, EV_MESSAGE, eSetBits);
    } else {
        draw_message();
    }
}

void oled_show_lines(const char *l0, const char *l1, const char *l2, const char *l3)
{
    const char *lines[] = { l0, l1, l2, l3 };

    if (!ready || title_task) {
        return;
    }
    oled_gfx_clear();
    for (int i = 0; i < OLED_ROWS && i < 4; i++) {
        oled_gfx_text(0, i * OLED_LINE_H, lines[i]);
    }
    oled_flush();
}
