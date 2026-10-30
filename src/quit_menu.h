// skate1 - quit entry in the main menu

#pragma once

#include <functional>

// `quit` runs when the player chooses the entry, on the game's thread.
void SetMainMenuQuitHandler(std::function<void()> quit);
