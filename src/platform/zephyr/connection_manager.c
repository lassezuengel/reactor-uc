#include "reactor-uc/platform/zephyr/connection_manager.h"

#if defined(PLATFORM_ZEPHYR) && defined(CONFIG_NET_CONNECTION_MANAGER)

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/net_linkaddr.h>
#include <string.h>
#include <stdio.h>

#define EVENT_MASK (NET_EVENT_IF_UP | NET_EVENT_IF_DOWN)
#define LF_CONN_LOG(fmt, ...) printk("LF/conn_mgr: " fmt "\n", ##__VA_ARGS__)

/*
 * EUI-64 for federate N:  00:00:00:ff:fe:00:00:N
 * Link-local for federate N: fe80::200:ff:fe00:N
 *
 * e.g. federate 1 -> fe80::200:ff:fe00:1
 *      federate 2 -> fe80::200:ff:fe00:2
 */

K_SEM_DEFINE(run_lf_fed, 0, 1);
static struct net_mgmt_event_callback mgmt_cb;
static struct k_work_delayable connection_work;
static char g_link_local_addr[NET_IPV6_ADDR_LEN];

static void lf_log_link_addr(struct net_if *iface, const char *context) {
  const struct net_linkaddr *ll;
  if (!iface) { LF_CONN_LOG("%s: iface=NULL", context); return; }
  ll = net_if_get_link_addr(iface);
  if (!ll || !ll->addr || ll->len == 0) {
    LF_CONN_LOG("%s: link-addr unavailable", context); return;
  }
  if (ll->len == 8) {
    LF_CONN_LOG("%s: link-addr=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                context,
                ll->addr[0], ll->addr[1], ll->addr[2], ll->addr[3],
                ll->addr[4], ll->addr[5], ll->addr[6], ll->addr[7]);
  }
}

static void lf_log_iface_state(struct net_if *iface, const char *context) {
  if (!iface) { LF_CONN_LOG("%s: iface=NULL", context); return; }
  LF_CONN_LOG("%s: is_up=%d", context, (int)net_if_is_up(iface));
  lf_log_link_addr(iface, context);
}

static void connection_work_handler(struct k_work *work) {
  ARG_UNUSED(work);
  k_sem_give(&run_lf_fed);
  LF_CONN_LOG("work-handler: network ready");
}

static void lf_connection_manager_event_handler(struct net_mgmt_event_callback *cb,
                                                 uint32_t mgmt_event,
                                                 struct net_if *iface) {
  ARG_UNUSED(cb);
  switch (mgmt_event) {
  case NET_EVENT_IF_UP:
    LF_CONN_LOG("event: IF_UP");
    k_work_schedule(&connection_work, K_NO_WAIT);
    break;
  case NET_EVENT_IF_DOWN:
    LF_CONN_LOG("event: IF_DOWN");
    k_sem_reset(&run_lf_fed);
    break;
  default:
    break;
  }
}

void lf_init_connection_manager(int federate_id) {
  int ret;
  struct net_if *iface;

  /* EUI-64: 00:00:00:ff:fe:00:HI:LO where HI:LO = federate_id as big-endian uint16 */
  uint8_t eui64[8] = {
    0x00, 0x00, 0x00, 0xff, 0xfe, 0x00,
    (uint8_t)((federate_id >> 8) & 0xff),
    (uint8_t)(federate_id & 0xff)
  };

  /* IID = eui64 with U/L bit toggled (byte 0 bit 1) -> 0x02 */
  snprintk(g_link_local_addr, sizeof(g_link_local_addr),
           "fe80::200:ff:fe00:%x", (unsigned int)(federate_id & 0xffff));

  LF_CONN_LOG("init: federate_id=%d link-local=%s", federate_id, g_link_local_addr);

  k_work_init_delayable(&connection_work, connection_work_handler);

  iface = net_if_get_default();
  lf_log_iface_state(iface, "init:before-setup");

#if defined(CONFIG_IEEE802154_NET_IF_NO_AUTO_START)
  if (!iface) {
    LF_CONN_LOG("init: no default iface");
    goto register_cb;
  }

  ret = net_if_set_link_addr(iface, eui64, sizeof(eui64), NET_LINK_IEEE802154);
  LF_CONN_LOG("init: net_if_set_link_addr ret=%d", ret);
  lf_log_link_addr(iface, "init:after-set-link-addr");

  if (!net_if_is_up(iface)) {
    ret = net_if_up(iface);
    LF_CONN_LOG("init: net_if_up ret=%d", ret);
  }

  lf_log_iface_state(iface, "init:after-setup");

register_cb:
#endif

  net_mgmt_init_event_callback(&mgmt_cb, lf_connection_manager_event_handler, EVENT_MASK);
  net_mgmt_add_event_callback(&mgmt_cb);

  iface = net_if_get_default();
  if (iface) {
    LF_CONN_LOG("init: iface exists, giving semaphore");
    k_sem_give(&run_lf_fed);
  } else {
    LF_CONN_LOG("init: waiting for IF_UP event");
  }

  /* Manually add the link-local since SLAAC may not fire on 802.15.4 */
  struct in6_addr ll_addr;
  net_addr_pton(AF_INET6, g_link_local_addr, &ll_addr);
  struct net_if_addr *ifaddr = net_if_ipv6_addr_add(iface, &ll_addr, NET_ADDR_MANUAL, 0);
  if (!ifaddr) {
    LF_CONN_LOG("init: failed to add IPv6 address %s", g_link_local_addr);
  } else {
    LF_CONN_LOG("init: added IPv6 address %s state=%d", g_link_local_addr, ifaddr->addr_state);
  }

  LF_CONN_LOG("init: end");
}

void lf_wait_for_network_connection(void) {
  LF_CONN_LOG("wait: blocking...");
  k_sem_take(&run_lf_fed, K_FOREVER);
  LF_CONN_LOG("wait: done");
}

const char *lf_get_link_local_addr(void) {
  return g_link_local_addr;
}

#endif /* PLATFORM_ZEPHYR */