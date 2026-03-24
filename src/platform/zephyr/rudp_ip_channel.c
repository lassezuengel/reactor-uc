#include "reactor-uc/platform/zephyr/rudp_ip_channel.h"

#include "reactor-uc/logging.h"
#include "reactor-uc/serialization.h"

#include <arpa/inet.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define RUDP_IP_CHANNEL_ERR(fmt, ...) LF_ERR(NET, "RUdpIpChannel: " fmt, ##__VA_ARGS__)
#define RUDP_IP_CHANNEL_WARN(fmt, ...) LF_WARN(NET, "RUdpIpChannel: " fmt, ##__VA_ARGS__)
#define RUDP_IP_CHANNEL_INFO(fmt, ...) LF_INFO(NET, "RUdpIpChannel: " fmt, ##__VA_ARGS__)
#define RUDP_IP_CHANNEL_DEBUG(fmt, ...) LF_DEBUG(NET, "RUdpIpChannel: " fmt, ##__VA_ARGS__)

#define NETWORK_CHANNEL_RET_OK LF_OK
#define NETWORK_CHANNEL_RET_ERROR LF_ERR
#define NETWORK_CHANNEL_RET_RETRY LF_NETWORK_CHANNEL_RETRY

static volatile uint32_t packets_sent = 0, packets_retransmitted = 0;
static volatile uint32_t packets_received = 0, packets_received_duplicates = 0;
#ifdef RUDP_IP_CHANNEL_LOG_STATS
#define LOG_STAT(fmt, ...) RUDP_IP_CHANNEL_INFO("[stat] " fmt, ##__VA_ARGS__)
#else
#define LOG_STAT(fmt, ...)                                                                                             \
  do {                                                                                                                 \
  } while (0)
#endif

#define RUDP_IP_CHANNEL_ZEPHYR_THREAD_PRIORITY K_PRIO_PREEMPT(0)

enum {
  // Normal data packet with payload.
  RUDP_PACKET_TYPE_DATA = 0x0,
  // ACK packet acknowledging receipt of a specific data packet.
  RUDP_PACKET_TYPE_ACK = 0x1,
  // Full ACK packet that acknowledges receipt of all packets up to and including the given UID,
  // useful for acknowledging multiple packets at once and for increasing ack reliability by
  // sending redundant ACKs.
  RUDP_PACKET_TYPE_FACK = 0x2,
  // Hole ACK packet that, while acking a specific packet, indicates another missing packet ("hole"),
  // used by receiver to request retransmission. This allows the sender to retransmit a packet that is
  // definitely missing at receiver-side before the ACK timeout occurs, reducing recovery time when
  // packets are lost.
  //
  // The receiver will send a HACK for the earliest/oldest missing packet it detects, indicating that
  // all older packets were received (even if their ACKs haven't arrived yet). This allows the sender
  // to mark all older packets as acknowledged when processing the HACK, preventing unnecessary
  // retransmissions of those packets in case of ACK loss.
  RUDP_PACKET_TYPE_HACK = 0x03
} PacketType;

static void _RUdpIpChannel_worker_thread(void* p1, void* p2, void* p3);

static int _RUdpIpChannel_fill_sockaddr(const char* host, unsigned short port, int protocol_family,
                                        struct sockaddr_storage* storage, socklen_t* addrlen) {
  memset(storage, 0, sizeof(*storage));

  switch (protocol_family) {
  case AF_INET: {
    struct sockaddr_in* addr4 = (struct sockaddr_in*)storage;
    addr4->sin_family = AF_INET;
    addr4->sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr4->sin_addr) <= 0) {
      RUDP_IP_CHANNEL_ERR("Invalid IPv4 address %s", host);
      return NETWORK_CHANNEL_RET_ERROR;
    }
    *addrlen = sizeof(struct sockaddr_in);
    return NETWORK_CHANNEL_RET_OK;
  }
  case AF_INET6: {
    struct sockaddr_in6* addr6 = (struct sockaddr_in6*)storage;
    addr6->sin6_family = AF_INET6;
    addr6->sin6_port = htons(port);
    if (inet_pton(AF_INET6, host, &addr6->sin6_addr) <= 0) {
      RUDP_IP_CHANNEL_ERR("Invalid IPv6 address %s", host);
      return NETWORK_CHANNEL_RET_ERROR;
    }
    *addrlen = sizeof(struct sockaddr_in6);
    return NETWORK_CHANNEL_RET_OK;
  }
  default:
    RUDP_IP_CHANNEL_ERR("Unsupported protocol family %d", protocol_family);
    return NETWORK_CHANNEL_RET_ERROR;
  }
}

static void _RUdpIpChannel_update_state_locked(RUdpIpChannel* self, NetworkChannelState state) { self->state = state; }

static void _RUdpIpChannel_update_state(RUdpIpChannel* self, NetworkChannelState state) {
  k_mutex_lock(&self->mutex, K_FOREVER);
  _RUdpIpChannel_update_state_locked(self, state);
  k_mutex_unlock(&self->mutex);
}

static NetworkChannelState _RUdpIpChannel_get_state_locked(RUdpIpChannel* self) { return self->state; }

static NetworkChannelState _RUdpIpChannel_get_state(RUdpIpChannel* self) {
  k_mutex_lock(&self->mutex, K_FOREVER);
  NetworkChannelState state = _RUdpIpChannel_get_state_locked(self);
  k_mutex_unlock(&self->mutex);
  return state;
}

