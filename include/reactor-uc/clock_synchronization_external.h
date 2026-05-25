#ifndef REACTOR_UC_CLOCK_SYNCHRONIZATION_EXTERNAL_H
#define REACTOR_UC_CLOCK_SYNCHRONIZATION_EXTERNAL_H

#include <stdint.h>

#include "reactor-uc/event.h"
#include "reactor-uc/tag.h"

typedef struct Environment Environment;

typedef struct {
  // Initialize the external clock synchronization mechanism. The parameters are passed to the API implementation
  // and may be used to configure it.
  int (*init)(bool grandmaster, int federate_id);
  // Schedule the next clock synchronization event. The API implementation should compute the next time to run the sync
  // and the clock offset to apply (if this is not the grandmaster) and return them through the output parameters.
  // The return value should be used to indicate whether the sync was successful (0) or not (<0).
  // If the schedule function returns a non-zero value, a warning will be logged, but the program will continue to run
  // and attempt to schedule the next sync event at the next scheduled sync time.
  //
  // The `next_sync_run_ms` is always expected to be mutated to the next scheduled sync time, even if the schedule function
  // returns a non-zero value, tand the `clock_offset_ms` is expected to be mutated to the clock offset to apply for the next
  // sync if the call succeeded (returned 0).
  int (*schedule)(int64_t* next_sync_run_ms, int64_t* clock_offset_ms);
} ExternalClockSyncApi;

typedef struct {
  int message_type;
} ExternalClockSyncEvent;

typedef struct ClockSynchronizationExternal {
  SystemEventHandler super;
  Environment* env;
  const ExternalClockSyncApi* api;
  bool is_grandmaster;
  int federate_id;
} ClockSynchronizationExternal;

void ClockSynchronizationExternal_ctor(ClockSynchronizationExternal* self, Environment* env,
                                       const ExternalClockSyncApi* api, bool is_grandmaster, int federate_id,
                                       size_t payload_size, void* payload_buf, bool* payload_used_buf,
                                       size_t payload_buf_capacity);

#endif // REACTOR_UC_CLOCK_SYNCHRONIZATION_EXTERNAL_H
