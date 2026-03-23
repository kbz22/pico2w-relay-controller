#include "wifi_store.h"

#include <stddef.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#endif

#define WIFI_STORE_MAGIC 0x57494649u
#define WIFI_STORE_VERSION 1u
#define WIFI_STORE_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

typedef struct {
    uint32_t magic;
    uint32_t version;
    wifi_credentials_t credentials;
    uint32_t checksum;
    uint8_t reserved[FLASH_PAGE_SIZE - (sizeof(uint32_t) + sizeof(uint32_t) + sizeof(wifi_credentials_t) + sizeof(uint32_t))];
} wifi_store_blob_t;

static uint32_t checksum_fnv1a(const uint8_t *data, size_t len) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t blob_checksum(const wifi_store_blob_t *blob) {
    return checksum_fnv1a((const uint8_t *)blob, offsetof(wifi_store_blob_t, checksum));
}

static const wifi_store_blob_t *flash_blob(void) {
    return (const wifi_store_blob_t *)(XIP_BASE + WIFI_STORE_OFFSET);
}

bool wifi_store_read(wifi_credentials_t *out) {
    if (!out) {
        return false;
    }

    const wifi_store_blob_t *blob = flash_blob();
    if (blob->magic != WIFI_STORE_MAGIC || blob->version != WIFI_STORE_VERSION) {
        return false;
    }

    if (blob->checksum != blob_checksum(blob)) {
        return false;
    }

    *out = blob->credentials;
    out->ssid[WIFI_SSID_MAX_LEN] = '\0';
    out->password[WIFI_PASSWORD_MAX_LEN] = '\0';

    return out->ssid[0] != '\0';
}

bool wifi_store_write(const char *ssid, const char *password) {
    if (!ssid || !password) {
        return false;
    }

    if (strlen(ssid) == 0 || strlen(ssid) > WIFI_SSID_MAX_LEN || strlen(password) > WIFI_PASSWORD_MAX_LEN) {
        return false;
    }

    wifi_store_blob_t blob;
    memset(&blob, 0xFF, sizeof(blob));

    blob.magic = WIFI_STORE_MAGIC;
    blob.version = WIFI_STORE_VERSION;
    strncpy(blob.credentials.ssid, ssid, WIFI_SSID_MAX_LEN);
    strncpy(blob.credentials.password, password, WIFI_PASSWORD_MAX_LEN);
    blob.credentials.ssid[WIFI_SSID_MAX_LEN] = '\0';
    blob.credentials.password[WIFI_PASSWORD_MAX_LEN] = '\0';
    blob.checksum = blob_checksum(&blob);

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(WIFI_STORE_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(WIFI_STORE_OFFSET, (const uint8_t *)&blob, FLASH_PAGE_SIZE);
    restore_interrupts(ints);

    return true;
}

bool wifi_store_clear(void) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(WIFI_STORE_OFFSET, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
    return true;
}
