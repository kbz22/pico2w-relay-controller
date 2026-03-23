#include <stdio.h>

#include "pico/stdlib.h"

#include "http_server.h"
#include "relay.h"
#include "wifi_manager.h"

int main(void) {
    stdio_init_all();

    relay_init();

    sleep_ms(4000); // delay so i can see the serial

    if (!wifi_manager_init()) {
        return 1;
    }

    if (wifi_manager_is_ap_mode()) {
        printf("Setup mode active. Connect to AP '%s'.\n", wifi_manager_get_ap_ssid());
        printf("Open in browser: http://%s/\n", wifi_manager_get_ip_string());
    } else if (wifi_manager_is_connected()) {
        printf("Open in browser: http://%s/\n", wifi_manager_get_ip_string());
    } else {
        printf("STA mode active. Waiting for IP...\n");
    }

    if (!http_server_start()) {
        printf("Failed to start HTTP server\n");
        return 1;
    }

    printf("HTTP server listening on port 80\n");

    while (true) {
        wifi_manager_maintain();
        sleep_ms(1000);
    }
}
