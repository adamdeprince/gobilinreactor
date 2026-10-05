package dev.goblinreactor.sentry;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Typeface;
import android.os.*;
import android.view.WindowManager;
import android.widget.*;
import java.nio.charset.StandardCharsets;

public final class HarnessActivity extends Activity {
    private final Handler handler = new Handler(Looper.getMainLooper());
    private TextView report;
    private final Runnable refresh = new Runnable() {
        public void run() {
            String text = new String(SessionService.transcriptNative(), StandardCharsets.UTF_8);
            report.setText(SessionService.stateNative() + "\n\n" + text.substring(Math.max(0, text.length() - 12000)));
            handler.postDelayed(this, 500);
        }
    };
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        ScrollView scroll = new ScrollView(this); report = new TextView(this);
        report.setTypeface(Typeface.MONOSPACE); report.setTextSize(12); report.setPadding(24, 80, 24, 32);
        scroll.addView(report); setContentView(scroll);
        if (saved == null) startForegroundService(new Intent(this, SessionService.class).setAction("tests")
                .putExtra("mode", getIntent().getIntExtra("mode", getIntent().getBooleanExtra("persistent", false) ? 1 : 0)));
    }
    @Override public void onResume() { super.onResume(); handler.post(refresh); }
    @Override public void onPause() { handler.removeCallbacks(refresh); super.onPause(); }
}
