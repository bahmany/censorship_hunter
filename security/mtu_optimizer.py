"""
MTU Optimizer - Fixing 5G PMTUD Attacks on Iranian Mobile Networks

From article Section 2.3.4:
A new technique on MCI and Irancell targets the transport layer. The system disrupts
"Path MTU Discovery" (PMTUD), causing large packets (usually carrying encrypted VPN
payloads) to drop on the 5G network. Since the ICMP "Fragmentation Needed" packet is
blocked by the firewall, the client doesn't realize it needs to lower packet size,
resulting in a connection timeout.

This module:
- Detects network type (fiber/ADSL/4G/5G) and adjusts MTU
- Sets optimal MTU values (1350 IPv4, 1280 IPv6) for mobile networks
- Configures TCP buffer sizes for high packet loss environments
- Disables rp_filter to prevent asymmetric routing issues
- Generates platform-specific scripts (Linux, Android, Windows)
- Provides Xray/Sing-box compatible MTU configuration
- Monitors PMTUD blackhole detection
"""

import logging
import os
import platform
import re
import socket
import struct
import subprocess
import sys
import time
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Dict, List, Optional, Tuple


class NetworkType(Enum):
    """Detected network type."""
    FIBER = "fiber"
    ADSL = "adsl"
    MOBILE_4G = "mobile_4g"
    MOBILE_5G = "mobile_5g"
    WIFI = "wifi"
    UNKNOWN = "unknown"


@dataclass
class MTUConfig:
    """MTU optimization configuration."""
    # MTU values per network type
    fiber_mtu: int = 1500
    adsl_mtu: int = 1492       # PPPoE overhead
    mobile_4g_mtu: int = 1350
    mobile_5g_mtu: int = 1280  # Most conservative for 5G PMTUD attacks
    ipv6_mtu: int = 1280       # IPv6 minimum MTU
    # VPN interface MTU (tun0)
    vpn_mtu: int = 1350
    # TCP buffer sizes for high packet loss
    tcp_rmem_min: int = 4096
    tcp_rmem_default: int = 131072
    tcp_rmem_max: int = 6291456
    tcp_wmem_min: int = 4096
    tcp_wmem_default: int = 16384
    tcp_wmem_max: int = 4194304
    # Disable rp_filter for asymmetric routing
    disable_rp_filter: bool = True
    # MSS clamping (MTU - 40 for IPv4, MTU - 60 for IPv6)
    enable_mss_clamping: bool = True


@dataclass
class MTUTestResult:
    """Result of MTU probe test."""
    target_host: str = ""
    max_working_mtu: int = 0
    min_failing_mtu: int = 0
    pmtud_blackhole: bool = False
    icmp_blocked: bool = False
    recommended_mtu: int = 1350
    test_time: float = 0.0


