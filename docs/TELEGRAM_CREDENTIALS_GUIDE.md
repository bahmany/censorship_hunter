# راهنمای ذخیره‌سازی دائمی Telegram Credentials

## مشکل

در هر بار اجرای Hunter، پیام زیر نمایش داده می‌شود:
```
WARNING | Missing Telegram credentials
```

این به این معنی است که credentials از `hunter_secrets.env` به درستی load نمی‌شوند.

## راه‌حل: ذخیره‌سازی دائمی

### 1. فایل `hunter_secrets.env` را ایجاد کنید

در مسیر اصلی پروژه (`D:\projects\v2ray\pythonProject1\hunter\`):

```bash
# فایل hunter_secrets.env

# Telegram API Credentials (از my.telegram.org دریافت کنید)
TELEGRAM_API_ID=31828870
TELEGRAM_API_HASH=your_api_hash_here

# شماره تلفن (با کد کشور)
TELEGRAM_PHONE=+989125248398

# Bot Token (اختیاری - برای گزارش‌دهی)
TELEGRAM_BOT_TOKEN=your_bot_token_here

# Group ID (اختیاری - برای گزارش‌دهی)
TELEGRAM_GROUP_ID=-1002567385742

# Session Name (نام فایل session)
TELEGRAM_SESSION=hunter_session
```

### 2. Session File

بعد از اولین ورود موفق، Telethon یک فایل session ایجاد می‌کند:
- `hunter_session.session`
- `hunter_session.session-journal`

**این فایل‌ها را حذف نکنید!** آنها شامل اطلاعات احراز هویت شما هستند.

### 3. چگونه API Credentials دریافت کنیم؟

1. به https://my.telegram.org وارد شوید
2. به "API development tools" بروید
3. یک اپلیکیشن جدید ایجاد کنید
4. `api_id` و `api_hash` را کپی کنید
5. در `hunter_secrets.env` قرار دهید

### 4. تنظیمات پیشرفته

#### الف) استفاده از متغیرهای محیطی سیستم

در PowerShell:
```powershell
# برای session فعلی
$env:TELEGRAM_API_ID = "31828870"
$env:TELEGRAM_API_HASH = "your_api_hash"
$env:TELEGRAM_PHONE = "+989125248398"

# برای ذخیره دائمی (System Environment Variables)
[System.Environment]::SetEnvironmentVariable("TELEGRAM_API_ID", "31828870", "User")
[System.Environment]::SetEnvironmentVariable("TELEGRAM_API_HASH", "your_api_hash", "User")
[System.Environment]::SetEnvironmentVariable("TELEGRAM_PHONE", "+989125248398", "User")
```

#### ب) استفاده از فرمت PowerShell در env file

```powershell
# hunter_secrets.env (PowerShell format)
$env:TELEGRAM_API_ID = "31828870"
$env:TELEGRAM_API_HASH = "your_api_hash"
$env:TELEGRAM_PHONE = "+989125248398"
```

## تغییرات اعمال شده در کد

### 1. `core/config.py`

**قبل**:
```python
"api_id": int(os.getenv("HUNTER_API_ID", "31828870")),
"api_hash": os.getenv("HUNTER_API_HASH", ""),
"phone": os.getenv("HUNTER_PHONE", ""),
```

**بعد**:
```python
# Default values are None - will be loaded from env file
"api_id": None,
"api_hash": None,
"phone": None,

# Support multiple env var names
env_mappings = {
    "HUNTER_API_ID": ("api_id", int),
    "TELEGRAM_API_ID": ("api_id", int),  # Added
    "HUNTER_API_HASH": ("api_hash", str),
    "TELEGRAM_API_HASH": ("api_hash", str),  # Added
    "HUNTER_PHONE": ("phone", str),
    "TELEGRAM_PHONE": ("phone", str),  # Added
}
```

### 2. Session File Management

Telethon به طور خودکار session را ذخیره می‌کند:
- **مکان**: در همان مسیری که Hunter اجرا می‌شود
- **نام**: `hunter_session.session`
- **محتوا**: اطلاعات احراز هویت رمزنگاری شده

## نحوه استفاده

### اولین بار (ورود اولیه)

1. فایل `hunter_secrets.env` را با credentials خود ایجاد کنید
2. Hunter را اجرا کنید: `.\run.bat`
3. کد تایید Telegram را وارد کنید
4. اگر 2FA دارید، رمز عبور را وارد کنید
5. Session file ذخیره می‌شود

### دفعات بعدی (بدون نیاز به ورود مجدد)

1. فقط Hunter را اجرا کنید: `.\run.bat`
2. **هیچ کد یا رمزی نیاز نیست!**
3. Session file موجود استفاده می‌شود

## عیب‌یابی

### مشکل 1: همچنان "Missing Telegram credentials" نمایش داده می‌شود

**راه‌حل**:
```bash
# بررسی کنید که فایل وجود دارد
ls hunter_secrets.env

# محتوای فایل را بررسی کنید
cat hunter_secrets.env

# مطمئن شوید که خطوط با # شروع نمی‌شوند (comment نباشند)
```

### مشکل 2: Session file حذف شده است

**راه‌حل**:
```bash
# Session files را بررسی کنید
ls hunter_session.session*

# اگر وجود ندارند، دوباره وارد شوید
.\run.bat
# کد تایید و 2FA را وارد کنید
```

### مشکل 3: "SecurityError" یا "Wrong session"

**راه‌حل**:
```bash
# Session files را حذف کنید
rm hunter_session.session*

# دوباره وارد شوید
.\run.bat
```

### مشکل 4: API credentials اشتباه است

**راه‌حل**:
1. به https://my.telegram.org بروید
2. API credentials را دوباره بررسی کنید
3. در `hunter_secrets.env` تصحیح کنید
4. Hunter را restart کنید

## فرمت‌های پشتیبانی شده برای env file

### فرمت 1: Standard (توصیه می‌شود)
```bash
TELEGRAM_API_ID=31828870
TELEGRAM_API_HASH=abc123def456
TELEGRAM_PHONE=+989125248398
```

### فرمت 2: با Quotes
```bash
TELEGRAM_API_ID="31828870"
TELEGRAM_API_HASH="abc123def456"
TELEGRAM_PHONE="+989125248398"
```

### فرمت 3: PowerShell
```powershell
$env:TELEGRAM_API_ID = "31828870"
$env:TELEGRAM_API_HASH = "abc123def456"
$env:TELEGRAM_PHONE = "+989125248398"
```

## امنیت

### ⚠️ مهم: فایل‌های زیر را در Git commit نکنید!

```gitignore
# .gitignore
hunter_secrets.env
*.session
*.session-journal
```

### ✅ بهترین روش‌ها

1. **فایل env را gitignore کنید**
2. **Session files را backup بگیرید** (در مکان امن)
3. **API credentials را به اشتراک نگذارید**
4. **از 2FA استفاده کنید** (امنیت بیشتر)

## خلاصه

### قبل از Fix:
```
❌ هر بار: "Missing Telegram credentials"
❌ هر بار: نیاز به ورود مجدد
❌ هر بار: وارد کردن کد و 2FA
```

### بعد از Fix:
```
✅ یکبار: ورود و ذخیره session
✅ همیشه: استفاده خودکار از session
✅ بدون نیاز: به ورود مجدد
```

## مثال کامل

### فایل `hunter_secrets.env`:
```bash
# Telegram Credentials
TELEGRAM_API_ID=31828870
TELEGRAM_API_HASH=1234567890abcdef1234567890abcdef
TELEGRAM_PHONE=+989125248398

# Bot (Optional)
TELEGRAM_BOT_TOKEN=1234567890:ABCdefGHIjklMNOpqrsTUVwxyz
TELEGRAM_GROUP_ID=-1002567385742

# Session
TELEGRAM_SESSION=hunter_session

# SSH Servers (Optional)
SSH1_HOST=71.143.156.145
SSH1_PORT=2
SSH1_USER=deployer
SSH1_PASS=009100mohammad_mrb
```

### اولین اجرا:
```
INFO | Loading configuration from hunter_secrets.env
INFO | Telegram credentials loaded successfully
INFO | Connecting to Telegram...
INFO | Please enter verification code: 12345
INFO | Please enter 2FA password: ******
INFO | Successfully authenticated
INFO | Session saved to hunter_session.session
```

### اجراهای بعدی:
```
INFO | Loading configuration from hunter_secrets.env
INFO | Telegram credentials loaded successfully
INFO | Using existing session: hunter_session.session
INFO | Connected to Telegram successfully
✅ بدون نیاز به ورود مجدد!
```

---

**وضعیت**: ✅ **کامل شده**  
**تاریخ**: 2026-02-15  
**نسخه**: Enhanced with Persistent Credentials  
**امنیت**: Session-based authentication با 2FA support
