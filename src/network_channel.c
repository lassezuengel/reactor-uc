#include "reactor-uc/network_channel.h"

#ifdef PLATFORM_POSIX
#ifdef NETWORK_CHANNEL_TCP_POSIX
#include "platform/posix/tcp_ip_channel.c"
#endif

#elif defined(PLATFORM_ZEPHYR)
#ifdef NETWORK_CHANNEL_TCP_POSIX
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/conn_mgr_monitor.h>

#define EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
K_SEM_DEFINE(run_lf_fed, 0, 1);
static struct net_mgmt_event_callback mgmt_cb;
static struct k_work_delayable connection_work;

#include "platform/posix/tcp_ip_channel.c"
#endif
#include "platform/zephyr/udp_ip_channel.c"
#elif defined(PLATFORM_RIOT)
#ifdef NETWORK_CHANNEL_TCP_POSIX
#include "platform/posix/tcp_ip_channel.c"
#endif
#ifdef NETWORK_CHANNEL_COAP
#include "platform/riot/coap_udp_ip_channel.c"
#endif
#ifdef NETWORK_CHANNEL_UART
#include "platform/riot/uart_channel.c"
#endif

#elif defined(PLATFORM_PICO)
#ifdef NETWORK_CHANNEL_UART
#include "platform/pico/uart_channel.c"
#endif

#elif defined(PLATFORM_FLEXPRET)
#ifdef NETWORK_CHANNEL_TCP_POSIX
#error "NETWORK_POSIC_TCP not supported on FlexPRET"
#endif

#elif defined(PLATFORM_PATMOS)
#ifdef NETWORK_CHANNEL_S4NOC
#include "platform/patmos/s4noc_channel.c"
#endif
#endif

#if defined(PLATFORM_ZEPHYR) && defined(NETWORK_CHANNEL_TCP_POSIX) && defined(CONFIG_NET_CONNECTION_MANAGER)

/**
 * @brief Signals network readiness from system work queue context.
 *
 * Runs in system work queue rather than net_mgmt event thread to avoid:
 * - Hard faults from limited net_mgmt thread stack
 * - Priority inversion when signaling the semaphore
 * - Violating Zephyr 4.x restrictions on operations in event callbacks
 */
static void connection_work_handler(struct k_work* work) { k_sem_give(&run_lf_fed); }

/**
 * @brief Network management event handler for Zephyr's connection manager.
 */
static void lf_connection_manager_event_handler(struct net_mgmt_event_callback* cb, uint32_t mgmt_event,
                                                struct net_if* iface) {
  ARG_UNUSED(iface);
  ARG_UNUSED(cb);

  switch (mgmt_event) {
  case NET_EVENT_L4_CONNECTED:
    k_work_schedule(&connection_work, K_NO_WAIT);
    break;

  case NET_EVENT_L4_DISCONNECTED:
    k_sem_reset(&run_lf_fed);
    break;

  default:
    break;
  }
}

/**
 * @brief Initializes the network connection manager and registers event callbacks.
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
    // Instead, we will check the current connection state and signal
    // the semaphore if we are already connected.

    // Instead, check if already connected
    struct net_if* iface = net_if_get_default();
    if (iface && net_if_is_up(iface)) {
      if (net_if_ipv6_get_global_addr(NET_ADDR_PREFERRED, &iface)) {
        k_sem_give(&run_lf_fed);
      } else {
      }
    } // else: just keep waiting for the event callback to trigger when the interface comes up

  } else {
    // Network manager is not enabled. This is usually not intended behavior,
    // but we will just signal the semaphore immediately in this case to avoid blocking forever.
    k_sem_give(&run_lf_fed);
  }
}

/**
 * @brief Waits for the network connection to be ready.
 */
void lf_wait_for_network_connection(void) { k_sem_take(&run_lf_fed, K_FOREVER); }
#endif // PLATFORM_ZEPHYR

char* NetworkChannel_state_to_string(NetworkChannelState state) {
  switch (state) {
  case NETWORK_CHANNEL_STATE_UNINITIALIZED:
    return "UNINITIALIZED";
  case NETWORK_CHANNEL_STATE_OPEN:
    return "OPEN";
  case NETWORK_CHANNEL_STATE_CONNECTION_IN_PROGRESS:
    return "CONNECTION_IN_PROGRESS";
  case NETWORK_CHANNEL_STATE_CONNECTION_FAILED:
    return "CONNECTION_FAILED";
  case NETWORK_CHANNEL_STATE_CONNECTED:
    return "CONNECTED";
  case NETWORK_CHANNEL_STATE_LOST_CONNECTION:
    return "LOST_CONNECTION";
  case NETWORK_CHANNEL_STATE_CLOSED:
    return "CLOSED";
  }

  return "UNKNOWN";
}
