// TODO: As this module is pretty much a singleton, we probably don't need to pass opaque
// `user_data` to the external implementation. Doesn't really hurt, though.
// TODO: Currently, this external clock sync mechanism is applied to the whole federation.
// This means that all federates have to be able to communicate and sync via the external mechanism,
// which may not be the case in heterogeneous deployments where only a subset of federates have the
// necessary connectivity (e.g., Glossy). We need to consider how to support partial deployment of
// the external clock sync mechanism, where only a subset of federates use it and the others use either
// yet another external clock sync mechanism or the LF-supplied mechanism.

#ifndef REACTOR_UC_CLOCK_SYNCHRONIZATION_EXTERNAL_H
#define REACTOR_UC_CLOCK_SYNCHRONIZATION_EXTERNAL_H

#include <stdint.h>

#include "reactor-uc/event.h"
#include "reactor-uc/tag.h"

typedef struct Environment Environment;
typedef struct ClockSynchronizationExternal ClockSynchronizationExternal;

/**
 * @brief Callback used by external clock synchronization implementations to report a completed sync run.
 *
 * The callback may be invoked synchronously from `ExternalClockSyncApi.schedule` or asynchronously
 * from another thread, interrupt handler, or worker queue, depending on the platform and external
 * implementation. The callback only schedules a reactor-uc system event. Clock stepping and scheduler
 * adjustment are performed later from the runtime context.
 *
 * The external implementation does not (need to) know about LF's time base and clock stepping.
 * It reports the next sync run time in its own local platform-clock domain (which LF must also be based upon,
 * see specific platform implementations!), and reactor-uc converts it to its corrected physical-clock
 * domain using the last successfully applied offset.
 *
 * @param user_data Opaque pointer passed to `ExternalClockSyncApi.init`.
 * @param sync_status Result of the completed sync run. Use 0 for success. Non-zero values are logged and do not
 *                    apply a clock offset.
 * @param next_sync_run_ms Absolute time in milliseconds for the next sync run, expressed in the external
 *                         implementation's uncorrected local platform-clock domain. reactor-uc converts it to its
 *                         corrected physical-clock domain using the last successfully applied offset. This is used only
 *                         when the implementation lets reactor-uc drive the sync schedule. An implementation that
 *                         drives its own schedule can ignore this (pass an arbitrary value such as @ref @c NEVER or 0)
 *                         and schedule the next run independently.
 * @param clock_offset_ms Clock offset in milliseconds. For non-grandmasters, a successful run interprets this as
 *                        local time minus reference time. For failed runs, this may be set to an arbitrary value
 *                        (e.g., 0) -- or the last successfully applied offset, although LF does not assume or
 *                        require this.
 */
typedef void (*ExternalClockSyncResultCallback)(void* user_data, int sync_status, int64_t next_sync_run_ms,
                                                int64_t clock_offset_ms);

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
  /**
   * @brief Initialize the external clock synchronization mechanism.
   *
   * The implementation should store @p result_callback and @p result_callback_user_data and call the callback whenever
   * a sync run completes. Set output parameter @p lf_drives_sync_schedule to true if reactor-uc should call schedule
   * periodically using the callback's next_sync_run_ms value. Set it to false if the implementation drives its own
   * timing and will call the result callback from its own thread, interrupt, or worker queue.
   */
  int (*init)(bool grandmaster, int federate_id, ExternalClockSyncResultCallback result_callback,
              void* result_callback_user_data, bool* lf_drives_sync_schedule);

  /**
   * @brief Request one clock synchronization run.
   *
   * reactor-uc calls this only when lf_drives_sync_schedule was set to true by init. The implementation must report the
   * completed run by invoking the callback passed to init. It may call the callback before schedule returns for a
   * synchronous implementation, or later from another execution context for an asynchronous implementation.
   *
   * The return value indicates whether the run request itself was accepted. It is not the sync result; the sync result
   * is passed to the callback as sync_status.
   */
  int (*schedule)(void);
} ExternalClockSyncApi;

typedef struct {
  int message_type;
  int sync_status;
  int64_t next_sync_run_ms;
  int64_t clock_offset_ms;
} ExternalClockSyncEvent;

/**
 * @brief Clock synchronization module that uses an external API to perform clock synchronization.
 *
 * This module is a normal system event handler. If the external API lets reactor-uc drive the
 * schedule, the handler requests sync runs at the times reported by the API callback. Completion
 * is always reported asynchronously through ExternalClockSyncResultCallback and applied later from
 * the runtime context.
 */
struct ClockSynchronizationExternal {
  SystemEventHandler super;
  Environment* env;
  const ExternalClockSyncApi* api;
  bool is_grandmaster;
  bool lf_drives_sync_schedule;
  /** Last successfully applied local-minus-reference clock offset. We store this because
   * in failed runs, the supplied offset may be (run was not able to compute new offset),
   * so the next schedule time needs to be adjusted using the last successfully applied offset.
   */
  int64_t last_clock_offset_ms;
  int federate_id;
};

void ClockSynchronizationExternal_ctor(ClockSynchronizationExternal* self, Environment* env,
                                       const ExternalClockSyncApi* api, bool is_grandmaster, int federate_id,
                                       size_t payload_size, void* payload_buf, bool* payload_used_buf,
                                       size_t payload_buf_capacity);

#endif // REACTOR_UC_CLOCK_SYNCHRONIZATION_EXTERNAL_H
