package com.forzahorizon2.android;

import android.content.Context;

/**
 * The app's own language (the launcher and the in-game controls panel): Portuguese or English, chosen on the
 * Language page (GameOptions.APP_LANGUAGE; by default the phone's, English when it is not Portuguese). The texts sit
 * next to the code that shows them, as tr("Jogar", "Play"): the screens are built in code, and two languages do not
 * need a resource per string. The game's own language is a separate option (GameOptions.GAME_LANGUAGE).
 */
final class AppLanguage {
    private static volatile boolean english;

    private AppLanguage() {
    }

    /** Reads the chosen language; each activity calls it in onCreate, before building its views. */
    static void load(Context context) {
        english = "en".equals(GameOptions.get(context, GameOptions.APP_LANGUAGE));
    }

    static boolean english() {
        return english;
    }

    static String tr(String portuguese, String english) {
        return AppLanguage.english ? english : portuguese;
    }
}
