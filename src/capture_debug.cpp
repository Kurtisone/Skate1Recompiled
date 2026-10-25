// skate1 - capture of the guest output image, for telling a broken game
// picture from a broken presentation on a device (custom GPU drivers).
//
// With capture_guest_output_seconds > 0, that many seconds after setup the
// last guest output image (before the presenter scales and draws it) is
// written to guest_output.ppm in the app folder.

#include <chrono>
#include <cstdio>
#include <thread>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/presenter.h>

REXCVAR_DEFINE_INT32(capture_guest_output_seconds, 0, "Skate1",
                     "Write the guest output image to guest_output.ppm this many seconds after "
                     "launch (0 = off)")
    .range(0, 3600);

void StartGuestOutputCapture(rex::Runtime* runtime) {
  int seconds = REXCVAR_GET(capture_guest_output_seconds);
  if (seconds <= 0 || !runtime) {
    return;
  }
  std::thread([runtime, seconds]() {
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    auto* graphics = runtime->graphics_system();
    auto* presenter = graphics ? graphics->presenter() : nullptr;
    rex::ui::RawImage image;
    if (!presenter || !presenter->CaptureGuestOutput(image)) {
      REXLOG_WARN("Guest output capture failed");
      return;
    }
    auto path = rex::filesystem::GetAppRootFolder() / "guest_output.ppm";
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
      REXLOG_WARN("Cannot write {}", path.string());
      return;
    }
    std::fprintf(file, "P6\n%u %u\n255\n", image.width, image.height);
    uint64_t sum = 0;
    for (uint32_t y = 0; y < image.height; ++y) {
      const uint8_t* row = image.data.data() + y * image.stride;
      for (uint32_t x = 0; x < image.width; ++x) {
        std::fwrite(row + x * 4, 1, 3, file);
        sum += row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2];
      }
    }
    std::fclose(file);
    REXLOG_INFO("Guest output {}x{} written to {} (average level {:.1f})", image.width,
                image.height, path.string(),
                double(sum) / (3.0 * image.width * image.height));
  }).detach();
}
