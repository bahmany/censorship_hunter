# Hunter v1.3.0 Release Notes

## Overview

Hunter v1.3.0 introduces a comprehensive packet-level DPI bypass system that effectively circumvents Iranian ISP censorship through proven techniques like TLS fragmentation, TTL manipulation, and targeted edge router bypass.

## Major New Features

### 🚀 Packet-Level DPI Bypass System

**TLS ClientHello Fragmentation**
- Splits TLS handshake into 100-200 byte chunks with 10-20ms delays
- Bypasses SNI-based filtering used by Iranian ISPs (TCI, Irancell, MCI)
- Automatically applied to all TLS proxy connections
- Zero configuration required - enabled by default

**TTL Manipulation**
- Configurable packet TTL to expire before DPI boxes (typically hop 8-12)
- Optional feature that can be enabled via .env configuration
- Works by making packets expire before reaching ISP DPI equipment

**TCP Fragmentation**
- Forces small MSS (536 bytes) and disables Nagle's algorithm
- Additional evasion layer for pattern-based DPI detection
- Minimal performance impact (~5% throughput reduction)

**Domain Fronting & Fake SNI**
- CDN-based destination hiding for compatible proxies
- Fake SNI injection for experimental evasion techniques
- GoodbyeDPI-style methods including HTTP Host fragmentation

### 🎯 Edge Router Bypass Enhancements

**Domestic Zone Filtering**
- Bypass methods now apply ONLY to Iranian/domestic network hops
- Automatically filters out international hops using IP range detection
- Prevents unnecessary attempts on foreign network equipment

**MAC Address Display**
- Shows resolved MAC addresses for each domestic hop in detailed logs
- Helps identify specific network equipment in the path
- Transparent logging with hop categorization

**Manual Testing API**
```cpp
// Test bypass on specific IP
orchestrator.testBypassOnSpecificIp("192.168.1.1");

// Get list of domestic hops with MAC addresses
auto hops = orchestrator.getDomesticHopsWithMac();
```

**Transparent Logging**
- Step-by-step bypass execution logs
- Clear categorization: [DOMESTIC], [INTERNATIONAL], [OK], [--]
- MAC address prominently displayed for each hop

### 🔧 XRay Integration

**Automatic Fragmentation**
All TLS proxy connections automatically receive:
```json
{
  "streamSettings": {
    "tlsSettings": {
      "fragment": {
        "packets": "tlshello",
        "length": "100-200",
        "interval": "10-20"
      }
    }
  }
}
```

**Zero Configuration**
- Bypass enabled by default
- No user setup required
- Works transparently with existing proxy configurations

## What's Changed

### DPI Evasion Strategy Shift

**From Route Injection to Packet-Level**
- Previous: Local route injection (ineffective against ISP-level filtering)
- New: Packet manipulation (proven to work against Iranian DPI)

**Targeted Application**
- Bypass methods now target only domestic (Iranian) network equipment
- International hops are automatically filtered out
- More efficient and effective bypass execution

### Enhanced Logging & Transparency

**Detailed Hop Information**
```
[DOMESTIC] Hop 1: 192.168.1.1 | MAC: AA:BB:CC:DD:EE:FF
[DOMESTIC] Hop 2: 10.20.30.1 | MAC: BB:CC:DD:EE:FF:00
[DOMESTIC] Hop 3: 5.160.1.1 | MAC: Not resolved
[*] Identified 3 domestic (Iranian) hops
[*] Skipping 8 international hops
```

**Step-by-Step Bypass Execution**
```
[*] ════════════════════════════════════════════
[*] Processing DOMESTIC hop 1: 192.168.1.1
[*]   MAC Address: AA:BB:CC:DD:EE:FF
[*]   Type: Huawei Switch
[*] ════════════════════════════════════════════
[*]   Trying Huawei SRv6...
[OK]   Huawei SRv6 succeeded on 192.168.1.1
```

## Technical Implementation

### New Files Added

**Core Bypass Module**
- `include/security/packet_bypass.h` (352 lines)
- `src/security/packet_bypass.cpp` (485 lines)

**API Classes**
- `PacketBypass` - Main packet manipulation class
- `GoodbyeDpiBypass` - GoodbyeDPI-style techniques
- `IranianDpiBypass` - Iranian-specific strategies

### Configuration

