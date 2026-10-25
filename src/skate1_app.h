// skate1 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/filesystem.h>
#include <rex/rex_app.h>

class Skate1App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Skate1App>(new Skate1App(ctx, "skate1",
        skate1_PPCImageConfig));
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // void OnShutdown() override {}
  // Portable layout: a relative (or missing) game_data_root resolves next to
  // the exe instead of the working directory, and a portable.txt beside the
  // exe keeps saves and caches in the exe folder too. The app root is the
  // exe folder on desktop and the app's external files folder on Android.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    const auto exe_dir = rex::filesystem::GetAppRootFolder();
    if (paths.game_data_root.empty()) paths.game_data_root = exe_dir / "game";
    if (paths.game_data_root.is_relative())
      paths.game_data_root = exe_dir / paths.game_data_root;
    if (std::filesystem::exists(exe_dir / "portable.txt")) {
      paths.user_data_root = exe_dir / "userdata";
      paths.cache_root = paths.user_data_root / "cache";
    }
  }
};
