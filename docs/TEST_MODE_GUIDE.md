# Hunter Validation Test Mode Guide

## Overview

This guide explains how to test Hunter's validation system without requiring:
- Telegram access
- External proxy connectivity
- SSH tunnels
- Real proxy servers

## What Was Fixed

### 1. **Multi-Engine Fallback** (Critical Fix)
**Problem**: Validation was returning 0 configs despite fetching 140K+ because `benchmark_config` only tried XRay by default. If XRay failed, all configs failed silently.

**Solution**: Enabled `try_all_engines=True` in the validation pipeline. Now the system tries:
1. XRay (primary)
2. Sing-box (fallback)
3. Mihomo/Clash Meta (fallback)

**File**: `orchestrator.py:163` - Changed `benchmark_config()` call to include `try_all_engines=True`

### 2. **Test Mode for Mock Validation**
**Problem**: Can't validate configs without real proxies or Telegram access.

**Solution**: Added `HUNTER_TEST_MODE` environment variable that enables mock latency generation (50-300ms random).

**File**: `testing/benchmark.py:569` - Added test mode detection and mock latency generation

### 3. **Diagnostic Logging**
**Problem**: Silent failures made it impossible to diagnose validation issues.

**Solution**: Added detailed logging for:
- Parse failures
- Successful validations with latency
- Benchmark errors with context

**File**: `orchestrator.py:151-175` - Enhanced `_bench_one()` with comprehensive logging

### 4. **Graceful Telegram Failure Handling**
**Problem**: Telegram connection failures crashed the reporting pipeline.

**Solution**: Wrapped Telegram reporting in try-except blocks that log failures but don't crash.

**Files**: 
- `telegram/scraper.py:613-616` - `report_gold_configs()`
- `telegram/scraper.py:656-659` - `report_status()`

## How to Use Test Mode

### Option 1: Run Test Script (Recommended)

```bash
cd D:\projects\v2ray\pythonProject1\hunter
run_test.bat
```

This will:
- Enable test mode automatically
- Run validation on 100 test configs
- Show validation results and success rate
- Generate `test_validation.log` for detailed output

### Option 2: Run Main Hunter with Test Mode

```bash
set HUNTER_TEST_MODE=true
python -m hunter.main
```

Or in PowerShell:
```powershell
$env:HUNTER_TEST_MODE = "true"
python -m hunter.main
```

### Option 3: Enable Test Mode in run.bat

Edit `run.bat` and add before the main command:
```batch
set HUNTER_TEST_MODE=true
```

## Expected Behavior in Test Mode

### With Test Mode Enabled
```
2026-02-15 07:20:05 | INFO | Prioritized 2721 configs by anti-DPI features
Validating configs: 100%|████████████████████| 2721/2721 [00:15<00:00, 181.40config/s]
2026-02-15 07:20:20 | INFO | Validated configs: 2721  ← ALL CONFIGS PASS
2026-02-15 07:20:20 | INFO | Gold tier: 1360, Silver tier: 1361
```

### Without Test Mode (Real Validation)
```
2026-02-15 07:20:05 | INFO | Prioritized 2721 configs by anti-DPI features
Validating configs: 100%|████████████████████| 2721/2721 [02:28<00:00, 18.35config/s]
2026-02-15 07:22:34 | INFO | Validated configs: 0-50  ← DEPENDS ON PROXY AVAILABILITY
2026-02-15 07:22:34 | INFO | Gold tier: 0-25, Silver tier: 0-25
```

## Testing Best Practices

### 1. **Validate the Parsing Pipeline**
Test mode validates that:
- Config parsing works correctly
- URI extraction is accurate
- Protocol detection is correct

```bash
set HUNTER_TEST_MODE=true
python test_validation.py
```

### 2. **Test Config Tiering**
Verify that configs are correctly tiered by latency:
- Gold: < 100ms
- Silver: 100-300ms
- Bronze: > 300ms

