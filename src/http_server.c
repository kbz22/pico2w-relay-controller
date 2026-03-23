#include "http_server.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/cyw43_arch.h"

#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "relay.h"
#include "wifi_manager.h"
#include "wifi_store.h"

#define HTTP_PORT 80
#define REQ_BUFFER_SIZE 1024
#define RESP_BUFFER_SIZE 6000

typedef struct {
    char *response;
    uint16_t total_len;
    uint16_t queued_len;
    uint16_t acked_len;
} http_conn_state_t;

static struct tcp_pcb *http_server_pcb;
static char ui_message[160] = "";

static int append_text(char *buf, size_t *used, size_t max, const char *text) {
    const size_t len = strlen(text);
    if (*used + len >= max) {
        return -1;
    }
    memcpy(buf + *used, text, len);
    *used += len;
    buf[*used] = '\0';
    return 0;
}

static int append_fmt(char *buf, size_t *used, size_t max, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int wrote = vsnprintf(buf + *used, max - *used, fmt, args);
    va_end(args);
    if (wrote < 0 || (size_t)wrote >= (max - *used)) {
        return -1;
    }
    *used += (size_t)wrote;
    return 0;
}

static void set_message(const char *msg) {
    if (!msg) {
        ui_message[0] = '\0';
        return;
    }
    strncpy(ui_message, msg, sizeof(ui_message) - 1);
    ui_message[sizeof(ui_message) - 1] = '\0';
}

