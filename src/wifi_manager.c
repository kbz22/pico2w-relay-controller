#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "lwip/ip4_addr.h"
#include "lwip/netif.h"

#include "wifi_store.h"

#define WIFI_CONNECT_TIMEOUT_MS 30000
#define WIFI_RETRY_INTERVAL_MS 5000
#define SETUP_AP_SSID "PicoRelaySetup"
#define SETUP_AP_PASSWORD ""

static absolute_time_t next_retry_at;
static int last_link_status = CYW43_LINK_DOWN;
static bool ap_mode_enabled = false;
static volatile bool reload_requested = false;
static bool sta_power_save_disabled = false;
static uint32_t reconnect_attempts = 0;
static uint32_t reconnect_successes = 0;
static uint32_t disconnect_events = 0;

static wifi_credentials_t current_credentials;
static bool has_credentials = false;

static int wifi_link_status(void) {
    cyw43_arch_lwip_begin();
    const int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    cyw43_arch_lwip_end();
    return status;
}

const char *wifi_manager_link_status_name(int status) {
    switch (status) {
        case CYW43_LINK_UP:
            return "UP";
        case CYW43_LINK_NOIP:
            return "NOIP";
        case CYW43_LINK_JOIN:
            return "JOIN";
        case CYW43_LINK_DOWN:
            return "DOWN";
        case CYW43_LINK_FAIL:
            return "FAIL";
        case CYW43_LINK_NONET:
            return "NONET";
        case CYW43_LINK_BADAUTH:
            return "BADAUTH";
        default:
            return "UNKNOWN";
    }
}

static void sta_apply_radio_settings(void) {
    cyw43_arch_lwip_begin();
    const int pm_rc = cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
    cyw43_arch_lwip_end();

    sta_power_save_disabled = pm_rc == 0;
    if (pm_rc == 0) {
        printf("STA power save disabled (CYW43_NONE_PM)\n");
    } else {
        printf("Failed to disable STA power save (rc=%d)\n", pm_rc);
    }
}

static const char *auth_name(uint32_t auth) {
    return auth == CYW43_AUTH_OPEN ? "OPEN" : "WPA2";
}

static bool connect_sta_once(void) {
    if (!has_credentials) {
        return false;
    }

    reconnect_attempts++;
    const uint32_t auth = current_credentials.password[0] == '\0' ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_AES_PSK;
    printf("Connecting to Wi-Fi SSID: %s (%s)\n", current_credentials.ssid, auth_name(auth));
    const int rc = cyw43_arch_wifi_connect_timeout_ms(
        current_credentials.ssid,
        current_credentials.password,
        auth,
        WIFI_CONNECT_TIMEOUT_MS);

    if (rc) {
        printf("Wi-Fi connect attempt failed (rc=%d)\n", rc);
        return false;
    }

    reconnect_successes++;
    sta_apply_radio_settings();
    printf("Wi-Fi connected, IP: %s\n", wifi_manager_get_ip_string());
    return true;
}

static void start_setup_ap_mode(void) {
    cyw43_arch_disable_ap_mode();
    cyw43_arch_enable_ap_mode(SETUP_AP_SSID, SETUP_AP_PASSWORD, CYW43_AUTH_OPEN);
    ap_mode_enabled = true;
    printf("Starting setup AP: %s\n", SETUP_AP_SSID);
    printf("Connect and open: http://%s/\n", wifi_manager_get_ip_string());
}

static void start_sta_mode(void) {
    cyw43_arch_disable_ap_mode();
    cyw43_arch_enable_sta_mode();
    sta_apply_radio_settings();
    ap_mode_enabled = false;
    last_link_status = wifi_link_status();
}

static void load_mode_from_flash(void) {
    has_credentials = wifi_store_read(&current_credentials);

    if (has_credentials) {
        start_sta_mode();
        if (!connect_sta_once()) {
            next_retry_at = make_timeout_time_ms(WIFI_RETRY_INTERVAL_MS);
        }
    } else {
        start_setup_ap_mode();
    }
}

