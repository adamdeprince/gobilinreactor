package dev.goblinreactor.sentry;

import android.app.*;
import android.content.Intent;
import android.content.ActivityNotFoundException;
import android.content.res.Configuration;
import android.graphics.Color;
import android.hardware.input.InputManager;
import android.os.*;
import android.net.Uri;
import android.provider.Settings;
import android.text.InputType;
import android.view.*;
import android.widget.*;
import java.nio.charset.StandardCharsets;

public final class TerminalActivity extends Activity {
    private final Handler handler = new Handler(Looper.getMainLooper());
    private KittyView terminal;
    private Button control, alt;
    private LinearLayout extraKeys;
    private Button hardwareMenu;
    private LinearLayout runtimePanel, recoveryActions;
    private TextView runtimeStatus;
    private ProgressBar startupProgress;
    private long lastResultCheck;
    private String accessibleTerminal = "";
    private InputManager inputManager;
    private final InputManager.InputDeviceListener inputListener = new InputManager.InputDeviceListener() {
        public void onInputDeviceAdded(int id) { updateKeyboard(); }
        public void onInputDeviceRemoved(int id) { updateKeyboard(); }
        public void onInputDeviceChanged(int id) { updateKeyboard(); }
    };
    private void updateKeyboard() {
        boolean connected = false;
        for (int id : inputManager.getInputDeviceIds()) {
            InputDevice device = inputManager.getInputDevice(id);
            if (device != null && !device.isVirtual() && device.isEnabled()
                && device.getKeyboardType() == InputDevice.KEYBOARD_TYPE_ALPHABETIC
                && device.supportsSource(InputDevice.SOURCE_KEYBOARD)) { connected = true; break; }
        }
        extraKeys.setVisibility(connected ? View.GONE : View.VISIBLE);
        hardwareMenu.setVisibility(connected ? View.VISIBLE : View.GONE);
        terminal.setPhysicalKeyboard(connected);
    }
    private int displayedId;
    private boolean closing;
    private final Runnable refresh = new Runnable() {
        public void run() {
            int id = SessionService.selectedNative();
            if (id != displayedId) { displayedId = id; terminal.terminalChanged(); }
            if (!MaintenanceService.keepTerminal() && SessionService.runningNative()
                && "Running".equals(SessionService.stateNative()) && id > 0 && SessionService.terminalStateNative(id) == 2) {
                if (finishTerminal(id)) return;
            }
            updateRuntimePanel();
            if (SystemClock.uptimeMillis() - lastResultCheck > 1000) {
                lastResultCheck = SystemClock.uptimeMillis(); showMaintenanceResult();
            }
            terminal.requestRender();
            String visible = new String(KittyRuntime.dumpNative(), StandardCharsets.UTF_8);
            String description = "Terminal output\n" + visible.substring(Math.max(0, visible.length() - 12000));
            if (!description.equals(accessibleTerminal)) { accessibleTerminal = description; terminal.setContentDescription(description); }
            handler.postDelayed(this, 100);
        }
    };
    private void updateRuntimePanel() {
        String state = SessionService.status();
        boolean maintenance = MaintenanceService.keepTerminal();
        boolean working = maintenance || SessionService.preparing() || SessionService.runningNative();
        if (!maintenance && SessionService.runningNative() && "Running".equals(state)) {
            runtimePanel.setVisibility(View.GONE); return;
        }
        runtimePanel.setVisibility(View.VISIBLE);
        startupProgress.setVisibility(working ? View.VISIBLE : View.GONE);
        recoveryActions.setVisibility(working ? View.GONE : View.VISIBLE);
        runtimeStatus.setText(maintenance ? MaintenanceService.status() : "Stopped".equals(state)
            ? "The environment is shut down. Your installation is saved." : state);
    }
    private void restartLinux() {
        if (!MaintenanceService.busy()) startForegroundService(new Intent(this, SessionService.class));
    }
    private void requestNotifications() {
        android.content.SharedPreferences preferences = getSharedPreferences("product", MODE_PRIVATE);
        if (Build.VERSION.SDK_INT >= 33 && !preferences.getBoolean("notification-asked", false)
            && checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            preferences.edit().putBoolean("notification-asked", true).apply();
            requestPermissions(new String[]{android.Manifest.permission.POST_NOTIFICATIONS}, 1);
        }
    }
    private void exportDiagnostics() {
        new AlertDialog.Builder(this).setTitle("Save diagnostics")
            .setMessage("Saves the app version, device model, environment status, storage and Android exit reasons. Terminal contents, commands and personal files are excluded. Choose where to save the report; nothing is sent automatically.")
            .setNegativeButton("Cancel", null).setPositiveButton("Choose file", (d, w) -> startActivityForResult(
                new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("text/plain")
                    .putExtra(Intent.EXTRA_TITLE, "goblin-diagnostics-" + new java.text.SimpleDateFormat("yyyyMMdd-HHmm", java.util.Locale.ROOT).format(new java.util.Date()) + ".txt"), 42)).show();
    }
    private void showMaintenanceResult() {
        if (MaintenanceService.keepTerminal() || isFinishing()) return;
        java.io.File interrupted = new java.io.File(getFilesDir(), "maintenance-active.txt");
        java.io.File result = new java.io.File(getFilesDir(), "maintenance-result.txt");
        if (interrupted.exists()) {
            try {
                java.nio.file.Files.write(result.toPath(), "Environment maintenance was interrupted. Review your current installation and Previous disks before retrying. Backups that did not finish may be incomplete.".getBytes(StandardCharsets.UTF_8));
                interrupted.delete();
            } catch (java.io.IOException ignored) { }
        }
        android.content.SharedPreferences preferences = getSharedPreferences("product", MODE_PRIVATE);
        long changed = result.lastModified();
        if (changed == 0 || changed <= preferences.getLong("maintenance-result-seen", 0)) return;
        try {
            String message = new String(java.nio.file.Files.readAllBytes(result.toPath()), StandardCharsets.UTF_8);
            preferences.edit().putLong("maintenance-result-seen", changed).apply();
            new AlertDialog.Builder(this).setTitle("Environment maintenance").setMessage(message)
                .setPositiveButton("OK", null).setNeutralButton("Storage & recovery", (d, w) -> storageDialog()).show();
        } catch (java.io.IOException ignored) { }
    }
    private void storageDialog() {
        new AlertDialog.Builder(this).setTitle("Storage & recovery").setMessage(AppHelp.storage(this))
            .setPositiveButton("Close", null).setNeutralButton("Previous disks", (d, w) -> previousDisks()).show();
    }
    private boolean finishTerminal(int id) {
        SessionService.releaseTerminalNative(id);
        String[] sessions = SessionService.terminalsNative().split("\n");
        if (sessions.length > 0 && !sessions[0].isEmpty()) {
            SessionService.selectNative(Integer.parseInt(sessions[0].split("\t")[0])); terminal.terminalChanged(); return false;
        }
        closing = true; finish(); return true;
    }
    private Button button(String label, String description, Runnable action) {
        Button button = new Button(this); button.setText(label); button.setContentDescription(description);
        button.setAllCaps(false); button.setTextSize(13);
        int[][] states = {{android.R.attr.state_selected}, {}};
        button.setTextColor(new android.content.res.ColorStateList(states, new int[]{Color.BLACK, Color.WHITE}));
        button.setBackgroundTintList(new android.content.res.ColorStateList(states, new int[]{Color.WHITE, Color.BLACK}));
        button.setMinWidth(0); button.setMinimumWidth(0); button.setPadding(0, 0, 0, 0); button.setFocusable(false);
        button.setOnClickListener(v -> { action.run(); terminal.requestFocus(); }); return button;
    }
    private void openTerminal(String user, boolean create) {
        if (SessionService.openTerminalNative(user, create) == 0)
            Toast.makeText(this, "Unable to open terminal: " + SessionService.stateNative(), Toast.LENGTH_SHORT).show();
        else terminal.terminalChanged();
    }
    private void accountDialog(boolean create) {
        EditText name = new EditText(this); name.setSingleLine(true); name.setHint("Username");
        name.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD);
        name.setPadding(32, 16, 32, 16);
        AlertDialog dialog = new AlertDialog.Builder(this).setTitle(create ? "Create account" : "Open terminal as user")
            .setView(name).setNegativeButton("Cancel", null).setPositiveButton(create ? "Create" : "Open", null).create();
        dialog.setOnShowListener(d -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
            String user = name.getText().toString().trim();
            if (!user.matches("[a-z][a-z0-9_-]{0,31}") || (create && user.equals("root"))) { name.setError("Use a regular username: letters, numbers, _ or -"); return; }
            openTerminal(user, create); dialog.dismiss();
        }));
        dialog.show(); name.requestFocus(); dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
    }
    private void menu(View anchor) {
        PopupMenu popup = new PopupMenu(this, anchor);
        popup.getMenu().add("New terminal").setOnMenuItemClickListener(item -> { openTerminal("goblin", false); return true; });
        for (String row : SessionService.terminalsNative().split("\n")) {
            if (row.isEmpty()) continue;
            String[] fields = row.split("\t"); int id = Integer.parseInt(fields[0]);
            popup.getMenu().add("Terminal " + id + " · " + fields[1]).setCheckable(true).setChecked(id == SessionService.selectedNative())
                .setOnMenuItemClickListener(item -> { SessionService.selectNative(id); terminal.terminalChanged(); return true; });
        }
        popup.getMenu().add("Root terminal").setOnMenuItemClickListener(item -> { openTerminal("root", false); return true; });
        popup.getMenu().add("Open as user…").setOnMenuItemClickListener(item -> { accountDialog(false); return true; });
        popup.getMenu().add("Create account…").setOnMenuItemClickListener(item -> { accountDialog(true); return true; });
        popup.getMenu().add("Network access…").setOnMenuItemClickListener(item -> { NetworkSettings.show(this); return true; });
        popup.getMenu().add("Back up environment…").setOnMenuItemClickListener(item -> {
            new AlertDialog.Builder(this).setTitle("Back up environment")
                .setMessage("The environment will shut down cleanly while its files, accounts, packages and configuration are saved, then start again.\n\n" + AppHelp.storage(this))
                .setNegativeButton("Cancel", null).setPositiveButton("Choose file", (d, w) -> startActivityForResult(
                    new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("application/octet-stream")
                        .putExtra(Intent.EXTRA_TITLE, "goblinreactor-" + new java.text.SimpleDateFormat("yyyyMMdd-HHmm", java.util.Locale.ROOT).format(new java.util.Date()) + ".goblin.gz"), 40)).show();
            return true;
        });
        popup.getMenu().add("Restore backup…").setOnMenuItemClickListener(item -> {
            startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"), 41); return true;
        });
        popup.getMenu().add("Previous disks…").setOnMenuItemClickListener(item -> { previousDisks(); return true; });
        popup.getMenu().add("Storage & recovery…").setOnMenuItemClickListener(item -> { storageDialog(); return true; });
        popup.getMenu().add(SessionService.rescueNative() ? "Restart environment…" : "Rescue shell…").setOnMenuItemClickListener(item -> {
            boolean normal = SessionService.rescueNative();
            new AlertDialog.Builder(this).setTitle(normal ? "Restart environment" : "Open rescue shell")
                .setMessage(normal ? "Close the rescue shell and start the installed environment." : "Shut down the environment cleanly and open a root shell with filesystem inspection and repair tools. Your disk is retained.")
                .setNegativeButton("Cancel", null).setPositiveButton("Restart", (d, w) -> startForegroundService(new Intent(this, MaintenanceService.class).setAction(normal ? "normal" : "rescue"))).show();
            return true;
        });
        popup.getMenu().add("Keep environment awake").setCheckable(true).setChecked(SessionService.keepAwake(this))
            .setOnMenuItemClickListener(item -> { SessionService.setKeepAwake(this, !item.isChecked()); return true; });
        popup.getMenu().add("Start environment after reboot").setCheckable(true).setChecked(BootReceiver.enabled(this))
            .setOnMenuItemClickListener(item -> {
                if (item.isChecked()) BootReceiver.setEnabled(this, false);
                else new AlertDialog.Builder(this).setTitle("Start environment after reboot?")
                    .setMessage("After restarting your phone and unlocking it for the first time, the environment will start in the background with its usual notification. Services enabled in the environment can start again. Open shells and unsaved work do not resume.\n\nYour Keep environment awake setting still applies. You can turn this option off here at any time.")
                    .setNegativeButton("Cancel", null).setPositiveButton("Enable", (d, w) -> BootReceiver.setEnabled(this, true)).show();
                return true;
            });
        popup.getMenu().add("Android battery settings…").setOnMenuItemClickListener(item -> {
            PowerManager power = getSystemService(PowerManager.class);
            Intent settings = power.isIgnoringBatteryOptimizations(getPackageName())
                ? new Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS)
                : new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:" + getPackageName()));
            try { startActivity(settings); }
            catch (ActivityNotFoundException unavailable) {
                startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:" + getPackageName())));
            }
            return true;
        });
        popup.getMenu().add("Close terminal").setOnMenuItemClickListener(item -> {
            int id = SessionService.selectedNative();
            if (SessionService.terminalStateNative(id) >= 2) finishTerminal(id);
            else SessionService.closeTerminalNative(id);
            return true;
        });
        popup.getMenu().add("Shut down environment…").setOnMenuItemClickListener(item -> {
            new AlertDialog.Builder(this).setTitle("Shut down environment?")
                .setMessage("This stops every terminal and guest service after syncing the disk. Your files and installed packages remain saved.")
                .setNegativeButton("Cancel", null).setPositiveButton("Shut down", (d, w) ->
                    startService(new Intent(this, SessionService.class).setAction("stop"))).show(); return true;
        });
        popup.getMenu().add("Getting started / About…").setOnMenuItemClickListener(item -> { AppHelp.guide(this); return true; });
        popup.getMenu().add("Open-source notices…").setOnMenuItemClickListener(item -> { AppHelp.licenses(this); return true; });
        popup.getMenu().add("Save diagnostics…").setOnMenuItemClickListener(item -> { exportDiagnostics(); return true; });
        popup.show();
    }
    private void previousDisks() {
        java.io.File directory = new java.io.File(getFilesDir(), "uml");
        java.io.File[] disks = directory.listFiles((dir, name) -> name.startsWith("rootfs.before-restore."));
        if (disks == null || disks.length == 0) {
            new AlertDialog.Builder(this).setMessage("Restoring a backup retains the replaced disk here until you delete it.").setPositiveButton("OK", null).show(); return;
        }
        java.util.Arrays.sort(disks, (a, b) -> Long.compare(b.lastModified(), a.lastModified()));
        String[] names = new String[disks.length];
        java.text.DateFormat dates = java.text.DateFormat.getDateTimeInstance();
        for (int i = 0; i < disks.length; ++i) names[i] = dates.format(new java.util.Date(disks[i].lastModified())) + " · " + disks[i].getName().substring(22);
        new AlertDialog.Builder(this).setTitle("Previous disks").setItems(names, (dialog, which) -> {
            java.io.File disk = disks[which];
            new AlertDialog.Builder(this).setTitle(names[which])
                .setMessage("Restore this disk and retain the current one, or permanently delete this saved copy to free storage.")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Restore", (d, w) -> startForegroundService(new Intent(this, MaintenanceService.class).setAction("previous").putExtra("disk", disk.getName())))
                .setNeutralButton("Delete…", (d, w) -> new AlertDialog.Builder(this).setTitle("Delete this saved disk?")
                    .setMessage("This permanently deletes only the selected previous disk.").setNegativeButton("Cancel", null)
                    .setPositiveButton("Delete", (a, b) -> {
                        if (MaintenanceService.busy() || !disk.delete()) android.widget.Toast.makeText(this, "Could not delete the saved disk; check for ongoing maintenance", android.widget.Toast.LENGTH_LONG).show();
                    }).show()).show();
        }).setNegativeButton("Close", null).show();
    }
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        if (Build.VERSION.SDK_INT >= 30) getWindow().setDecorFitsSystemWindows(false);
        getWindow().setStatusBarColor(Color.BLACK); getWindow().setNavigationBarColor(Color.BLACK);
        LinearLayout page = new LinearLayout(this); page.setOrientation(LinearLayout.VERTICAL); page.setBackgroundColor(Color.BLACK);
        page.setOnApplyWindowInsetsListener((view, insets) -> {
            if (Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                android.graphics.Insets keyboard = insets.getInsets(WindowInsets.Type.ime());
                view.setPadding(bars.left, bars.top, bars.right, Math.max(bars.bottom, keyboard.bottom));
            } else view.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(), insets.getSystemWindowInsetRight(), insets.getSystemWindowInsetBottom());
            return insets;
        });
        terminal = new KittyView(this);
        FrameLayout viewport = new FrameLayout(this);
        viewport.addView(terminal, new FrameLayout.LayoutParams(-1, -1));
        runtimePanel = new LinearLayout(this); runtimePanel.setOrientation(LinearLayout.VERTICAL);
        runtimePanel.setGravity(Gravity.CENTER); runtimePanel.setBackgroundColor(Color.BLACK);
        int padding = Math.round(20 * getResources().getDisplayMetrics().density);
        runtimePanel.setPadding(padding, padding, padding, padding);
        runtimeStatus = new TextView(this); runtimeStatus.setTextColor(Color.WHITE); runtimeStatus.setTextSize(17);
        runtimeStatus.setGravity(Gravity.CENTER); runtimeStatus.setText("Starting environment…");
        runtimeStatus.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        startupProgress = new ProgressBar(this); runtimePanel.addView(startupProgress); runtimePanel.addView(runtimeStatus);
        recoveryActions = new LinearLayout(this); recoveryActions.setOrientation(LinearLayout.VERTICAL);
        recoveryActions.addView(button("Open environment", "Open environment", this::restartLinux));
        recoveryActions.addView(button("Storage & recovery", "Storage and recovery", this::storageDialog));
        recoveryActions.addView(button("Save diagnostics", "Save diagnostics", this::exportDiagnostics));
        runtimePanel.addView(recoveryActions);
        viewport.addView(runtimePanel, new FrameLayout.LayoutParams(-1, -2, Gravity.CENTER));
        int keyHeight = Math.round(48 * getResources().getDisplayMetrics().density);
        hardwareMenu = button("⋮", "Terminal menu", () -> {});
        hardwareMenu.setOnClickListener(this::menu);
        viewport.addView(hardwareMenu, new FrameLayout.LayoutParams(keyHeight, keyHeight, Gravity.TOP | Gravity.END));
        page.addView(viewport, new LinearLayout.LayoutParams(-1, 0, 1));
        extraKeys = new LinearLayout(this);
        extraKeys.setContentDescription("Extra terminal keys");
        String[] labels = {"Esc", "Ctrl", "Alt", "Tab", "←", "↓", "↑", "→", "⋮"};
        String[] descriptions = {"Escape", "Control", "Alt", "Tab", "Left arrow", "Down arrow", "Up arrow", "Right arrow", "Terminal menu"};
        int[] codes = {57344, 0, 0, 57346, 57350, 57353, 57352, 57351};
        for (int i = 0; i < labels.length; i++) {
            final int index = i;
            Button key = button(labels[i], descriptions[i], () -> {
                if (index == 1) terminal.toggleModifier(4);
                else if (index == 2) terminal.toggleModifier(2);
                else if (index < 8) terminal.specialKey(codes[index]);
            });
            if (i == 1) control = key; if (i == 2) alt = key;
            if (i == 8) key.setOnClickListener(this::menu);
            extraKeys.addView(key, new LinearLayout.LayoutParams(0, keyHeight, 1));
        }
        terminal.modifiersChanged = () -> {
            control.setSelected(terminal.modifierSelected(4)); alt.setSelected(terminal.modifierSelected(2));
            control.setTextColor(control.isSelected() ? Color.BLACK : Color.WHITE); alt.setTextColor(alt.isSelected() ? Color.BLACK : Color.WHITE);
            control.setBackgroundTintList(android.content.res.ColorStateList.valueOf(control.isSelected() ? Color.WHITE : Color.BLACK));
            alt.setBackgroundTintList(android.content.res.ColorStateList.valueOf(alt.isSelected() ? Color.WHITE : Color.BLACK));
        };
        page.addView(extraKeys); setContentView(page); terminal.requestFocus();
        inputManager = getSystemService(InputManager.class); updateKeyboard();
        startForegroundService(new Intent(this, SessionService.class));
        android.content.SharedPreferences preferences = getSharedPreferences("product", MODE_PRIVATE);
        boolean welcome = false;
        if (!preferences.getBoolean("welcomed", false)) {
            preferences.edit().putBoolean("welcomed", true).apply();
            welcome = !new java.io.File(getFilesDir(), "uml/rootfs.ext4").exists() && !new java.io.File(getFilesDir(), "debian").exists();
        }
        if (welcome) handler.post(() -> { if (!isFinishing()) AppHelp.guide(this).setOnDismissListener(d -> requestNotifications()); });
        else requestNotifications();
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getAction() == KeyEvent.ACTION_DOWN && (event.isCtrlPressed() || event.isAltPressed())) return terminal.onKeyDown(event.getKeyCode(), event);
        return super.dispatchKeyEvent(event);
    }
    @Override public void onBackPressed() {
        closing = true; SessionService.closeTerminalNative(SessionService.selectedNative()); super.onBackPressed();
    }
    @Override public void onStart() { super.onStart(); inputManager.registerInputDeviceListener(inputListener, handler); updateKeyboard(); }
    @Override public void onStop() { inputManager.unregisterInputDeviceListener(inputListener); super.onStop(); }
    @Override public void onConfigurationChanged(Configuration config) { super.onConfigurationChanged(config); updateKeyboard(); }
    @Override public void onResume() { super.onResume(); terminal.onResume(); if (!closing) handler.post(refresh); }
    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == 42 && result == RESULT_OK && data != null && data.getData() != null) {
            Uri destination = data.getData();
            new Thread(() -> {
                String message;
                try (java.io.OutputStream output = getContentResolver().openOutputStream(destination, "wt")) {
                    if (output == null) throw new java.io.IOException("Cannot open the selected file");
                    output.write(AppHelp.diagnostics(this).getBytes(StandardCharsets.UTF_8));
                    message = "Diagnostics saved. Include what happened and what you expected when reporting the problem.";
                } catch (Exception error) { message = "Could not save diagnostics: " + error.getMessage(); }
                final String response = message;
                runOnUiThread(() -> { if (!isDestroyed()) AppHelp.text(this, "Diagnostics", response); });
            }, "goblin-diagnostics").start();
            return;
        }
        if ((request != 40 && request != 41) || result != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        Intent operation = new Intent(this, MaintenanceService.class).setAction(request == 40 ? "backup" : "restore").setData(uri)
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        if (request == 40) startForegroundService(operation);
        else new AlertDialog.Builder(this).setTitle("Restore this environment backup?")
            .setMessage("The environment will shut down. After the backup is checked, it will replace the current environment. The current disk will be retained for recovery.\n\n" + AppHelp.storage(this))
            .setNegativeButton("Cancel", null).setPositiveButton("Restore", (d, w) -> startForegroundService(operation)).show();
    }
    @Override public void onPause() { handler.removeCallbacks(refresh); terminal.onPause(); super.onPause(); }
}
