/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 *
 * fork-aivision: shim for upstream-expected lwip_netconf.h.
 *
 * Upstream's ice_controller_net.c reads the local IPv4 address via the macro
 * LwIP_GetIP(if_idx) -- originally an Ameba Pro2 lwIP-port API. On our project
 * we run plain lwIP with a single default interface (netif_default), so we
 * map LwIP_GetIP(if_idx) to a pointer into netif_default->ip_addr regardless
 * of if_idx. The single consumer (GetLocalIPAdresses in ice_controller_net.c)
 * only ever calls with if_idx==0 to fetch the primary STUN candidate address.
 *
 * If multi-interface support is ever needed (it isn't in this project),
 * replace this macro with a real netif-table lookup.
 */

#ifndef LWIP_NETCONF_H
#define LWIP_NETCONF_H

#include <stdint.h>
#include "lwip/netif.h"

#define LwIP_GetIP( if_idx )    ( ( uint8_t * ) ( &( netif_default->ip_addr ) ) )

#endif /* LWIP_NETCONF_H */
