# Privacy Policy - تلاش برای عبور از فیلترینگ ایران (Iran Filter Bypass)

**Last Updated: February 2026**

## Overview

This privacy policy describes how "Iran Filter Bypass" (the "App") collects, uses, and protects your information.

## Information We Collect

### Network Traffic
- The App routes your internet traffic through VPN tunnels to bypass censorship
- We do NOT log, store, or monitor your browsing activity or traffic content
- Traffic is encrypted end-to-end and we cannot see what you access

### Local Data Storage
- **Configuration files**: V2Ray/XRay configurations are stored locally on your device
- **Settings**: Your preferences (per-app VPN, auto-connect) are stored locally
- **Telegram credentials** (optional): If you choose to connect Telegram, your bot token is stored locally and only used to fetch configurations

### No Server-Side Collection
- We do NOT operate servers that collect user data
- We do NOT track users across apps or websites
- We do NOT collect device identifiers
- We do NOT use analytics services

## Permissions

The App requires the following permissions:

| Permission | Purpose |
|------------|---------|
| `INTERNET` | Required for VPN functionality |
| `ACCESS_NETWORK_STATE` | Check connectivity status |
| `FOREGROUND_SERVICE` | Keep VPN running in background |
| `BIND_VPN_SERVICE` | Android VPN system requirement |
| `POST_NOTIFICATIONS` | Show VPN status notification |
| `QUERY_ALL_PACKAGES` | Per-app VPN feature (select which apps use VPN) |
| `RECEIVE_BOOT_COMPLETED` | Optional auto-start on device boot |

## Third-Party Services

### V2Ray/XRay Core
- The App uses open-source V2Ray/XRay protocol engines
- These run locally on your device
- No data is sent to V2Ray/XRay servers

### GitHub Config Sources
- Configurations may be fetched from public GitHub repositories
- This is optional and only fetches publicly available data
- Your IP address may be visible to GitHub during fetch (standard HTTP behavior)

### Telegram (Optional)
- If you configure Telegram integration, the App may fetch configs from public channels
- This uses the official Telegram Bot API
- Your bot token is stored locally only

## Data Security

- All VPN traffic is encrypted using modern cryptographic protocols
- Local data is stored in the app's private directory
- We use Android's secure storage mechanisms

## Children's Privacy

This App is not intended for children under 13. We do not knowingly collect data from children.

## Changes to This Policy

We may update this policy. The updated date will be shown at the top.

## Contact

For privacy concerns, please open an issue on our GitHub repository.

---

## سیاست حفظ حریم خصوصی (فارسی)

### خلاصه
- ما ترافیک شما را ذخیره یا نظارت نمی‌کنیم
- تمام تنظیمات فقط روی دستگاه شما ذخیره می‌شوند
- هیچ سروری برای جمع‌آوری داده‌های کاربران نداریم
- از سرویس‌های آنالیتیک استفاده نمی‌کنیم
- ترافیک VPN کاملاً رمزنگاری شده است

### مجوزهای برنامه
- `INTERNET`: برای عملکرد VPN
- `VPN_SERVICE`: الزام سیستم اندروید
- `QUERY_ALL_PACKAGES`: برای انتخاب برنامه‌ها در حالت Split Tunneling
