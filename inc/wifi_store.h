#ifndef WIFI_STORE_H
#define WIFI_STORE_H

#include <stdbool.h>

#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASSWORD_MAX_LEN 64

typedef struct {
    char ssid[WIFI_SSID_MAX_LEN + 1];
    char password[WIFI_PASSWORD_MAX_LEN + 1];
} wifi_credentials_t;

bool wifi_store_read(wifi_credentials_t *out);
bool wifi_store_write(const char *ssid, const char *password);
bool wifi_store_clear(void);

#endif
