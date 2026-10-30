// skate1 - quit entry in the main menu
//
// The console game leaves through the dashboard, so its main menu has no way
// out. main_menu_quit adds one, with the game files untouched: mainmenu.apt is
// patched in memory once loaded, before its script runs.
//
// mainmenu::SetState fills the list with mMenuPicker.AddText(id) calls, and its
// branch for the demo build is never run. Code cannot move (the script holds
// relocated pointers), so the demo branch's two calls give the room: the
// OPTIONS call becomes a jump there, the branch adds OPTIONS and EXTRAS, and its
// own jump comes back to where EXTRAS was, which now adds the demo's "exit"
// entry, translated in the game's languages. The list shows 9 items instead of 8.
//
// The picker reports a choice with Game.OnMenuSelect(controller, index, id).
// Choosing the entry closes the window like the quit dialog, the shutdown that
// cannot hang: XamLoaderTerminateTitle leaves a process without a window behind.

#include "quit_menu.h"

#include <cstring>
#include <utility>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "generated/default/skate1_init.h"

REXCVAR_DEFINE_BOOL(main_menu_quit, true, "Skate1", "Add a quit entry to the main menu");

namespace {

std::function<void()> g_quit;

constexpr char kQuitId[] = "ID_DEMO_MAIN_MENU_EXIT_DEMO";
// Screen load entry: the name and the loaded script.
constexpr uint32_t kEntryName = 264, kEntryApt = 528;
// mainmenu.apt, in SetState.
constexpr uint32_t kDemoBlock = 0xD1F0F, kDemoJumpTarget = 0xD1F20, kOptions = 0xD1F7D, kExtras = 0xD1F84;
constexpr uint32_t kNumVisibleItems = 0xD2D80;  // push byte 8, in the picker's setup

constexpr uint8_t kOptionsExtras[14] = {0xA2, 0x1E, 0x5A, 0xAE, 0x11, 0xB2, 0x12,
                                        0xA2, 0x1F, 0x5A, 0xAE, 0x11, 0xB2, 0x12};
constexpr uint8_t kDemoEntries[14] = {0xA2, 0x10, 0x5A, 0xAE, 0x11, 0xB2, 0x12,
                                      0xA2, 0x13, 0x5A, 0xAE, 0x11, 0xB2, 0x12};
constexpr uint8_t kDemoJump[4] = {0, 0, 0, 0x6C};
constexpr uint8_t kToExtras[4] = {0, 0, 0, 0x60};           // kExtras from kDemoJumpTarget + 4
constexpr uint8_t kToDemoBlock[7] = {0x99, 0, 0, 0xFF, 0xFF, 0xFF, 0x8B};  // jump to kDemoBlock

void PatchMainMenu(uint8_t* apt) {
  // Another version of the game, or a mod: leave the menu alone.
  if (std::memcmp(apt + kDemoBlock, kDemoEntries, 14) || std::memcmp(apt + kDemoJumpTarget, kDemoJump, 4) ||
      std::memcmp(apt + kOptions, kOptionsExtras, 14) || apt[kNumVisibleItems] != 0xB5 ||
      apt[kNumVisibleItems + 1] != 8) {
    REXLOG_WARN("main_menu_quit: mainmenu.apt is not the expected one, no quit entry");
    return;
  }
  std::memcpy(apt + kDemoBlock, kOptionsExtras, 14);
  std::memcpy(apt + kDemoJumpTarget, kToExtras, 4);
  std::memcpy(apt + kOptions, kToDemoBlock, 7);
  std::memcpy(apt + kExtras, kDemoEntries + 7, 7);
  apt[kNumVisibleItems + 1] = 9;
}

}  // namespace

void SetMainMenuQuitHandler(std::function<void()> quit) { g_quit = std::move(quit); }

// Runs once a screen's files are loaded, before its script.
REX_HOOK_RAW(sub_82426FB0) {
  const uint32_t entry = ctx.r4.u32;
  if (REXCVAR_GET(main_menu_quit) &&
      !std::strcmp(reinterpret_cast<const char*>(base + entry + kEntryName), "mainmenu")) {
    const uint32_t apt = REX_LOAD_U32(entry + kEntryApt);
    if (apt) PatchMainMenu(base + apt);
  }
  __imp__sub_82426FB0(ctx, base);
}

// Game.OnMenuSelect
REX_HOOK_RAW(sub_824359E0) {
  if (REXCVAR_GET(main_menu_quit) && g_quit) {
    PPCContext id = ctx;
    id.r3.u64 = 2;
    __imp__sub_8242E500(id, base);  // the third argument, as a string object
    const uint32_t text = REX_LOAD_U32(id.r3.u32);
    if (text && !std::strcmp(reinterpret_cast<const char*>(base + text), kQuitId)) {
      REXLOG_INFO("main_menu_quit: leaving the game");
      g_quit();
      return;
    }
  }
  __imp__sub_824359E0(ctx, base);
}
