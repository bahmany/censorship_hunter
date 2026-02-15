package com.hunter.app;

import android.os.Bundle;
import android.view.MenuItem;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;

import com.google.android.material.appbar.MaterialToolbar;
import com.google.android.material.button.MaterialButton;
import com.google.android.material.textfield.TextInputEditText;

import java.io.File;
import java.io.FileOutputStream;

/**
 * Activity for setting up Telegram bot credentials.
 * Used for receiving configs from Telegram channels and sending reports.
 */
public class TelegramSetupActivity extends AppCompatActivity {

    private TextInputEditText botTokenInput, chatIdInput;
    private MaterialButton saveButton, testButton;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_telegram_setup);

        MaterialToolbar toolbar = findViewById(R.id.toolbar);
        setSupportActionBar(toolbar);
        if (getSupportActionBar() != null) {
            getSupportActionBar().setDisplayHomeAsUpEnabled(true);
            getSupportActionBar().setTitle("تنظیم تلگرام");
        }

        initViews();
        loadCurrentSettings();
        setupListeners();
    }

    private void initViews() {
        botTokenInput = findViewById(R.id.bot_token_input);
        chatIdInput = findViewById(R.id.chat_id_input);
        saveButton = findViewById(R.id.save_button);
        testButton = findViewById(R.id.test_button);
    }

    private void loadCurrentSettings() {
        String botToken = HunterNative.nativeGetConfig("bot_token");
        String chatId = HunterNative.nativeGetConfig("chat_id");
        
        if (botToken != null && !botToken.isEmpty()) {
            botTokenInput.setText(botToken);
        }
        if (chatId != null && !chatId.isEmpty()) {
            chatIdInput.setText(chatId);
        }
    }

    private void setupListeners() {
        saveButton.setOnClickListener(v -> saveSettings());
        testButton.setOnClickListener(v -> testConnection());
    }

    private void saveSettings() {
        String botToken = botTokenInput.getText() != null ? botTokenInput.getText().toString().trim() : "";
        String chatId = chatIdInput.getText() != null ? chatIdInput.getText().toString().trim() : "";

        if (botToken.isEmpty()) {
            botTokenInput.setError("توکن ربات الزامی است");
            return;
        }
        if (chatId.isEmpty()) {
            chatIdInput.setError("شناسه چت الزامی است");
            return;
        }

        try {
            // Save to secrets.env file
            File secretsFile = new File(getFilesDir(), "secrets.env");
            StringBuilder content = new StringBuilder();
            content.append("bot_token=").append(botToken).append("\n");
            content.append("chat_id=").append(chatId).append("\n");

            try (FileOutputStream fos = new FileOutputStream(secretsFile)) {
                fos.write(content.toString().getBytes());
            }

            // Reinitialize native config
            HunterCallbackImpl callback = FilterBypassApplication.getInstance().getCallbackImpl();
            HunterNative.nativeInit(getFilesDir().getAbsolutePath(), secretsFile.getAbsolutePath(), callback);

            Toast.makeText(this, "تنظیمات ذخیره شد", Toast.LENGTH_SHORT).show();
            finish();

        } catch (Exception e) {
            Toast.makeText(this, "خطا در ذخیره: " + e.getMessage(), Toast.LENGTH_SHORT).show();
        }
    }

    private void testConnection() {
        String botToken = botTokenInput.getText() != null ? botTokenInput.getText().toString().trim() : "";
        String chatId = chatIdInput.getText() != null ? chatIdInput.getText().toString().trim() : "";

        if (botToken.isEmpty() || chatId.isEmpty()) {
            Toast.makeText(this, "ابتدا توکن و شناسه چت را وارد کنید", Toast.LENGTH_SHORT).show();
            return;
        }

        testButton.setEnabled(false);
        testButton.setText("در حال تست...");

        new Thread(() -> {
            boolean success = testTelegramConnection(botToken, chatId);
            runOnUiThread(() -> {
                testButton.setEnabled(true);
                testButton.setText("تست اتصال");
                if (success) {
                    Toast.makeText(this, "✓ اتصال موفق", Toast.LENGTH_SHORT).show();
                } else {
                    Toast.makeText(this, "✗ اتصال ناموفق - توکن یا شناسه را بررسی کنید", Toast.LENGTH_LONG).show();
                }
            });
        }).start();
    }

    private boolean testTelegramConnection(String botToken, String chatId) {
        try {
            String url = "https://api.telegram.org/bot" + botToken + "/getChat?chat_id=" + chatId;
            java.net.HttpURLConnection conn = (java.net.HttpURLConnection) new java.net.URL(url).openConnection();
            conn.setConnectTimeout(10000);
            conn.setReadTimeout(10000);
            int responseCode = conn.getResponseCode();
            conn.disconnect();
            return responseCode == 200;
        } catch (Exception e) {
            return false;
        }
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        if (item.getItemId() == android.R.id.home) {
            finish();
            return true;
        }
        return super.onOptionsItemSelected(item);
    }
}
