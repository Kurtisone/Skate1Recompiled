// skate1 - Adreno GPU clock control on Android (adrenotools).
//
// gpu_turbo turns the kernel GPU driver's power control off, so the Adreno
// runs at its top clock (thermal limits still apply). Polled every second,
// so it can be switched while playing (and A/B tested).

#if REX_HAS_ADRENOTOOLS

#include <chrono>
#include <thread>

#include <adrenotools/driver.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(gpu_turbo, false, "Skate1",
                    "Android (Adreno): keep the GPU at its top clock (more heat and battery)");

namespace {

void GpuTurboLoop() {
  bool applied = false;
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    bool turbo = REXCVAR_GET(gpu_turbo);
    if (turbo != applied) {
      adrenotools_set_turbo(turbo);
      applied = turbo;
      REXLOG_INFO("GPU turbo {}", turbo ? "on" : "off");
    }
  }
}

struct GpuTurboStarter {
  GpuTurboStarter() { std::thread(GpuTurboLoop).detach(); }
} gpu_turbo_starter;

}  // namespace

#endif  // REX_HAS_ADRENOTOOLS
