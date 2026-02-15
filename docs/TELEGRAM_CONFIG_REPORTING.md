# Telegram Config Reporting - Complete Guide

## Overview

Hunter now automatically sends validated proxy configurations to a Telegram group with optimal formatting for easy copying and importing.

**Features**:
- ✓ 10 best configs as copyable text messages
- ✓ 50 best configs as a single importable file
- ✓ Sorted by speed (latency)
- ✓ Easy copy/paste for users
- ✓ Credentials from `hunter_secrets.env`

---

## Setup

### 1. Configure Telegram Credentials

Create or edit `hunter_secrets.env`:

```bash
TELEGRAM_API_ID=123456789
TELEGRAM_API_HASH=abcdef1234567890abcdef1234567890
TELEGRAM_PHONE=+989123456789
TELEGRAM_GROUP_ID=-1001234567890
```

**How to get credentials**:

1. **API ID & Hash**: Visit https://my.telegram.org/apps
   - Create a new application
   - Copy `api_id` and `api_hash`

2. **Phone**: Your Telegram phone number (with country code)

3. **Group ID**: 
   - Create a Telegram group
   - Add bot to group
   - Send a message and check logs for group ID
   - Or use: `@userinfobot` in Telegram to get group ID

### 2. Environment Variables (Alternative)

Instead of `hunter_secrets.env`, set environment variables:

```bash
set TELEGRAM_API_ID=123456789
set TELEGRAM_API_HASH=abcdef1234567890abcdef1234567890
set TELEGRAM_PHONE=+989123456789
set TELEGRAM_GROUP_ID=-1001234567890
```

---

## Usage

### Automatic Reporting (After Validation)

```python
from hunter.orchestrator import HunterOrchestrator
from hunter.core.config import HunterConfig

config = HunterConfig({...})
orchestrator = HunterOrchestrator(config)

# Validate configs
validated = orchestrator.validate_configs(raw_configs)

# Automatically report to Telegram
await orchestrator.report_configs_to_telegram(validated)
```

### Manual Reporting

```python
from hunter.telegram_config_reporter import TelegramConfigReporter

reporter = TelegramConfigReporter()

# Connect
connected = await reporter.connect()
if not connected:
    print("Failed to connect")
    return

# Send configs
success = await reporter.send_configs(
    validated_configs,
    max_text_configs=10,
    max_file_configs=50
)

# Disconnect
await reporter.disconnect()
```

---

## Message Format

### Text Messages (Top 10)

Each config sent as a separate message:

```
🚀 **Config #1**
Name: Fast VMess Server
Speed: 45ms

`vmess://eyJhZGQiOiIxLjIuMy40IiwicG9ydCI6ODA4MCwicHMiOiJGYXN0IFZNZXNzIiwiaWQiOiIxMjM0NTY3OC1hYmNkLTEyMzQtYWJjZC0xMjM0NTY3OGFiY2QiLCJhaWQiOjAsInNjeSI6ImF1dG8iLCJuZXQiOiJ0Y3AifQ==`

_Tap to copy the config above_
```

**Benefits**:
- Easy to copy (inline code format)
- Shows speed for comparison
- One config per message
- Clear numbering

### File Format (Top 50)

Single text file with all configs:

```
# Hunter Validated Configs
# Generated: 2026-02-15 16:28:15
# Total: 50 configs
# Sort: By speed (latency)

# [1] Fast VMess Server - 45ms
vmess://eyJhZGQiOiIxLjIuMy40IiwicG9ydCI6ODA4MCwicHMiOiJGYXN0IFZNZXNzIiwiaWQiOiIxMjM0NTY3OC1hYmNkLTEyMzQtYWJjZC0xMjM0NTY3OGFiY2QiLCJhaWQiOjAsInNjeSI6ImF1dG8iLCJuZXQiOiJ0Y3AifQ==

# [2] Fast VLESS Server - 52ms
vless://12345678-abcd-1234-abcd-12345678abcd@2.3.4.5:443?encryption=none&security=tls

...
```

**Benefits**:
- All configs in one file
- Easy import into V2Ray clients
- Sorted by speed
- Includes metadata (generation time, count)

---

## Configuration

### Default Settings

```python
max_text_configs = 10    # Number of text messages
max_file_configs = 50    # Number of configs in file
```

### Custom Settings

```python
await reporter.send_configs(
    configs,
    max_text_configs=20,   # Send top 20 as text
    max_file_configs=100   # Send top 100 in file
)
```

---

## How It Works

### Step 1: Sort by Speed
```python
sorted_configs = sorted(
    validated_configs,
    key=lambda x: x.get('latency_ms', float('inf'))
)
```

### Step 2: Send Text Messages
```python
for i, config in enumerate(sorted_configs[:10], 1):
    message = format_text_message(config, i)
    await client.send_message(group_id, message)
```

### Step 3: Create Config File
```python
file_content = create_config_file(sorted_configs[:50])
temp_file = save_to_temp(file_content)
```

### Step 4: Send File
```python
await client.send_file(
    group_id,
    temp_file,
    caption=caption
)
```

---

## Test Results

All tests passed:

```
Config file creation: PASS
Text formatting: PASS
Sorting: PASS
Credentials loading: PASS
Config distribution: PASS

