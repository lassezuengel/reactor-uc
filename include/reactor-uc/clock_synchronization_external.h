#ifndef REACTOR_UC_CLOCK_SYNCHRONIZATION_EXTERNAL_H
#define REACTOR_UC_CLOCK_SYNCHRONIZATION_EXTERNAL_H

#include "reactor-uc/event.h"
#include "reactor-uc/tag.h"

typedef struct Environment Environment;

typedef struct {
  int (*init)(void);
  int (*schedule)(void);
} ExternalClockSyncApi;

typedef struct {
  int message_type;
} ExternalClockSyncEvent;

typedef struct ClockSynchronizationExternal {
  SystemEventHandler super;
  Environment* env;
  const ExternalClockSyncApi* api;
} ClockSynchronizationExternal;

void ClockSynchronizationExternal_ctor(ClockSynchronizationExternal* self, Environment* env,
                                       const ExternalClockSyncApi* api, size_t payload_size, void* payload_buf,
                                       bool* payload_used_buf, size_t payload_buf_capacity);

#endif // REACTOR_UC_CLOCK_SYNCHRONIZATION_EXTERNAL_H
