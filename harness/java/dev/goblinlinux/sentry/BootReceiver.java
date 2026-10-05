package dev.goblinlinux.sentry;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.UserManager;
import android.util.Log;

/** Optional startup after credential-encrypted guest storage becomes available. */
public final class BootReceiver extends BroadcastReceiver {
    static final String START_ACTION = "dev.goblinlinux.sentry.START_AFTER_BOOT";
    private static final String PREFERENCES = "linux-startup";
    private static final String ENABLED = "after-reboot";
    static boolean enabled(Context context) {
        return context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).getBoolean(ENABLED, false);
    }
    static void setEnabled(Context context, boolean enabled) {
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).edit().putBoolean(ENABLED, enabled).apply();
    }
    static String lastRequest(Context context) {
        return context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).getString("last-request", "None");
    }
    static void blocked(Context context, RuntimeException error) {
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).edit().putString("last-request", "Blocked by Android").apply();
        Log.w("goblin-boot", "Android did not allow Linux to start after reboot", error);
    }
    @Override public void onReceive(Context context, Intent intent) {
        if (intent == null || !Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction())) return;
        // Never move the Linux disk or preferences into device-protected storage.
        if (!context.getSystemService(UserManager.class).isUserUnlocked() || !enabled(context)) return;
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).edit().putString("last-request", "Requested").apply();
        try {
            context.startForegroundService(new Intent(context, SessionService.class).setAction(START_ACTION));
        } catch (IllegalStateException | SecurityException error) {
            blocked(context, error);
        }
    }
}