static bool RUdpIpChannel_is_connected(NetworkChannel* untyped_self) {
  RUdpIpChannel* self = (RUdpIpChannel*)untyped_self;
  return _RUdpIpChannel_get_state(self) == NETWORK_CHANNEL_STATE_CONNECTED;
}

static void _RUdpIpChannel_close_socket(RUdpIpChannel* self) {
  if (self->fd >= 0) {
    if (shutdown(self->fd, SHUT_RDWR) < 0 && errno != ENOTCONN) {
      RUDP_IP_CHANNEL_WARN("RUDP shutdown failed errno=%d", errno);
    }
    if (close(self->fd) < 0) {
      RUDP_IP_CHANNEL_WARN("RUDP close failed errno=%d", errno);
    }
    self->fd = -1;
  }
}

static void _RUdpIpChannel_spawn_worker_thread(RUdpIpChannel* self) {
  assert(!self->worker_thread_started);

  self->worker_thread_id = k_thread_create(
      &self->worker_thread, self->worker_thread_stack, K_KERNEL_STACK_SIZEOF(self->worker_thread_stack),
      _RUdpIpChannel_worker_thread, self, NULL, NULL, RUDP_IP_CHANNEL_ZEPHYR_THREAD_PRIORITY, 0, K_NO_WAIT);

  assert(self->worker_thread_id != NULL);
  (void)k_thread_name_set(self->worker_thread_id, "lf_rudp_rx");
  self->worker_thread_started = true;
}

static int _RUdpIpChannel_connect(RUdpIpChannel* self) {
  struct sockaddr_storage local_addr;
  socklen_t local_addrlen = 0;
  struct sockaddr_storage remote_addr;
  socklen_t remote_addrlen = 0;

  if (_RUdpIpChannel_fill_sockaddr(self->local_host, self->local_port, self->protocol_family, &local_addr,
                                   &local_addrlen) != NETWORK_CHANNEL_RET_OK) {
    RUDP_IP_CHANNEL_ERR("Invalid local endpoint %s:%u", self->local_host, self->local_port);
    return NETWORK_CHANNEL_RET_ERROR;
  }

  if (_RUdpIpChannel_fill_sockaddr(self->remote_host, self->remote_port, self->protocol_family, &remote_addr,
                                   &remote_addrlen) != NETWORK_CHANNEL_RET_OK) {
    RUDP_IP_CHANNEL_ERR("Invalid remote endpoint %s:%u", self->remote_host, self->remote_port);
    return NETWORK_CHANNEL_RET_ERROR;
  }

  _RUdpIpChannel_close_socket(self);

  self->fd = socket(self->protocol_family, SOCK_DGRAM, 0);
  if (self->fd < 0) {
    RUDP_IP_CHANNEL_ERR("Failed to create UDP socket errno=%d", errno);
    return NETWORK_CHANNEL_RET_ERROR;
  }

  /* Set recv timeout equal to the retransmit timeout so the worker wakes up to check
   * retransmissions even when no packet arrives. The scheduler puts the thread to sleep
   * properly inside recv — no busy-wait or k_msleep needed. */
#if defined(CONFIG_NET_CONTEXT_RCVTIMEO) && (CONFIG_NET_CONTEXT_RCVTIMEO == 1)
  struct timeval recv_timeout = {
      .tv_sec = RUDP_RETRANSMIT_TIMEOUT_MS / 1000,
      .tv_usec = (RUDP_RETRANSMIT_TIMEOUT_MS % 1000) * 1000,
  };
  if (setsockopt(self->fd, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout)) < 0) {
    RUDP_IP_CHANNEL_WARN("Failed to set socket recv timeout errno=%d", errno);
  }
#else
  RUDP_IP_CHANNEL_DEBUG("SO_RCVTIMEO unsupported or disabled, recv will block until data arrives");
#endif

  if (bind(self->fd, (struct sockaddr*)&local_addr, local_addrlen) < 0) {
    RUDP_IP_CHANNEL_ERR("Failed to bind RUDP socket to %s:%u errno=%d", self->local_host, self->local_port, errno);
    _RUdpIpChannel_close_socket(self);
    return NETWORK_CHANNEL_RET_ERROR;
  }

  if (connect(self->fd, (struct sockaddr*)&remote_addr, remote_addrlen) < 0) {
    RUDP_IP_CHANNEL_ERR("Failed to connect RUDP socket to %s:%u errno=%d", self->remote_host, self->remote_port, errno);
    _RUdpIpChannel_close_socket(self);
    return NETWORK_CHANNEL_RET_ERROR;
  }

  RUDP_IP_CHANNEL_INFO("Connected RUDP socket local=%s:%u remote=%s:%u", self->local_host, self->local_port,
                       self->remote_host, self->remote_port);
  return NETWORK_CHANNEL_RET_OK;
}

static lf_ret_t RUdpIpChannel_open_connection(NetworkChannel* untyped_self) {
  RUdpIpChannel* self = (RUdpIpChannel*)untyped_self;
  RUDP_IP_CHANNEL_DEBUG("Open connection");

  _RUdpIpChannel_update_state(self, NETWORK_CHANNEL_STATE_OPEN);

  return NETWORK_CHANNEL_RET_OK;
}

