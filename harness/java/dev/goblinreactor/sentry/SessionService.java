package dev.goblinreactor.sentry;

import android.app.*;
import android.content.Intent;
import android.content.Context;
import android.content.res.AssetManager;
import android.os.*;

public final class SessionService extends Service {
    static final String POWER_ACTION = "dev.goblinreactor.sentry.POWER_POLICY";
    static final String POWER_PREFERENCES = "linux-power";
    static final String KEEP_AWAKE = "keep-awake";
    static { System.loadLibrary("goblinuml"); }
    static native void startNative(String directory, String library, AssetManager assets);
    static native void testsNative(String directory, String library, AssetManager assets, int mode);
    static native byte[] readGuestNative(String path);
    static native void stopNative();
    static native boolean runningNative();
    static native boolean rescueNative();
    static native byte[] transcriptNative();
    static native String stateNative();
    static native boolean inputNative(byte[] bytes);
    static native void resizeNative(int rows, int columns);
    static native int selectedNative();
    static native int terminalStateNative(int id);
    static native String terminalsNative();
    static native int openTerminalNative(String user, boolean create);
    static native void selectNative(int id);
    static native void closeTerminalNative(int id);
    static native void releaseTerminalNative(int id);
    private PowerManager.WakeLock wakeLock;
    private static volatile boolean preparing;
    private static volatile String startupError = "";
    private int startupGeneration;
    private Thread setup;
    private boolean destroyed;
    static boolean preparing() { return preparing; }
    static String status() { return preparing ? KittyRuntime.progress() : startupError.isEmpty() ? stateNative() : startupError; }
    static boolean keepAwake(Context context) {
        return context.getSharedPreferences(POWER_PREFERENCES, MODE_PRIVATE).getBoolean(KEEP_AWAKE, true);
    }
    static void setKeepAwake(Context context, boolean enabled) {
        context.getSharedPreferences(POWER_PREFERENCES, MODE_PRIVATE).edit().putBoolean(KEEP_AWAKE, enabled).apply();
        context.startService(new Intent(context, SessionService.class).setAction(POWER_ACTION));
    }
    @Override public void onCreate() {
        super.onCreate();
        ManagedLinux.initialize(this);
        wakeLock = getSystemService(PowerManager.class).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "Goblin:Linux");
        wakeLock.setReferenceCounted(false);
    }
    private void updateWakeLock() {
        boolean needed = (preparing || runningNative()) && keepAwake(this);
        // A user-started Linux machine has no fixed runtime deadline. The
        // service owns the lock until shutdown, failure or an explicit opt-out.
        if (needed && !wakeLock.isHeld()) wakeLock.acquire();
        else if (!needed && wakeLock.isHeld()) wakeLock.release();
    }
    private void releaseWakeLock() { if (wakeLock != null && wakeLock.isHeld()) wakeLock.release(); }
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable watch = new Runnable() {
        public void run() {
            updateWakeLock();
            if (!preparing && !runningNative()) { stopForeground(STOP_FOREGROUND_REMOVE); stopSelf(); }
            else handler.postDelayed(this, 1000);
        }
    };
    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        boolean fromBoot = intent != null && BootReceiver.START_ACTION.equals(intent.getAction());
        if (fromBoot && !BootReceiver.enabled(this)) {
            if (!preparing && !runningNative()) stopSelf(startId);
            return START_NOT_STICKY;
        }
        if (MaintenanceService.busy()) { stopSelf(); return START_NOT_STICKY; }
        if (intent != null && POWER_ACTION.equals(intent.getAction())) {
            updateWakeLock();
            if (!preparing && !runningNative()) stopSelf();
            return START_NOT_STICKY;
        }
        if (intent != null && "stop".equals(intent.getAction())) {
            cancelSetup();
            stopNative();
            handler.removeCallbacks(watch); handler.post(watch);
            return START_NOT_STICKY;
        }
        NotificationManager manager = getSystemService(NotificationManager.class);
        manager.createNotificationChannel(new NotificationChannel("session", AppHelp.name(this), NotificationManager.IMPORTANCE_LOW));
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, TerminalActivity.class), PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        PendingIntent stop = PendingIntent.getService(this, 1, new Intent(this, SessionService.class).setAction("stop"), PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Notification notification = new Notification.Builder(this, "session")
                .setSmallIcon(android.R.drawable.stat_notify_more).setContentTitle(AppHelp.name(this))
                .setContentText("Terminals and background processes are running").setContentIntent(open)
                .addAction(new Notification.Action.Builder(null, "Shut down Linux", stop).build())
                .setOngoing(true).build();
        try {
            if (Build.VERSION.SDK_INT >= 34) startForeground(1, notification, android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
            else startForeground(1, notification);
        } catch (IllegalStateException | SecurityException error) {
            if (!fromBoot) throw error;
            BootReceiver.blocked(this, error);
            startupError = "Android prevented Linux from starting after reboot. Open Linux to start it manually.";
            stopSelf(startId);
            return START_NOT_STICKY;
        }
        if (intent != null && "tests".equals(intent.getAction())) {
            int mode=intent.getIntExtra("mode",0);
            java.io.File directory=getFilesDir();
            // A separate test-owned root verifies fresh deployment without
            // deleting or altering the installed Debian environment.
            if(mode==5) { directory=new java.io.File(directory,"uml-fresh-check"); directory.mkdirs(); mode=0; }
            testsNative(directory.getAbsolutePath(),getApplicationInfo().nativeLibraryDir,getAssets(),mode);
        }
        else {
            if (runningNative()) {
                if (!fromBoot && "Running".equals(stateNative()) && (selectedNative() == 0 || terminalStateNative(selectedNative()) >= 2)) {
                    String list = terminalsNative();
                    if (list.isEmpty()) openTerminalNative("goblin", false);
                    else selectNative(Integer.parseInt(list.split("\t")[0]));
                }
            }
            else if (!preparing) {
                preparing = true; startupError = "";
                final int generation = ++startupGeneration;
                setup = new Thread(() -> {
                    String error = KittyRuntime.prepare(getApplicationContext());
                    handler.post(() -> {
                        if (destroyed || generation != startupGeneration) return;
                        preparing = false; setup = null;
                        if (!error.isEmpty()) startupError = "Terminal setup failed: " + error;
                        else if (!MaintenanceService.busy())
                            startNative(getFilesDir().getAbsolutePath(), getApplicationInfo().nativeLibraryDir, getAssets());
                        updateWakeLock();
                        handler.removeCallbacks(watch); handler.post(watch);
                    });
                }, "goblin-terminal-setup");
                setup.start();
            }
        }
        updateWakeLock();
        handler.removeCallbacks(watch); handler.postDelayed(watch, 1000);
        MaintenanceService.started();
        return START_NOT_STICKY;
    }
    @Override public IBinder onBind(Intent intent) { return null; }
    private void cancelSetup() {
        ++startupGeneration;
        if (setup != null) { setup.interrupt(); setup = null; }
        preparing = false;
    }
    @Override public void onDestroy() {
        destroyed = true; cancelSetup();
        handler.removeCallbacks(watch);
        try { stopNative(); } finally { releaseWakeLock(); super.onDestroy(); }
    }
}
