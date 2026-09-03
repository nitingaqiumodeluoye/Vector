package com.example.nathookdemo;

import android.app.Activity;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

/**
 * Minimal control surface for the demo (module's own process only).
 *
 * <p>Install / remove the RegisterNatives hook in this process. Read the intercepted JNI
 * registrations in logcat under the tag {@code VectorNativeHookDemo}.</p>
 */
public final class MainActivity extends Activity {

    private static final String TAG = "VectorNativeHookDemo";

    private TextView status;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        status = new TextView(this);
        status.setText("RegisterNatives hook demo (logcat tag: " + TAG + ")\n\n"
                + "Install/remove below only affect THIS process.\n"
                + "Scoped target processes hook RegisterNatives from native_init.");
        status.setTextSize(14);

        Button install = new Button(this);
        install.setText("安装 RegisterNatives hook");
        install.setOnClickListener(v -> onStartRecording());

        Button unhook = new Button(this);
        unhook.setText("卸载 RegisterNatives hook");
        unhook.setOnClickListener(v -> onStopRecording());

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        root.setPadding(48, 48, 48, 48);
        root.addView(status);
        root.addView(install);
        root.addView(unhook);
        int margin = (int) (12 * getResources().getDisplayMetrics().density);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = margin;
        install.setLayoutParams(lp);
        unhook.setLayoutParams(lp);

        setContentView(root);
    }

    private void onStartRecording() {
        boolean ok = NativeBridge.startRecording();
        String msg = ok ? "RegisterNatives hook installed (see logcat)" : "install failed (see logcat)";
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show();
    }

    private void onStopRecording() {
        boolean ok = NativeBridge.stopRecording();
        String msg = ok ? "RegisterNatives hook removed (see logcat)" : "remove failed (see logcat)";
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show();
    }
}
