package dev.goblinreactor.sentry;

import android.app.*;
import android.text.InputType;
import android.view.View;
import android.widget.*;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

final class NetworkSettings {
    static native String applyNative(String rules);
    private final Activity activity;
    private final List<String> rules = new ArrayList<>();
    private final LinearLayout rows;
    private final TextView status;
    private final AlertDialog dialog;
    static void show(Activity activity) { new NetworkSettings(activity).dialog.show(); }
    private NetworkSettings(Activity a) {
        activity = a;
        try { for (String line : new String(Files.readAllBytes(new File(a.getFilesDir(), "uml/ports.conf").toPath()), StandardCharsets.UTF_8).split("\n")) if (!line.isEmpty()) rules.add(line); }
        catch (Exception absent) { }
        LinearLayout body = new LinearLayout(a); body.setOrientation(LinearLayout.VERTICAL); body.setPadding(24, 16, 24, 8);
        TextView help = new TextView(a); help.setText("Forward a phone port to a guest server listening on 0.0.0.0 or ::. Local rules are reachable from Android apps at localhost. DNS follows Android automatically."); body.addView(help);
        rows = new LinearLayout(a); rows.setOrientation(LinearLayout.VERTICAL); body.addView(rows);
        Button add = new Button(a); add.setText("Add port"); add.setOnClickListener(v -> add()); body.addView(add);
        status = new TextView(a); body.addView(status);
        ScrollView scroll = new ScrollView(a); scroll.addView(body);
        dialog = new AlertDialog.Builder(a).setTitle("Network access").setView(scroll).setNegativeButton("Cancel", null).setPositiveButton("Apply", null).create();
        dialog.setOnShowListener(d -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
            final String config = String.join("\n", rules) + (rules.isEmpty() ? "" : "\n");
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(false); status.setText("Applying port forwarding…");
            new Thread(() -> {
                String error = applyNative(config);
                a.runOnUiThread(() -> {
                    if (error.isEmpty()) { Toast.makeText(a, "Port forwarding updated", Toast.LENGTH_SHORT).show(); dialog.dismiss(); }
                    else { status.setText(error); dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(true); }
                });
            }, "goblin-ports").start();
        }));
        render();
    }
    private void render() {
        rows.removeAllViews();
        for (int i = 0; i < rules.size(); i++) {
            final int index = i; String[] fields = rules.get(i).split("\\s+");
            if (fields.length != 4) continue;
            LinearLayout row = new LinearLayout(activity);
            TextView text = new TextView(activity); text.setText(fields[0].toUpperCase() + " " + fields[1] + " → " + fields[2] + (fields[3].equals("lan") ? " · LAN" : " · this phone"));
            row.addView(text, new LinearLayout.LayoutParams(0, -2, 1));
            Button remove = new Button(activity); remove.setText("Remove"); remove.setOnClickListener(v -> { rules.remove(index); render(); }); row.addView(remove); rows.addView(row);
        }
    }
    private void add() {
        LinearLayout body = new LinearLayout(activity); body.setOrientation(LinearLayout.VERTICAL); body.setPadding(24, 8, 24, 8);
        Spinner protocol = new Spinner(activity); protocol.setAdapter(new ArrayAdapter<>(activity, android.R.layout.simple_spinner_dropdown_item, new String[]{"TCP", "UDP"})); body.addView(protocol);
        EditText host = new EditText(activity), guest = new EditText(activity);
        host.setHint("Phone port, e.g. 8080"); guest.setHint("Guest port, e.g. 8080");
        host.setInputType(InputType.TYPE_CLASS_NUMBER); guest.setInputType(InputType.TYPE_CLASS_NUMBER);
        body.addView(host); body.addView(guest);
        CheckBox lan = new CheckBox(activity); lan.setText("Allow connections from other devices on the network"); body.addView(lan);
        AlertDialog add = new AlertDialog.Builder(activity).setTitle("Add forwarded port").setView(body).setNegativeButton("Cancel", null).setPositiveButton("Add", null).create();
        add.setOnShowListener(d -> add.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
            try {
                int h = Integer.parseInt(host.getText().toString()), g = Integer.parseInt(guest.getText().toString());
                if (h < 1 || h > 65535 || g < 1 || g > 65535) throw new NumberFormatException();
                rules.add(protocol.getSelectedItem().toString().toLowerCase(java.util.Locale.ROOT) + " " + h + " " + g + " " + (lan.isChecked() ? "lan" : "local")); render(); add.dismiss();
            } catch (NumberFormatException error) { host.setError("Use ports between 1 and 65535"); }
        }));
        add.show();
    }
}