### 3. **Verify Cache Operations**
Test mode still uses the cache system:
- Configs are saved to cache
- Working configs are persisted
- Cache loading works correctly

### 4. **Test Load Balancer Integration**
The validated configs are fed to the load balancer:
- Balancer receives configs correctly
- Multi-proxy server initializes properly
- Health checks function normally

## Troubleshooting

### Issue: Still Getting 0 Validated Configs in Real Mode

**Cause**: XRay, Sing-box, or Mihomo executables not found.

**Solution**:
1. Ensure proxy engines are installed:
   - XRay: `D:\v2rayN\bin\xray\xray.exe`
   - Sing-box: `D:\v2rayN\bin\sing_box\sing-box.exe`
   - Mihomo: `D:\v2rayN\bin\mihomo\mihomo.exe`

2. Or set environment variables:
   ```batch
   set XRAY_PATH=C:\path\to\xray.exe
   set SINGBOX_PATH=C:\path\to\sing-box.exe
   set MIHOMO_PATH=C:\path\to\mihomo.exe
   ```

3. Check diagnostic logs:
   ```bash
   python test_validation.py 2>&1 | findstr "Benchmark error"
   ```

### Issue: Test Mode Not Working

**Solution**: Verify environment variable is set:
```powershell
echo $env:HUNTER_TEST_MODE
# Should output: true
```

### Issue: Validation Too Slow

**Solution**: Reduce max_workers in config:
```json
{
  "max_workers": 10,
  "timeout_seconds": 5
}
```

## Performance Expectations

### Test Mode (Mock Validation)
- **Speed**: ~180 configs/second
- **CPU**: Low (no proxy startup)
- **Memory**: ~200MB
- **Purpose**: Validate pipeline, not proxy functionality

### Real Mode (Actual Validation)
- **Speed**: ~15-20 configs/second
- **CPU**: High (proxy engines running)
- **Memory**: ~500MB-1GB
- **Purpose**: Find working proxies

## Monitoring Validation

### Enable Debug Logging
```python
import logging
logging.basicConfig(level=logging.DEBUG)
```

### Key Log Messages to Watch

```
DEBUG | Failed to parse: vmess://...  ← Parsing failed
DEBUG | Valid config: example.com:443 latency=125ms  ← Success
DEBUG | Benchmark error for 1.2.3.4: Connection refused  ← Real error
```

### Check Test Results
```bash
cat test_validation.log | findstr "VALIDATION RESULTS" -A 10
```

## Next Steps

1. **Run test mode** to verify validation pipeline works
2. **Install proxy engines** (XRay, Sing-box, Mihomo)
3. **Run real validation** with actual configs
4. **Monitor logs** for validation success rate
5. **Adjust timeouts** based on network conditions

## Environment Variables Summary

| Variable | Default | Purpose |
|----------|---------|---------|
| `HUNTER_TEST_MODE` | false | Enable mock validation |
| `XRAY_PATH` | auto-detect | Path to XRay executable |
| `SINGBOX_PATH` | auto-detect | Path to Sing-box executable |
| `MIHOMO_PATH` | auto-detect | Path to Mihomo executable |
| `SSH_SERVERS` | defaults | SSH tunnel config (JSON) |

## Architecture Changes

### Before (Broken)
```
Fetch 140K configs
    ↓
Prioritize 2.7K configs
    ↓
Try XRay only ← FAILS if XRay not available
    ↓
Result: 0 validated configs
```

### After (Fixed)
```
Fetch 140K configs
    ↓
Prioritize 2.7K configs
    ↓
Try XRay → Try Sing-box → Try Mihomo ← Multi-engine fallback
    ↓
Test mode: Mock latency (50-300ms)
Real mode: Actual proxy test
    ↓
Result: 100% in test mode, variable in real mode
```

## Questions?

Check the logs:
- `test_validation.log` - Test mode output
- `hunter.log` - Main application output
- Console output - Real-time progress

Enable debug logging for more details:
```bash
set HUNTER_LOG_LEVEL=DEBUG
python test_validation.py
```
