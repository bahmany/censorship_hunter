"""
Advanced TLS ClientHello Fragmentation Engine

Designed to defeat Iran's TSG DPI system by exploiting its packet reassembly limitations.
The DPI must see the full first packet (ClientHello) to identify the SNI destination.
By splitting this packet into multiple fragments with millisecond delays, we force the
DPI to buffer and reassemble. Under high load (millions of concurrent connections),
maintaining these buffers is impossible, and many firewalls allow incomplete packets
through to avoid adding latency.

Algorithm (from article Section 4.3):
1. Intercept outgoing ClientHello packet (starts with 0x16 0x03)
2. Fragment Strategy:
   - Fragment 1: First 5 bytes (TLS record header)
   - Fragment 2: The SNI portion
   - Fragment 3: Remainder of the packet
3. Send with random timing jitter (10-50ms between fragments)
4. Fallback to simple bi-furcation on RST

This module also implements:
- Adaptive fragment sizing based on network conditions
- Multi-strategy fragmentation (3-part, random-split, byte-by-byte)
- RST detection and automatic fallback
- Integration with Xray/Sing-box fragment settings
"""

import logging
import os
import random
import socket
import struct
import time
import threading
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Callable, Dict, List, Optional, Tuple


class FragmentStrategy(Enum):
    """Fragmentation strategies for different DPI bypass scenarios."""
    THREE_PART = "three_part"          # Article-recommended: header + SNI + rest
    RANDOM_SPLIT = "random_split"      # Random chunk sizes (100-200 bytes)
    BYTE_LEVEL = "byte_level"          # 1-5 byte micro-fragments (ADEE style)
    SNI_SPLIT = "sni_split"            # Split exactly at SNI boundary
    DUAL_FRAGMENT = "dual_fragment"    # Simple bi-furcation fallback
    ADAPTIVE = "adaptive"              # Auto-detect best strategy


@dataclass
class FragmentConfig:
    """Configuration for TLS fragmentation."""
    enabled: bool = True
    strategy: FragmentStrategy = FragmentStrategy.THREE_PART
    min_delay_ms: float = 10.0    # Minimum inter-fragment delay
    max_delay_ms: float = 50.0    # Maximum inter-fragment delay
    min_fragment_size: int = 1     # For byte-level fragmentation
    max_fragment_size: int = 200   # For random-split
    fallback_on_rst: bool = True   # Auto-fallback on RST
    max_retries: int = 3
    timeout_seconds: float = 10.0
    # Xray/Sing-box compatible settings
    xray_fragment_length: str = "100-200"
    xray_fragment_interval: str = "10-50"
    xray_fragment_packets: str = "tlshello"


@dataclass
class FragmentMetrics:
    """Metrics for fragmentation effectiveness."""
    total_connections: int = 0
    successful_fragments: int = 0
    failed_fragments: int = 0
    rst_received: int = 0
    fallback_used: int = 0
    avg_fragment_count: float = 0.0
    avg_delay_ms: float = 0.0
    strategy_success: Dict[str, int] = field(default_factory=dict)


