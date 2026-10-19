package com.forzahorizon2.android;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;

/**
 * The Vulkan test of a driver (the launcher's Performance page), from nfsmw-android. It runs in a process of its own
 * (":gpu_probe" in the manifest), started for each test and killed after it: adrenotools hooks the process's loader
 * to open a custom driver, and that cannot be undone in a process that keeps running. Transparent, with no views:
 * the launcher shows the test and its result. Answers with the probe's JSON (Diagnostics.probe) in "gpu_report".
 */
public final class GpuProbeActivity extends Activity {
    static final String EXTRA_DRIVER = "driver";      // GpuDrivers name; "" = the system driver
    static final String EXTRA_REPORT = "gpu_report";
    private boolean finished;

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(AppLanguage.wrap(base));
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        String name = getIntent().getStringExtra(EXTRA_DRIVER);
        Handler main = new Handler(Looper.getMainLooper());
        // A driver that hangs while loading must not hold the launcher.
        main.postDelayed(() -> finishReport(error(AppLanguage.tr(R.string.the_driver_test_took_over_20))), 20000);
        new Thread(() -> {
            String report;
            try {
                GpuDrivers.Driver driver = name == null || name.isEmpty() ? null : GpuDrivers.find(this, name);
                if (name != null && !name.isEmpty() && driver == null) {
                    report = error(AppLanguage.tr(R.string.the_driver_is_no_longer_installed));
                } else {
                    report = Diagnostics.probe(this, driver);
                }
            } catch (Exception | LinkageError e) {
                report = error(AppLanguage.tr(R.string.could_not_load_the_diagnostics_library) + ": " + e);
            }
            String result = report;
            main.post(() -> finishReport(result));
        }, "VulkanDriverProbe").start();
    }

    private static String error(String message) {
        try {
            return new org.json.JSONObject().put("probeError", message).put("driverLoadFailed", true)
                    .put("compatible", false).toString();
        } catch (org.json.JSONException e) {
            return "{\"driverLoadFailed\":true,\"compatible\":false}";
        }
    }

    private void finishReport(String report) {
        if (finished) return;
        finished = true;
        setResult(RESULT_OK, new Intent().putExtra(EXTRA_REPORT, report));
        finish();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        // The Vulkan handles and the loader hooks belong to this throwaway process.
        Process.killProcess(Process.myPid());
    }
}