static void RUdpIpChannel_close_connection(NetworkChannel* untyped_self) {
  RUdpIpChannel* self = (RUdpIpChannel*)untyped_self;
  RUDP_IP_CHANNEL_DEBUG("Closing connection");

  if (_RUdpIpChannel_get_state(self) == NETWORK_CHANNEL_STATE_CLOSED) {
    return;
  }

  _RUdpIpChannel_close_socket(self);
  _RUdpIpChannel_update_state(self, NETWORK_CHANNEL_STATE_CLOSED);
}

/********************************************************************************************************** */

static bool _RUdpIpChannel_is_ack_packet(const unsigned char* data, size_t length) {
  return length == 9 && (data[0] & 0x0F) == RUDP_PACKET_TYPE_ACK;
}

static bool _RUdpIpChannel_is_fack_packet(const unsigned char* data, size_t length) {
  return length == 9 && (data[0] & 0x0F) == RUDP_PACKET_TYPE_FACK;
}

static bool _RUdpIpChannel_is_hack_packet(const unsigned char* data, size_t length) {
  return length == 9 && (data[0] & 0x0F) == RUDP_PACKET_TYPE_HACK;
}

/**
 * Packets sent over the RUDP channel have the following format:
 * [1 byte:  type and flags]
 * [4 bytes: UID][payload]
 * [4 bytes: size of payload, in bytes]
 * [payload]
 *
 */
static int _RUdpIpChannel_protocol_extract_uid(const unsigned char* data, size_t length) {
  if (length < 5) {
    RUDP_IP_CHANNEL_ERR("Received RUDP packet with invalid length %zu", length);
    return -1;
  }

  return *(const int*)(data + 1);
}

/**
 * Find an empty slot in the outgoing buffer (one that is acknowledged and can be reused).
 * Returns index >= 0 on success, or -1 if buffer is full.
 */
static int _RUdpIpChannel_find_empty_outgoing_slot(RUdpIpChannel* self) {
  for (int i = 0; i < RUDP_OUTGOING_BUFFER_SIZE; i++) {
    if (self->outgoing_buffer[i].is_acked) {
      return i;
    }
  }
  return -1;
}

/**
 * Find an existing slot by UID in the outgoing buffer.
 * Returns index >= 0 on success, or -1 if not found.
 */
static int _RUdpIpChannel_find_outgoing_slot_by_uid(RUdpIpChannel* self, int uid) {
  for (int i = 0; i < RUDP_OUTGOING_BUFFER_SIZE; i++) {
    if (!self->outgoing_buffer[i].is_acked && self->outgoing_buffer[i].uid == uid) {
      return i;
    }
  }
  return -1;
}

/**
 * Actually send a packet stored in the outgoing buffer.
 */
static int _RUdpIpChannel_send_outgoing_packet(RUdpIpChannel* self, int buffer_idx) {
  RUdpOutgoingPacket* pkt = &self->outgoing_buffer[buffer_idx];
  ssize_t bytes_sent = send(self->fd, pkt->packet_data, (size_t)pkt->packet_length, 0);

  if (packets_sent++ % 100 == 0) {
    LOG_STAT("%d packets sent, %d retransmissions", packets_sent, packets_retransmitted);
  }

  if (bytes_sent < 0) {
    RUDP_IP_CHANNEL_ERR("Failed to send RUDP packet uid=%d errno=%d", pkt->uid, errno);
    return NETWORK_CHANNEL_RET_ERROR;
  }

  if (bytes_sent != pkt->packet_length) {
    RUDP_IP_CHANNEL_WARN("Partial RUDP packet send %d/%d uid=%d", (int)bytes_sent, pkt->packet_length, pkt->uid);
    return NETWORK_CHANNEL_RET_RETRY;
  }

  pkt->last_send_time_ms = k_uptime_get();
  return NETWORK_CHANNEL_RET_OK;
}

/**
 * Retransmit a packet from the outgoing buffer.
 */