bool wifi_manager_init(void) {
    if (cyw43_arch_init()) {
        printf("Failed to init CYW43\n");
        return false;
    }

    load_mode_from_flash();
    next_retry_at = make_timeout_time_ms(WIFI_RETRY_INTERVAL_MS);
    return true;
}

void wifi_manager_reload_from_flash(void) {
    reload_requested = true;
}

void wifi_manager_get_metrics(wifi_manager_metrics_t *out_metrics) {
    if (!out_metrics) {
        return;
    }

    const int status = wifi_link_status();
    int32_t rssi = 0;
    bool rssi_valid = false;

    if (!ap_mode_enabled && (status == CYW43_LINK_UP || status == CYW43_LINK_NOIP || status == CYW43_LINK_JOIN)) {
        cyw43_arch_lwip_begin();
        rssi_valid = cyw43_wifi_get_rssi(&cyw43_state, &rssi) == 0;
        cyw43_arch_lwip_end();
    }

    out_metrics->ap_mode = ap_mode_enabled;
    out_metrics->connected = !ap_mode_enabled && status == CYW43_LINK_UP;
    out_metrics->sta_power_save_disabled = sta_power_save_disabled;
    out_metrics->link_status = status;
    out_metrics->rssi_dbm = rssi;
    out_metrics->rssi_valid = rssi_valid;
    out_metrics->reconnect_attempts = reconnect_attempts;
    out_metrics->reconnect_successes = reconnect_successes;
    out_metrics->disconnect_events = disconnect_events;
}

bool wifi_manager_is_connected(void) {
    return !ap_mode_enabled && wifi_link_status() == CYW43_LINK_UP;
}

bool wifi_manager_is_ap_mode(void) {
    return ap_mode_enabled;
}

const char *wifi_manager_get_ap_ssid(void) {
    return SETUP_AP_SSID;
}

const char *wifi_manager_get_ip_string(void) {
    cyw43_arch_lwip_begin();
    const ip4_addr_t *ip = netif_ip4_addr(netif_default);
    const char *ip_txt = ip4addr_ntoa(ip);
    cyw43_arch_lwip_end();
    return ip_txt ? ip_txt : "0.0.0.0";
}

void wifi_manager_maintain(void) {
    if (reload_requested) {
        reload_requested = false;
        printf("Reloading Wi-Fi mode from flash settings\n");
        load_mode_from_flash();
        next_retry_at = make_timeout_time_ms(WIFI_RETRY_INTERVAL_MS);
        return;
    }

    if (ap_mode_enabled || !has_credentials) {
        return;
    }

    const int status = wifi_link_status();
    if (status != last_link_status) {
        printf("Wi-Fi link status changed: %d\n", status);
        if ((last_link_status == CYW43_LINK_UP || last_link_status == CYW43_LINK_NOIP || last_link_status == CYW43_LINK_JOIN) &&
            (status == CYW43_LINK_DOWN || status == CYW43_LINK_FAIL || status == CYW43_LINK_NONET || status == CYW43_LINK_BADAUTH)) {
            disconnect_events++;
        }
        if (status == CYW43_LINK_UP) {
            sta_apply_radio_settings();
            printf("Wi-Fi connected, IP: %s\n", wifi_manager_get_ip_string());
        }
        last_link_status = status;
    }

    if (status == CYW43_LINK_UP || status == CYW43_LINK_JOIN || status == CYW43_LINK_NOIP) {
        return;
    }

    if (absolute_time_diff_us(get_absolute_time(), next_retry_at) > 0) {
        return;
    }

    if (connect_sta_once()) {
        printf("Wi-Fi reconnection successful\n");
    }
    next_retry_at = make_timeout_time_ms(WIFI_RETRY_INTERVAL_MS);
}
