#include "reactor-uc/clock_synchronization_external.h"

#include "reactor-uc/environments/federated_environment.h"
#include "reactor-uc/environment.h"
#include "reactor-uc/error.h"
#include "reactor-uc/logging.h"

#include <inttypes.h>

#define EXTERNAL_CLOCK_SYNC_RESERVED_EVENTS 1

enum {
  // Requests one external clock sync run.
  EXTERNAL_CLOCK_SYNC_EVENT_SYNC = 1,
  // Applies a completed external clock sync run.
  EXTERNAL_CLOCK_SYNC_EVENT_RESULT = 2,
};

static void ClockSynchronizationExternal_schedule_system_event(ClockSynchronizationExternal* self, instant_t time,
                                                               const ExternalClockSyncEvent* source_payload) {
  ExternalClockSyncEvent* payload = NULL;
  lf_ret_t ret;

  if (source_payload->message_type == EXTERNAL_CLOCK_SYNC_EVENT_SYNC) {
    ret = self->super.payload_pool.allocate_reserved(&self->super.payload_pool, (void**)&payload);
  } else {
    ret = self->super.payload_pool.allocate(&self->super.payload_pool, (void**)&payload);
    if (ret != LF_OK && !self->lf_drives_sync_schedule) {
      ret = self->super.payload_pool.allocate_reserved(&self->super.payload_pool, (void**)&payload);
    }
  }

  if (ret != LF_OK) {
    LF_ERR(CLOCK_SYNC_EXT, "Failed to allocate payload for external clock sync event.");
    if (source_payload->message_type == EXTERNAL_CLOCK_SYNC_EVENT_SYNC) {
      validate(false);
    }
    return;
  }

  *payload = *source_payload;
  tag_t tag = {.time = time, .microstep = 0};
  SystemEvent event = SYSTEM_EVENT_INIT(tag, &self->super, payload);

  ret = self->env->scheduler->schedule_system_event_at(self->env->scheduler, &event);
  if (ret != LF_OK) {
    LF_ERR(CLOCK_SYNC_EXT, "Failed to schedule external clock sync event.");
    self->super.payload_pool.free(&self->super.payload_pool, payload);
    validate(false);
  }
}

/**
 * @brief Request one sync iteration from the external API.
 *
 * Completion is reported later through ClockSynchronizationExternal_report_result_callback, even if the
 * implementation finishes synchronously before schedule returns.
 */
static bool ClockSynchronizationExternal_request_sync(ClockSynchronizationExternal* self) {
  if (!self->api || !self->api->schedule) {
    return false;
  }

  instant_t clock_time = self->env->get_physical_time(self->env);
  LF_DEBUG(CLOCK_SYNC_EXT, "Starting sync run at %" PRId64 " ms", clock_time / 1000000);

  int ret = self->api->schedule();

  if (ret != 0) {
    LF_WARN(CLOCK_SYNC_EXT, "External clock sync schedule request returned %d", ret);
  }
  return ret == 0;
}

static void ClockSynchronizationExternal_schedule_next_lf_driven_sync(ClockSynchronizationExternal* self,
                                                                      int64_t next_sync_run_ms) {
  if (next_sync_run_ms <= 0) {
    LF_WARN(CLOCK_SYNC_EXT, "External clock sync schedule returned invalid next time %" PRId64, next_sync_run_ms);
    return;
  }

  int64_t schedule_time_ms = next_sync_run_ms;
  if (!self->is_grandmaster) {
    // For non-grandmaster nodes, convert local schedule time to reference time domain.
    // We stepped the clock by the offset, so the schedule time in the local time domain
    // is now correct with respect to the reference time domain!
    schedule_time_ms = next_sync_run_ms - self->last_clock_offset_ms;
  }

  // Schedule the next sync event at the time returned by the API (reference time domain).
  instant_t next_time = (instant_t)(schedule_time_ms) * (instant_t)1000000;
  int64_t now_ms = (int64_t)(self->env->get_physical_time(self->env) / 1000000);
  LF_DEBUG(CLOCK_SYNC_EXT, "Scheduling at next time = %" PRId64 " ms (now = %" PRId64 " ms)", schedule_time_ms, now_ms);
  ExternalClockSyncEvent payload = {.message_type = EXTERNAL_CLOCK_SYNC_EVENT_SYNC};
  ClockSynchronizationExternal_schedule_system_event(self, next_time, &payload);
}

