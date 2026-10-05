package dev.goblinreactor.sentry;

import android.app.*;
import android.content.*;
import android.os.*;
import android.system.Os;
import android.system.OsConstants;
import android.text.format.Formatter;
import android.widget.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;

/** Local help and explicitly exported, limited diagnostic information. */
final class AppHelp {
    static String name(Context context) {
        return context.getApplicationInfo().loadLabel(context.getPackageManager()).toString();
    }
    static String version(Context context) {
        try {
            android.content.pm.PackageInfo info = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
            return info.versionName + " (" + info.getLongVersionCode() + ")";
        } catch (Exception unavailable) { return "unknown"; }
    }
    static AlertDialog text(Activity activity, String title, String message) {
        TextView body = new TextView(activity); android.text.SpannableString styled = new android.text.SpannableString(message);
        java.util.regex.Matcher marks = java.util.regex.Pattern.compile("\\bDebian\\b").matcher(message);
        while (marks.find()) styled.setSpan(new android.text.style.StyleSpan(android.graphics.Typeface.BOLD),
            marks.start(), marks.end(), android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        body.setText(styled); body.setTextIsSelectable(true);
        int pad = Math.round(20 * activity.getResources().getDisplayMetrics().density);
        body.setPadding(pad, pad, pad, pad);
        ScrollView scroll = new ScrollView(activity); scroll.addView(body);
        return new AlertDialog.Builder(activity).setTitle(title).setView(scroll).setPositiveButton("Close", null).show();
    }
    static AlertDialog guide(Activity activity) {
        return text(activity, name(activity) + " · " + version(activity),
            "A terminal and local development environment for Android. Uses a Linux® kernel and packages from Debian.\n\n"
            + "Your account is goblin and your home is /home/goblin.\n\n"
            + "Tap the terminal to type. Pinch to change text size. Ctrl and Alt apply to the next key. A physical keyboard hides the extra-key row. Use ⋮ for the menu.\n\n"
            + "Install software with sudo apt update and sudo apt install PACKAGE. Sudo is passwordless. sudo su - opens a root login shell; exit returns to your account.\n\n"
            + "New terminal opens another shell in the same environment. exit closes that shell. Detached services can keep running after every terminal closes. Use Shut down environment in the menu or notification to stop the whole instance cleanly.\n\n"
            + "Keep environment awake lets work continue with the screen off and uses battery. You control it in the menu. Android can still suspend or end the app; battery settings vary by phone.\n\n"
            + "Start environment after reboot is optional and off by default. Enable it in the menu to start the environment in the background after your first unlock following a phone restart. Services enabled in the environment can start again; open shells and unsaved work do not resume. Android force-stop or battery restrictions can prevent automatic startup until you open the app again.\n\n"
            + "The environment uses your phone's memory and storage. Storage & recovery shows the phone's actual free space. Back up environment saves your files, accounts, packages and configuration to a file you choose.\n\n"
            + "App updates preserve your installation. Uninstalling this app or clearing its Android storage deletes the local installation. Keep backups outside the app before doing either.\n\n"
            + "If something breaks, save diagnostics from the menu and note what you were doing. Rescue shell and Previous disks provide recovery options. Diagnostics exclude terminal contents and personal files.\n\n"
            + "GoblinReactor is independent of the Debian Project, which does not sponsor or endorse it. Debian is a registered trademark owned by Software in the Public Interest, Inc.\n\n"
            + "Linux® is the registered trademark of Linus Torvalds in the U.S. and other countries.");
    }
    static long allocated(File file) {
        try { return Os.stat(file.getAbsolutePath()).st_blocks * 512L; }
        catch (Exception unavailable) { return 0; }
    }
    static String storage(Context context) {
        File directory = new File(context.getFilesDir(), "uml");
        File disk = new File(directory, "rootfs.ext4");
        File[] previous = directory.listFiles((dir, name) -> name.startsWith("rootfs.before-restore."));
        long saved = 0; if (previous != null) for (File file : previous) saved += allocated(file);
        StatFs space = new StatFs(context.getFilesDir().getAbsolutePath());
        return "Phone space available: " + Formatter.formatFileSize(context, space.getAvailableBytes())
            + "\nEnvironment disk stored on phone: " + Formatter.formatFileSize(context, allocated(disk))
            + "\nPrevious disks: " + (previous == null ? 0 : previous.length) + " · " + Formatter.formatFileSize(context, saved)
            + "\n\nThe environment’s reported free space is shared with Android and other apps. It is not reserved."
            + "\n\nA backup needs space at the selected destination. Restoring needs space for the restored data while retaining your current disk.";
    }
    static String diagnostics(Context context) throws IOException {
        StringBuilder report = new StringBuilder(name(context) + " diagnostic report\n");
        report.append("Created: ").append(new java.text.SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ssXXX", Locale.ROOT).format(new Date())).append('\n');
        report.append("App: ").append(version(context)).append('\n');
        report.append("Package: ").append(context.getPackageName()).append('\n');
        report.append("Device: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL).append('\n');
        report.append("Android: ").append(Build.VERSION.RELEASE).append(" / API ").append(Build.VERSION.SDK_INT).append('\n');
        report.append("ABIs: ").append(Arrays.toString(Build.SUPPORTED_ABIS)).append('\n');
        report.append("Host page size: ").append(Os.sysconf(OsConstants._SC_PAGESIZE)).append('\n');
        report.append("Host configured CPUs: ").append(Os.sysconf(OsConstants._SC_NPROCESSORS_CONF)).append('\n');
        ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
        ActivityManager manager = context.getSystemService(ActivityManager.class); manager.getMemoryInfo(memory);
        report.append("Memory total/available: ").append(memory.totalMem).append('/').append(memory.availMem).append('\n');
        report.append("Low memory: ").append(memory.lowMemory).append('\n');
        report.append("Environment state: ").append(SessionService.status()).append('\n');
        report.append("Environment running: ").append(SessionService.runningNative()).append('\n');
        report.append("Rescue mode: ").append(SessionService.rescueNative()).append('\n');
        String terminals = SessionService.terminalsNative();
        report.append("Open terminals: ").append(terminals.isEmpty() ? 0 : terminals.split("\n").length).append('\n');
        report.append("Maintenance: ").append(MaintenanceService.status()).append('\n');
        PowerManager power = context.getSystemService(PowerManager.class);
        report.append("Keep environment awake: ").append(SessionService.keepAwake(context)).append('\n');
        report.append("Start environment after reboot: ").append(BootReceiver.enabled(context)).append('\n');
        report.append("Last boot start request: ").append(BootReceiver.lastRequest(context)).append('\n');
        report.append("Battery optimization exempt: ").append(power.isIgnoringBatteryOptimizations(context.getPackageName())).append('\n');
        report.append("Power save / device idle: ").append(power.isPowerSaveMode()).append('/').append(power.isDeviceIdleMode()).append('\n');
        report.append("Notifications enabled: ").append(context.getSystemService(NotificationManager.class).areNotificationsEnabled()).append('\n');
        report.append('\n').append(storage(context)).append('\n');
        // Fixed app-owned metadata only: never read a terminal transcript, guest
        // file, logcat, boot log, account list, network address or device ID.
        File release = new File(context.getFilesDir(), "uml/kernel-release");
        if (release.isFile()) report.append("\nGuest kernel: ").append(new String(Files.readAllBytes(release.toPath()), StandardCharsets.UTF_8));
        try (InputStream input = context.getAssets().open("release-info.json")) {
            report.append("\nBuild:\n").append(read(input));
        }
        if (Build.VERSION.SDK_INT >= 30) {
            report.append("\nAndroid process exits (no traces or descriptions):\n");
            for (ApplicationExitInfo exit : manager.getHistoricalProcessExitReasons(context.getPackageName(), 0, 0))
                report.append("time=").append(exit.getTimestamp()).append(" reason=").append(exit.getReason())
                    .append(" status=").append(exit.getStatus()).append(" importance=").append(exit.getImportance()).append('\n');
        }
        return report.append("\nNo terminal contents, commands, guest files, IP addresses, or device identifiers are included.\n").toString();
    }
    static String read(InputStream input) throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream(); byte[] bytes = new byte[8192]; int n;
        while ((n = input.read(bytes)) != -1) output.write(bytes, 0, n);
        return new String(output.toByteArray(), StandardCharsets.UTF_8);
    }
    static void licenses(Activity activity) {
        new Thread(() -> {
            try {
                ArrayList<String> paths = new ArrayList<>();
                for (String root : new String[]{"uml-licenses", "terminal-licenses", "release-notices"}) collect(activity, root, paths);
                Collections.sort(paths);
                activity.runOnUiThread(() -> {
                    if (activity.isDestroyed()) return;
                    new AlertDialog.Builder(activity).setTitle("Open-source notices")
                        .setItems(paths.toArray(new String[0]), (dialog, index) -> new Thread(() -> {
                            try (InputStream input = activity.getAssets().open(paths.get(index))) {
                                String content = read(input);
                                activity.runOnUiThread(() -> { if (!activity.isDestroyed()) text(activity, paths.get(index), content); });
                            } catch (IOException error) { activity.runOnUiThread(() -> Toast.makeText(activity, "Could not read notice", Toast.LENGTH_LONG).show()); }
                        }, "goblin-notice").start()).setNegativeButton("Close", null).show();
                });
            } catch (IOException error) { activity.runOnUiThread(() -> Toast.makeText(activity, "Could not read notices", Toast.LENGTH_LONG).show()); }
        }, "goblin-notices").start();
    }
    private static void collect(Context context, String path, ArrayList<String> files) throws IOException {
        String[] entries = context.getAssets().list(path);
        if (entries == null || entries.length == 0) { files.add(path); return; }
        for (String entry : entries) collect(context, path + "/" + entry, files);
    }
}
