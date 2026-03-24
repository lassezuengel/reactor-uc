#ifndef R_UDP_IP_CHANNEL_H
#define R_UDP_IP_CHANNEL_H

#include <zephyr/kernel.h>
#include <nanopb/pb.h>
#include <pthread.h>

#include "proto/message.pb.h"
#include "reactor-uc/error.h"
#include "reactor-uc/network_channel.h"

#define RUDP_IP_CHANNEL_BUFFER_SIZE 1024
#define RUDP_OUTGOING_BUFFER_SIZE 16
#define RUDP_INCOMING_BUFFER_SIZE 32
#define RUDP_RETRANSMIT_TIMEOUT_MS 50
#define RUDP_MAX_RETRIES 5

#ifndef RUDP_IP_CHANNEL_RECV_THREAD_STACK_SIZE
#define RUDP_IP_CHANNEL_RECV_THREAD_STACK_SIZE 2048
#endif

typedef struct RUdpIpChannel RUdpIpChannel;
typedef struct FederatedConnectionBundle FederatedConnectionBundle;

/**
 * Outgoing packet buffer entry: tracks packets sent but not yet acknowledged.
 */
typedef struct {
  unsigned char packet_data[9 + RUDP_IP_CHANNEL_BUFFER_SIZE];
  int packet_length;
  int uid;
  bool is_acked;
  int64_t last_send_time_ms;
  int retry_count;
  int next_allowed_hack_uid;
} RUdpOutgoingPacket;

/**
 * Incoming packet buffer entry: stores received packets to handle out-of-order delivery.
 */
typedef struct {
  unsigned char payload_data[RUDP_IP_CHANNEL_BUFFER_SIZE];
  int data_length;
  int uid;
  bool is_filled;
} RUdpIncomingPacket;

struct RUdpIpChannel {
  NetworkChannel super;

  int fd;
  NetworkChannelState state;
  struct k_mutex mutex;

  const char* local_host;
  unsigned short local_port;
  const char* remote_host;
  unsigned short remote_port;
  int protocol_family;
  bool is_client_role;

  bool worker_thread_started;
  struct k_thread worker_thread;
  k_tid_t worker_thread_id;
  K_KERNEL_STACK_MEMBER(worker_thread_stack, RUDP_IP_CHANNEL_RECV_THREAD_STACK_SIZE);

  /* TX slot semaphore: counts free outgoing slots, used to block send_blocking when window is full */
  struct k_sem tx_slots_sem;

  /* Packet reception and transmission buffers */
  RUdpOutgoingPacket outgoing_buffer[RUDP_OUTGOING_BUFFER_SIZE];
  int next_uid;

  /* Ring buffer for incoming packets */
  RUdpIncomingPacket incoming_buffer[RUDP_INCOMING_BUFFER_SIZE];
  int next_expected_uid;

  FederateMessage output;
  unsigned char write_buffer[RUDP_IP_CHANNEL_BUFFER_SIZE];

  unsigned char read_buffer[9 + RUDP_IP_CHANNEL_BUFFER_SIZE];

  FederatedConnectionBundle* federated_connection;
  void (*receive_callback)(FederatedConnectionBundle* conn, const FederateMessage* message);

  /* Handshake state: client sends HELLO, server replies with HELLO_ACK. */
  bool handshake_hello_sent;
  bool handshake_hello_acked;
  bool handshake_ready_sent;
  bool handshake_ready_acked;
  int64_t handshake_last_hello_send_time_ms;
  int64_t handshake_last_ready_send_time_ms;
  int handshake_hello_retry_count;
  int handshake_ready_retry_count;
};

void RUdpIpChannel_ctor(RUdpIpChannel* self, const char* local_host, unsigned short local_port, const char* remote_host,
                        unsigned short remote_port, int protocol_family, bool is_client_role);

#endif // R_UDP_IP_CHANNEL_H