static int _RUdpIpChannel_retransmit_outgoing_packet(RUdpIpChannel* self, int buffer_idx, bool triggered_by_timeout,
                                                     int hack_request_uid) {
  k_mutex_lock(&self->mutex, K_FOREVER);

  if (buffer_idx < 0 || buffer_idx >= RUDP_OUTGOING_BUFFER_SIZE) {
    k_mutex_unlock(&self->mutex);
    return NETWORK_CHANNEL_RET_ERROR;
  }

  RUdpOutgoingPacket* pkt = &self->outgoing_buffer[buffer_idx];
  if (pkt->is_acked) {
    k_mutex_unlock(&self->mutex);
    return NETWORK_CHANNEL_RET_OK;
  }

  if (!triggered_by_timeout && hack_request_uid < pkt->next_allowed_hack_uid) {
    RUDP_IP_CHANNEL_DEBUG("Ignoring stale/old HACK retransmit request for uid=%d (requester uid=%d, next_allowed=%d)",
                          pkt->uid, hack_request_uid, pkt->next_allowed_hack_uid);
    LOG_STAT("Ignoring stale/old HACK retransmit request for uid=%d (requester uid=%d, next_allowed=%d)", pkt->uid,
             hack_request_uid, pkt->next_allowed_hack_uid);
    k_mutex_unlock(&self->mutex);
    return NETWORK_CHANNEL_RET_OK;
  }

  if (pkt->retry_count >= RUDP_MAX_RETRIES) {
    RUDP_IP_CHANNEL_WARN("RUDP packet uid=%d exceeded max retries, giving up", pkt->uid);
    pkt->is_acked = true;
    k_mutex_unlock(&self->mutex);
    k_sem_give(&self->tx_slots_sem);
    _RUdpIpChannel_update_state(self, NETWORK_CHANNEL_STATE_LOST_CONNECTION);
    return NETWORK_CHANNEL_RET_ERROR;
  }

  int64_t elapsed_ms = k_uptime_get() - pkt->last_send_time_ms;

  /* After any retransmit (timeout or HACK), only HACKs from newer sender progress
   * are allowed to trigger another HACK retransmit. This makes sure that pending
   * HACKs are ignored and a packet is not retransmitted multiple times due to multiple
   * HACKS from the receiver while waiting for the ACK to arrive after the first retransmission. */
  pkt->next_allowed_hack_uid = self->next_uid;

  pkt->retry_count++;
  int retry_attempt = pkt->retry_count;
  int uid = pkt->uid;
  packets_retransmitted++;

  k_mutex_unlock(&self->mutex);

  if (triggered_by_timeout) {
    RUDP_IP_CHANNEL_DEBUG("Retransmitting packet uid=%d after %d ms due to timeout (retry %d/%d)", uid, (int)elapsed_ms,
                          retry_attempt, RUDP_MAX_RETRIES);
    LOG_STAT("Retransmitting packet uid=%d after %d ms due to timeout (retry %d/%d)", uid, (int)elapsed_ms,
             retry_attempt, RUDP_MAX_RETRIES);
  } else {
    RUDP_IP_CHANNEL_DEBUG("Retransmitting packet uid=%d after %d ms due to HACK request (retry %d/%d)", uid,
                          (int)elapsed_ms, retry_attempt, RUDP_MAX_RETRIES);
    LOG_STAT("Retransmitting packet uid=%d after %d ms due to HACK request (retry %d/%d)", uid, (int)elapsed_ms,
             retry_attempt, RUDP_MAX_RETRIES);
  }

  int send_ret = _RUdpIpChannel_send_outgoing_packet(self, buffer_idx);
  if (send_ret != NETWORK_CHANNEL_RET_OK) {
    _RUdpIpChannel_update_state(self, NETWORK_CHANNEL_STATE_LOST_CONNECTION);
    return send_ret;
  }

  return NETWORK_CHANNEL_RET_OK;
}

/* Check outgoing buffer for packets that need retransmission due to timeout.
 * Called periodically from worker thread. */
static void _RUdpIpChannel_handle_retransmissions(RUdpIpChannel* self) {
  int64_t now = k_uptime_get();
  int retransmit_slot = -1;

  k_mutex_lock(&self->mutex, K_FOREVER);

  for (int i = 0; i < RUDP_OUTGOING_BUFFER_SIZE; i++) {
    RUdpOutgoingPacket* pkt = &self->outgoing_buffer[i];

    if (pkt->is_acked) {
      continue; // Already acknowledged
    }

    int64_t elapsed = now - pkt->last_send_time_ms;
    if (elapsed >= RUDP_RETRANSMIT_TIMEOUT_MS) {
      retransmit_slot = i;
      // Send one packet at a time to avoid bursts of retransmissions;
      // will check others on next invocation
      break;
    }
  }

  k_mutex_unlock(&self->mutex);

  if (retransmit_slot >= 0) {
    (void)_RUdpIpChannel_retransmit_outgoing_packet(self, retransmit_slot, true, -1);
  }
}

/**
 * Creates and manages the outgoing (F)ACK packet for a received data packet.
 */
static int _RUdpIpChannel_handle_outgoing_ack(RUdpIpChannel* self, int uid, int ack_packet_type) {
  assert(ack_packet_type == RUDP_PACKET_TYPE_ACK || ack_packet_type == RUDP_PACKET_TYPE_FACK);

  char ack_packet[9] = {0};
  ack_packet[0] = ack_packet_type & 0x0F;
  *(int*)(ack_packet + 1) = uid;
  *(int*)(ack_packet + 5) = 0; // payload length of 0 for ACK packets

  RUDP_IP_CHANNEL_DEBUG("Sending ACK for received packet; uid=%d", uid);

  ssize_t sent = send(self->fd, ack_packet, sizeof(ack_packet), 0);
  if (sent < 0) {
    RUDP_IP_CHANNEL_ERR("Failed to send ACK errno=%d", errno);
    return NETWORK_CHANNEL_RET_ERROR;
  }
  return NETWORK_CHANNEL_RET_OK;
}

/**
 * Process received ACK packet: mark the corresponding outgoing packet as acknowledged.
 */
static void _RUdpIpChannel_handle_received_ack(RUdpIpChannel* self, int uid) {
  k_mutex_lock(&self->mutex, K_FOREVER);
  int slot = _RUdpIpChannel_find_outgoing_slot_by_uid(self, uid);
  if (slot >= 0) {
    RUDP_IP_CHANNEL_DEBUG("ACK received for uid=%d, marking as acknowledged", uid);
    self->outgoing_buffer[slot].is_acked = true;
    k_mutex_unlock(&self->mutex);
    /* Signal send_blocking that a slot is now free */
    k_sem_give(&self->tx_slots_sem);
  } else {
    RUDP_IP_CHANNEL_DEBUG("Received ACK for unknown uid=%d", uid);
    k_mutex_unlock(&self->mutex);
  }
}

