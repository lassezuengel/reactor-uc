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
#include "platform/posix/tcp_ip_channel.c"
#endif
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
 * @brief Event handler for connection manager events.
 * @ingroup Federated
 * @todo We need to handle more events, such as auto-configuration of IPv6 addresses!
 *
 * @param mgmt_event The management event.
 */
static void lf_connection_manager_event_handler(struct net_mgmt_event_callback *cb,
                                                uint32_t mgmt_event, struct net_if *iface) {
  ARG_UNUSED(iface);
  ARG_UNUSED(cb);

  if ((mgmt_event & EVENT_MASK) != mgmt_event) {
    return;
  }

  if (mgmt_event == NET_EVENT_L4_CONNECTED) {
    k_sem_give(&run_lf_fed);

    return;
  }

  if (mgmt_event == NET_EVENT_L4_DISCONNECTED) {
    k_sem_reset(&run_lf_fed);

    return;
  }
}

void lf_init_connection_manager(void) {
  if (IS_ENABLED(CONFIG_NET_CONNECTION_MANAGER)) {
    net_mgmt_init_event_callback(&mgmt_cb,
                                 lf_connection_manager_event_handler,
                                 EVENT_MASK);
    net_mgmt_add_event_callback(&mgmt_cb);

    conn_mgr_mon_resend_status();
  } else {
    k_sem_give(&run_lf_fed);
  }
}

void lf_wait_for_network_connection(void) {
  k_sem_take(&run_lf_fed, K_FOREVER);
}
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
