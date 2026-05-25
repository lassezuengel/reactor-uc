#ifndef REACTOR_UC_CLOCK_SYNCHRONIZATION_EXTERNAL_H
#define REACTOR_UC_CLOCK_SYNCHRONIZATION_EXTERNAL_H

#include <stdint.h>

#include "reactor-uc/event.h"
#include "reactor-uc/tag.h"

typedef struct Environment Environment;

/**
 * @brief API that the external clock synchronization mechanism needs to implement.
 *
 * The API implementation is provided by the user of the clock synchronization module
 * and is called by the module to perform the actual clock synchronization logic.
 *
 * Currently, it's completely separate from the LF federation structure, meaning that
 * we do not use local neighborhoods, especially no connection bundles. This allows
 * to implement more general synchronization mechanisms like glossy network flooding
 * and other mechanisms based on concurrent transmissions (CT), which do not fit well
 * into the LF federation structure.
 */
typedef struct {
  // Initialize the external clock synchronization mechanism. The parameters are passed to
  // the API implementation and may be used to configure it.
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


/**
 * @brief Clock synchronization module that uses an external API to perform clock synchronization.
 *
 * This module is a normal system event handler, which schedules its own events to perform clock
 * synchronization at the times determined by the API. When the scheduled event is handled, it
 * calls the API's schedule function to perform one round of clock synchronization and schedule
 * the next sync event.
 */
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