static void ClockSynchronizationExternal_apply_sync_result(ClockSynchronizationExternal* self,
                                                           const ExternalClockSyncEvent* payload) {
  FederatedEnvironment* env_fed = (FederatedEnvironment*)self->env;
  int sync_status = payload->sync_status;
  int64_t next_sync_run_ms = payload->next_sync_run_ms;
  int64_t clock_offset_ms = payload->clock_offset_ms;

  if (sync_status != 0) {
    // This is not an error condition per se, since the external implementation may report non-zero
    // to indicate that the last round failed. For now, log it and continue.
    LF_WARN(CLOCK_SYNC_EXT, "External clock sync run returned %d", sync_status);
  }

  if (!self->is_grandmaster && sync_status == 0) {
    // The sync run succeeded and we're not the grandmaster. Update the clock
    // with the offset returned by the API (offset is local - reference).
    interval_t offset_ns = (interval_t)clock_offset_ms * (interval_t)1000000;
    instant_t raw_now = self->env->platform->get_physical_time(self->env->platform);
    instant_t corrected = raw_now - offset_ns;
    LF_INFO(CLOCK_SYNC_EXT, "Stepping clock by offset %" PRId64 " ms", clock_offset_ms);
    env_fed->clock.set_time(&env_fed->clock, corrected);
    self->last_clock_offset_ms = clock_offset_ms;

    interval_t offset_abs = offset_ns > 0 ? offset_ns : -offset_ns;
    if (offset_abs > MSEC(100)) {
      LF_WARN(CLOCK_SYNC_EXT, "Large (possibly initial) clock offset applied: %" PRId64 " ms, informing scheduler",
              clock_offset_ms);
      // TODO: Correct sign? Also, doesn't step_clock need the offset relative to synchronized time?
      self->env->scheduler->step_clock(self->env->scheduler, -offset_ns);
      self->env->platform->notify(self->env->platform);
    }
  }

  if (self->lf_drives_sync_schedule) {
    ClockSynchronizationExternal_schedule_next_lf_driven_sync(self, next_sync_run_ms);
  }
}

static void ClockSynchronizationExternal_report_result_callback(void* user_data, int sync_status,
                                                                int64_t next_sync_run_ms, int64_t clock_offset_ms) {
  ClockSynchronizationExternal* self = (ClockSynchronizationExternal*)user_data;
  if (!self) {
    LF_ERR(CLOCK_SYNC_EXT, "External clock sync result callback called without user data.");
    validate(false);
    return;
  }

  ExternalClockSyncEvent payload = {.message_type = EXTERNAL_CLOCK_SYNC_EVENT_RESULT,
                                    .sync_status = sync_status,
                                    .next_sync_run_ms = next_sync_run_ms,
                                    .clock_offset_ms = clock_offset_ms};
  instant_t now = self->env->get_physical_time(self->env);
  LF_DEBUG(CLOCK_SYNC_EXT, "External clock sync result reported with next=%" PRId64 " ms offset=%" PRId64 " ms",
           next_sync_run_ms, clock_offset_ms);
  ClockSynchronizationExternal_schedule_system_event(self, now, &payload);
}

static void ClockSynchronizationExternal_handle_system_event(SystemEventHandler* _self, SystemEvent* event) {
  ClockSynchronizationExternal* self = (ClockSynchronizationExternal*)_self;
  ExternalClockSyncEvent* payload = (ExternalClockSyncEvent*)event->super.payload;

  if (payload->message_type == EXTERNAL_CLOCK_SYNC_EVENT_SYNC) {
    LF_INFO(CLOCK_SYNC_EXT, "External clock sync event handled (type=%d)", payload->message_type);
    self->super.payload_pool.free(&self->super.payload_pool, event->super.payload);
    ClockSynchronizationExternal_request_sync(self);
    return;
  }

  if (payload->message_type == EXTERNAL_CLOCK_SYNC_EVENT_RESULT) {
    LF_INFO(CLOCK_SYNC_EXT, "External clock sync result event handled (type=%d)", payload->message_type);
    ClockSynchronizationExternal_apply_sync_result(self, payload);
    self->super.payload_pool.free(&self->super.payload_pool, event->super.payload);
    return;
  }

  // If a new clock sync event type was added but this handler was not updated to handle it.
  LF_WARN(CLOCK_SYNC_EXT, "Received unknown external clock sync event with type %d", payload->message_type);
  self->super.payload_pool.free(&self->super.payload_pool, event->super.payload);
}

void ClockSynchronizationExternal_ctor(ClockSynchronizationExternal* self, Environment* env,
                                       const ExternalClockSyncApi* api, bool is_grandmaster, int federate_id,
                                       size_t payload_size, void* payload_buf, bool* payload_used_buf,
                                       size_t payload_buf_capacity) {
  self->env = env;
  self->api = api;
  self->is_grandmaster = is_grandmaster;
  self->lf_drives_sync_schedule = true;
  self->last_clock_offset_ms = 0;
  self->federate_id = federate_id;
  self->super.handle = ClockSynchronizationExternal_handle_system_event;

  EventPayloadPool_ctor(&self->super.payload_pool, (char*)payload_buf, payload_used_buf, payload_size,
                        payload_buf_capacity, EXTERNAL_CLOCK_SYNC_RESERVED_EVENTS);

  if (self->api && self->api->init) {
    int ret =
        self->api->init(self->is_grandmaster, self->federate_id, ClockSynchronizationExternal_report_result_callback,
                        self, &self->lf_drives_sync_schedule);
    if (ret != 0) {
      LF_WARN(CLOCK_SYNC_EXT, "External clock sync init returned %d", ret);
    }
  }
  LF_INFO(CLOCK_SYNC_EXT, "Initialized external clock sync mechanism");

  if (self->lf_drives_sync_schedule) {
    // Schedule the first sync event to kick off the clock synchronization process.
    // We use a fixed delay of 3 seconds here to give the program some time to start up before the first sync,
    // but this could be made configurable.
    ExternalClockSyncEvent payload = {.message_type = EXTERNAL_CLOCK_SYNC_EVENT_SYNC};
    ClockSynchronizationExternal_schedule_system_event(self, self->env->get_physical_time(self->env) + MSEC(3000),
                                                       &payload);
  }
}