/**
 * Process received FACK packet: All packets until the given UID (inclusive) are acknowledged,
 * useful for acknowledging multiple packets at once and for increasing ack reliability by sending redundant ACKs.
 *
 * TODO: We might not want to loop all the way through the buffer every time we receive a FACK, especially if the buffer
 * size grows. We could optimize this by checking if there are actually unacked "holes" before doing this loop, and
 * otherwise, just handle the FACK like an ACK.
 */
static void _RUdpIpCHannel_handle_received_fack(RUdpIpChannel* self, int uid) {
  k_mutex_lock(&self->mutex, K_FOREVER);
  for (int i = 0; i < RUDP_OUTGOING_BUFFER_SIZE; i++) {
    if (!self->outgoing_buffer[i].is_acked && self->outgoing_buffer[i].uid <= uid) {
      RUDP_IP_CHANNEL_DEBUG("FACK received for uid=%d, marking as acknowledged", self->outgoing_buffer[i].uid);
      self->outgoing_buffer[i].is_acked = true;
      k_sem_give(&self->tx_slots_sem);
    }
  }
  k_mutex_unlock(&self->mutex);
}

/**
 * Process received HACK packet: peer is requesting retransmission of the specified UID.
 */
static void _RUdpIpChannel_handle_received_hack(RUdpIpChannel* self, int uid, int uid_to_retransmit) {
  /* A HACK is essentially an ACK for the received packet (uid),
   * combined with a request to retransmit another packet (uid_to_retransmit) that the receiver considers missing. */
  _RUdpIpChannel_handle_received_ack(self, uid);

  RUDP_IP_CHANNEL_DEBUG("Received HACK for packet uid=%d, requesting retransmission of uid=%d", uid, uid_to_retransmit);

#ifdef RUDP_IP_CHANNEL_LOG_STATS
  int acked_by_hole = 0;
#endif
  int slot = -1;
  k_mutex_lock(&self->mutex, K_FOREVER);

  // Find the requested packet to retransmit, and at the same time,
  // mark all older packets as acknowledged since the receiver's HACK indicates
  // that those were received.
  for (int i = 0; i < RUDP_OUTGOING_BUFFER_SIZE; i++) {
    RUdpOutgoingPacket* pkt = &self->outgoing_buffer[i];
    if (pkt->is_acked) {
      continue;
    }

    if (pkt->uid < uid_to_retransmit) {
      pkt->is_acked = true;
#ifdef RUDP_IP_CHANNEL_LOG_STATS
      acked_by_hole++;
#endif
      k_sem_give(&self->tx_slots_sem);
      continue;
    }

    if (pkt->uid == uid_to_retransmit) {
      slot = i;
    }
  }

  k_mutex_unlock(&self->mutex);

#ifdef RUDP_IP_CHANNEL_LOG_STATS
  if (acked_by_hole > 0) {
    RUDP_IP_CHANNEL_DEBUG("HACK inferred ACK for %d pending packets with uid < %d", acked_by_hole, uid_to_retransmit);
    LOG_STAT("HACK inferred ACK for %d pending packets with uid < %d", acked_by_hole, uid_to_retransmit);
  }
#endif

  if (slot >= 0) {
    (void)_RUdpIpChannel_retransmit_outgoing_packet(self, slot, false, uid);
  } else {
    RUDP_IP_CHANNEL_DEBUG("Received HACK for unknown uid=%d", uid_to_retransmit);
  }
}

/**
 * Store a received data packet in the incoming buffer and deliver in-order packets to callback.
 */