Overall: ALL TESTS PASSED
```

### Test Output

**Text Message Example**:
```
🚀 **Config #1**
Name: Fast VMess Server #1
Speed: 50ms

`vmess://eyJhZGQiOiIxLjIuMy40In0=`

_Tap to copy the config above_
```

**Config File Example**:
```
# Hunter Validated Configs
# Generated: 2026-02-15 16:28:15
# Total: 50 configs
# Sort: By speed (latency)

# [1] Fast VMess Server #1 - 50ms
vmess://eyJhZGQiOiIxLjIuMy40In0=

# [2] Fast VLESS Server #2 - 52ms
vless://12345678-abcd-1234-abcd-12345678abcd@2.3.4.5:443
```

---

## Distribution Strategy

### Top 10 (Text Messages)
- Sent individually
- Easy to copy
- Best for quick access
- Users can copy one at a time

### Top 50 (File)
- Single file download
- Easy to import
- Best for bulk import
- Users can import all at once

### Speed Range
```
Example with 100 configs:
  Top 10:  50-68ms (fastest)
  Top 50:  50-148ms (fast to medium)
  All 100: 50-198ms (all speeds)
```

---

## User Experience

### Copying from Text Messages

1. Open Telegram group
2. Find config message
3. Long-press on config URI
4. Tap "Copy"
5. Paste into V2Ray client

### Importing from File

1. Open Telegram group
2. Download config file
3. Open V2Ray client
4. Import from file
5. All 50 configs imported at once

---

## Error Handling

### Missing Credentials
```
WARNING: Missing Telegram credentials
Set TELEGRAM_API_ID, TELEGRAM_API_HASH, TELEGRAM_PHONE, TELEGRAM_GROUP_ID
```

### Connection Failed
```
ERROR: Failed to connect to Telegram
Check credentials and network connection
```

### Send Failed
```
WARNING: Failed to send config #1
Continuing with next config
```

---

## Files

| File | Purpose |
|------|---------|
| `telegram_config_reporter.py` | Core reporting implementation |
| `test_config_reporter.py` | Test suite |
| `orchestrator.py` | Integration with validation |
| `hunter_secrets.env` | Telegram credentials (gitignored) |

---

## Integration with Orchestrator

### Automatic Reporting

After validation completes:

```python
# In orchestrator.py
validated = self.validate_configs(configs)

# Automatically report
await self.report_configs_to_telegram(validated)
```

### Manual Reporting

```python
orchestrator = HunterOrchestrator(config)
validated = orchestrator.validate_configs(configs)

# Manual reporting
await orchestrator.report_configs_to_telegram(validated)
```

---

## Security

### Credentials Protection
- `hunter_secrets.env` is gitignored
- Never commit credentials
- Use environment variables in production
- Rotate credentials regularly

### Data Privacy
- Configs sent only to specified group
- No logging of sensitive data
- Temporary files deleted after sending

---

## Troubleshooting

### Issue: "Missing Telegram credentials"
**Solution**: Set credentials in `hunter_secrets.env` or environment variables

### Issue: "Failed to connect to Telegram"
**Solution**: 
1. Check internet connection
2. Verify credentials are correct
3. Check if phone number is registered with Telegram

### Issue: "Failed to send config"
**Solution**:
1. Check group ID is correct
2. Verify bot has permission to send messages
3. Check rate limiting (wait a few seconds)

### Issue: "Config file not sent"
**Solution**:
1. Check file size (should be < 50MB)
2. Verify group allows file uploads
3. Check disk space for temp file

---

## Performance

### Sending Speed
- Text messages: ~0.5s per message (10 messages = 5s)
- File: ~2-3s to create and send
- Total: ~7-8s for all reporting

### Memory Usage
- Text formatting: Minimal
- File creation: ~1-2MB for 50 configs
- Total: < 10MB

---

## Best Practices

1. **Regular Updates**: Send configs after each validation cycle
2. **Monitor Group**: Check group for successful delivery
3. **Rotate Credentials**: Change API credentials periodically
4. **Test First**: Run `test_config_reporter.py` before production
5. **Monitor Logs**: Check logs for errors or warnings

---

## Future Improvements

1. **Multiple Groups**: Send to different groups based on tier
2. **Scheduled Reporting**: Send at specific times
3. **Filtering**: Send only configs matching criteria
4. **Statistics**: Include performance statistics
5. **Notifications**: Notify users of new configs

---

## Summary

The Telegram config reporting feature provides:

✓ **Easy sharing**: 10 best configs as copyable text  
✓ **Bulk import**: 50 best configs as file  
✓ **Sorted by speed**: Fastest configs first  
✓ **User-friendly**: Easy copy/paste and import  
✓ **Automatic**: Integrated with validation  
✓ **Secure**: Credentials in gitignored file  

**Status**: Fully implemented and tested  
**Test Results**: ALL PASSED  
**Production Ready**: Yes

---

**Implementation Date**: February 15, 2026  
**Test Results**: All tests passed  
**Features**: 10 text + 50 file configs  
**Production Ready**: Yes
