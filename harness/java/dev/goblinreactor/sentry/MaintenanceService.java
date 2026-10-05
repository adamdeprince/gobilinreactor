package dev.goblinreactor.sentry;

import android.app.*;
import android.content.*;
import android.net.Uri;
import android.os.*;
import android.widget.Toast;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.atomic.AtomicBoolean;

/** Foreground owner for consistent disk transfers and rescue transitions. */
public final class MaintenanceService extends Service {
    static { System.loadLibrary("goblinuml"); }
    private static final AtomicBoolean BUSY = new AtomicBoolean();
    private static volatile boolean restarting;
    private static volatile String currentStatus = "Idle";
    private static native String transferNative(String directory, int fd, boolean restore) throws java.io.IOException;
    private static native long progressNative();
    private static native String previousNative(String directory, String name) throws java.io.IOException;
    static boolean busy() { return BUSY.get(); }
    static String status() { return currentStatus; }
    static boolean keepTerminal() { return BUSY.get() || restarting; }
    static void started() { restarting = false; }
    private PowerManager.WakeLock lock;
    private final Handler main = new Handler(Looper.getMainLooper());
    private String action;
    private final Runnable progress = new Runnable() {
        @Override public void run() {
            if (!BUSY.get()) return;
            currentStatus = "Environment " + action + " · " + android.text.format.Formatter.formatFileSize(MaintenanceService.this, progressNative());
            notifyProgress(currentStatus);
            main.postDelayed(this, 1000);
        }
    };
    private Notification notification(String text) {
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, TerminalActivity.class), PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        return new Notification.Builder(this, "maintenance").setSmallIcon(android.R.drawable.stat_notify_sync)
            .setContentTitle("Environment maintenance").setContentText(text).setOngoing(true).setContentIntent(open).build();
    }
    private void notifyProgress(String text) { getSystemService(NotificationManager.class).notify(2, notification(text)); }
    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null || !BUSY.compareAndSet(false, true)) return START_NOT_STICKY;
        action = intent.getAction();
        currentStatus = "Stopping environment cleanly…";
        try { Files.write(new File(getFilesDir(), "maintenance-active.txt").toPath(), action.getBytes(StandardCharsets.UTF_8)); }
        catch (Exception ignored) { }
        getSystemService(NotificationManager.class).createNotificationChannel(new NotificationChannel("maintenance", "Environment backup and recovery", NotificationManager.IMPORTANCE_LOW));
        if (Build.VERSION.SDK_INT >= 34) startForeground(2, notification("Stopping environment cleanly…"), android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        else startForeground(2, notification("Stopping environment cleanly…"));
        lock = getSystemService(PowerManager.class).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "Goblin:Maintenance"); lock.acquire();
        boolean wasRunning = SessionService.runningNative() || SessionService.preparing(); Uri uri = intent.getData();
        SessionService.stopNative();
        new Thread(() -> {
            String message; boolean restart = wasRunning;
            try {
                while (SessionService.runningNative()) Thread.sleep(100);
                File directory = new File(getFilesDir(), "uml"); directory.mkdirs();
                File rescue = new File(directory, "rescue-requested");
                if ("rescue".equals(action)) {
                    Files.write(rescue.toPath(), new byte[]{1}); restart = true; message = "Opening the rescue shell";
                } else if ("normal".equals(action)) {
                    Files.deleteIfExists(rescue.toPath()); restart = true; message = "Starting environment";
                } else if ("previous".equals(action)) {
                    main.post(progress);
                    previousNative(directory.getAbsolutePath(), intent.getStringExtra("disk"));
                    Files.deleteIfExists(rescue.toPath()); restart = true;
                    message = "Previous environment disk restored; replaced disk retained";
                } else {
                    boolean restore = "restore".equals(action);
                    if (!restore && !"backup".equals(action)) throw new IllegalArgumentException("Unknown maintenance action");
                    main.post(progress);
                    try (ParcelFileDescriptor stream = getContentResolver().openFileDescriptor(uri, restore ? "r" : "wt")) {
                        if (stream == null) throw new java.io.IOException("Cannot open the selected file");
                        String previous = transferNative(directory.getAbsolutePath(), stream.getFd(), restore);
                        if (restore) { Files.deleteIfExists(rescue.toPath()); restart = true; }
                        message = restore ? "Backup restored" + (previous.isEmpty() ? "" : ". The replaced disk is available in Previous disks") : "Environment backup saved";
                    }
                }
            } catch (Exception error) { message = "Environment " + action + " failed: " + error.getMessage(); }
            final String result = message; final boolean start = restart;
            main.post(() -> {
                main.removeCallbacks(progress);
                try { Files.write(new File(getFilesDir(), "maintenance-result.txt").toPath(), result.getBytes(StandardCharsets.UTF_8)); } catch (Exception ignored) { }
                new File(getFilesDir(), "maintenance-active.txt").delete();
                currentStatus = "Idle";
                restarting = start; BUSY.set(false);
                if (lock.isHeld()) lock.release();
                stopForeground(STOP_FOREGROUND_REMOVE); stopSelf();
                if (start) startForegroundService(new Intent(this, SessionService.class));
                Toast.makeText(this, result, Toast.LENGTH_LONG).show();
            });
        }, "goblin-maintenance").start();
        return START_NOT_STICKY;
    }
    @Override public IBinder onBind(Intent intent) { return null; }
    @Override public void onDestroy() { main.removeCallbacks(progress); if (lock != null && lock.isHeld()) lock.release(); super.onDestroy(); }
}