static void _RUdpIpChannel_handle_received_data(RUdpIpChannel* self, int uid, const unsigned char* payload,
                                                int payload_length) {
  if (payload_length < 0 || payload_length > RUDP_IP_CHANNEL_BUFFER_SIZE) {
    RUDP_IP_CHANNEL_ERR("Invalid payload length %d", payload_length);
    return;
  }

  if (packets_received++ % 100 == 0) {
    LOG_STAT("%u packets received, %u duplicates", packets_received, packets_received_duplicates);
  }

  RUDP_IP_CHANNEL_DEBUG("Received data packet uid=%d, payload_length=%d, next_expected=%d", uid, payload_length,
                        self->next_expected_uid);

  /* Check if this packet is older than what we've already processed (already handled by callback) */
  if (uid < self->next_expected_uid) {
    packets_received_duplicates++;
    RUDP_IP_CHANNEL_DEBUG("Received duplicate/old packet uid=%d (next_expected=%d), sending ACK and discarding", uid,
                          self->next_expected_uid);

    _RUdpIpChannel_handle_outgoing_ack(self, uid, RUDP_PACKET_TYPE_ACK);
    return;
  }

  /* Enforce receive window size: we only buffer up to RUDP_INCOMING_BUFFER_SIZE
   * packets ahead of next_expected_uid. */
  int slot_offset = uid - self->next_expected_uid;
  if (slot_offset >= RUDP_INCOMING_BUFFER_SIZE) {
    RUDP_IP_CHANNEL_WARN("Received packet with uid=%d too far ahead (next_expected=%d), discarding", uid,
                         self->next_expected_uid);
    return;
  }

  int slot_idx = uid % RUDP_INCOMING_BUFFER_SIZE;

  /* Check if we already have this packet (but perhaps not yet handled by callback) */
  if (self->incoming_buffer[slot_idx].is_filled && self->incoming_buffer[slot_idx].uid == uid) {
    RUDP_IP_CHANNEL_DEBUG("Received duplicate packet uid=%d", uid);
    packets_received_duplicates++;

    _RUdpIpChannel_handle_outgoing_ack(self, uid, RUDP_PACKET_TYPE_ACK);
    return;
  }

  /* Store the packet */
  RUdpIncomingPacket* pkt = &self->incoming_buffer[slot_idx];
  pkt->uid = uid;
  pkt->data_length = payload_length;
  pkt->is_filled = true;
  memcpy(pkt->payload_data, payload, (size_t)payload_length);

  RUDP_IP_CHANNEL_DEBUG("Stored data packet in buffer slot %d", slot_idx);

  int pending_uid;
  bool no_holes_before_uid = true;
  for (pending_uid = self->next_expected_uid; pending_uid < uid; pending_uid++) {
    int pending_slot = pending_uid % RUDP_INCOMING_BUFFER_SIZE;
    RUdpIncomingPacket* pending_pkt = &self->incoming_buffer[pending_slot];
    if (!pending_pkt->is_filled || pending_pkt->uid != pending_uid) {
      no_holes_before_uid = false;
      break;
    }
  }

  // FACK only when all previous packets are present.
  if (no_holes_before_uid) {
    _RUdpIpChannel_handle_outgoing_ack(self, uid, RUDP_PACKET_TYPE_FACK);
  } else {
#if 0 /* send regular ACK */
    /* Send ACK immediately */
    _RUdpIpChannel_handle_outgoing_ack(self, uid, RUDP_PACKET_TYPE_ACK);
#else /* send advanced HACK */
    unsigned char hack_packet[9] = {0};
    hack_packet[0] = RUDP_PACKET_TYPE_HACK & 0x0F;
    *(int*)(hack_packet + 1) = uid;
    *(int*)(hack_packet + 5) = pending_uid;

    RUDP_IP_CHANNEL_DEBUG("Sending HACK for received packet uid=%d, next_expected_uid=%d", uid,
                          self->next_expected_uid);

    ssize_t sent = send(self->fd, hack_packet, sizeof(hack_packet), 0);
    if (sent < 0) {
      RUDP_IP_CHANNEL_ERR("Failed to send HACK errno=%d", errno);
      _RUdpIpChannel_update_state(self, NETWORK_CHANNEL_STATE_LOST_CONNECTION);
    }
#endif
  }

  /* Deliver contiguous packets. */
  while (true) {
    int expected_slot = self->next_expected_uid % RUDP_INCOMING_BUFFER_SIZE;
    RUdpIncomingPacket* front = &self->incoming_buffer[expected_slot];
    if (!front->is_filled || front->uid != self->next_expected_uid) {
      break;
    }

    if (self->receive_callback != NULL) {
      RUDP_IP_CHANNEL_DEBUG("Delivering packet uid=%d to callback, payload_length=%d", front->uid, front->data_length);
      if (self->federated_connection == NULL) {
        RUDP_IP_CHANNEL_WARN("Received %d bytes but no federated connection is registered", front->data_length);
      } else if (deserialize_from_protobuf(&self->output, front->payload_data, (size_t)front->data_length) == LF_OK) {
        self->receive_callback(self->federated_connection, &self->output);
      } else {
        RUDP_IP_CHANNEL_WARN("Failed to deserialize incoming RUDP payload uid=%d", front->uid);
      }
    } else {
      RUDP_IP_CHANNEL_WARN("Received %d bytes but no callback is registered", front->data_length);
    }

    front->is_filled = false;
    front->data_length = 0;
    front->uid = -1;

    self->next_expected_uid++;
  }
}

static lf_ret_t RUdpIpChannel_send_blocking(NetworkChannel* untyped_self, const FederateMessage* message) {
  RUdpIpChannel* self = (RUdpIpChannel*)untyped_self;
  if (message == NULL) {
    RUDP_IP_CHANNEL_ERR("Invalid RUDP send buffer");
    return NETWORK_CHANNEL_RET_ERROR;
  }

  int data_length = serialize_to_protobuf(message, self->write_buffer, sizeof(self->write_buffer));
  if (data_length < 0 || data_length > RUDP_IP_CHANNEL_BUFFER_SIZE) {
    RUDP_IP_CHANNEL_ERR("Failed to serialize outgoing RUDP message");
    return NETWORK_CHANNEL_RET_ERROR;
  }

  if (_RUdpIpChannel_get_state(self) != NETWORK_CHANNEL_STATE_CONNECTED || self->fd < 0) {
    return NETWORK_CHANNEL_RET_ERROR;
  }

  /* Block until a TX slot is free — backpressure is handled entirely inside the channel.
   * k_sem_take yields the thread so the worker can process ACKs while we wait. */
  if (k_sem_take(&self->tx_slots_sem, K_FOREVER) != 0) {
    return NETWORK_CHANNEL_RET_ERROR;
  }

  k_mutex_lock(&self->mutex, K_FOREVER);
  int slot = _RUdpIpChannel_find_empty_outgoing_slot(self);
  if (slot < 0) {
    /* Should not happen if semaphore is correct, but be safe */
    k_mutex_unlock(&self->mutex);
    k_sem_give(&self->tx_slots_sem);
    return NETWORK_CHANNEL_RET_ERROR;
  }

  RUdpOutgoingPacket* pkt = &self->outgoing_buffer[slot];

  /* Serialize packet: [type][uid:4][length:4][payload] */
  pkt->packet_data[0] = RUDP_PACKET_TYPE_DATA;
  *(int*)(pkt->packet_data + 1) = self->next_uid;
  *(int*)(pkt->packet_data + 5) = data_length;
  memcpy(pkt->packet_data + 9, self->write_buffer, (size_t)data_length);

  pkt->packet_length = 9 + data_length;
  pkt->uid = self->next_uid;
  pkt->is_acked = false;
  pkt->retry_count = 0;
  pkt->next_allowed_hack_uid = -1;
  pkt->last_send_time_ms = k_uptime_get();

  RUDP_IP_CHANNEL_DEBUG("Queuing data packet uid=%d, data_length=%d", self->next_uid, data_length);
  self->next_uid++;

  k_mutex_unlock(&self->mutex);

  /* Send the packet immediately */
  int send_result = _RUdpIpChannel_send_outgoing_packet(self, slot);
  if (send_result != NETWORK_CHANNEL_RET_OK) {
    k_mutex_lock(&self->mutex, K_FOREVER);
    pkt->is_acked = true; // Free the slot on error
    k_mutex_unlock(&self->mutex);
    k_sem_give(&self->tx_slots_sem); // Return the slot count
    return send_result;
  }

  return NETWORK_CHANNEL_RET_OK;
}

