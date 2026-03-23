#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>

bool wifi_manager_init(void);
void wifi_manager_maintain(void);
void wifi_manager_reload_from_flash(void);

bool wifi_manager_is_connected(void);
bool wifi_manager_is_ap_mode(void);
const char *wifi_manager_get_ip_string(void);
const char *wifi_manager_get_ap_ssid(void);

#endif
