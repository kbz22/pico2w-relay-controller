#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	bool ap_mode;
	bool connected;
	bool sta_power_save_disabled;
	int link_status;
	int32_t rssi_dbm;
	bool rssi_valid;
	uint32_t reconnect_attempts;
	uint32_t reconnect_successes;
	uint32_t disconnect_events;
} wifi_manager_metrics_t;

bool wifi_manager_init(void);
void wifi_manager_maintain(void);
void wifi_manager_reload_from_flash(void);

bool wifi_manager_is_connected(void);
bool wifi_manager_is_ap_mode(void);
const char *wifi_manager_get_ip_string(void);
const char *wifi_manager_get_ap_ssid(void);
const char *wifi_manager_link_status_name(int status);
void wifi_manager_get_metrics(wifi_manager_metrics_t *out_metrics);

#endif
