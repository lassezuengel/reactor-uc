#include "reactor-uc/platform/zephyr/connection_manager.h"

#if defined(PLATFORM_ZEPHYR) && defined(CONFIG_NET_CONNECTION_MANAGER)

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/conn_mgr_monitor.h>

#define EVENT_MASK (NET_EVENT_IF_UP | NET_EVENT_IF_DOWN)
K_SEM_DEFINE(run_lf_fed, 0, 1);
static struct net_mgmt_event_callback mgmt_cb;
static struct k_work_delayable connection_work;

/**
 * @brief Signals network readiness from system work queue context.
 *
 * Runs in the system work queue (instead of directly in the `net_mgmt`
 * callback context) to keep callback work minimal and robust.
 */
static void connection_work_handler(struct k_work* work) { k_sem_give(&run_lf_fed); }

/**
 * @brief Handle Zephyr network-management connectivity events.
 */
static void lf_connection_manager_event_handler(struct net_mgmt_event_callback* cb, uint32_t mgmt_event,
                                                struct net_if* iface) {
  ARG_UNUSED(iface);
  ARG_UNUSED(cb);

  switch (mgmt_event) {
  case NET_EVENT_IF_UP:
    k_work_schedule(&connection_work, K_NO_WAIT);
    break;

  case NET_EVENT_IF_DOWN:
    k_sem_reset(&run_lf_fed);
    break;

  default:
    break;
  }
}

/**
 * @brief Initialize Zephyr connectivity monitoring.
 *
 * Registers a network-management callback and signals readiness immediately
 * when the default interface is already available.
 */
void lf_init_connection_manager(void) {
  k_work_init_delayable(&connection_work, connection_work_handler);

  if (IS_ENABLED(CONFIG_NET_CONNECTION_MANAGER)) {
    net_mgmt_init_event_callback(&mgmt_cb, lf_connection_manager_event_handler, EVENT_MASK);
    net_mgmt_add_event_callback(&mgmt_cb);

    // We would usually call `conn_mgr_mon_resend_status()` now in order
    // to trigger an immediate status update, but this causes a crash in
    // Zephyr 4.1.0 (but not 3.7.0, interestingly).
    //
    // Instead, check the current state directly.
    // For startup sequencing we only need the default interface to exist so
    // that IPv6 can be configured right after lf_wait_for_network_connection().
    struct net_if* iface = net_if_get_default();
    if (iface) {
      k_sem_give(&run_lf_fed);
    } // Otherwise, wait for a later interface event.

  } else {
    // Network manager is not enabled. This is usually not intended behavior,
    // but we will just signal the semaphore immediately in this case to avoid blocking forever.
    k_sem_give(&run_lf_fed);
  }
}

/**
 * @brief Wait until network readiness is signaled.
 */
void lf_wait_for_network_connection(void) { k_sem_take(&run_lf_fed, K_FOREVER); }

/**
 * @brief Set the IPv6 address for the default network interface.
 *
 * Call this after lf_wait_for_network_connection() to configure the
 * IPv6 address. The address string should be in standard IPv6 notation
 * (e.g., "fd01::1").
 *
 * @param ipv6_addr The IPv6 address string to set
 * @return 0 on success, negative error code on failure
 */
int lf_set_ipv6_address(const char* ipv6_addr) {
  struct net_if* iface = net_if_get_default();
  struct in6_addr addr;
  int pton_ret;

  if (!iface) {
    return -ENODEV;
  }

  if (!ipv6_addr) {
    return -EINVAL;
  }

  // Parse the IPv6 address string.
  // Zephyr's net_addr_pton() returns 0 on success and a negative errno-style value on failure.
  pton_ret = net_addr_pton(AF_INET6, ipv6_addr, &addr);
  if (pton_ret < 0) {
    return pton_ret;
  }

  // Add the address to the interface
  struct net_if_addr* ifaddr = net_if_ipv6_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);

  if (!ifaddr) {
    return -EADDRNOTAVAIL;
  }

  return 0;
}
#endif // PLATFORM_ZEPHYR