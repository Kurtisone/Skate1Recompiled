package com.kurtisone.skate1;

import org.libsdl.app.SDLActivity;

/**
 * Loads the C++ runtime, the rexglue runtime (which carries SDL) and the game.
 * The game's data, config and logs live in the app's external files folder:
 * /storage/emulated/0/Android/data/com.kurtisone.skate1/files
 */
public class Skate1Activity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {
            "c++_shared",
            "rexruntime",
            "main"
        };
    }
}