class TLSFragmentationEngine:
    """
    Advanced TLS ClientHello Fragmentation Engine.
    
    Splits TLS ClientHello packets to defeat Iran's TSG DPI reassembly.
    Supports multiple strategies with automatic fallback.
    """
    
    def __init__(self, config: Optional[FragmentConfig] = None):
        self.logger = logging.getLogger(__name__)
        self.config = config or FragmentConfig()
        self.metrics = FragmentMetrics()
        self._lock = threading.Lock()
        
        # Track which strategies work for different destinations
        self._strategy_scores: Dict[str, Dict[FragmentStrategy, float]] = {}
        
        self.logger.info(f"TLS Fragmentation Engine initialized (strategy: {self.config.strategy.value})")
    
    def fragment_and_send(self, sock: socket.socket, data: bytes, 
                          address: Optional[Tuple[str, int]] = None) -> bool:
        """
        Fragment data and send through socket with timing jitter.
        
        Primary entry point for the fragmentation engine.
        
        Args:
            sock: Connected TCP socket
            data: Full TLS ClientHello data to fragment
            address: Optional destination address for metrics
            
        Returns:
            True if all fragments sent successfully
        """
        if not self.config.enabled:
            sock.sendall(data)
            return True
        
        # Check if this is a TLS ClientHello
        if not self._is_tls_client_hello(data):
            sock.sendall(data)
            return True
        
        self.logger.debug(f"Fragmenting TLS ClientHello ({len(data)} bytes)")
        
        strategy = self.config.strategy
        if strategy == FragmentStrategy.ADAPTIVE:
            strategy = self._select_best_strategy(address)
        
        try:
            success = self._apply_strategy(sock, data, strategy)
            
            with self._lock:
                self.metrics.total_connections += 1
                if success:
                    self.metrics.successful_fragments += 1
                    key = strategy.value
                    self.metrics.strategy_success[key] = \
                        self.metrics.strategy_success.get(key, 0) + 1
                else:
                    self.metrics.failed_fragments += 1
            
            return success
            
        except ConnectionResetError:
            with self._lock:
                self.metrics.rst_received += 1
            
            if self.config.fallback_on_rst:
                self.logger.debug("RST received, falling back to dual fragment")
                with self._lock:
                    self.metrics.fallback_used += 1
                return self._apply_strategy(sock, data, FragmentStrategy.DUAL_FRAGMENT)
            
            return False
        
        except Exception as e:
            self.logger.debug(f"Fragmentation error: {e}")
            with self._lock:
                self.metrics.failed_fragments += 1
            # Send unfragmented as last resort
            try:
                sock.sendall(data)
                return True
            except Exception:
                return False
    
    def _is_tls_client_hello(self, data: bytes) -> bool:
        """Check if data is a TLS ClientHello."""
        if len(data) < 6:
            return False
        # TLS record: 0x16 (handshake), 0x03 0x0X (version)
        # Handshake type: 0x01 (ClientHello)
        return (data[0] == 0x16 and 
                data[1] == 0x03 and 
                data[5] == 0x01)
    
    def _apply_strategy(self, sock: socket.socket, data: bytes, 
                        strategy: FragmentStrategy) -> bool:
        """Apply a specific fragmentation strategy."""
        if strategy == FragmentStrategy.THREE_PART:
            return self._fragment_three_part(sock, data)
        elif strategy == FragmentStrategy.RANDOM_SPLIT:
            return self._fragment_random_split(sock, data)
        elif strategy == FragmentStrategy.BYTE_LEVEL:
            return self._fragment_byte_level(sock, data)
        elif strategy == FragmentStrategy.SNI_SPLIT:
            return self._fragment_sni_split(sock, data)
        elif strategy == FragmentStrategy.DUAL_FRAGMENT:
            return self._fragment_dual(sock, data)
        else:
            return self._fragment_three_part(sock, data)
    
    def _fragment_three_part(self, sock: socket.socket, data: bytes) -> bool:
        """
        Article-recommended 3-part fragmentation:
        Fragment 1: TLS record header (first 5 bytes)
        Fragment 2: SNI portion
        Fragment 3: Remainder
        """
        if len(data) < 10:
            sock.sendall(data)
            return True
        
        # Fragment 1: TLS record header (5 bytes)
        frag1 = data[:5]
        
        # Find SNI in the ClientHello
        sni_start, sni_end = self._find_sni_bounds(data)
        
        if sni_start > 0 and sni_end > sni_start:
            # Fragment 2: From after header to end of SNI
            frag2 = data[5:sni_end]
            # Fragment 3: Everything after SNI
            frag3 = data[sni_end:]
        else:
            # SNI not found - split at 1/3 and 2/3 points
            third = len(data) // 3
            frag2 = data[5:5 + third]
            frag3 = data[5 + third:]
        
        # Send with jitter delays
        fragments = [frag1, frag2, frag3]
        return self._send_fragments_with_jitter(sock, fragments)
    
    def _fragment_random_split(self, sock: socket.socket, data: bytes) -> bool:
        """
        Random chunk splitting (100-200 bytes per fragment).
        Mimics SplitHTTP / modern web browsing packet sizes.
        """
        fragments = []
        offset = 0
        min_size = max(1, self.config.min_fragment_size)
        max_size = max(min_size + 1, self.config.max_fragment_size)
        
        while offset < len(data):
            chunk_size = random.randint(min_size, max_size)
            chunk_size = min(chunk_size, len(data) - offset)
            fragments.append(data[offset:offset + chunk_size])
            offset += chunk_size
        
        return self._send_fragments_with_jitter(sock, fragments)
    
    def _fragment_byte_level(self, sock: socket.socket, data: bytes) -> bool:
        """
        Micro-fragmentation (1-5 bytes per fragment).
        Most aggressive - designed for maximum DPI buffer stress.
        """
        fragments = []
        offset = 0
        
        while offset < len(data):
            frag_size = random.randint(1, 5)
            frag_size = min(frag_size, len(data) - offset)
            fragments.append(data[offset:offset + frag_size])
            offset += frag_size
        
        return self._send_fragments_with_jitter(sock, fragments)
    
    def _fragment_sni_split(self, sock: socket.socket, data: bytes) -> bool:
        """
        Split exactly at SNI boundary.
        Most targeted - splits the exact field DPI needs to read.
        """
        sni_start, sni_end = self._find_sni_bounds(data)
        
        if sni_start > 0:
            # Split right before SNI value
            sni_value_start = sni_start
            # Split SNI value itself into 2-3 parts
            sni_len = sni_end - sni_start
            
            fragments = [data[:sni_value_start]]
            
            if sni_len > 4:
                mid = sni_value_start + sni_len // 2
                fragments.append(data[sni_value_start:mid])
                fragments.append(data[mid:sni_end])
            else:
                fragments.append(data[sni_value_start:sni_end])
            
            fragments.append(data[sni_end:])
        else:
            # Fallback to dual fragment
            mid = len(data) // 2
            fragments = [data[:mid], data[mid:]]
        
        return self._send_fragments_with_jitter(sock, fragments)
    
    def _fragment_dual(self, sock: socket.socket, data: bytes) -> bool:
        """
        Simple bi-furcation (split in half).
        Fallback strategy when more complex methods trigger RST.
        """
        mid = len(data) // 2
        fragments = [data[:mid], data[mid:]]
        return self._send_fragments_with_jitter(sock, fragments)
    
    def _send_fragments_with_jitter(self, sock: socket.socket, 
                                     fragments: List[bytes]) -> bool:
        """Send fragments with random timing jitter between them."""
        total_delay = 0.0
        
        for i, fragment in enumerate(fragments):
            if not fragment:
                continue
                
            try:
                sock.send(fragment)
            except Exception as e:
                self.logger.debug(f"Fragment {i+1}/{len(fragments)} send failed: {e}")
                return False
            
            # Add jitter delay between fragments (not after last)
            if i < len(fragments) - 1:
                delay_ms = random.uniform(
                    self.config.min_delay_ms,
                    self.config.max_delay_ms
                )
                delay_s = delay_ms / 1000.0
                time.sleep(delay_s)
                total_delay += delay_ms
        
        # Update average delay metric
        with self._lock:
            if self.metrics.total_connections > 0:
                self.metrics.avg_delay_ms = (
                    (self.metrics.avg_delay_ms * (self.metrics.total_connections - 1) + total_delay) /
                    self.metrics.total_connections
                )
            self.metrics.avg_fragment_count = (
                (self.metrics.avg_fragment_count * max(1, self.metrics.total_connections - 1) + len(fragments)) /
                max(1, self.metrics.total_connections)
            )
        
        return True
    
    def _find_sni_bounds(self, data: bytes) -> Tuple[int, int]:
        """
        Find the SNI (Server Name Indication) field bounds in ClientHello.
        
        SNI extension type = 0x0000
        Structure:
        - Extension type (2 bytes): 0x00 0x00
        - Extension length (2 bytes)
        - Server name list length (2 bytes)  
        - Server name type (1 byte): 0x00 (hostname)
        - Server name length (2 bytes)
        - Server name value (variable)
        
        Returns:
            Tuple of (sni_value_start, sni_value_end) or (-1, -1) if not found
        """
        try:
            # TLS record header is 5 bytes
            # Handshake header is 4 bytes (type + 3-byte length)
            # ClientHello: version(2) + random(32) + session_id_len(1) + session_id(var)
            # + cipher_suites_len(2) + cipher_suites(var) + comp_len(1) + comp(var)
            # + extensions_len(2) + extensions(var)
            
            offset = 5  # Skip TLS record header
            if offset >= len(data):
                return (-1, -1)
            
            # Skip handshake type and length
            offset += 4
            if offset + 2 >= len(data):
                return (-1, -1)
            
            # Skip version
            offset += 2
            # Skip random
            offset += 32
            
            if offset >= len(data):
                return (-1, -1)
            
            # Skip session ID
            session_id_len = data[offset]
            offset += 1 + session_id_len
            
            if offset + 2 >= len(data):
                return (-1, -1)
            
            # Skip cipher suites
            cipher_len = struct.unpack(">H", data[offset:offset+2])[0]
            offset += 2 + cipher_len
            
            if offset >= len(data):
                return (-1, -1)
            
            # Skip compression methods
            comp_len = data[offset]
            offset += 1 + comp_len
            
            if offset + 2 >= len(data):
                return (-1, -1)
            
            # Extensions length
            ext_total_len = struct.unpack(">H", data[offset:offset+2])[0]
            offset += 2
            ext_end = offset + ext_total_len
            
            # Search through extensions for SNI (type 0x0000)
            while offset + 4 <= ext_end and offset + 4 <= len(data):
                ext_type = struct.unpack(">H", data[offset:offset+2])[0]
                ext_len = struct.unpack(">H", data[offset+2:offset+4])[0]
                
                if ext_type == 0x0000:  # SNI extension
                    # Parse SNI extension
                    sni_offset = offset + 4
                    if sni_offset + 2 > len(data):
                        break
                    
                    # Server name list length
                    sni_list_len = struct.unpack(">H", data[sni_offset:sni_offset+2])[0]
                    sni_offset += 2
                    
                    if sni_offset + 1 > len(data):
                        break
                    
                    # Server name type
                    sni_type = data[sni_offset]
                    sni_offset += 1
                    
                    if sni_type == 0x00 and sni_offset + 2 <= len(data):
                        # Hostname
                        name_len = struct.unpack(">H", data[sni_offset:sni_offset+2])[0]
                        sni_offset += 2
                        
                        sni_value_start = sni_offset
                        sni_value_end = sni_offset + name_len
                        
                        return (sni_value_start, sni_value_end)
                
                offset += 4 + ext_len
            
            return (-1, -1)
            
        except Exception:
            return (-1, -1)
    
    def _select_best_strategy(self, 
                               address: Optional[Tuple[str, int]]) -> FragmentStrategy:
        """Select the best strategy based on past success for this destination."""
        if address and address[0] in self._strategy_scores:
            scores = self._strategy_scores[address[0]]
            if scores:
                best = max(scores, key=scores.get)
                return best
        
        # Default: use three-part (article recommendation)
        return FragmentStrategy.THREE_PART
    
    def get_xray_fragment_config(self) -> Dict[str, Any]:
        """
        Generate Xray-compatible fragment configuration.
        
        For Xray-core streamSettings.sockopt.
        """
        return {
            "dialerProxy": "",
            "fragment": {
                "packets": self.config.xray_fragment_packets,
                "length": self.config.xray_fragment_length,
                "interval": self.config.xray_fragment_interval,
            }
        }
    
    def get_singbox_fragment_config(self) -> Dict[str, Any]:
        """Generate Sing-box compatible fragment configuration."""
        min_len, max_len = self.config.xray_fragment_length.split("-")
        min_int, max_int = self.config.xray_fragment_interval.split("-")
        
        return {
            "tls_fragment": {
                "enabled": True,
                "size": f"{min_len}:{max_len}",
                "sleep": f"{min_int}:{max_int}",
            }
        }
    
    def create_fragmented_dialer(self) -> Callable:
        """
        Create a socket connect wrapper that applies fragmentation.
        
        Can be used as a custom dialer for proxy connections.
        """
        engine = self
        
        def fragmented_connect(sock: socket.socket, address: Tuple[str, int],
                               original_send: Optional[Callable] = None):
            """Connect and intercept first TLS packet for fragmentation."""
            # Connect normally
            sock.connect(address)
            
            # Monkey-patch send to intercept first packet
            original_sock_send = sock.send
            first_packet = [True]
            
            def intercepting_send(data, flags=0):
                if first_packet[0] and engine._is_tls_client_hello(data):
                    first_packet[0] = False
                    return engine.fragment_and_send(sock, data, address)
                return original_sock_send(data, flags)
            
            sock.send = intercepting_send
            return sock
        
        return fragmented_connect
    
    def wrap_socket(self, sock: socket.socket, 
                    address: Tuple[str, int]) -> socket.socket:
        """
        Wrap a socket to apply fragmentation on first TLS send.
        
        Non-invasive wrapper that intercepts the first send() call.
        """
        engine = self
        original_send = sock.send
        original_sendall = sock.sendall
        first_packet = [True]
        
        def intercepting_send(data, flags=0):
            if first_packet[0] and engine._is_tls_client_hello(data):
                first_packet[0] = False
                success = engine.fragment_and_send(sock, data, address)
                return len(data) if success else 0
            return original_send(data, flags)
        
        def intercepting_sendall(data, flags=0):
            if first_packet[0] and engine._is_tls_client_hello(data):
                first_packet[0] = False
                engine.fragment_and_send(sock, data, address)
                return None
            return original_sendall(data, flags)
        
        sock.send = intercepting_send
        sock.sendall = intercepting_sendall
        return sock
    
    def get_metrics(self) -> Dict[str, Any]:
        """Get fragmentation metrics."""
        with self._lock:
            success_rate = (
                self.metrics.successful_fragments / max(1, self.metrics.total_connections)
            ) * 100
            
            return {
                "total_connections": self.metrics.total_connections,
                "successful": self.metrics.successful_fragments,
                "failed": self.metrics.failed_fragments,
                "rst_received": self.metrics.rst_received,
                "fallback_used": self.metrics.fallback_used,
                "success_rate": round(success_rate, 1),
                "avg_fragment_count": round(self.metrics.avg_fragment_count, 1),
                "avg_delay_ms": round(self.metrics.avg_delay_ms, 1),
                "strategy": self.config.strategy.value,
                "strategy_success": dict(self.metrics.strategy_success),
            }
