#pragma once
#include "a32ipc.h"
uint32_t a32_event_handle(Obj *o, int id);   // a copy handle of the per-object event `id` (created on first use)
uint32_t a32_new_event_handle(void);         // a kernel event handle, signalled by the host with a32_signal_event
void a32_signal_event(uint32_t handle);
void a32_clear_event(uint32_t handle);
