package dev.goblinlinux.sentry;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Intent;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import java.io.File;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.UUID;
import java.util.function.BooleanSupplier;
import java.util.regex.Pattern;

/** Debug instrumentation: verify the OS lock and Linux progress, not a mock. */
public final class PowerAcceptance extends Instrumentation {
    private final StringBuilder report = new StringBuilder();
    private static final Pattern HELD = Pattern.compile("(?m)^\\s*PARTIAL_WAKE_LOCK\\s+'Goblin:Linux'.*$");
    private void require(boolean value, String message) { if (!value) throw new AssertionError(message); }
    private void waitFor(BooleanSupplier condition, String message) {
        long end = SystemClock.elapsedRealtime() + 90000;
        while (SystemClock.elapsedRealtime() < end) {
            if (condition.getAsBoolean()) return;
            SystemClock.sleep(100);
        }
        throw new AssertionError(message);
    }
    private String shell(String command) {
        try (InputStream input = new ParcelFileDescriptor.AutoCloseInputStream(getUiAutomation().executeShellCommand(command))) {
            ByteArrayOutputStream output = new ByteArrayOutputStream();
            byte[] buffer = new byte[8192]; int size;
            while ((size = input.read(buffer)) != -1) output.write(buffer, 0, size);
            return new String(output.toByteArray(), StandardCharsets.UTF_8);
        } catch (Exception error) { throw new RuntimeException(error); }
    }
    private boolean held() { return HELD.matcher(shell("dumpsys power")).find(); }
    private void awake(boolean value) { runOnMainSync(() -> SessionService.setKeepAwake(getTargetContext(), value)); }
    private void command(String value) {
        require(SessionService.inputNative((value + "\n").getBytes(StandardCharsets.UTF_8)), "PTY input");
    }
    private String read(String path) { return new String(SessionService.readGuestNative(path), StandardCharsets.UTF_8).trim(); }
    private int count(String path) {
        try { return Integer.parseInt(read(path)); } catch (NumberFormatException absent) { return 0; }
    }
    private TerminalActivity startTerminal() {
        return (TerminalActivity)startActivitySync(new Intent(getTargetContext(), TerminalActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK));
    }
    private void pass(String message) { report.append("PASS: ").append(message).append('\n'); }
    @Override public void onCreate(Bundle args) { super.onCreate(args); start(); }
    @Override public void onStart() {
        Bundle result = new Bundle(); boolean ok = false;
        boolean original = SessionService.keepAwake(getTargetContext());
        String token = "/tmp/goblin-power-" + UUID.randomUUID();
        boolean stopped = false;
        try {
            TerminalActivity activity = startTerminal();
            waitFor(() -> "Running".equals(SessionService.stateNative()) && SessionService.selectedNative() != 0 &&
                    SessionService.terminalStateNative(SessionService.selectedNative()) == 1, "Linux ready");
            // The transport opens before PAM and the login shell finish. Early
            // PTY input can be consumed by login rather than the user's shell.
            waitFor(() -> new String(KittyRuntime.dumpNative(), StandardCharsets.UTF_8).contains("goblin@goblin:"), "Login shell ready");
            awake(true); waitFor(this::held, "OS partial wake lock acquisition");
            awake(false); waitFor(() -> !held(), "opt-out releases OS lock");
            require(SessionService.runningNative(), "opt-out must leave Linux running");
            awake(true); waitFor(this::held, "re-enable acquires OS lock");
            pass("keep-awake preference acquires/releases the actual Android partial wake lock without restarting Linux");

            command("sh -c 'i=0; while :; do i=$((i+1)); printf \"%s\\n\" \"$i\" > " + token + "; sleep 1; done' </dev/null >/dev/null 2>&1 & echo $! > " + token + ".pid");
            waitFor(() -> count(token) > 0, "background Linux heartbeat");
            int before = count(token);
            runOnMainSync(() -> activity.moveTaskToBack(true));
            shell("input keyevent KEYCODE_SLEEP");
            SystemClock.sleep(6000);
            require(count(token) >= before + 3, "Linux made no progress with screen off");
            require(held(), "screen-off Linux lost its wake lock");
            pass("background Linux process advances while the terminal is defocused and the screen is off");

            runOnMainSync(() -> getTargetContext().startService(new Intent(getTargetContext(), SessionService.class).setAction("stop")));
            waitFor(() -> !SessionService.runningNative(), "Linux shutdown");
            stopped = true;
            waitFor(() -> !held(), "shutdown releases wake lock");
            pass("Linux shutdown releases the OS wake lock");

            shell("input keyevent KEYCODE_WAKEUP");
            shell("wm dismiss-keyguard");
            startTerminal();
            runOnMainSync(() -> getTargetContext().startForegroundService(new Intent(getTargetContext(), SessionService.class)));
            waitFor(() -> "Running".equals(SessionService.stateNative()), "Linux restart");
            waitFor(this::held, "restart reacquires wake lock");
            pass("restarting Linux reacquires the lock");
            ok = true;
        } catch (Throwable error) {
            report.append("FAIL: ").append(error).append('\n');
        } finally {
            shell("input keyevent KEYCODE_WAKEUP");
            awake(original);
            if (SessionService.runningNative()) {
                // A successful shutdown already terminated the heartbeat. On
                // failure, check its unique command line before sending a signal.
                if (!stopped) command("p=$(cat " + token + ".pid 2>/dev/null); if [ -n \"$p\" ] && grep -Fq '" + token + "' /proc/\"$p\"/cmdline 2>/dev/null; then kill \"$p\"; fi");
                command("rm -f " + token + " " + token + ".pid");
            }
            report.append(ok ? "GOBLIN POWER PASS\n" : "GOBLIN POWER FAIL\n");
            try { Files.write(new File(getTargetContext().getFilesDir(), "power-acceptance-report.txt").toPath(), report.toString().getBytes(StandardCharsets.UTF_8)); }
            catch (Exception error) { report.append(error).append('\n'); ok = false; }
            result.putString("stream", report.toString());
            finish(ok ? Activity.RESULT_OK : Activity.RESULT_CANCELED, result);
        }
    }
}
