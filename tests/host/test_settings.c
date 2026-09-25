/*
 * Host tests for the device settings on the external flash.
 *   tests/host/run.sh
 * Uses the real main/settings.c on a RAM model of the NOR flash.
 */
#include <stdio.h>
#include <string.h>

#include "esp_rom_crc.h"
#include "flash_layout.h"
#include "mock_ext_flash.h"
#include "settings.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_defaults(void)
{
    settings_t s;

    mock_flash_reset(0xff);
    CHECK(settings_init() == ESP_OK);
    CHECK(!settings_stored());
    settings_get(&s);
    CHECK(strcmp(s.hostname, "RadioFloppy") == 0);
    CHECK(strcmp(s.wifi_ssid, "DefaultNet") == 0);
    CHECK(s.wifi_security == WIFI_SEC_WPA2);
}

static void test_save_and_reload(void)
{
    settings_t s, r;

    mock_flash_reset(0xff);
    settings_init();
    settings_get(&s);
    snprintf(s.hostname, sizeof(s.hostname), "Atari-B");
    snprintf(s.wifi_ssid, sizeof(s.wifi_ssid), "Zolder");
    snprintf(s.wifi_pass, sizeof(s.wifi_pass), "geheim123");
    CHECK(settings_save(&s) == ESP_OK);
    CHECK(settings_stored());
    CHECK(mock_program_violations == 0);

    settings_init();                            /* as after a restart */
    settings_get(&r);
    CHECK(settings_stored());
    CHECK(memcmp(&s, &r, sizeof(s)) == 0);

    /* Second save goes to the other copy; the newest wins. */
    snprintf(s.hostname, sizeof(s.hostname), "Atari-C");
    CHECK(settings_save(&s) == ESP_OK);
    settings_init();
    settings_get(&r);
    CHECK(strcmp(r.hostname, "Atari-C") == 0);
    CHECK(mock_flash[RF_SETTINGS_A] != 0xff && mock_flash[RF_SETTINGS_B] != 0xff);

    /* Interrupted save (no commit word) of a third version: previous stays. */
    mock_flash[RF_SETTINGS_A + RF_SECTOR_SIZE - 1] = 0xff;
    mock_flash[RF_SETTINGS_B + RF_SECTOR_SIZE - 1] = 0xff;
    settings_init();
    CHECK(!settings_stored());
    CHECK(mock_program_violations == 0);
}

static void test_corrupt_copy(void)
{
    settings_t s, r;

    mock_flash_reset(0xff);
    settings_init();
    settings_get(&s);
    snprintf(s.hostname, sizeof(s.hostname), "One");
    settings_save(&s);                          /* copy A */
    snprintf(s.hostname, sizeof(s.hostname), "Two");
    settings_save(&s);                          /* copy B */
    mock_flash[RF_SETTINGS_B + 20] ^= 0x01;     /* damage the newest */
    settings_init();
    settings_get(&r);
    CHECK(strcmp(r.hostname, "One") == 0);
}

static void test_validation(void)
{
    settings_t s;

    CHECK(settings_hostname_valid("RadioFloppy"));
    CHECK(settings_hostname_valid("rf-2"));
    CHECK(!settings_hostname_valid(""));
    CHECK(!settings_hostname_valid("-rf"));
    CHECK(!settings_hostname_valid("rf-"));
    CHECK(!settings_hostname_valid("radio floppy"));
    CHECK(!settings_hostname_valid("radio.floppy"));
    CHECK(!settings_hostname_valid("abcdefghijabcdefghijabcdefghijabc"));   /* 33 */

    CHECK(settings_password_valid("12345678", WIFI_SEC_WPA2));
    CHECK(!settings_password_valid("1234567", WIFI_SEC_WPA2));
    CHECK(!settings_password_valid("", WIFI_SEC_WPA3));
    CHECK(settings_password_valid("", WIFI_SEC_OPEN));
    CHECK(!settings_password_valid("12345678", WIFI_SEC_OPEN));
    CHECK(settings_password_valid("0123456789abcdef0123456789abcdef0123456789abcdef0123456789ABCDEF",
                                  WIFI_SEC_WPA2));
    CHECK(!settings_password_valid("0123456789abcdef0123456789abcdef0123456789abcdef0123456789ABCDEG",
                                   WIFI_SEC_WPA2));

    CHECK(settings_security_from_name("wpa3") == WIFI_SEC_WPA3);
    CHECK(settings_security_from_name("wep") == -1);
    CHECK(strcmp(settings_security_name(WIFI_SEC_OPEN), "open") == 0);

    mock_flash_reset(0xff);
    settings_init();
    settings_get(&s);
    snprintf(s.hostname, sizeof(s.hostname), "bad name");
    CHECK(settings_save(&s) == ESP_ERR_INVALID_ARG);
    settings_get(&s);
    snprintf(s.wifi_pass, sizeof(s.wifi_pass), "short");
    CHECK(settings_save(&s) == ESP_ERR_INVALID_ARG);
    CHECK(!settings_stored());
}

static void test_drive_select(void)
{
    settings_t s, r;

    mock_flash_reset(0xff);
    settings_init();
    settings_get(&s);
    CHECK(s.drive_select == 1);                 /* default DS1 = drive B: */
    s.drive_select = 0;
    CHECK(settings_save(&s) == ESP_OK);
    settings_init();
    settings_get(&r);
    CHECK(r.drive_select == 0);
    s.drive_select = 2;
    CHECK(settings_save(&s) == ESP_ERR_INVALID_ARG);

    /* A version 1 record (148 bytes, no drive_select) is still read: DS1. */
    mock_flash_reset(0xff);
    settings_init();
    settings_get(&s);
    snprintf(s.hostname, sizeof(s.hostname), "OldOne");
    s.drive_select = 0;
    CHECK(settings_save(&s) == ESP_OK);         /* copy A */
    uint8_t *rec = mock_flash + RF_SETTINGS_A;
    rec[4] = 1; rec[5] = 0;                     /* version 1 */
    rec[6] = 148; rec[7] = 0;                   /* size 148 */
    memset(rec + 148, 0xff, 4);
    memset(rec + 12, 0, 4);
    uint32_t crc = esp_rom_crc32_le(0, rec, 148);
    memcpy(rec + 12, &crc, 4);
    settings_init();
    settings_get(&r);
    CHECK(settings_stored() && strcmp(r.hostname, "OldOne") == 0 && r.drive_select == 1);
}

int main(void)
{
    test_defaults();
    test_save_and_reload();
    test_corrupt_copy();
    test_validation();
    test_drive_select();
    printf(failures ? "FAILED (%d)\n" : "settings OK\n", failures);
    return failures != 0;
}
