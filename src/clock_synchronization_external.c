#include "reactor-uc/clock_synchronization_external.h"

#include "reactor-uc/environment.h"
#include "reactor-uc/error.h"
#include "reactor-uc/logging.h"

#define EXTERNAL_CLOCK_SYNC_RESERVED_EVENTS 1

enum { EXTERNAL_CLOCK_SYNC_EVENT_HELLO = 1 };

static void ClockSynchronizationExternal_schedule_system_event(ClockSynchronizationExternal* self, instant_t time,
                                                               int message_type) {
  ExternalClockSyncEvent* payload = NULL;
  lf_ret_t ret = self->super.payload_pool.allocate_reserved(&self->super.payload_pool, (void**)&payload);

  if (ret != LF_OK) {
    LF_ERR(CLOCK_SYNC, "Failed to allocate payload for external clock sync event.");
    validate(false);
    return;
  }

  payload->message_type = message_type;
  tag_t tag = {.time = time, .microstep = 0};
  SystemEvent event = SYSTEM_EVENT_INIT(tag, &self->super, payload);

  ret = self->env->scheduler->schedule_system_event_at(self->env->scheduler, &event);
  if (ret != LF_OK) {
    LF_ERR(CLOCK_SYNC, "Failed to schedule external clock sync event.");
    self->super.payload_pool.free(&self->super.payload_pool, payload);
    validate(false);
  }
}

static void ClockSynchronizationExternal_handle_system_event(SystemEventHandler* _self, SystemEvent* event) {
  ClockSynchronizationExternal* self = (ClockSynchronizationExternal*)_self;
  ExternalClockSyncEvent* payload = (ExternalClockSyncEvent*)event->super.payload;
  LF_INFO(CLOCK_SYNC, "External clock sync event handled (type=%d)", payload->message_type);
  (void)self;
  self->super.payload_pool.free(&self->super.payload_pool, event->super.payload);
}

void ClockSynchronizationExternal_ctor(ClockSynchronizationExternal* self, Environment* env,
                                       const ExternalClockSyncApi* api, size_t payload_size, void* payload_buf,
                                       bool* payload_used_buf, size_t payload_buf_capacity) {
  self->env = env;
  self->api = api;
  self->super.handle = ClockSynchronizationExternal_handle_system_event;

  EventPayloadPool_ctor(&self->super.payload_pool, (char*)payload_buf, payload_used_buf, payload_size,
                        payload_buf_capacity, EXTERNAL_CLOCK_SYNC_RESERVED_EVENTS);

  if (self->api && self->api->init) {
    int ret = self->api->init();
    if (ret != 0) {
      LF_WARN(CLOCK_SYNC, "External clock sync init returned %d", ret);
    }
  }
  if (self->api && self->api->schedule) {
    int ret = self->api->schedule();
    if (ret != 0) {
      LF_WARN(CLOCK_SYNC, "External clock sync schedule returned %d", ret);
    }
  }
  LF_INFO(CLOCK_SYNC, "Hello, external clock sync mechanism");
  ClockSynchronizationExternal_schedule_system_event(self, self->env->get_physical_time(self->env),
                                                     EXTERNAL_CLOCK_SYNC_EVENT_HELLO);
}
