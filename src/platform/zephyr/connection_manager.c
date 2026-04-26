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

K_SEM_DEFINE(run_lf_fed, 0, 1);
static struct net_mgmt_event_callback mgmt_cb;
static struct k_work_delayable connection_work;

static void lf_log_link_addr(struct net_if* iface, const char* context) {
  const struct net_linkaddr* ll;

  if (!iface) {
    LF_CONN_LOG("%s: iface=NULL", context);
    return;
  }

  ll = net_if_get_link_addr(iface);
  if (!ll || !ll->addr || ll->len == 0) {
    LF_CONN_LOG("%s: iface=%p link-addr unavailable", context, iface);
    return;
  }

  if (ll->len == 8) {
    LF_CONN_LOG("%s: iface=%p link-addr(8)=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x type=%u",
                context,
                iface,
                ll->addr[0],
                ll->addr[1],
                ll->addr[2],
                ll->addr[3],
                ll->addr[4],
                ll->addr[5],
                ll->addr[6],
                ll->addr[7],
                (unsigned int)ll->type);
  } else {
    LF_CONN_LOG("%s: iface=%p link-addr-len=%u type=%u (non-EUI64)",
                context,
                iface,
                (unsigned int)ll->len,
                (unsigned int)ll->type);
  }
}

static void lf_log_iface_state(struct net_if* iface, const char* context) {
  if (!iface) {
    LF_CONN_LOG("%s: iface=NULL", context);
    return;
  }

  LF_CONN_LOG("%s: iface=%p is_up=%d", context, iface, (int)net_if_is_up(iface));
  lf_log_link_addr(iface, context);
}

static int lf_set_ieee802154_link_addr_from_ipv6(struct net_if* iface, const char* ipv6_addr) {
  struct in6_addr addr;
  uint8_t eui64[8];
  char ipv6_buf[NET_IPV6_ADDR_LEN];

  if (!iface) {
    LF_CONN_LOG("set-link-addr: no default interface available");
    return -ENODEV;
  }

  if (!ipv6_addr) {
    LF_CONN_LOG("set-link-addr: ipv6_addr=NULL");
    return -EINVAL;
  }

  LF_CONN_LOG("set-link-addr: input-ipv6=%s", ipv6_addr);
  if (net_addr_pton(AF_INET6, ipv6_addr, &addr) < 0) {
    LF_CONN_LOG("set-link-addr: failed to parse IPv6 address '%s'", ipv6_addr);
    return -EINVAL;
  }

  if (net_addr_ntop(AF_INET6, &addr, ipv6_buf, sizeof(ipv6_buf)) != NULL) {
    LF_CONN_LOG("set-link-addr: parsed-ipv6=%s", ipv6_buf);
  }

  // Use the lower 64 bits (IID) and toggle U/L bit to recover IEEE EUI-64.
  memcpy(eui64, &addr.s6_addr[8], sizeof(eui64));
  eui64[0] ^= 0x02;

  LF_CONN_LOG("set-link-addr: derived-eui64=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
              eui64[0],
              eui64[1],
              eui64[2],
              eui64[3],
              eui64[4],
              eui64[5],
              eui64[6],
              eui64[7]);

  net_if_set_link_addr(iface, eui64, sizeof(eui64), NET_LINK_IEEE802154);
  lf_log_link_addr(iface, "set-link-addr:after-net_if_set_link_addr");
  return 0;
}

/**
 * @brief Signals network readiness from system work queue context.
 *
 * Runs in the system work queue (instead of directly in the `net_mgmt`
 * callback context) to keep callback work minimal and robust.
 */
static void connection_work_handler(struct k_work* work) {
  ARG_UNUSED(work);
  LF_CONN_LOG("work-handler: signaling network readiness (sem-count-before=%u)",
              (unsigned int)k_sem_count_get(&run_lf_fed));
  k_sem_give(&run_lf_fed);
  LF_CONN_LOG("work-handler: signaled (sem-count-after=%u)", (unsigned int)k_sem_count_get(&run_lf_fed));
}

