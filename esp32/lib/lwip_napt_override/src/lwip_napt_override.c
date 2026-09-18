/*
 * Force-link the ESP-IDF 4.4.7 lwIP IPv4 implementation with forwarding and
 * NAPT enabled. The included upstream files retain their original BSD notice.
 *
 * When HOSHINO_DISABLE_LWIP_OVERRIDE is defined (ESP-IDF framework build),
 * the built-in lwIP NAPT is used instead, so we only provide the marker
 * function to satisfy the reference in main.cpp.
 */
struct netif;
int hoshino_napt_netif_enabled(const struct netif *network);

#ifndef HOSHINO_DISABLE_LWIP_OVERRIDE
#include "icmp_override.inc"
#include "ip4_override.inc"
#include "ip4_napt_override.inc"
#endif

void hoshino_lwip_napt_override_marker(void) {}
