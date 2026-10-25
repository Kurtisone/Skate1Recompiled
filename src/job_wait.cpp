// skate1 - job worker idle loop without burning host cores
//
// The job manager workers (sub_828ACFB0) run:
//   while (!stop) { if (!scheduler->TryRunJob(mask)) WaitForSingleObject(work_event, 1); }
// The work event stays signaled while the queue holds jobs a given worker
// cannot take yet (affinity mask, dependencies), so an idle worker keeps
// returning from the wait at once and rescans: about 40,000 empty passes per
// second per worker. After a short streak of such back-to-back passes, sleep
// briefly instead, so a job released right away is still picked up at once.

#include <chrono>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "generated/default/skate1_init.h"

REXCVAR_DEFINE_INT32(job_wait_sleep_us, 50, "Skate1",
                     "Host sleep in microseconds once an idle job worker keeps finding nothing "
                     "to run (0 = spin like the console)")
    .range(0, 2000);
REXCVAR_DEFINE_INT32(job_wait_spin_passes, 16, "Skate1",
                     "Back-to-back empty passes an idle job worker makes before sleeping")
    .range(0, 100000);

namespace {
// Return address of the worker loop's WaitForSingleObject(work_event, 1).
constexpr uint32_t kJobWorkerWaitReturn = 0x828AD008;
// Passes closer together than this are a worker spinning on an empty queue.
constexpr auto kBackToBack = std::chrono::microseconds(20);
}  // namespace

// WaitForSingleObject(handle, milliseconds): WaitForSingleObjectEx, not
// alertable.
REX_HOOK_RAW(sub_82DF9068) {
  const bool job_worker = uint32_t(ctx.lr) == kJobWorkerWaitReturn;
  ctx.r5.u64 = 0;
  __imp__sub_82E01E00(ctx, base);
  if (!job_worker || ctx.r3.u32 != 0) {
    return;
  }
  int32_t sleep_us = REXCVAR_GET(job_wait_sleep_us);
  if (sleep_us <= 0) {
    return;
  }
  thread_local auto last_pass = std::chrono::steady_clock::time_point();
  thread_local int32_t streak = 0;
  auto now = std::chrono::steady_clock::now();
  streak = now - last_pass < kBackToBack ? streak + 1 : 0;
  last_pass = now;
  if (streak >= REXCVAR_GET(job_wait_spin_passes)) {
    std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
    last_pass = std::chrono::steady_clock::now();
  }
}
