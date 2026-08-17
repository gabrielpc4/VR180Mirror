package com.vr180mirror.launcher;

// VR180 Spectator launcher for Meta Quest - USB-CABLE ONLY, by design:
// the stream runs at maximum quality (AV1 6144x3072@72, 150 Mbps) and the
// cable is the only transport that guarantees it. The app continuously checks
// the adb-reverse tunnel (localhost), shows pipeline/stream status plus the
// viewing options, and opens the WebXR player in the Quest Browser with those
// options pre-applied. If the cable tunnel is not up, it is not working - on
// purpose. No Wi-Fi fallback.

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;

public class MainActivity extends Activity {

    private static final int HTTP_PORT = 9080;
    private static final String HOST = "127.0.0.1";   // adb-reverse tunnel only

    private TextView statusBig;
    private TextView statusDetail;
    private Button watchBtn;
    private CheckBox headlockBox;
    private CheckBox mutedBox;
    private CheckBox fullpicBox;
    private CheckBox delayBox;
    private SharedPreferences prefs;

    private volatile boolean alive = true;
    private volatile boolean connected = false;
    private volatile boolean streamReady = false;
    private volatile String tracks = "";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = getSharedPreferences("vr180", Context.MODE_PRIVATE);
        buildUi();
        startPolling();
    }

    @Override
    protected void onDestroy() {
        alive = false;
        super.onDestroy();
    }

    // ------------------------------------------------------------------ UI
    private void buildUi() {
        int bg = Color.parseColor("#0e141b");
        int ink = Color.parseColor("#e2eaf1");
        int dim = Color.parseColor("#93a3b1");
        int accent = Color.parseColor("#3ec3dc");

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(bg);
        int pad = dp(28);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("VR180 Spectator");
        title.setTextColor(ink);
        title.setTextSize(30);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);

        TextView sub = new TextView(this);
        sub.setText("USB-cable spectating - max quality only (AV1 6144x3072 @ 72fps)");
        sub.setTextColor(dim);
        sub.setTextSize(15);
        sub.setPadding(0, dp(4), 0, dp(20));
        root.addView(sub);

        statusBig = new TextView(this);
        statusBig.setText("Checking the USB tunnel...");
        statusBig.setTextColor(accent);
        statusBig.setTextSize(19);
        root.addView(statusBig);

        statusDetail = new TextView(this);
        statusDetail.setText("");
        statusDetail.setTextColor(dim);
        statusDetail.setTextSize(14);
        statusDetail.setPadding(0, dp(4), 0, dp(20));
        root.addView(statusDetail);

        headlockBox = new CheckBox(this);
        headlockBox.setText("Hard-lock to the player's view (no free look - intense)");
        headlockBox.setTextColor(ink);
        headlockBox.setTextSize(15);
        headlockBox.setChecked(prefs.getBoolean("headlock", false));
        root.addView(headlockBox);

        mutedBox = new CheckBox(this);
        mutedBox.setText("Start muted");
        mutedBox.setTextColor(ink);
        mutedBox.setTextSize(15);
        mutedBox.setChecked(prefs.getBoolean("muted", true));
        root.addView(mutedBox);

        fullpicBox = new CheckBox(this);
        fullpicBox.setText("Full picture (no edge softening at the image border)");
        fullpicBox.setTextColor(ink);
        fullpicBox.setTextSize(15);
        fullpicBox.setChecked(prefs.getBoolean("fullpic", false));
        root.addView(fullpicBox);

        delayBox = new CheckBox(this);
        delayBox.setText("Prefer delaying over skipping (constant 72fps, ~1.5s buffer)");
        delayBox.setTextColor(ink);
        delayBox.setTextSize(15);
        delayBox.setChecked(prefs.getBoolean("delay", false));
        root.addView(delayBox);

        TextView spacer = new TextView(this);
        spacer.setHeight(dp(20));
        root.addView(spacer);

        watchBtn = new Button(this);
        watchBtn.setText("WATCH");
        watchBtn.setTextSize(20);
        watchBtn.setEnabled(false);
        watchBtn.setOnClickListener(v -> watch());
        root.addView(watchBtn, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(64)));

        TextView hint = new TextView(this);
        hint.setText("Checklist on the PC:\n"
                + "  1. Start-Spectator.ps1 is running\n"
                + "  2. this headset is plugged in with a USB 3 data cable\n"
                + "  3. Connect-SpectatorUSB.ps1 was run after plugging in\n"
                + "This screen re-checks automatically every 2 seconds.");
        hint.setTextColor(dim);
        hint.setTextSize(13);
        hint.setPadding(0, dp(22), 0, 0);
        root.addView(hint);

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(bg);
        scroll.addView(root);
        setContentView(scroll);
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    // ---------------------------------------------------------------- polling
    private void startPolling() {
        new Thread(() -> {
            while (alive) {
                JSONObject j = probe();
                connected = (j != null);
                streamReady = connected && j.optBoolean("ready", false);
                if (connected) {
                    JSONArray t = j.optJSONArray("tracks");
                    StringBuilder sb = new StringBuilder();
                    if (t != null) {
                        for (int i = 0; i < t.length(); i++) {
                            if (i > 0) sb.append(" + ");
                            sb.append(t.optString(i));
                        }
                    }
                    tracks = sb.toString();
                }
                runOnUiThread(this::render);
                try { Thread.sleep(2000); } catch (InterruptedException e) { return; }
            }
        }, "poll").start();
    }

    private void render() {
        if (!connected) {
            statusBig.setText("USB tunnel not detected");
            statusDetail.setText("Plug the cable and run Connect-SpectatorUSB.ps1 on the PC. Cable-only by design: no Wi-Fi fallback.");
            watchBtn.setEnabled(false);
            watchBtn.setText("WATCH");
            return;
        }
        if (streamReady) {
            statusBig.setText("USB tunnel up  -  STREAM LIVE");
            statusDetail.setText("Tracks: " + tracks + "   (video rides the cable)");
            watchBtn.setText("WATCH");
        } else {
            statusBig.setText("USB tunnel up  -  waiting for the stream");
            statusDetail.setText("OBS is not publishing yet; Start-Spectator.ps1 brings it up.");
            watchBtn.setText("OPEN VIEWER (stream not live yet)");
        }
        watchBtn.setEnabled(true);
    }

    /** GET /info over the tunnel; null if unreachable or not our server. */
    private JSONObject probe() {
        HttpURLConnection c = null;
        try {
            URL u = new URL("http://" + HOST + ":" + HTTP_PORT + "/info");
            c = (HttpURLConnection) u.openConnection();
            c.setConnectTimeout(600);
            c.setReadTimeout(900);
            if (c.getResponseCode() != 200) return null;
            BufferedReader r = new BufferedReader(new InputStreamReader(c.getInputStream()));
            StringBuilder sb = new StringBuilder();
            String line;
            while ((line = r.readLine()) != null) sb.append(line);
            JSONObject j = new JSONObject(sb.toString());
            return "vr180mirror".equals(j.optString("app")) ? j : null;
        } catch (Exception e) {
            return null;
        } finally {
            if (c != null) c.disconnect();
        }
    }

    // ---------------------------------------------------------------- watch
    private void watch() {
        prefs.edit()
                .putBoolean("headlock", headlockBox.isChecked())
                .putBoolean("muted", mutedBox.isChecked())
                .putBoolean("fullpic", fullpicBox.isChecked())
                .putBoolean("delay", delayBox.isChecked())
                .apply();
        String url = "http://localhost:" + HTTP_PORT + "/"
                + "?auto=1"
                + "&headlock=" + (headlockBox.isChecked() ? 1 : 0)
                + "&muted=" + (mutedBox.isChecked() ? 1 : 0)
                + "&fullpic=" + (fullpicBox.isChecked() ? 1 : 0)
                + "&delay=" + (delayBox.isChecked() ? 1 : 0);
        startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
    }
}