static void RUdpIpChannel_register_receive_callback(NetworkChannel* untyped_self,
                                                    void (*receive_callback)(FederatedConnectionBundle* conn,
                                                                             const FederateMessage* message),
                                                    FederatedConnectionBundle* conn) {
  RUdpIpChannel* self = (RUdpIpChannel*)untyped_self;
  RUDP_IP_CHANNEL_DEBUG("Register receive callback");
  self->federated_connection = conn;
  self->receive_callback = receive_callback;
}

static void RUdpIpChannel_free(NetworkChannel* untyped_self) {
  RUdpIpChannel* self = (RUdpIpChannel*)untyped_self;
  RUDP_IP_CHANNEL_DEBUG("Free");

  if (!self->worker_thread_started) {
    RUdpIpChannel_close_connection(untyped_self);
    return;
  }

  self->worker_thread_started = false;

  k_thread_abort(self->worker_thread_id);
  int join_err = k_thread_join(&self->worker_thread, K_FOREVER);
  if (join_err != 0) {
    RUDP_IP_CHANNEL_ERR("Error joining worker thread %d", join_err);
  }
  self->worker_thread_id = NULL;

  RUdpIpChannel_close_connection(untyped_self);
}

static void _RUdpIpChannel_handle_connected(RUdpIpChannel* self) {
  /* Blocking recv: returns immediately on packet arrival, or after SO_RCVTIMEO
   * (= RUDP_RETRANSMIT_TIMEOUT_MS) when quiet, which triggers retransmission checks.
   * TODO: Notice that this means that retransmissions happen RUDP_RETRANSMIT_TIMEOUT_MS after the original
   *       send at the earliest. Worst-case, we wait up to 2*RUDP_RETRANSMIT_TIMEOUT_MS for a retransmission
   *       because we only check for retransmissions after recv returns. To improve this, we could use a timerfd or
   *       eventfd to wake up the thread immediately when a retransmission timeout occurs, instead of waiting for the
   * next recv timeout. For now, we keep it simple with just SO_RCV TIMEO and accept the potential extra delay in
   * retransmissions.
   */
  ssize_t bytes_received = recv(self->fd, self->read_buffer, sizeof(self->read_buffer), 0);
  if (bytes_received < 0) {
    switch (errno) {
    case EAGAIN:
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EAGAIN)
    case EWOULDBLOCK:
#endif
    case EINTR:
      /* Non-blocking timeout, just continue */
      break;
    default:
      RUDP_IP_CHANNEL_ERR("RUDP recv failed errno=%d", errno);
      _RUdpIpChannel_update_state(self, NETWORK_CHANNEL_STATE_LOST_CONNECTION);
      break;
    }
    return;
  }

  if (bytes_received == 0) {
    RUDP_IP_CHANNEL_WARN("RUDP peer became unavailable");
    _RUdpIpChannel_update_state(self, NETWORK_CHANNEL_STATE_LOST_CONNECTION);
    return;
  }

  if (bytes_received < 5) {
    RUDP_IP_CHANNEL_WARN("Received packet too short (%d bytes), discarding", (int)bytes_received);
    return;
  }

  int uid = _RUdpIpChannel_protocol_extract_uid(self->read_buffer, (size_t)bytes_received);

  if (_RUdpIpChannel_is_ack_packet(self->read_buffer, (size_t)bytes_received)) {
    _RUdpIpChannel_handle_received_ack(self, uid);
    return;
  }

  if (_RUdpIpChannel_is_fack_packet(self->read_buffer, (size_t)bytes_received)) {
    _RUdpIpCHannel_handle_received_fack(self, uid);
    return;
  }

  if (_RUdpIpChannel_is_hack_packet(self->read_buffer, (size_t)bytes_received)) {
    int uid_to_retransmit = *(int*)(self->read_buffer + 5);
    _RUdpIpChannel_handle_received_hack(self, uid, uid_to_retransmit);
    return;
  }

  /* This is a data packet */
  int payload_length = (int)bytes_received - 9;
  if (payload_length < 0) {
    RUDP_IP_CHANNEL_WARN("Invalid data packet length %d", (int)bytes_received);
    return;
  }

  _RUdpIpChannel_handle_received_data(self, uid, self->read_buffer + 9, payload_length);
}

