@echo off
echo Creating Google Play release keystore for Iran Filter Bypass...
echo.

echo You will be prompted to enter keystore password and key information.
echo Use a strong password and store it securely in CI/CD environment variables.
echo.

keytool -genkeypair -v -keystore release.keystore -alias filterbypass -keyalg RSA -keysize 2048 -validity 10000 -storetype PKCS12

echo.
echo Keystore created successfully!
echo.
echo For Google Play upload, set these environment variables in your CI/CD:
echo FILTERBYPASS_STORE_PASSWORD=your_keystore_password
echo FILTERBYPASS_KEY_ALIAS=filterbypass
echo FILTERBYPASS_KEY_PASSWORD=your_key_password
echo.
echo Keep this keystore file secure and never commit it to version control!
pause