static int hex_to_int(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void url_decode(char *s) {
    size_t r = 0;
    size_t w = 0;

    while (s[r] != '\0') {
        if (s[r] == '+') {
            s[w++] = ' ';
            r++;
            continue;
        }

        if (s[r] == '%' && s[r + 1] != '\0' && s[r + 2] != '\0') {
            const int hi = hex_to_int(s[r + 1]);
            const int lo = hex_to_int(s[r + 2]);
            if (hi >= 0 && lo >= 0) {
                s[w++] = (char)((hi << 4) | lo);
                r += 3;
                continue;
            }
        }

        s[w++] = s[r++];
    }

    s[w] = '\0';
}

static bool extract_query(const char *req, char *query, size_t query_len) {
    char path[300] = {0};
    if (sscanf(req, "GET %299s HTTP/", path) != 1) {
        return false;
    }

    const char *q = strchr(path, '?');
    if (!q) {
        query[0] = '\0';
        return true;
    }

    q++;
    strncpy(query, q, query_len - 1);
    query[query_len - 1] = '\0';
    return true;
}

static bool query_get_param(const char *query, const char *key, char *out, size_t out_len) {
    if (!query || !key || !out || out_len == 0) {
        return false;
    }

    const size_t key_len = strlen(key);
    const char *cursor = query;

    while (*cursor != '\0') {
        const char *amp = strchr(cursor, '&');
        const size_t seg_len = amp ? (size_t)(amp - cursor) : strlen(cursor);

        if (seg_len > key_len + 1 && strncmp(cursor, key, key_len) == 0 && cursor[key_len] == '=') {
            const size_t value_len = seg_len - key_len - 1;
            const size_t copy_len = value_len < (out_len - 1) ? value_len : (out_len - 1);
            memcpy(out, cursor + key_len + 1, copy_len);
            out[copy_len] = '\0';
            url_decode(out);
            return true;
        }

        if (!amp) {
            break;
        }
        cursor = amp + 1;
    }

    return false;
}

static bool query_has_flag(const char *query, const char *flag) {
    char value[8] = {0};
    if (!query_get_param(query, flag, value, sizeof(value))) {
        return false;
    }
    return strcmp(value, "1") == 0;
}

static void parse_request_and_update(const char *req) {
    if (!req) {
        return;
    }

    char query[320] = {0};
    if (!extract_query(req, query, sizeof(query))) {
        return;
    }

    if (query_has_flag(query, "wipe_wifi")) {
        if (wifi_store_clear()) {
            set_message("Wi-Fi credentials erased. Rebooting into AP setup mode.");
            wifi_manager_reload_from_flash();
        } else {
            set_message("Failed to erase Wi-Fi credentials.");
        }
        return;
    }

    if (query_has_flag(query, "set_wifi")) {
        char ssid[WIFI_SSID_MAX_LEN + 1] = {0};
        char password[WIFI_PASSWORD_MAX_LEN + 1] = {0};

        if (!query_get_param(query, "ssid", ssid, sizeof(ssid))) {
            set_message("SSID is required.");
            return;
        }

        query_get_param(query, "password", password, sizeof(password));

        if (!wifi_store_write(ssid, password)) {
            set_message("Invalid Wi-Fi values. SSID 1-32 chars, password up to 64 chars.");
            return;
        }

        set_message("Wi-Fi credentials saved. Switching from AP to STA now.");
        wifi_manager_reload_from_flash();
        return;
    }

    if (query_has_flag(query, "reset")) {
        relay_reset_all_on();
        set_message("All relays turned ON (pins low).");
        return;
    }

    int light_index = -1;
    if (sscanf(req, "GET /?light=%d&toggle=1", &light_index) == 1) {
        if (light_index >= 1 && (size_t)light_index <= relay_get_count()) {
            relay_toggle((size_t)(light_index - 1));
        }
        return;
    }
}

static int build_page(char *out, size_t out_len) {
    size_t used = 0;

    const bool ap_mode = wifi_manager_is_ap_mode();
    const char *wifi_mode = ap_mode ? "AP Setup" : "Station";
    const char *ip_text = wifi_manager_get_ip_string();

    if (append_text(out, &used, out_len,
                    "<!doctype html><html><head><meta charset='utf-8'>"
                    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                    "<title>Pico Lights</title>"
                    "<style>"
                    ":root{--bg:#f2efe7;--paper:#fffdf8;--ink:#1f2937;--on:#0f766e;--off:#b91c1c;--muted:#6b7280;--tog:#1d4ed8;--rst:#7c3aed;--ok:#065f46;--warn:#92400e;}"
                    "*{box-sizing:border-box}body{margin:0;font-family:Verdana,sans-serif;background:radial-gradient(circle at 20% 20%,#fff,#f2efe7 55%);color:var(--ink);}"
                    ".wrap{max-width:900px;margin:24px auto;padding:16px;}"
                    "h1{margin:0 0 6px;font-size:1.8rem;letter-spacing:.03em;}"
                    "h2{margin:0 0 10px;}"
                    "p{margin:0 0 12px;color:var(--muted);}"
                    ".meta{margin-bottom:14px;padding:10px;border-radius:10px;background:#f7f3e9;border:1px solid #d8cfbc;}"
                    ".msg{margin-bottom:14px;padding:10px;border-radius:10px;background:#ecfdf5;border:1px solid #a7f3d0;color:var(--ok);font-weight:700;}"
                    ".section{background:var(--paper);border:2px solid #ddd6c4;border-radius:14px;padding:14px;box-shadow:0 6px 20px rgba(0,0,0,.06);margin-bottom:14px;}"
                    ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:14px;}"
                    ".card{background:#fff;border:1px solid #e6dfcf;border-radius:12px;padding:12px;}"
                    ".state{font-weight:700;margin:8px 0 12px;}"
                    ".on{color:var(--on)}.off{color:var(--off)}"
                    "a.btn,button.btn{display:block;text-align:center;padding:10px 8px;border-radius:10px;text-decoration:none;font-weight:700;color:#fff;border:0;cursor:pointer;width:100%;}"
                    "a.toggle{background:var(--tog)}"
                    "a.reset-all{display:block;margin-top:12px;text-align:center;padding:12px;border-radius:12px;text-decoration:none;font-weight:700;color:#fff;background:var(--rst);}"
                    ".wifi-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;}"
                    ".wifi-grid input{width:100%;padding:10px;border:1px solid #d6cfbf;border-radius:10px;font-size:1rem;}"
                    ".wifi-actions{display:flex;gap:10px;margin-top:10px;}"
                    ".save{background:#065f46}.wipe{background:#b91c1c}"
                    "@media (max-width:760px){.wifi-grid{grid-template-columns:1fr}.wifi-actions{flex-direction:column}}"
                    "</style></head><body><div class='wrap'><h1>Pico Relay Controller</h1>") < 0) {
        return -1;
    }

    if (append_fmt(out, &used, out_len,
                   "<div class='meta'><b>Mode:</b> %s | <b>IP:</b> %s",
                   wifi_mode,
                   ip_text) < 0) {
        return -1;
    }

    if (ap_mode) {
        if (append_fmt(out, &used, out_len,
                       " | <b>AP SSID:</b> %s",
                       wifi_manager_get_ap_ssid()) < 0) {
            return -1;
        }
    }

    if (append_text(out, &used, out_len, "</div>") < 0) {
        return -1;
    }

    if (ui_message[0] != '\0') {
        if (append_fmt(out, &used, out_len, "<div class='msg'>%s</div>", ui_message) < 0) {
            return -1;
        }
    }

    if (append_text(out, &used, out_len,
                    "<section class='section'><h2>Wi-Fi Setup</h2>"
                    "<p>If no credentials are stored, device starts in AP setup mode. Saving below writes credentials to flash.</p>"
                    "<form method='GET'><input type='hidden' name='set_wifi' value='1'>"
                    "<div class='wifi-grid'>"
                    "<input name='ssid' maxlength='32' placeholder='Wi-Fi SSID' required>"
                    "<input name='password' maxlength='64' placeholder='Wi-Fi Password (empty for open network)'>"
                    "</div><div class='wifi-actions'>"
                    "<button class='btn save' type='submit'>Save Wi-Fi And Switch To STA</button>"
                    "<a class='btn wipe' href='/?wipe_wifi=1'>Wipe Wi-Fi (Back To AP Setup)</a>"
                    "</div></form></section>") < 0) {
        return -1;
    }

    if (append_text(out, &used, out_len,
                    "<section class='section'><h2>Relay Control</h2>"
                    "<p>Relays are active-low: pin HIGH means relay OFF, pin LOW means relay ON.</p><div class='grid'>") < 0) {
        return -1;
    }

    for (size_t i = 0; i < relay_get_count(); ++i) {
        const relay_light_t *light = relay_get(i);
        if (!light) {
            return -1;
        }

        if (append_fmt(out, &used, out_len,
                       "<section class='card'><h3>%s</h3><div class='state %s'>%s</div>"
                       "<a class='btn toggle' href='/?light=%u&toggle=1'>Toggle</a></section>",
                       light->label,
                       light->on ? "on" : "off",
                       light->on ? "ON" : "OFF",
                       (unsigned)(i + 1)) < 0) {
            return -1;
        }
    }

    if (append_text(out, &used, out_len,
                    "</div><a class='reset-all' href='/?reset=1'>Reset All (All ON)</a></section></div></body></html>") < 0) {
        return -1;
    }

    return (int)used;
}

