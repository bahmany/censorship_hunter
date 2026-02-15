# XRay Binary Setup for Iranian Free V2Ray

## مشکل فعلی
```
XRay binary not found at: /data/app/ir.filterbypass.app-.../lib/x86/libxray.so
```

## راه‌حل: دانلود و نصب XRay Binary

### گام 1: دانلود XRay Core
از لینک زیر آخرین نسخه XRay را دانلود کنید:
https://github.com/XTLS/Xray-core/releases

### گام 2: انتخاب فایل مناسب برای هر ABI

#### برای ARM 32-bit (armeabi-v7a):
```bash
# دانلود
wget https://github.com/XTLS/Xray-core/releases/download/v1.8.7/Xray-android-arm32-v7a.zip

# استخراج
unzip Xray-android-arm32-v7a.zip

# تغییر نام و کپی
cp xray app/src/main/jniLibs/armeabi-v7a/libxray.so
```

#### برای ARM 64-bit (arm64-v8a):
```bash
wget https://github.com/XTLS/Xray-core/releases/download/v1.8.7/Xray-android-arm64-v8a.zip
unzip Xray-android-arm64-v8a.zip
cp xray app/src/main/jniLibs/arm64-v8a/libxray.so
```

#### برای x86 (Intel 32-bit):
```bash
wget https://github.com/XTLS/Xray-core/releases/download/v1.8.7/Xray-linux-32.zip
unzip Xray-linux-32.zip
cp xray app/src/main/jniLibs/x86/libxray.so
```

#### برای x86_64 (Intel 64-bit):
```bash
wget https://github.com/XTLS/Xray-core/releases/download/v1.8.7/Xray-linux-64.zip
unzip Xray-linux-64.zip
cp xray app/src/main/jniLibs/x86_64/libxray.so
```

### گام 3: ساختار پوشه‌ها
```
app/src/main/jniLibs/
├── armeabi-v7a/
│   └── libxray.so
├── arm64-v8a/
│   └── libxray.so
├── x86/
│   └── libxray.so
└── x86_64/
    └── libxray.so
```

### گام 4: Build مجدد
```bash
./gradlew clean
./gradlew assembleDebug
```

## نکات مهم

1. **اندازه فایل**: هر فایل libxray.so حدود 15-20 MB است
2. **مجوزها**: فایل‌ها باید executable باشند
3. **نام فایل**: حتماً `libxray.so` باشد (نه `xray`)
4. **Git**: این فایل‌ها را به `.gitignore` اضافه کنید (حجم زیاد)

## راه‌حل جایگزین: استفاده از V2Ray-Core

اگر XRay در دسترس نیست، می‌توانید از V2Ray-Core استفاده کنید:
https://github.com/v2fly/v2ray-core/releases

فرآیند مشابه است، فقط فایل را از v2ray-core دانلود کنید.

## تست
بعد از کپی فایل‌ها:
```bash
# نصب روی دستگاه
./gradlew installDebug

# چک لاگ
adb logcat | grep FilterBypassVPN
```

اگر موفق باشد، باید این پیام را ببینید:
```
XRay binary found at: /data/app/.../lib/arm64-v8a/libxray.so
```

## لینک‌های مفید
- XRay Releases: https://github.com/XTLS/Xray-core/releases
- V2Ray Releases: https://github.com/v2fly/v2ray-core/releases
- Documentation: https://xtls.github.io/
