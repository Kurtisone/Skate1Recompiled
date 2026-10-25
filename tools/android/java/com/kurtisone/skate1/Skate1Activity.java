package com.kurtisone.skate1;

import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;

import org.libsdl.app.SDLActivity;

/**
 * Loads the C++ runtime, the rexglue runtime (which carries SDL) and the game.
 *
 * The game folder (game/, skate1.toml, logs) is the first of:
 *   <internal storage>/skate1, <microSD>/skate1  (needs "All files access")
 *   Android/data/com.kurtisone.skate1/files      (always allowed)
 * The native side picks it; this only asks for the permission once.
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

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
                && !Environment.isExternalStorageManager()
                && !getPreferences(MODE_PRIVATE).getBoolean("asked_all_files", false)) {
            getPreferences(MODE_PRIVATE).edit().putBoolean("asked_all_files", true).apply();
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                         Uri.parse("package:" + getPackageName())));
            } catch (Exception e) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        }
    }
}