static void free_conn_state(http_conn_state_t *state) {
    if (!state) {
        return;
    }
    free(state->response);
    free(state);
}

static err_t close_connection(struct tcp_pcb *tpcb, http_conn_state_t *state) {
    tcp_arg(tpcb, NULL);
    tcp_sent(tpcb, NULL);
    tcp_recv(tpcb, NULL);
    tcp_poll(tpcb, NULL, 0);
    tcp_err(tpcb, NULL);
    free_conn_state(state);

    const err_t close_err = tcp_close(tpcb);
    if (close_err != ERR_OK) {
        tcp_abort(tpcb);
        return ERR_ABRT;
    }
    return ERR_OK;
}

static err_t queue_pending_data(struct tcp_pcb *tpcb, http_conn_state_t *state) {
    while (state->queued_len < state->total_len) {
        const uint16_t remaining = state->total_len - state->queued_len;
        const uint16_t snd = tcp_sndbuf(tpcb);
        if (snd == 0) {
            break;
        }
        const uint16_t chunk = remaining < snd ? remaining : snd;
        const err_t wr = tcp_write(tpcb,
                                   state->response + state->queued_len,
                                   chunk,
                                   TCP_WRITE_FLAG_COPY);
        if (wr == ERR_MEM) {
            break;
        }
        if (wr != ERR_OK) {
            return wr;
        }
        state->queued_len += chunk;
    }

    return tcp_output(tpcb);
}

static void http_err_cb(void *arg, err_t err) {
    (void)err;
    http_conn_state_t *state = (http_conn_state_t *)arg;
    free_conn_state(state);
}

static err_t http_sent_cb(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    http_conn_state_t *state = (http_conn_state_t *)arg;
    if (!state) {
        tcp_abort(tpcb);
        return ERR_ABRT;
    }

    if (state->acked_len + len > state->total_len) {
        state->acked_len = state->total_len;
    } else {
        state->acked_len += len;
    }

    const err_t q = queue_pending_data(tpcb, state);
    if (q != ERR_OK) {
        return close_connection(tpcb, state);
    }

    if (state->acked_len >= state->total_len) {
        return close_connection(tpcb, state);
    }

    return ERR_OK;
}

