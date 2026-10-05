package dev.goblinreactor.sentry;

import android.app.*;
import android.content.*;
import android.net.Uri;
import android.os.*;
import android.system.Os;
import android.view.*;
import android.view.accessibility.AccessibilityNodeInfo;
import android.widget.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.UUID;
import java.util.function.BooleanSupplier;

/** Exercises the handoff UI and failure recovery against the release APK. */
public final class ProductAcceptance extends Instrumentation {
    private TerminalActivity activity;
    private final StringBuilder report = new StringBuilder();
    @Override public void onCreate(Bundle args) { super.onCreate(args); start(); }
    private void require(boolean value, String message) { if (!value) throw new AssertionError(message); }
    private void waitFor(BooleanSupplier condition, String message) {
        waitFor(condition, message, 120000);
    }
    private void waitFor(BooleanSupplier condition, String message, long timeout) {
        long end = SystemClock.elapsedRealtime() + timeout;
        while (SystemClock.elapsedRealtime() < end) {
            if (condition.getAsBoolean()) return;
            SystemClock.sleep(100);
        }
        throw new AssertionError(message + "; Linux=" + SessionService.status());
    }
    private void pass(String message) { report.append("PASS: ").append(message).append('\n'); }
    private String screen() { return new String(KittyRuntime.dumpNative(), StandardCharsets.UTF_8); }
    private boolean click(String label) {
        AccessibilityNodeInfo root = getUiAutomation().getRootInActiveWindow();
        if (root == null) return false;
        for (AccessibilityNodeInfo node : root.findAccessibilityNodeInfosByText(label))
            if (node.isClickable() && (label.equalsIgnoreCase(node.getText() == null ? "" : node.getText().toString())
                || label.equalsIgnoreCase(node.getContentDescription() == null ? "" : node.getContentDescription().toString())))
                return node.performAction(AccessibilityNodeInfo.ACTION_CLICK);
        return false;
    }
    private void dismiss() { click("Close"); click("OK"); }
    private Button button(View view, String label) {
        if (view instanceof Button && label.contentEquals(((Button)view).getText())) return (Button)view;
        if (view instanceof ViewGroup) for (int i = 0; i < ((ViewGroup)view).getChildCount(); i++) {
            Button found = button(((ViewGroup)view).getChildAt(i), label); if (found != null) return found;
        }
        return null;
    }
    private void press(String label) {
        runOnMainSync(() -> {
            Button target = button(activity.getWindow().getDecorView(), label);
            require(target != null && target.isShown(), "Missing visible button: " + label); target.performClick();
        });
    }
    private String read(File file) {
        try { return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8); }
        catch (IOException missing) { return ""; }
    }
    @Override public void onStart() {
        Bundle result = new Bundle(); boolean ok = false;
        Context context = getTargetContext();
        String token = UUID.randomUUID().toString().replace("-", "");
        String marker = "GOBLIN_PRIVATE_" + token;
        String guestFile = "/home/goblin/.goblin-product-" + token;
        File exported = new File(context.getCacheDir(), "diagnostics-" + token + ".txt");
        File invalid = new File(context.getCacheDir(), "invalid-backup-" + token);
        try {
            activity = (TerminalActivity)startActivitySync(new Intent(context, TerminalActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            waitFor(() -> { dismiss(); return "Running".equals(SessionService.stateNative()) && screen().contains("goblin@goblin:"); }, "Initial Linux shell", 600000);
            require((activity.getWindow().getAttributes().flags & WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON) == 0,
                "Terminal overrides the user's screen timeout");
            require(button(activity.getWindow().getDecorView(), "Open environment") != null, "Recovery controls are absent");
            require(!button(activity.getWindow().getDecorView(), "Open environment").isShown(), "Startup panel covers a healthy terminal");
            pass("healthy terminal has no startup banner and respects Android screen timeout");

            require(SessionService.inputNative(("printf '%s' '" + marker + "' | tee " + guestFile + "; printf '\\n'\n").getBytes(StandardCharsets.UTF_8)), "Send terminal canary");
            waitFor(() -> new String(SessionService.readGuestNative(guestFile), StandardCharsets.UTF_8).equals(marker), "Durable guest canary");
            String reportText = AppHelp.diagnostics(context);
            require(reportText.contains("App: " + AppHelp.version(context)) && reportText.contains("Host page size:") && reportText.contains("kernel_sha256"), "Missing diagnostic identity");
            require(!reportText.contains(marker) && !reportText.contains(guestFile), "Terminal or guest contents leaked into report");
            runOnMainSync(() -> activity.onActivityResult(42, Activity.RESULT_OK, new Intent().setData(Uri.fromFile(exported))));
            waitFor(() -> read(exported).contains("No terminal contents"), "Diagnostic destination written");
            require(!read(exported).contains(marker), "Exported report leaked terminal content");
            waitFor(() -> click("Close"), "Diagnostic confirmation");
            pass("diagnostic export writes build/device/recovery information and excludes terminal and guest-file canaries");

            runOnMainSync(() -> AppHelp.guide(activity));
            waitFor(() -> click("Close"), "Getting-started help");
            runOnMainSync(() -> AppHelp.licenses(activity));
            waitFor(() -> click("Close"), "Packaged open-source notices");
            require(AppHelp.storage(context).contains("Phone space available:"), "Actual phone free space missing");
            pass("help, notices and actual phone storage are available inside the app");

            File disk = new File(context.getFilesDir(), "uml/rootfs.ext4");
            long originalInode = Os.stat(disk.getAbsolutePath()).st_ino;
            Files.write(invalid.toPath(), new byte[]{1, 2, 3, 4});
            runOnMainSync(() -> context.startForegroundService(new Intent(context, MaintenanceService.class)
                .setAction("restore").setData(Uri.fromFile(invalid))));
            waitFor(() -> read(new File(context.getFilesDir(), "maintenance-result.txt")).contains("restore failed"), "Invalid backup rejection");
            waitFor(() -> !MaintenanceService.keepTerminal() && "Running".equals(SessionService.stateNative()), "Linux restarts after rejected backup");
            waitFor(() -> click("OK"), "Persistent maintenance outcome");
            require(Os.stat(disk.getAbsolutePath()).st_ino == originalInode, "Rejected backup replaced the active disk");
            require(new String(SessionService.readGuestNative(guestFile), StandardCharsets.UTF_8).equals(marker), "Rejected backup lost guest data");
            pass("invalid backup leaves the same disk and guest data intact, restarts Linux and displays a persistent result");

            runOnMainSync(() -> context.startService(new Intent(context, SessionService.class).setAction("stop")));
            waitFor(() -> !SessionService.runningNative() && !SessionService.preparing(), "Clean shutdown");
            // Allow the foreground service to complete its shutdown before a
            // user opens a replacement instance.
            SystemClock.sleep(1500);
            press("Open environment");
            waitFor(() -> "Running".equals(SessionService.stateNative()) && screen().contains("goblin@goblin:"), "Recovery button restart");
            require(new String(SessionService.readGuestNative(guestFile), StandardCharsets.UTF_8).equals(marker), "Restart lost guest data");
            pass("the shutdown screen can reopen Linux with the installation preserved");
            ok = true;
        } catch (Throwable error) { report.append("FAIL: ").append(error).append('\n'); }
        finally {
            if (SessionService.runningNative()) SessionService.inputNative(("rm -f '" + guestFile + "'\n").getBytes(StandardCharsets.UTF_8));
            exported.delete(); invalid.delete();
        }
        report.append(ok ? "GOBLIN PRODUCT PASS\n" : "GOBLIN PRODUCT FAIL\n");
        result.putString("stream", report.toString()); finish(ok ? Activity.RESULT_OK : Activity.RESULT_CANCELED, result);
    }
}
