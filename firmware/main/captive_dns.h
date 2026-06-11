#pragma once
#include "esp_err.h"

/* Tiny UDP/53 DNS responder used during the SoftAP provisioning window.
 *
 * - A-record queries return 192.168.4.1 (the SoftAP IP) so every name a
 *   client resolves while joined to the AP lands on the local HTTP portal.
 * - AAAA / HTTPS / SVCB / unsupported types get NOERROR with ANCOUNT=0
 *   (NODATA). iOS and Android often query A and AAAA in parallel; a
 *   timeout on either delays or skips the captive popup.
 * - Malformed packets are dropped silently.
 *
 * Start exactly once per provisioning window. Safe to call captive_dns_stop
 * even if start was never called. */
esp_err_t captive_dns_start(void);
void      captive_dns_stop(void);
