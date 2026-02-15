package com.hunter.app;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

public class SimpleMainActivity extends Activity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Create a simple text view
        TextView textView = new TextView(this);
        textView.setText("Hunter Proxy App v2.0.0\n\n" +
                        "Features:\n" +
                        "• Proxy Server Detection\n" +
                        "• V2Ray Protocol Support\n" +
                        "• Anti-DPI Obfuscation\n" +
                        "• Telegram Integration\n" +
                        "• Smart Load Balancing\n\n" +
                        "Status: Native C++ Code Ready\n" +
                        "Build with Android Studio for full features\n\n" +
                        "For full functionality, build with:\n" +
                        "Android Studio -> Build -> Make Project");
        textView.setPadding(20, 20, 20, 20);
        textView.setTextSize(14);

        setContentView(textView);
    }
}
