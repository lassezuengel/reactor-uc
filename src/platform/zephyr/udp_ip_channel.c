#include "reactor-uc/platform/zephyr/udp_ip_channel.h"

#include "reactor-uc/federated.h"
#include "reactor-uc/logging.h"
#include "reactor-uc/serialization.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define UDP_IP_CHANNEL_ERR(fmt, ...) LF_ERR(NET, "UdpIpChannel: " fmt, ##__VA_ARGS__)
#define UDP_IP_CHANNEL_WARN(fmt, ...) LF_WARN(NET, "UdpIpChannel: " fmt, ##__VA_ARGS__)
#define UDP_IP_CHANNEL_DEBUG(fmt, ...) LF_DEBUG(NET, "UdpIpChannel: " fmt, ##__VA_ARGS__)

#define UDP_IP_CHANNEL_ZEPHYR_THREAD_PRIORITY K_PRIO_PREEMPT(0)

static void _UdpIpChannel_worker_main(UdpIpChannel* self);
static void _UdpIpChannel_worker_thread(void* p1, void* p2, void* p3);

static lf_ret_t _UdpIpChannel_fill_sockaddr(const char* host, unsigned short port, int protocol_family,
                                            struct sockaddr_storage* storage, socklen_t* addrlen) {
  memset(storage, 0, sizeof(*storage));

  switch (protocol_family) {
  case AF_INET: {
    struct sockaddr_in* addr4 = (struct sockaddr_in*)storage;
    addr4->sin_family = AF_INET;
    addr4->sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr4->sin_addr) <= 0) {
      return LF_INVALID_VALUE;
    }
    *addrlen = sizeof(struct sockaddr_in);
    return LF_OK;
  }
  case AF_INET6: {
    struct sockaddr_in6* addr6 = (struct sockaddr_in6*)storage;
    addr6->sin6_family = AF_INET6;
    addr6->sin6_port = htons(port);
    if (inet_pton(AF_INET6, host, &addr6->sin6_addr) <= 0) {
      return LF_INVALID_VALUE;
    }
    *addrlen = sizeof(struct sockaddr_in6);
    return LF_OK;
  }
  default:
    return LF_INVALID_VALUE;
  }
}

static void _UdpIpChannel_set_state(UdpIpChannel* self, NetworkChannelState state) {
  pthread_mutex_lock(&self->mutex);
  self->state = state;
  pthread_mutex_unlock(&self->mutex);
}

static NetworkChannelState _UdpIpChannel_get_state(UdpIpChannel* self) {
  NetworkChannelState state;
  pthread_mutex_lock(&self->mutex);
  state = self->state;
  pthread_mutex_unlock(&self->mutex);
  return state;
}

static bool UdpIpChannel_is_connected(NetworkChannel* untyped_self) {
  UdpIpChannel* self = (UdpIpChannel*)untyped_self;
  return _UdpIpChannel_get_state(self) == NETWORK_CHANNEL_STATE_CONNECTED;
}

static void _UdpIpChannel_spawn_worker_thread(UdpIpChannel* self) {
  self->worker_thread_id =
      k_thread_create(&self->worker_thread, self->worker_thread_stack, K_KERNEL_STACK_SIZEOF(self->worker_thread_stack),
                      _UdpIpChannel_worker_thread, self, NULL, NULL, UDP_IP_CHANNEL_ZEPHYR_THREAD_PRIORITY, 0,
                      K_NO_WAIT);
  validate(self->worker_thread_id != NULL);
  (void)k_thread_name_set(self->worker_thread_id, "lf_udpip_rx");
  self->worker_thread_started = true;
}

static lf_ret_t UdpIpChannel_open_connection(NetworkChannel* untyped_self) {
  UdpIpChannel* self = (UdpIpChannel*)untyped_self;

  struct sockaddr_storage local_addr;
  socklen_t local_addrlen = 0;
  if (_UdpIpChannel_fill_sockaddr(self->local_host, self->local_port, self->protocol_family, &local_addr,
                                  &local_addrlen) != LF_OK) {
    UDP_IP_CHANNEL_ERR("Invalid local endpoint %s:%u", self->local_host, self->local_port);
    _UdpIpChannel_set_state(self, NETWORK_CHANNEL_STATE_CONNECTION_FAILED);
    return LF_INVALID_VALUE;
  }

  struct sockaddr_storage remote_addr;
  socklen_t remote_addrlen = 0;
  if (_UdpIpChannel_fill_sockaddr(self->remote_host, self->remote_port, self->protocol_family, &remote_addr,
                                  &remote_addrlen) != LF_OK) {
    UDP_IP_CHANNEL_ERR("Invalid remote endpoint %s:%u", self->remote_host, self->remote_port);
    _UdpIpChannel_set_state(self, NETWORK_CHANNEL_STATE_CONNECTION_FAILED);
    return LF_INVALID_VALUE;
  }

  if (self->fd > 0) {
    close(self->fd);
  }

  self->fd = socket(self->protocol_family, SOCK_DGRAM, 0);
  if (self->fd < 0) {
    UDP_IP_CHANNEL_ERR("Failed to create UDP socket errno=%d", errno);
    _UdpIpChannel_set_state(self, NETWORK_CHANNEL_STATE_CONNECTION_FAILED);
    return LF_ERR;
  }

  if (bind(self->fd, (struct sockaddr*)&local_addr, local_addrlen) < 0) {
    UDP_IP_CHANNEL_ERR("Failed to bind UDP socket to %s:%u errno=%d", self->local_host, self->local_port, errno);
    close(self->fd);
    self->fd = -1;
    _UdpIpChannel_set_state(self, NETWORK_CHANNEL_STATE_CONNECTION_FAILED);
    return LF_ERR;
  }

  if (connect(self->fd, (struct sockaddr*)&remote_addr, remote_addrlen) < 0) {
    UDP_IP_CHANNEL_ERR("Failed to connect UDP socket to %s:%u errno=%d", self->remote_host, self->remote_port, errno);
    close(self->fd);
    self->fd = -1;
    _UdpIpChannel_set_state(self, NETWORK_CHANNEL_STATE_CONNECTION_FAILED);
    return LF_ERR;
  }

  _UdpIpChannel_set_state(self, NETWORK_CHANNEL_STATE_CONNECTED);
  if (!self->worker_thread_started) {
    _UdpIpChannel_spawn_worker_thread(self);
  }
  return LF_OK;
}