static void _RUdpIpChannel_worker_thread(void* p1, void* p2, void* p3) {
  (void)p2;
  (void)p3;
  RUdpIpChannel* self = (RUdpIpChannel*)p1;

  RUDP_IP_CHANNEL_DEBUG("Starting worker thread");

  while (true) {
    switch (_RUdpIpChannel_get_state(self)) {
    case NETWORK_CHANNEL_STATE_OPEN:
      if (_RUdpIpChannel_connect(self) == NETWORK_CHANNEL_RET_OK) {
        _RUdpIpChannel_update_state(self, NETWORK_CHANNEL_STATE_CONNECTED);
      } else {
        _RUdpIpChannel_update_state(self, NETWORK_CHANNEL_STATE_CONNECTION_FAILED);
        k_msleep(100);
      }
      break;

    case NETWORK_CHANNEL_STATE_CONNECTION_FAILED:
    case NETWORK_CHANNEL_STATE_LOST_CONNECTION:
      RUDP_IP_CHANNEL_WARN("RUDP channel lost connection, retrying");
      _RUdpIpChannel_close_socket(self);
      _RUdpIpChannel_update_state(self, NETWORK_CHANNEL_STATE_OPEN);
      k_msleep(100);
      break;

    case NETWORK_CHANNEL_STATE_CONNECTED: {
      /* Check for retransmissions periodically */
      _RUdpIpChannel_handle_retransmissions(self);

      /* Blocking recv (with SO_RCVTIMEO) handles incoming packets and wakes up
       * periodically to allow retransmission checks — no sleep needed here. */
      _RUdpIpChannel_handle_connected(self);
      break;
    }
    case NETWORK_CHANNEL_STATE_CONNECTION_IN_PROGRESS:
      // TODO: We don't have a real "connection in progress" state with UDP. We should add a handshake-based
      // connection establishment to move to this state, and and handle this case properly. For now, we don't
      // expect to reach this state. Arbitrarily sleep here to avoid busy loop if we do reach it.
      k_msleep(10);
      break;
    case NETWORK_CHANNEL_STATE_UNINITIALIZED:
    case NETWORK_CHANNEL_STATE_CLOSED:
      k_msleep(10);
      break;
    }
  }
}

void RUdpIpChannel_ctor(RUdpIpChannel* self, const char* local_host, unsigned short local_port, const char* remote_host,
                        unsigned short remote_port, int protocol_family) {
  assert(self != NULL);
  assert(local_host != NULL);
  assert(remote_host != NULL);

  memset(self, 0, sizeof(*self));
  self->super.mode = NETWORK_CHANNEL_MODE_ASYNC;
  self->super.type = NETWORK_CHANNEL_TYPE_UDP_IP;
  self->super.expected_connect_duration = MSEC(100);
  self->super.is_connected = RUdpIpChannel_is_connected;
  self->super.open_connection = RUdpIpChannel_open_connection;
  self->super.close_connection = RUdpIpChannel_close_connection;
  self->super.send_blocking = RUdpIpChannel_send_blocking;
  self->super.register_receive_callback = RUdpIpChannel_register_receive_callback;
  self->super.free = RUdpIpChannel_free;

  self->fd = -1;
  self->state = NETWORK_CHANNEL_STATE_UNINITIALIZED;
  self->local_host = local_host;
  self->local_port = local_port;
  self->remote_host = remote_host;
  self->remote_port = remote_port;
  self->protocol_family = protocol_family;
  self->federated_connection = NULL;
  self->receive_callback = NULL;
  self->worker_thread_started = false;
  self->worker_thread_id = NULL;

  /* Initialize buffers */
  for (int i = 0; i < RUDP_OUTGOING_BUFFER_SIZE; i++) {
    self->outgoing_buffer[i].is_acked = true;
    self->outgoing_buffer[i].retry_count = 0;
    self->outgoing_buffer[i].uid = -1;
    self->outgoing_buffer[i].next_allowed_hack_uid = -1;
  }
  self->next_uid = 0;

  for (int i = 0; i < RUDP_INCOMING_BUFFER_SIZE; i++) {
    self->incoming_buffer[i].is_filled = false;
    self->incoming_buffer[i].uid = -1;
    self->incoming_buffer[i].data_length = 0;
  }
  self->next_expected_uid = 0;

  int mutex_err = k_mutex_init(&self->mutex);
  if (mutex_err != 0) {
    RUDP_IP_CHANNEL_ERR("Failed to initialize channel mutex: %d", mutex_err);
    return;
  }

  k_sem_init(&self->tx_slots_sem, RUDP_OUTGOING_BUFFER_SIZE, RUDP_OUTGOING_BUFFER_SIZE);

  RUDP_IP_CHANNEL_DEBUG("Configured RUDP channel local=%s:%u remote=%s:%u", self->local_host, self->local_port,
                        self->remote_host, self->remote_port);
  _RUdpIpChannel_spawn_worker_thread(self);
}