**Environment Variables (.env)**
```bash
# DPI Bypass Settings
DPI_BYPASS_ENABLED=true
DPI_TLS_FRAGMENT=true
DPI_FRAGMENT_SIZE=100
DPI_FRAGMENT_DELAY=10
DPI_TTL_TRICK=false
DPI_TTL_VALUE=8
```

**Programmatic Configuration**
```cpp
DpiEvasionOrchestrator::PacketBypassSettings settings;
settings.tls_fragment = true;
settings.fragment_size = 100;
settings.fragment_delay_ms = 10;
orchestrator.setPacketBypassSettings(settings);
```

## Performance Impact

**Latency**
- TLS Fragmentation: ~10-20ms added latency (negligible)
- TTL Manipulation: No overhead
- TCP Fragmentation: ~5% throughput reduction

**Resource Usage**
- Minimal CPU/Memory overhead
- No additional processes required
- Integrated into existing XRay workflows

## Compatibility

**Supported ISPs**
- ✅ TCI (Telecommunication Company of Iran)
- ✅ Irancell (Mobile Communication Company of Iran)
- ✅ MCI (Mobile Communications Infrastructure Company)
- ✅ Other Iranian ISPs using DPI boxes at hops 8-12

**Proxy Protocols**
- ✅ VMess
- ✅ VLESS
- ✅ Trojan
- ✅ Shadowsocks
- ✅ Hysteria2
- ✅ TUIC

**Windows Versions**
- ✅ Windows 10 (1809+)
- ✅ Windows 11
- ✅ Windows Server 2019+

## Installation

### Automated Setup
```bash
# Run as Administrator
setup.bat
```

### Manual Setup
1. Extract release archive
2. Run `bin\huntercensor.exe`
3. Configure proxy settings: 127.0.0.1:10808 (SOCKS5)

### Configuration
Edit `.env` file to customize DPI bypass settings:
- Enable/disable specific bypass techniques
- Adjust fragment sizes and delays
- Configure TTL values

## Usage

### Automatic Mode (Default)
1. Start Hunter
2. All TLS connections automatically use fragmentation
3. No configuration needed

### Manual Testing
Use the API to test specific IPs:
```cpp
orchestrator.testBypassOnSpecificIp("192.168.1.1");
```

### View Domestic Hops
```cpp
auto hops = orchestrator.getDomesticHopsWithMac();
for (const auto& [ip, mac, ttl] : hops) {
    std::cout << "Hop " << ttl << ": " << ip << " | MAC: " << mac << std::endl;
}
```

## Troubleshooting

### Bypass Not Working
1. Check if target is TLS connection
2. Verify fragment settings (100-200 bytes)
3. Try combined approach (TLS + TTL)
4. Check logs for domestic hop detection

### Connection Fails
1. Increase fragment size to 150 bytes
2. Reduce delay to 5ms
3. Disable TTL trick if enabled
4. Try different proxy server

### No Domestic Hops Found
1. Check network connectivity
2. Verify Iranian ISP ranges
3. Run traceroute manually
4. Check IP range definitions

## Security Notes

### Privacy
- All bypass techniques are client-side only
- No data sent to external servers
- MAC addresses only used for local network identification

### Legality
- Packet manipulation is legal in most jurisdictions
- Techniques are defensive (bypassing censorship)
- Does not modify ISP infrastructure

### Detection
- Bypass methods are designed to be stealthy
- Mimics normal network behavior
- Low risk of detection by ISPs

## Known Issues

### Limitations
- MAC resolution may fail for non-local hops
- TTL manipulation requires careful tuning
- Some proxies may not support fragmentation
- Effectiveness varies by ISP equipment

### Future Improvements
- Adaptive fragmentation based on success rate
- Per-ISP optimization profiles
- Machine learning for strategy selection
- QUIC protocol support

## Support

### Documentation
- User Guide: `docs/dpi_bypass_guide.md`
- Technical Summary: `docs/IMPLEMENTATION_SUMMARY.md`
- API Reference: Header files in `include/security/`

### Community
- GitHub Issues: Report bugs and request features
- Telegram Channel: Updates and discussions
- Wiki: Detailed tutorials and troubleshooting

---

**Hunter v1.3.0** - Effective DPI bypass for Iranian censorship
*Released: March 21, 2026*