static void UdpIpChannel_close_connection(NetworkChannel* untyped_self) {
  UdpIpChannel* self = (UdpIpChannel*)untyped_self;
  _UdpIpChannel_set_state(self, NETWORK_CHANNEL_STATE_CLOSED);
  if (self->fd > 0) {
    shutdown(self->fd, SHUT_RDWR);
    close(self->fd);
    self->fd = -1;
  }
}

static lf_ret_t UdpIpChannel_send_blocking(NetworkChannel* untyped_self, const FederateMessage* message) {
  UdpIpChannel* self = (UdpIpChannel*)untyped_self;
  if (_UdpIpChannel_get_state(self) != NETWORK_CHANNEL_STATE_CONNECTED) {
    return LF_ERR;
  }

  int msg_size = serialize_to_protobuf(message, self->write_buffer, UDP_IP_CHANNEL_BUFFERSIZE);
  if (msg_size < 0) {
    UDP_IP_CHANNEL_ERR("Failed to serialize outgoing message");
    return LF_ERR;
  }

  ssize_t n = send(self->fd, self->write_buffer, msg_size, 0);
  if (n != msg_size) {
    UDP_IP_CHANNEL_WARN("Failed to send UDP message errno=%d", errno);
    _UdpIpChannel_set_state(self, NETWORK_CHANNEL_STATE_LOST_CONNECTION);
    return LF_ERR;
  }
  return LF_OK;
}

static void UdpIpChannel_register_receive_callback(NetworkChannel* untyped_self,
                                                   void (*receive_callback)(FederatedConnectionBundle* conn,
                                                                            const FederateMessage* message),
                                                   FederatedConnectionBundle* conn) {
  UdpIpChannel* self = (UdpIpChannel*)untyped_self;
  self->federated_connection = conn;
  self->receive_callback = receive_callback;
}

static void UdpIpChannel_free(NetworkChannel* untyped_self) {
  UdpIpChannel* self = (UdpIpChannel*)untyped_self;
  UdpIpChannel_close_connection(untyped_self);
  if (self->worker_thread_started && self->worker_thread_id != NULL) {
    k_thread_abort(self->worker_thread_id);
  }
}

static void _UdpIpChannel_worker_main(UdpIpChannel* self) {
  for (;;) {
    NetworkChannelState state = _UdpIpChannel_get_state(self);
    if (state == NETWORK_CHANNEL_STATE_CLOSED) {
      break;
    }

    if (state != NETWORK_CHANNEL_STATE_CONNECTED || self->fd < 0) {
      k_sleep(K_MSEC(10));
      continue;
    }

    ssize_t bytes = recv(self->fd, self->read_buffer, sizeof(self->read_buffer), 0);
    if (bytes <= 0) {
      if (_UdpIpChannel_get_state(self) != NETWORK_CHANNEL_STATE_CLOSED) {
        _UdpIpChannel_set_state(self, NETWORK_CHANNEL_STATE_LOST_CONNECTION);
      }
      continue;
    }

    if (deserialize_from_protobuf(&self->output, self->read_buffer, (size_t)bytes) == LF_OK) {
      if (self->receive_callback != NULL && self->federated_connection != NULL) {
        self->receive_callback(self->federated_connection, &self->output);
      }
    } else {
      UDP_IP_CHANNEL_WARN("Failed to deserialize incoming UDP message");
    }
  }
}

static void _UdpIpChannel_worker_thread(void* p1, void* p2, void* p3) {
  (void)p2;
  (void)p3;
  UdpIpChannel* self = (UdpIpChannel*)p1;
  _UdpIpChannel_worker_main(self);
}

void UdpIpChannel_ctor(UdpIpChannel* self, const char* local_host, unsigned short local_port, const char* remote_host,
                       unsigned short remote_port, int protocol_family) {
  memset(self, 0, sizeof(*self));
  self->super.mode = NETWORK_CHANNEL_MODE_ASYNC;
  self->super.type = NETWORK_CHANNEL_TYPE_UDP_IP;
  self->super.expected_connect_duration = UDP_IP_CHANNEL_EXPECTED_CONNECT_DURATION;
  self->super.is_connected = UdpIpChannel_is_connected;
  self->super.open_connection = UdpIpChannel_open_connection;
  self->super.close_connection = UdpIpChannel_close_connection;
  self->super.send_blocking = UdpIpChannel_send_blocking;
  self->super.register_receive_callback = UdpIpChannel_register_receive_callback;
  self->super.free = UdpIpChannel_free;

  self->fd = -1;
  self->state = NETWORK_CHANNEL_STATE_OPEN;
  self->local_host = local_host;
  self->local_port = local_port;
  self->remote_host = remote_host;
  self->remote_port = remote_port;
  self->protocol_family = protocol_family;
  self->worker_thread_started = false;
  validate(pthread_mutex_init(&self->mutex, NULL) == 0);

  UDP_IP_CHANNEL_DEBUG("Configured UDP channel local=%s:%u remote=%s:%u", self->local_host, self->local_port,
                       self->remote_host, self->remote_port);
}
