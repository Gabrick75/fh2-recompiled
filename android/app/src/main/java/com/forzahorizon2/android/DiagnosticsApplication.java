package com.forzahorizon2.android;

import android.app.Application;
import android.content.Context;
import android.os.Process;

import java.io.File;
import java.io.PrintWriter;

/** Java failures are saved before delegating to Android's normal crash handler. */
public final class DiagnosticsApplication extends Application {
    // The app's language for the texts of code that runs outside an activity (AppLanguage.tr).
    @Override protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        AppLanguage.wrap(base);
    }

    @Override public void onCreate() {
        super.onCreate();
        Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, error) -> {
            try (PrintWriter writer = new PrintWriter(new File(getFilesDir(), "last-java-crash.txt"), "UTF-8")) {
                writer.println("Timestamp: " + System.currentTimeMillis());
                writer.println("Thread: " + thread.getName());
                error.printStackTrace(writer);
            } catch (Exception ignored) { }
            if (previous != null) previous.uncaughtException(thread, error);
            else {
                Process.killProcess(Process.myPid());
                System.exit(1);
            }
        });
    }
}
