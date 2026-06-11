#include "captive_dns.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

static const char *TAG = "captive_dns";

#define DNS_PORT          53
#define DNS_MAX_PACKET    512
#define DNS_BIND_DEVICE   "ap0"  /* esp_netif AP impl name */

#define DNS_TYPE_A        1
#define DNS_TYPE_AAAA     28
#define DNS_TYPE_SVCB     64
#define DNS_TYPE_HTTPS    65
#define DNS_CLASS_IN      1

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

typedef struct __attribute__((packed)) {
    uint16_t name_ptr;   /* 0xC00C points back to the offset-12 question name */
    uint16_t type;
    uint16_t class_;
    uint32_t ttl;
    uint16_t rdlength;
    uint8_t  rdata[4];   /* IPv4 only for A record answers */
} dns_answer_a_t;

static TaskHandle_t s_task = NULL;
static int          s_sock = -1;
static volatile bool s_run = false;

/* Walk the question name section. Returns the index just past the QNAME's
 * terminating zero, or -1 on malformed input. */
static int skip_qname(const uint8_t *buf, int len, int start)
{
    int i = start;
    while (i < len) {
        uint8_t l = buf[i];
        if (l == 0) return i + 1;
        if ((l & 0xC0) != 0) return -1;     /* pointer in question = malformed */
        i += 1 + l;
    }
    return -1;
}

static void dns_task(void *arg)
{
    (void)arg;
    uint8_t rx[DNS_MAX_PACKET];
    uint8_t tx[DNS_MAX_PACKET];

    while (s_run) {
        struct sockaddr_in src = {0};
        socklen_t src_len = sizeof(src);
        int n = recvfrom(s_sock, rx, sizeof(rx), 0,
                         (struct sockaddr *)&src, &src_len);
        if (n < (int)sizeof(dns_header_t)) continue;

        dns_header_t *hdr = (dns_header_t *)rx;
        uint16_t flags    = ntohs(hdr->flags);
        uint16_t qdcount  = ntohs(hdr->qdcount);
        /* Only handle standard queries (opcode 0) with at least one question
         * and the QR bit clear (it's a query, not a response). */
        if ((flags & 0x8000) || ((flags >> 11) & 0xF) || qdcount == 0) continue;

        int qname_end = skip_qname(rx, n, sizeof(dns_header_t));
        if (qname_end < 0 || qname_end + 4 > n) continue;
        uint16_t qtype  = ((uint16_t)rx[qname_end]     << 8) | rx[qname_end + 1];
        uint16_t qclass = ((uint16_t)rx[qname_end + 2] << 8) | rx[qname_end + 3];
        if (qclass != DNS_CLASS_IN) continue;

        int qsection_len = qname_end + 4 - sizeof(dns_header_t);

        /* Build the response: copy header + question, set QR/RA, set RCODE=0,
         * set ANCOUNT based on whether we have an answer to give. */
        memcpy(tx, rx, sizeof(dns_header_t) + qsection_len);
        dns_header_t *out = (dns_header_t *)tx;
        out->flags   = htons(0x8180);  /* QR=1, RD copied, RA=1, RCODE=0 */
        out->ancount = 0;
        out->nscount = 0;
        out->arcount = 0;

        int out_len = sizeof(dns_header_t) + qsection_len;

        if (qtype == DNS_TYPE_A) {
            /* Append an A record pointing at 192.168.4.1. */
            dns_answer_a_t ans = {
                .name_ptr = htons(0xC00C),
                .type     = htons(DNS_TYPE_A),
                .class_   = htons(DNS_CLASS_IN),
                .ttl      = htonl(60),
                .rdlength = htons(4),
                .rdata    = {192, 168, 4, 1},
            };
            if (out_len + (int)sizeof(ans) <= (int)sizeof(tx)) {
                memcpy(tx + out_len, &ans, sizeof(ans));
                out_len += sizeof(ans);
                out->ancount = htons(1);
            }
        }
        /* AAAA / HTTPS / SVCB / anything else → NOERROR + ANCOUNT=0
         * (already set above). Respond promptly so the client moves on. */

        sendto(s_sock, tx, out_len, 0,
               (struct sockaddr *)&src, src_len);
    }

    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t captive_dns_start(void)
{
    if (s_task) return ESP_OK;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        return ESP_FAIL;
    }

    /* Bind to the AP netif only so we never answer DNS on the upstream
     * STA interface. The AP netif impl name on ESP-IDF lwIP is "ap0". */
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap) {
        char ifname[8] = {0};
        if (esp_netif_get_netif_impl_name(ap, ifname) == ESP_OK && ifname[0]) {
            setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname));
        }
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr   = { .s_addr = htonl(INADDR_ANY) },
        .sin_port   = htons(DNS_PORT),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind(:53) failed: %d", errno);
        close(sock);
        return ESP_FAIL;
    }

    struct timeval rcv_to = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_to, sizeof(rcv_to));

    s_sock = sock;
    s_run  = true;
    if (xTaskCreate(dns_task, "captive_dns", 4096, NULL, 5, &s_task) != pdPASS) {
        s_run = false;
        close(sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Captive DNS responder up on :53");
    return ESP_OK;
}

void captive_dns_stop(void)
{
    if (!s_task) return;
    s_run = false;
    /* The task closes the socket on exit; closing here too prevents recvfrom
     * from blocking up to the recv timeout when shutdown is urgent. */
    if (s_sock >= 0) shutdown(s_sock, SHUT_RDWR);
    /* Wait briefly for the task to exit. */
    for (int i = 0; i < 10 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(100));
}
