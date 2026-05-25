#include "reactor-uc/clock_synchronization_external.h"

#include "reactor-uc/environments/federated_environment.h"
#include "reactor-uc/environment.h"
#include "reactor-uc/error.h"
#include "reactor-uc/logging.h"

#include <inttypes.h>

#define EXTERNAL_CLOCK_SYNC_RESERVED_EVENTS 1

enum {
  // Schedules the next external clock sync event.
  EXTERNAL_CLOCK_SYNC_EVENT_SYNC = 1,
};

static void ClockSynchronizationExternal_schedule_system_event(ClockSynchronizationExternal* self, instant_t time,
                                                               int message_type) {
  ExternalClockSyncEvent* payload = NULL;
  lf_ret_t ret = self->super.payload_pool.allocate_reserved(&self->super.payload_pool, (void**)&payload);

  if (ret != LF_OK) {
    LF_ERR(CLOCK_SYNC_EXT, "Failed to allocate payload for external clock sync event.");
    validate(false);
    return;
  }

  payload->message_type = message_type;
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
 * @brief Run one sync iteration by calling the schedule function of the API and scheduling the next sync event.
 * If this is the grandmaster, it will just schedule the next sync event at the time returned by the API.
 * If this is a non-grandmaster, it will also apply the clock offset returned by the API to its physical clock.
 */
static bool ClockSynchronizationExternal_schedule_next_sync(ClockSynchronizationExternal* self) {
  if (!self->api || !self->api->schedule) {
    return false;
  }

  int64_t next_sync_run_ms = 0;
  int64_t clock_offset_ms = 0;
  int ret = self->api->schedule(&next_sync_run_ms, &clock_offset_ms);
  if (ret != 0) {
    // This is not an error condition per se, since the schedule function may return non-zero
    // to indicate that the last round failed. Not sure if we actually want to expose that to LF
    // but for now, as we do expose it, just log a nice little warning.
    LF_WARN(CLOCK_SYNC_EXT, "External clock sync schedule returned %d", ret);
  }
  if (next_sync_run_ms <= 0) {
    LF_WARN(CLOCK_SYNC_EXT, "External clock sync schedule returned invalid next time %" PRId64, next_sync_run_ms);
    return false;
  }

  if (!self->is_grandmaster && ret == 0) {
    // The sync run succeeded and we're not the grandmaster. Update the clock
    // with the offset returned by the API.
    // FederatedEnvironment* env_fed = (FederatedEnvironment*)self->env;
    interval_t offset_ns = (interval_t)clock_offset_ms * (interval_t)1000000;
    LF_ERR(CLOCK_SYNC_EXT, "Applying clock offset of %" PRId64 " ms (%" PRId64 " ns)", clock_offset_ms, offset_ns);
    // env_fed->clock.adjust_time(&env_fed->clock, offset_ns);
  }

  // Schedule the next sync event at the time returned by the API.
  instant_t next_time = (instant_t)next_sync_run_ms * (instant_t)1000000;
  int64_t now_ms = (int64_t)(self->env->get_physical_time(self->env) / 1000000);
  LF_DEBUG(CLOCK_SYNC_EXT, "Scheduling at next time = %" PRId64 " ms (now = %" PRId64 " ms)", next_sync_run_ms, now_ms);
  ClockSynchronizationExternal_schedule_system_event(self, next_time, EXTERNAL_CLOCK_SYNC_EVENT_SYNC);
  return true;
}

static void ClockSynchronizationExternal_handle_system_event(SystemEventHandler* _self, SystemEvent* event) {
  ClockSynchronizationExternal* self = (ClockSynchronizationExternal*)_self;
  ExternalClockSyncEvent* payload = (ExternalClockSyncEvent*)event->super.payload;

  // We only have one type of event, so if it's not that, something went wrong,
  // or a new clock sync event type was added but this handler was not updated to handle it.
  if (payload->message_type != EXTERNAL_CLOCK_SYNC_EVENT_SYNC) {
    LF_WARN(CLOCK_SYNC_EXT, "Received unknown external clock sync event with type %d", payload->message_type);
    self->super.payload_pool.free(&self->super.payload_pool, event->super.payload);
    return;
  }

  LF_INFO(CLOCK_SYNC_EXT, "External clock sync event handled (type=%d)", payload->message_type);
  self->super.payload_pool.free(&self->super.payload_pool, event->super.payload);
  ClockSynchronizationExternal_schedule_next_sync(self);
}

void ClockSynchronizationExternal_ctor(ClockSynchronizationExternal* self, Environment* env,
                                       const ExternalClockSyncApi* api, bool is_grandmaster, int federate_id,
                                       size_t payload_size, void* payload_buf, bool* payload_used_buf,
                                       size_t payload_buf_capacity) {
  self->env = env;
  self->api = api;
  self->is_grandmaster = is_grandmaster;
  self->federate_id = federate_id;
  self->super.handle = ClockSynchronizationExternal_handle_system_event;

  EventPayloadPool_ctor(&self->super.payload_pool, (char*)payload_buf, payload_used_buf, payload_size,
                        payload_buf_capacity, EXTERNAL_CLOCK_SYNC_RESERVED_EVENTS);

  if (self->api && self->api->init) {
    int ret = self->api->init(self->is_grandmaster, self->federate_id);
    if (ret != 0) {
      LF_WARN(CLOCK_SYNC_EXT, "External clock sync init returned %d", ret);
    }
  }
  LF_INFO(CLOCK_SYNC_EXT, "Initialized external clock sync mechanism");

  // Schedule the first sync event immediately to kick off the clock synchronization process.
  ClockSynchronizationExternal_schedule_system_event(self, self->env->get_physical_time(self->env),
                                                     EXTERNAL_CLOCK_SYNC_EVENT_SYNC);
}