/**
 * @brief Handle Zephyr network-management connectivity events.
 */
static void lf_connection_manager_event_handler(struct net_mgmt_event_callback* cb, uint32_t mgmt_event,
                                                struct net_if* iface) {
  ARG_UNUSED(cb);

  LF_CONN_LOG("event-handler: mgmt_event=0x%08x iface=%p", (unsigned int)mgmt_event, iface);
  lf_log_iface_state(iface, "event-handler:iface-state");

  switch (mgmt_event) {
  case NET_EVENT_IF_UP:
    LF_CONN_LOG("event-handler: NET_EVENT_IF_UP -> schedule readiness work");
    k_work_schedule(&connection_work, K_NO_WAIT);
    break;

  case NET_EVENT_IF_DOWN:
    LF_CONN_LOG("event-handler: NET_EVENT_IF_DOWN -> reset readiness semaphore");
    k_sem_reset(&run_lf_fed);
    LF_CONN_LOG("event-handler: semaphore reset (sem-count=%u)", (unsigned int)k_sem_count_get(&run_lf_fed));
    break;

  default:
    LF_CONN_LOG("event-handler: unhandled mgmt event 0x%08x", (unsigned int)mgmt_event);
    break;
  }
}

/**
 * @brief Initialize Zephyr connectivity monitoring.
 *
 * Registers a network-management callback and signals readiness immediately
 * when the default interface is already available.
 */
void lf_init_connection_manager(const char* ipv6_addr) {
  int ret;
  struct net_if* iface;

  LF_CONN_LOG("init: begin ipv6_addr=%s", ipv6_addr ? ipv6_addr : "<null>");
  LF_CONN_LOG("init: config NET_CONNECTION_MANAGER=%d IEEE802154_NET_IF_NO_AUTO_START=%d",
              IS_ENABLED(CONFIG_NET_CONNECTION_MANAGER),
              IS_ENABLED(CONFIG_IEEE802154_NET_IF_NO_AUTO_START));

  k_work_init_delayable(&connection_work, connection_work_handler);
  LF_CONN_LOG("init: delayable work initialized");

  iface = net_if_get_default();
  lf_log_iface_state(iface, "init:default-if-before-setup");

#if defined(CONFIG_IEEE802154_NET_IF_NO_AUTO_START)
  if (iface) {
    LF_CONN_LOG("init: configuring IEEE802154 link address before net_if_up");
    ret = lf_set_ieee802154_link_addr_from_ipv6(iface, ipv6_addr);
    if (ret != 0) {
      // Keep startup behavior robust: networking may still work without manual EUI-64 configuration.
      LF_CONN_LOG("init: failed to set IEEE802154 link address from IPv6 (%d)", ret);
    } else {
      LF_CONN_LOG("init: IEEE802154 link address configured successfully");
    }

    if (!net_if_is_up(iface)) {
      LF_CONN_LOG("init: default interface is down, calling net_if_up");
      ret = net_if_up(iface);
      if (ret != 0) {
        LF_CONN_LOG("init: failed to bring network interface up (%d)", ret);
      } else {
        LF_CONN_LOG("init: net_if_up succeeded");
      }
    } else {
      LF_CONN_LOG("init: default interface already up, skip net_if_up");
    }

    lf_log_iface_state(iface, "init:default-if-after-setup");
  } else {
    LF_CONN_LOG("init: default interface not available yet (cannot pre-configure link address)");
  }
#else
  ARG_UNUSED(ipv6_addr);
  LF_CONN_LOG("init: CONFIG_IEEE802154_NET_IF_NO_AUTO_START disabled, skipping pre-up link-addr config");
#endif

  if (IS_ENABLED(CONFIG_NET_CONNECTION_MANAGER)) {
    LF_CONN_LOG("init: registering net_mgmt callback for IF_UP/IF_DOWN");
    net_mgmt_init_event_callback(&mgmt_cb, lf_connection_manager_event_handler, EVENT_MASK);
    net_mgmt_add_event_callback(&mgmt_cb);
    LF_CONN_LOG("init: net_mgmt callback registered");

    // We would usually call `conn_mgr_mon_resend_status()` now in order
    // to trigger an immediate status update, but this causes a crash in
    // Zephyr 4.1.0 (but not 3.7.0, interestingly).
    //
    // Instead, check the current state directly.
    // For startup sequencing we only need the default interface to exist so
    // that IPv6 can be configured right after lf_wait_for_network_connection().
    iface = net_if_get_default();
    lf_log_iface_state(iface, "init:default-if-after-callback-registration");
    if (iface) {
      LF_CONN_LOG("init: default interface exists -> give readiness semaphore now");
      k_sem_give(&run_lf_fed);
      LF_CONN_LOG("init: readiness semaphore given (sem-count=%u)", (unsigned int)k_sem_count_get(&run_lf_fed));
    } // Otherwise, wait for a later interface event.
    else {
      LF_CONN_LOG("init: default interface still NULL -> waiting for future IF_UP event");
    }

  } else {
    // Network manager is not enabled. This is usually not intended behavior,
    // but we will just signal the semaphore immediately in this case to avoid blocking forever.
    LF_CONN_LOG("init: NET_CONNECTION_MANAGER disabled -> force readiness semaphore give");
    k_sem_give(&run_lf_fed);
    LF_CONN_LOG("init: readiness semaphore given (sem-count=%u)", (unsigned int)k_sem_count_get(&run_lf_fed));
  }

  LF_CONN_LOG("init: end");
}