static err_t http_poll_cb(void *arg, struct tcp_pcb *tpcb) {
    http_conn_state_t *state = (http_conn_state_t *)arg;
    if (!state) {
        tcp_abort(tpcb);
        return ERR_ABRT;
    }

    const err_t q = queue_pending_data(tpcb, state);
    if (q != ERR_OK) {
        return close_connection(tpcb, state);
    }

    if (state->acked_len >= state->total_len) {
        return close_connection(tpcb, state);
    }

    return ERR_OK;
}

static err_t http_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    http_conn_state_t *state = (http_conn_state_t *)arg;
    if (!state) {
        if (p) {
            pbuf_free(p);
        }
        tcp_abort(tpcb);
        return ERR_ABRT;
    }

    if (err != ERR_OK) {
        if (p) {
            pbuf_free(p);
        }
        return close_connection(tpcb, state);
    }

    if (!p) {
        return close_connection(tpcb, state);
    }

    if (state->response) {
        tcp_recved(tpcb, p->tot_len);
        pbuf_free(p);
        return ERR_OK;
    }

    char req[REQ_BUFFER_SIZE];
    const uint16_t to_copy = p->tot_len < (REQ_BUFFER_SIZE - 1) ? p->tot_len : (REQ_BUFFER_SIZE - 1);
    pbuf_copy_partial(p, req, to_copy, 0);
    req[to_copy] = '\0';
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    parse_request_and_update(req);

    char html[RESP_BUFFER_SIZE];
    const int html_len = build_page(html, sizeof(html));
    char header[160];
    int header_len;
    if (html_len > 0) {
        header_len = snprintf(header,
                              sizeof(header),
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: text/html; charset=utf-8\r\n"
                              "Content-Length: %d\r\n"
                              "Connection: close\r\n\r\n",
                              html_len);
    } else {
        header_len = snprintf(header,
                              sizeof(header),
                              "HTTP/1.1 500 Internal Server Error\r\n"
                              "Content-Length: 0\r\n"
                              "Connection: close\r\n\r\n");
    }

    if (header_len <= 0 || header_len >= (int)sizeof(header)) {
        return close_connection(tpcb, state);
    }

    const int total_len = header_len + (html_len > 0 ? html_len : 0);
    if (total_len <= 0 || total_len > 65535) {
        return close_connection(tpcb, state);
    }

    state->response = (char *)malloc((size_t)total_len);
    if (!state->response) {
        return close_connection(tpcb, state);
    }

    memcpy(state->response, header, (size_t)header_len);
    if (html_len > 0) {
        memcpy(state->response + header_len, html, (size_t)html_len);
    }
    state->total_len = (uint16_t)total_len;
    state->queued_len = 0;
    state->acked_len = 0;

    const err_t q = queue_pending_data(tpcb, state);
    if (q != ERR_OK) {
        return close_connection(tpcb, state);
    }

    if (state->queued_len >= state->total_len && state->acked_len >= state->total_len) {
        return close_connection(tpcb, state);
    }

    return ERR_OK;
}

static err_t http_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || !newpcb) {
        return ERR_VAL;
    }

    http_conn_state_t *state = (http_conn_state_t *)calloc(1, sizeof(http_conn_state_t));
    if (!state) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    tcp_arg(newpcb, state);
    tcp_recv(newpcb, http_recv_cb);
    tcp_sent(newpcb, http_sent_cb);
    tcp_poll(newpcb, http_poll_cb, 2);
    tcp_err(newpcb, http_err_cb);

    return ERR_OK;
}

bool http_server_start(void) {
    cyw43_arch_lwip_begin();
    http_server_pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!http_server_pcb) {
        cyw43_arch_lwip_end();
        return false;
    }

    if (tcp_bind(http_server_pcb, NULL, HTTP_PORT) != ERR_OK) {
        tcp_close(http_server_pcb);
        http_server_pcb = NULL;
        cyw43_arch_lwip_end();
        return false;
    }

    http_server_pcb = tcp_listen_with_backlog(http_server_pcb, 4);
    if (!http_server_pcb) {
        cyw43_arch_lwip_end();
        return false;
    }

    tcp_accept(http_server_pcb, http_accept_cb);
    cyw43_arch_lwip_end();
    return true;
}