class MTUOptimizer:
    """
    MTU Optimizer for Iranian network conditions.
    
    Handles PMTUD blackhole attacks on 5G networks and optimizes
    TCP settings for high packet loss environments.
    """
    
    def __init__(self, config: Optional[MTUConfig] = None):
        self.logger = logging.getLogger(__name__)
        self.config = config or MTUConfig()
        self._detected_network: Optional[NetworkType] = None
        self._detected_mtu: Optional[int] = None
        self._is_windows = sys.platform == "win32"
        self._is_linux = sys.platform.startswith("linux")
        self._is_android = hasattr(sys, 'getandroidapilevel') or os.path.exists("/system/build.prop")
    
    def detect_network_type(self) -> NetworkType:
        """
        Detect current network type.
        
        Uses interface names and connection properties to determine
        if we're on fiber, ADSL, 4G, 5G, or WiFi.
        """
        try:
            if self._is_windows:
                return self._detect_network_windows()
            elif self._is_android:
                return self._detect_network_android()
            elif self._is_linux:
                return self._detect_network_linux()
            else:
                return NetworkType.UNKNOWN
        except Exception as e:
            self.logger.debug(f"Network detection failed: {e}")
            return NetworkType.UNKNOWN
    
    def _detect_network_windows(self) -> NetworkType:
        """Detect network type on Windows."""
        try:
            result = subprocess.run(
                ["netsh", "wlan", "show", "interfaces"],
                capture_output=True, text=True, timeout=2,
                creationflags=subprocess.CREATE_NO_WINDOW
            )
            if "connected" in result.stdout.lower():
                return NetworkType.WIFI
        except Exception:
            pass
        
        try:
            result = subprocess.run(
                ["netsh", "interface", "show", "interface"],
                capture_output=True, text=True, timeout=2,
                creationflags=subprocess.CREATE_NO_WINDOW
            )
            output = result.stdout.lower()
            if "cellular" in output or "mobile" in output:
                # Try to determine 4G vs 5G
                if "5g" in output or "nr" in output:
                    return NetworkType.MOBILE_5G
                return NetworkType.MOBILE_4G
            if "ethernet" in output:
                return NetworkType.FIBER
        except Exception:
            pass
        
        return NetworkType.FIBER  # Default assumption for Windows
    
    def _detect_network_android(self) -> NetworkType:
        """Detect network type on Android."""
        try:
            result = subprocess.run(
                ["getprop", "gsm.network.type"],
                capture_output=True, text=True, timeout=3
            )
            net_type = result.stdout.strip().lower()
            if "nr" in net_type or "5g" in net_type:
                return NetworkType.MOBILE_5G
            elif "lte" in net_type or "4g" in net_type:
                return NetworkType.MOBILE_4G
        except Exception:
            pass
        
        try:
            result = subprocess.run(
                ["ip", "route", "show", "default"],
                capture_output=True, text=True, timeout=3
            )
            if "wlan" in result.stdout:
                return NetworkType.WIFI
            elif "rmnet" in result.stdout or "ccmni" in result.stdout:
                return NetworkType.MOBILE_4G
        except Exception:
            pass
        
        return NetworkType.MOBILE_4G  # Default for Android
    
    def _detect_network_linux(self) -> NetworkType:
        """Detect network type on Linux."""
        try:
            result = subprocess.run(
                ["ip", "route", "show", "default"],
                capture_output=True, text=True, timeout=3
            )
            output = result.stdout.lower()
            if "wlan" in output or "wlp" in output:
                return NetworkType.WIFI
            elif "ppp" in output:
                return NetworkType.ADSL
            elif "eth" in output or "enp" in output or "ens" in output:
                return NetworkType.FIBER
        except Exception:
            pass
        
        return NetworkType.FIBER
    
    def get_optimal_mtu(self, network_type: Optional[NetworkType] = None) -> int:
        """
        Get optimal MTU for the current or specified network type.
        
        Critical values from the article:
        - 1350 bytes for IPv4 mobile
        - 1280 bytes for IPv6 (and aggressive 5G filtering)
        """
        if network_type is None:
            network_type = self.detect_network_type()
        
        self._detected_network = network_type
        
        mtu_map = {
            NetworkType.FIBER: self.config.fiber_mtu,
            NetworkType.ADSL: self.config.adsl_mtu,
            NetworkType.MOBILE_4G: self.config.mobile_4g_mtu,
            NetworkType.MOBILE_5G: self.config.mobile_5g_mtu,
            NetworkType.WIFI: self.config.fiber_mtu,
            NetworkType.UNKNOWN: self.config.mobile_4g_mtu,  # Conservative default
        }
        
        mtu = mtu_map.get(network_type, self.config.mobile_4g_mtu)
        self._detected_mtu = mtu
        self.logger.info(f"Optimal MTU for {network_type.value}: {mtu}")
        return mtu
    
    def probe_mtu(self, target_host: str = "1.1.1.1", 
                  min_mtu: int = 1200, max_mtu: int = 1500) -> MTUTestResult:
        """
        Probe to find actual working MTU by sending progressively larger packets.
        
        Detects PMTUD blackholes by checking if ICMP Fragmentation Needed
        is being blocked (the core of Iran's 5G attack).
        """
        result = MTUTestResult(target_host=target_host)
        start_time = time.time()
        
        last_working = min_mtu
        first_failing = max_mtu
        
        # Binary search for optimal MTU
        low, high = min_mtu, max_mtu
        
        while low <= high:
            mid = (low + high) // 2
            
            if self._test_mtu_size(target_host, mid):
                last_working = mid
                low = mid + 1
            else:
                first_failing = mid
                high = mid - 1
        
        result.max_working_mtu = last_working
        result.min_failing_mtu = first_failing
        result.test_time = time.time() - start_time
        
        # Check for PMTUD blackhole
        # If there's a gap between working and failing > 100, likely ICMP is blocked
        if first_failing - last_working > 100:
            result.pmtud_blackhole = True
            result.icmp_blocked = True
            result.recommended_mtu = last_working - 28  # Safety margin
            self.logger.warning(
                f"PMTUD blackhole detected! ICMP likely blocked. "
                f"Max working: {last_working}, Recommended: {result.recommended_mtu}"
            )
        else:
            result.recommended_mtu = last_working
        
        return result
    
    def _test_mtu_size(self, host: str, size: int) -> bool:
        """Test if a specific MTU size works (DF bit set)."""
        try:
            if self._is_windows:
                result = subprocess.run(
                    ["ping", "-n", "1", "-w", "1000", "-f", "-l", str(size - 28), host],
                    capture_output=True, text=True, timeout=2,
                    creationflags=subprocess.CREATE_NO_WINDOW
                )
                return "bytes=" in result.stdout.lower() or "reply from" in result.stdout.lower()
            else:
                result = subprocess.run(
                    ["ping", "-c", "1", "-W", "1", "-M", "do", "-s", str(size - 28), host],
                    capture_output=True, text=True, timeout=2
                )
                return result.returncode == 0
        except Exception:
            return False
    
    def generate_optimization_script(self, 
                                      network_type: Optional[NetworkType] = None,
                                      vpn_interface: str = "tun0") -> str:
        """
        Generate platform-specific MTU optimization script.
        
        From article Section 5.3 - Shell script for Android/Linux:
        - Set MTU to 1350 (IPv4) and 1280 (IPv6)
        - Disable rp_filter
        - Increase TCP buffers
        """
        if network_type is None:
            network_type = self.detect_network_type()
        
        mtu = self.get_optimal_mtu(network_type)
        
        if self._is_windows:
            return self._generate_windows_script(vpn_interface, mtu)
        elif self._is_android:
            return self._generate_android_script(vpn_interface, mtu)
        else:
            return self._generate_linux_script(vpn_interface, mtu)
    
    def _generate_linux_script(self, vpn_interface: str, mtu: int) -> str:
        """Generate Linux/Android MTU optimization script."""
        ipv6_mtu = min(mtu, self.config.ipv6_mtu)
        
        script = f"""#!/bin/bash
# Hunter MTU Optimizer - Iranian Network PMTUD Attack Mitigation
# Generated for {self._detected_network.value if self._detected_network else 'unknown'} network

set -e

VPN_IF="{vpn_interface}"
IPV4_MTU={mtu}
IPV6_MTU={ipv6_mtu}

echo "[*] Setting MTU for $VPN_IF..."

# Set IPv4 MTU
if ip link show "$VPN_IF" &>/dev/null; then
    ip link set dev "$VPN_IF" mtu $IPV4_MTU
    echo "[+] IPv4 MTU set to $IPV4_MTU on $VPN_IF"
else
    echo "[-] Interface $VPN_IF not found, will apply when available"
fi

# Set IPv6 MTU
if [ -f /proc/sys/net/ipv6/conf/$VPN_IF/mtu ]; then
    echo $IPV6_MTU > /proc/sys/net/ipv6/conf/$VPN_IF/mtu
    echo "[+] IPv6 MTU set to $IPV6_MTU on $VPN_IF"
fi

# Disable rp_filter to prevent asymmetric routing issues
echo "[*] Disabling rp_filter..."
for iface in /proc/sys/net/ipv4/conf/*/rp_filter; do
    echo 0 > "$iface" 2>/dev/null || true
done
echo "[+] rp_filter disabled"

# Increase TCP buffers for high packet loss environments
echo "[*] Optimizing TCP buffers..."
sysctl -w net.ipv4.tcp_rmem="{self.config.tcp_rmem_min} {self.config.tcp_rmem_default} {self.config.tcp_rmem_max}"
sysctl -w net.ipv4.tcp_wmem="{self.config.tcp_wmem_min} {self.config.tcp_wmem_default} {self.config.tcp_wmem_max}"

# Enable TCP window scaling
sysctl -w net.ipv4.tcp_window_scaling=1

# Enable TCP SACK for better loss recovery
sysctl -w net.ipv4.tcp_sack=1
sysctl -w net.ipv4.tcp_dsack=1

# Increase max backlog for high packet rates
sysctl -w net.core.netdev_max_backlog=16384

# Enable TCP Fast Open (for compatible servers)
sysctl -w net.ipv4.tcp_fastopen=3

# Disable PMTUD on problematic networks (forced for 5G)
sysctl -w net.ipv4.ip_no_pmtu_disc=1

# MSS clamping via iptables
if command -v iptables &>/dev/null; then
    iptables -t mangle -A POSTROUTING -o "$VPN_IF" -p tcp --tcp-flags SYN,RST SYN \\
        -j TCPMSS --set-mss $(($IPV4_MTU - 40))
    echo "[+] MSS clamped to $(($IPV4_MTU - 40))"
fi

echo "[+] MTU optimization complete for {self._detected_network.value if self._detected_network else 'unknown'} network"
"""
        return script
    
    def _generate_android_script(self, vpn_interface: str, mtu: int) -> str:
        """Generate Android-specific (root) MTU optimization script."""
        base_script = self._generate_linux_script(vpn_interface, mtu)
        
        android_additions = f"""
# Android-specific optimizations
echo "[*] Applying Android-specific settings..."

# Set mobile interface MTU if available
for iface in rmnet0 rmnet_data0 ccmni0 wwan0; do
    if ip link show "$iface" &>/dev/null; then
        ip link set dev "$iface" mtu {mtu}
        echo "[+] Set MTU {mtu} on $iface"
    fi
done

# Optimize for mobile network jitter
if [ -f /proc/sys/net/ipv4/tcp_low_latency ]; then
    echo 1 > /proc/sys/net/ipv4/tcp_low_latency
fi

# Disable TCP timestamps to reduce overhead
sysctl -w net.ipv4.tcp_timestamps=0

echo "[+] Android MTU optimization complete"
"""
        return base_script + android_additions
    
    def _generate_windows_script(self, vpn_interface: str, mtu: int) -> str:
        """Generate Windows PowerShell MTU optimization script."""
        return f"""# Hunter MTU Optimizer - Windows
# Run as Administrator

$VPN_IF = "{vpn_interface}"
$MTU = {mtu}

Write-Host "[*] Optimizing MTU for Iranian network conditions..." -ForegroundColor Cyan

# Find VPN adapter
$vpnAdapter = Get-NetAdapter | Where-Object {{ $_.InterfaceDescription -like "*TUN*" -or $_.InterfaceDescription -like "*TAP*" -or $_.Name -like "*{vpn_interface}*" }}

if ($vpnAdapter) {{
    # Set MTU on VPN interface
    netsh interface ipv4 set subinterface $vpnAdapter.InterfaceIndex mtu=$MTU store=active
    Write-Host "[+] IPv4 MTU set to $MTU on $($vpnAdapter.Name)" -ForegroundColor Green
    
    # Set IPv6 MTU
    netsh interface ipv6 set subinterface $vpnAdapter.InterfaceIndex mtu={min(mtu, self.config.ipv6_mtu)} store=active
    Write-Host "[+] IPv6 MTU set to {min(mtu, self.config.ipv6_mtu)} on $($vpnAdapter.Name)" -ForegroundColor Green
}} else {{
    Write-Host "[-] VPN adapter not found, setting MTU on all adapters..." -ForegroundColor Yellow
    Get-NetAdapter | Where-Object {{ $_.Status -eq 'Up' }} | ForEach-Object {{
        netsh interface ipv4 set subinterface $_.InterfaceIndex mtu=$MTU store=active
        Write-Host "[+] MTU set to $MTU on $($_.Name)" -ForegroundColor Green
    }}
}}

# Increase TCP receive/send buffers
netsh int tcp set global autotuninglevel=experimental
Write-Host "[+] TCP auto-tuning set to experimental (maximum buffers)" -ForegroundColor Green

# Enable TCP Fast Open
netsh int tcp set global fastopen=enabled
Write-Host "[+] TCP Fast Open enabled" -ForegroundColor Green

# Disable PMTUD blackhole detection workaround
netsh int ip set global mtu=1 store=active 2>$null
reg add "HKLM\\SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters" /v "EnablePMTUDiscovery" /t REG_DWORD /d 0 /f 2>$null
Write-Host "[+] PMTUD disabled (anti-blackhole)" -ForegroundColor Green

Write-Host "[+] MTU optimization complete!" -ForegroundColor Green
"""
    
    def apply_socket_optimization(self, sock: socket.socket, 
                                   network_type: Optional[NetworkType] = None) -> socket.socket:
        """
        Apply MTU-aware socket optimizations.
        
        Non-invasive: configures socket options without changing behavior.
        """
        if network_type is None:
            network_type = self._detected_network or self.detect_network_type()
        
        mtu = self.get_optimal_mtu(network_type)
        mss = mtu - 40  # IPv4 header (20) + TCP header (20)
        
        try:
            # Set TCP_NODELAY for immediate sends
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            
            # Set TCP_MAXSEG (MSS) if possible
            try:
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG, mss)
            except (AttributeError, OSError):
                pass  # Not supported on all platforms
            
            # Increase socket buffer sizes
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 
                          self.config.tcp_rmem_max)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 
                          self.config.tcp_wmem_max)
            
            # Enable TCP keepalive
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
            
            # Platform-specific keepalive settings
            if self._is_windows:
                # Windows: TCP_KEEPIDLE, TCP_KEEPINTVL via SIO_KEEPALIVE_VALS
                try:
                    sock.ioctl(socket.SIO_KEEPALIVE_VALS, (1, 60000, 10000))
                except (AttributeError, OSError):
                    pass
            elif self._is_linux:
                try:
                    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE, 60)
                    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL, 10)
                    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT, 5)
                except (AttributeError, OSError):
                    pass
            
        except Exception as e:
            self.logger.debug(f"Socket optimization error: {e}")
        
        return sock
    
    def get_xray_sockopt(self, network_type: Optional[NetworkType] = None) -> Dict[str, Any]:
        """Get Xray-compatible sockopt configuration."""
        if network_type is None:
            network_type = self._detected_network or self.detect_network_type()
        
        sockopt = {
            "tcpFastOpen": True,
            "tcpKeepAliveInterval": 60,
            "tcpNoDelay": True,
        }
        
        if network_type in (NetworkType.MOBILE_4G, NetworkType.MOBILE_5G):
            sockopt["tcpMptcp"] = True  # Multi-path TCP for mobile
            sockopt["mark"] = 255  # SO_MARK for policy routing
        
        return sockopt
    
    def get_metrics(self) -> Dict[str, Any]:
        """Get MTU optimizer metrics."""
        return {
            "detected_network": self._detected_network.value if self._detected_network else "unknown",
            "detected_mtu": self._detected_mtu,
            "platform": "windows" if self._is_windows else ("android" if self._is_android else "linux"),
            "config": {
                "fiber_mtu": self.config.fiber_mtu,
                "mobile_4g_mtu": self.config.mobile_4g_mtu,
                "mobile_5g_mtu": self.config.mobile_5g_mtu,
                "vpn_mtu": self.config.vpn_mtu,
            }
        }
