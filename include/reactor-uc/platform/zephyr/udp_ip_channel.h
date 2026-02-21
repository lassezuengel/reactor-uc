#ifndef REACTOR_UC_UDP_IP_CHANNEL_H
#define REACTOR_UC_UDP_IP_CHANNEL_H

#include <nanopb/pb.h>
#include <pthread.h>
#include <zephyr/kernel.h>

#include "proto/message.pb.h"
#include "reactor-uc/error.h"
#include "reactor-uc/network_channel.h"

#define UDP_IP_CHANNEL_EXPECTED_CONNECT_DURATION MSEC(0)
#define UDP_IP_CHANNEL_BUFFERSIZE 1024

#ifndef UDP_IP_CHANNEL_RECV_THREAD_STACK_SIZE
#define UDP_IP_CHANNEL_RECV_THREAD_STACK_SIZE 2048
#endif

typedef struct UdpIpChannel UdpIpChannel;
typedef struct FederatedConnectionBundle FederatedConnectionBundle;

struct UdpIpChannel {
  NetworkChannel super;

  int fd;
  NetworkChannelState state;
  pthread_mutex_t mutex;

  const char* local_host;
  unsigned short local_port;
  const char* remote_host;
  unsigned short remote_port;
  int protocol_family;

  bool worker_thread_started;
  struct k_thread worker_thread;
  k_tid_t worker_thread_id;
  K_KERNEL_STACK_MEMBER(worker_thread_stack, UDP_IP_CHANNEL_RECV_THREAD_STACK_SIZE);

  FederateMessage output;
  unsigned char write_buffer[UDP_IP_CHANNEL_BUFFERSIZE];
  unsigned char read_buffer[UDP_IP_CHANNEL_BUFFERSIZE];

  FederatedConnectionBundle* federated_connection;
  void (*receive_callback)(FederatedConnectionBundle* conn, const FederateMessage* message);
};

void UdpIpChannel_ctor(UdpIpChannel* self, const char* local_host, unsigned short local_port, const char* remote_host,
                       unsigned short remote_port, int protocol_family);

#endif
