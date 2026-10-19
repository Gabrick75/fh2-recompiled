package com.forzahorizon2.android;

import android.content.Context;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.os.LocaleList;

import java.util.Locale;

/**
 * The app's own language (the launcher and the in-game controls panel). The texts are Android string resources:
 * res/values (English, also what a phone in any other language sees), values-pt and values-es. By default Android
 * picks among them from the phone's languages (or the app's language in the system settings, Android 13+:
 * xml/locales_config); the Language page can pin one (GameOptions.APP_LANGUAGE), which every activity applies to
 * its context in attachBaseContext. The game's own language is a separate option (GameOptions.GAME_LANGUAGE).
 */
final class AppLanguage {
    static final String[] CODES = {"", "pt", "en", "es"};        // "": the system's
    static final String[] NAMES = {null, "Português", "English", "Español"};  // each in its own language

    private static volatile Resources resources = Resources.getSystem();

    private AppLanguage() {
    }

    /** The context with the chosen language, for attachBaseContext; tr() reads the texts from it from then on. */
    static Context wrap(Context base) {
        String code = GameOptions.get(base, GameOptions.APP_LANGUAGE);
        Context context = base;
        if (!code.isEmpty()) {
            Configuration config = new Configuration(base.getResources().getConfiguration());
            config.setLocales(new LocaleList(Locale.forLanguageTag(code)));
            context = base.createConfigurationContext(config);
        }
        resources = context.getResources();
        return context;
    }

    /** Where the chosen language is in CODES. */
    static int chosen(Context context) {
        int index = java.util.Arrays.asList(CODES).indexOf(GameOptions.get(context, GameOptions.APP_LANGUAGE));
        return Math.max(0, index);
    }

    /** For dates and numbers in the language the texts are in. */
    static Locale locale() {
        return resources.getConfiguration().getLocales().get(0);
    }

    static String tr(int id, Object... args) {
        return args.length == 0 ? resources.getString(id) : resources.getString(id, args);
    }
}
