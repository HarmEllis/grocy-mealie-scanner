#include "provision_qr.h"
#include <stdint.h>
#include <string.h>
#define DECODED_MAX 330
static int value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}
static bool decode(const char *src, uint8_t *dst, size_t cap, size_t *out_len) {
    size_t n = strlen(src), used = 0; uint32_t acc = 0; unsigned bits = 0;
    if (!n || n % 4 == 1) return false;
    for (size_t i = 0; i < n; i++) {
        int v = value(src[i]); if (v < 0) return false;
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; if (used >= cap) return false;
            dst[used++] = (uint8_t)(acc >> bits);
            if (!bits) acc = 0; else acc &= (1U << bits) - 1U; }
    }
    if (bits && acc) return false;
    *out_len = used;
    return true;
}
static bool field(const uint8_t *data, size_t n, size_t *at, char *dst, size_t cap) {
    if (*at >= n) return false;
    size_t len = data[(*at)++];
    if (len >= cap || len > n - *at) return false;
    memcpy(dst, data + *at, len);
    dst[len] = '\0';
    *at += len;
    return true;
}
bool provision_qr_is_payload(const char *text) {
    return text && strncmp(text, PROVISION_QR_PREFIX, strlen(PROVISION_QR_PREFIX)) == 0;
}
bool provision_qr_parse(const char *text, provision_qr_config_t *out) {
    if (!out || !provision_qr_is_payload(text) || strlen(text) >= PROVISION_QR_MAX_TEXT_LEN) return false;
    uint8_t data[DECODED_MAX]; size_t n = 0, at = 0; provision_qr_config_t cfg = {0};
    if (!decode(text + strlen(PROVISION_QR_PREFIX), data, sizeof(data), &n) ||
        !field(data,n,&at,cfg.wifi_ssid,sizeof(cfg.wifi_ssid)) ||
        !field(data,n,&at,cfg.wifi_pass,sizeof(cfg.wifi_pass)) ||
        !field(data,n,&at,cfg.api_url,sizeof(cfg.api_url)) ||
        !field(data,n,&at,cfg.api_token,sizeof(cfg.api_token)) ||
        !field(data,n,&at,cfg.wifi_country,sizeof(cfg.wifi_country)) ||
        !field(data,n,&at,cfg.language,sizeof(cfg.language)) || at >= n) return false;
    uint8_t flags = data[at++]; if ((flags & ~1U) || at != n) return false;
    cfg.api_insecure = (flags & 1U) != 0;
    *out = cfg;
    return true;
}