/**
 * @brief Wait until network readiness is signaled.
 */
void lf_wait_for_network_connection(void) {
  int ret;
  LF_CONN_LOG("wait: begin (sem-count-before=%u)", (unsigned int)k_sem_count_get(&run_lf_fed));
  ret = k_sem_take(&run_lf_fed, K_FOREVER);
  LF_CONN_LOG("wait: completed ret=%d (sem-count-after=%u)", ret, (unsigned int)k_sem_count_get(&run_lf_fed));
}

/**
 * @brief Set the IPv6 address for the default network interface.
 *
 * Call this after lf_wait_for_network_connection() to configure the
 * IPv6 address. The address string should be in standard IPv6 notation
 * (e.g., "fe80::1").
 *
 * @param ipv6_addr The IPv6 address string to set
 * @return 0 on success, negative error code on failure
 */
int lf_set_ipv6_address(const char* ipv6_addr) {
  struct net_if* iface = net_if_get_default();
  struct in6_addr addr;
  int pton_ret;
  char ipv6_buf[NET_IPV6_ADDR_LEN];

  LF_CONN_LOG("set-ipv6: begin addr=%s", ipv6_addr ? ipv6_addr : "<null>");
  lf_log_iface_state(iface, "set-ipv6:default-if");

  if (!iface) {
    LF_CONN_LOG("set-ipv6: no default interface");
    return -ENODEV;
  }

  if (!ipv6_addr) {
    LF_CONN_LOG("set-ipv6: ipv6_addr=NULL");
    return -EINVAL;
  }

  // Parse the IPv6 address string.
  // Zephyr's net_addr_pton() returns 0 on success and a negative errno-style value on failure.
  pton_ret = net_addr_pton(AF_INET6, ipv6_addr, &addr);
  if (pton_ret < 0) {
    LF_CONN_LOG("set-ipv6: failed to parse IPv6 '%s' ret=%d", ipv6_addr, pton_ret);
    return pton_ret;
  }

  if (net_addr_ntop(AF_INET6, &addr, ipv6_buf, sizeof(ipv6_buf)) != NULL) {
    LF_CONN_LOG("set-ipv6: parsed-ipv6=%s", ipv6_buf);
  }

  // Add the address to the interface
  struct net_if_addr* ifaddr = net_if_ipv6_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);

  if (!ifaddr) {
    LF_CONN_LOG("set-ipv6: net_if_ipv6_addr_add returned NULL");
    return -EADDRNOTAVAIL;
  }

  LF_CONN_LOG("set-ipv6: success ifaddr=%p", ifaddr);

  return 0;
}
#endif // PLATFORM_ZEPHYR