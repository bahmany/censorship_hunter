package com.hunter.app;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import androidx.appcompat.app.AppCompatActivity;
import com.google.android.material.button.MaterialButton;
import com.google.android.material.textfield.TextInputEditText;
import com.google.android.material.textfield.TextInputLayout;

public class LoginActivity extends AppCompatActivity {

    private TextInputLayout phoneLayout, codeLayout;
    private TextInputEditText phoneInput, codeInput;
    private MaterialButton sendCodeButton, loginButton;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_login);

        phoneLayout = findViewById(R.id.phone_layout);
        phoneInput = findViewById(R.id.phone_input);
        codeLayout = findViewById(R.id.code_layout);
        codeInput = findViewById(R.id.code_input);
        sendCodeButton = findViewById(R.id.send_code_button);
        loginButton = findViewById(R.id.login_button);

        sendCodeButton.setOnClickListener(v -> sendCode());
        loginButton.setOnClickListener(v -> login());
    }

    private void sendCode() {
        String phone = phoneInput.getText().toString().trim();
        if (phone.isEmpty()) {
            phoneLayout.setError("Enter phone number");
            return;
        }
        phoneLayout.setError(null);

        // Open Telegram bot if configured
        String botUsername = HunterNative.nativeGetConfig("bot_username");
        if (!botUsername.isEmpty()) {
            Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse("tg://resolve?domain=" + botUsername + "&start=" + phone));
            startActivity(intent);
        }

        // Show code input
        phoneLayout.setVisibility(View.GONE);
        sendCodeButton.setVisibility(View.GONE);
        codeLayout.setVisibility(View.VISIBLE);
        loginButton.setVisibility(View.VISIBLE);
    }

    private void login() {
        String code = codeInput.getText().toString().trim();
        if (code.isEmpty()) {
            codeLayout.setError("Enter verification code");
            return;
        }
        codeLayout.setError(null);
        String phone = phoneInput.getText().toString().trim();
        // Call native loginTelegram(phone, code)
        loginTelegram(phone, code);
        // On success, start MainActivity
        startActivity(new Intent(this, MainActivity.class));
        finish();
    }

    private native void loginTelegram(String phone, String code);
}
