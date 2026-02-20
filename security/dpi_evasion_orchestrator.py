"""
DPI Evasion Orchestrator - Adaptive Strategy Selection Engine

The central coordinator for all anti-censorship evasion modules, implementing
the article's recommended technical solutions by network type (Table 2):

| Network Type          | Recommended Protocol      | Key Settings                    |
|-----------------------|---------------------------|---------------------------------|
| Home Internet (Fiber) | VLESS-Reality-Vision      | flow: xtls-rprx-vision          |
| Mobile (4G/5G)        | Hysteria2 / TUIC v5       | brutal congestion, MTU: 1350    |
| Blackout Conditions   | SplitHTTP (Xray v26)      | fragment: 100-200 bytes         |
| UDP-Restricted        | VLESS over WebSocket (CDN) | path: /random, Early Data      |

This orchestrator:
- Detects current network conditions (type, quality, ISP)
- Selects optimal evasion strategy automatically
- Manages strategy fallback chains when primary strategy fails
- Coordinates all evasion modules (fingerprint, fragmentation, MTU, probe defense)
- Provides unified metrics and status reporting
- Generates complete Xray/Sing-box configs with all optimizations applied
- Adapts in real-time based on connection success/failure feedback
"""

import json
import logging
import os
import random
import socket
import threading
import time
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Callable, Dict, List, Optional, Tuple

from .tls_fingerprint_evasion import TLSFingerprintEvasion, BrowserProfile, REALITY_DEST_WHITELIST
from .tls_fragmentation import TLSFragmentationEngine, FragmentConfig, FragmentStrategy
from .reality_config_generator import RealityConfigGenerator, X25519KeyGenerator
from .udp_protocols import UDPProtocolManager, CongestionControl, PortHoppingStrategy
from .mtu_optimizer import MTUOptimizer, MTUConfig, NetworkType
from .active_probe_defense import ActiveProbeDefender, EntropyNormalizer
from .split_http_transport import SplitHTTPTransport


class EvasionStrategy(Enum):
    """Available evasion strategies ordered by stealth level."""
    REALITY_VISION = "reality_vision"           # Best for fiber/ADSL
    REALITY_VISION_FRAGMENT = "reality_fragment" # Reality + aggressive fragmentation
    SPLITHTTP_CDN = "splithttp_cdn"             # Best for blackout conditions
    HYSTERIA2_BRUTAL = "hysteria2_brutal"       # Best for mobile 4G/5G
    TUIC_BBR = "tuic_bbr"                       # Alternative for mobile
    VLESS_WS_CDN = "vless_ws_cdn"               # Best for UDP-restricted
    VLESS_GRPC_CDN = "vless_grpc_cdn"           # Alternative CDN-based
    VMESS_WS_TLS = "vmess_ws_tls"               # Legacy fallback
    DIRECT_FRAGMENT = "direct_fragment"          # Fragment-only (no protocol change)


class NetworkCondition(Enum):
    """Detected network conditions."""
    NORMAL = "normal"               # Standard filtering
    THROTTLED = "throttled"         # Bandwidth throttling active
    HEAVY_DPI = "heavy_dpi"         # Aggressive DPI inspection
    BLACKOUT = "blackout"           # Stealth blackout mode
    UDP_BLOCKED = "udp_blocked"     # UDP/QUIC blocked
    TLS_BLOCKED = "tls_blocked"     # Anonymous TLS blocked
    WHITELISTED_ONLY = "whitelist"  # Only whitelisted traffic passes


@dataclass
class StrategyResult:
    """Result of applying an evasion strategy."""
    strategy: EvasionStrategy
    success: bool
    latency_ms: float = 0.0
    error: str = ""
    timestamp: float = 0.0
    network_type: str = ""


@dataclass 
class EvasionState:
    """Current state of the evasion orchestrator."""
    active_strategy: Optional[EvasionStrategy] = None
    network_type: NetworkType = NetworkType.UNKNOWN
    network_condition: NetworkCondition = NetworkCondition.NORMAL
    detected_isp: str = "unknown"
    strategy_attempts: Dict[str, int] = field(default_factory=dict)
    strategy_successes: Dict[str, int] = field(default_factory=dict)
    strategy_failures: Dict[str, int] = field(default_factory=dict)
    last_successful_strategy: Optional[EvasionStrategy] = None
    fallback_chain_position: int = 0
    total_connections: int = 0
    successful_connections: int = 0
    last_connection_time: float = 0.0


# Strategy fallback chains per network condition
FALLBACK_CHAINS = {
    NetworkCondition.NORMAL: [
        EvasionStrategy.REALITY_VISION,
        EvasionStrategy.REALITY_VISION_FRAGMENT,
        EvasionStrategy.VLESS_WS_CDN,
        EvasionStrategy.SPLITHTTP_CDN,
        EvasionStrategy.VMESS_WS_TLS,
    ],
    NetworkCondition.THROTTLED: [
        EvasionStrategy.HYSTERIA2_BRUTAL,
        EvasionStrategy.TUIC_BBR,
        EvasionStrategy.REALITY_VISION_FRAGMENT,
        EvasionStrategy.SPLITHTTP_CDN,
    ],
    NetworkCondition.HEAVY_DPI: [
        EvasionStrategy.REALITY_VISION,
        EvasionStrategy.REALITY_VISION_FRAGMENT,
        EvasionStrategy.SPLITHTTP_CDN,
        EvasionStrategy.VLESS_WS_CDN,
    ],
    NetworkCondition.BLACKOUT: [
        EvasionStrategy.SPLITHTTP_CDN,
        EvasionStrategy.VLESS_WS_CDN,
        EvasionStrategy.REALITY_VISION_FRAGMENT,
        EvasionStrategy.DIRECT_FRAGMENT,
    ],
    NetworkCondition.UDP_BLOCKED: [
        EvasionStrategy.REALITY_VISION,
        EvasionStrategy.VLESS_WS_CDN,
        EvasionStrategy.SPLITHTTP_CDN,
        EvasionStrategy.VLESS_GRPC_CDN,
    ],
    NetworkCondition.TLS_BLOCKED: [
        EvasionStrategy.REALITY_VISION,       # Reality mimics legitimate TLS
        EvasionStrategy.SPLITHTTP_CDN,
        EvasionStrategy.HYSTERIA2_BRUTAL,     # UDP-based, no TLS
    ],
    NetworkCondition.WHITELISTED_ONLY: [
        EvasionStrategy.REALITY_VISION,       # Borrows whitelisted identity
        EvasionStrategy.SPLITHTTP_CDN,        # Looks like web browsing
        EvasionStrategy.VLESS_WS_CDN,         # Through CDN (whitelisted)
    ],
}

# Strategy -> Required protocol/transport mapping
STRATEGY_PROTOCOL_MAP = {
    EvasionStrategy.REALITY_VISION: {
        "protocol": "vless",
        "transport": "tcp",
        "security": "reality",
        "flow": "xtls-rprx-vision",
    },
    EvasionStrategy.REALITY_VISION_FRAGMENT: {
        "protocol": "vless",
        "transport": "tcp",
        "security": "reality",
        "flow": "xtls-rprx-vision",
        "fragment": True,
    },
    EvasionStrategy.SPLITHTTP_CDN: {
        "protocol": "vless",
        "transport": "splithttp",
        "security": "tls",
    },
    EvasionStrategy.HYSTERIA2_BRUTAL: {
        "protocol": "hysteria2",
        "congestion": "brutal",
    },
    EvasionStrategy.TUIC_BBR: {
        "protocol": "tuic",
        "congestion": "bbr",
    },
    EvasionStrategy.VLESS_WS_CDN: {
        "protocol": "vless",
        "transport": "ws",
        "security": "tls",
        "cdn": True,
    },
    EvasionStrategy.VLESS_GRPC_CDN: {
        "protocol": "vless",
        "transport": "grpc",
        "security": "tls",
        "cdn": True,
    },
    EvasionStrategy.VMESS_WS_TLS: {
        "protocol": "vmess",
        "transport": "ws",
        "security": "tls",
    },
    EvasionStrategy.DIRECT_FRAGMENT: {
        "fragment_only": True,
    },
}


class DPIEvasionOrchestrator:
    """
    Central DPI Evasion Orchestrator.
    
    Coordinates all evasion modules and selects optimal strategies
    based on real-time network conditions in Iran.
    """
    
    def __init__(self):
        self.logger = logging.getLogger(__name__)
        
        # Initialize all evasion modules
        self.fingerprint_engine = TLSFingerprintEvasion()
        self.fragmentation_engine = TLSFragmentationEngine()
        self.reality_generator = RealityConfigGenerator()
        self.udp_manager = UDPProtocolManager()
        self.mtu_optimizer = MTUOptimizer()
        self.probe_defender = ActiveProbeDefender()
        self.entropy_normalizer = EntropyNormalizer()
        self.splithttp_transport = SplitHTTPTransport()
        
        # State
        self.state = EvasionState()
        self._lock = threading.Lock()
        self._running = False
        self._adaptation_thread: Optional[threading.Thread] = None
        
        # Strategy scoring (learned from success/failure)
        self._strategy_scores: Dict[EvasionStrategy, float] = {
            s: 0.5 for s in EvasionStrategy
        }
        
        self.logger.info("DPI Evasion Orchestrator initialized with all modules")
    
    def start(self):
        """Start the evasion orchestrator with background adaptation."""
        self._running = True
        
        # Detect network in background with timeout to avoid blocking
        detect_done = threading.Event()
        def _detect_and_select():
            try:
                self._detect_network()
                self._select_optimal_strategy()
            except Exception as e:
                self.logger.warning(f"Network detection failed: {e}")
                # Fallback defaults
                self.state.network_type = NetworkType.UNKNOWN
                self.state.network_condition = NetworkCondition.NORMAL
                self._select_optimal_strategy()
            finally:
                detect_done.set()
        
        detect_thread = threading.Thread(target=_detect_and_select, daemon=True)
        detect_thread.start()
        
        # Wait max 3 seconds for detection, then proceed anyway
        if not detect_done.wait(timeout=3.0):
            self.logger.warning("Network detection timed out, using defaults")
            self.state.network_type = NetworkType.UNKNOWN
            self.state.network_condition = NetworkCondition.NORMAL
            self._select_optimal_strategy()
        
        # Start background adaptation thread
        self._adaptation_thread = threading.Thread(
            target=self._adaptation_loop,
            daemon=True,
            name="DPI-Evasion-Adaptation"
        )
        self._adaptation_thread.start()
        
        self.logger.info(
            f"Evasion orchestrator started: network={self.state.network_type.value}, "
            f"condition={self.state.network_condition.value}, "
            f"strategy={self.state.active_strategy.value if self.state.active_strategy else 'none'}"
        )
    
    def stop(self):
        """Stop the evasion orchestrator."""
        self._running = False
        if self._adaptation_thread:
            self._adaptation_thread.join(timeout=5)
        self.logger.info("Evasion orchestrator stopped")
    
    def get_optimal_strategy(self) -> EvasionStrategy:
        """Get the current optimal evasion strategy."""
        with self._lock:
            if self.state.active_strategy:
                return self.state.active_strategy
        return EvasionStrategy.REALITY_VISION  # Safe default
    
    def report_connection_result(self, strategy: EvasionStrategy, 
                                  success: bool, latency_ms: float = 0.0,
                                  error: str = ""):
        """
        Report the result of a connection attempt.
        Used to adapt strategy selection over time.
        """
        with self._lock:
            key = strategy.value
            self.state.strategy_attempts[key] = \
                self.state.strategy_attempts.get(key, 0) + 1
            self.state.total_connections += 1
            
            if success:
                self.state.strategy_successes[key] = \
                    self.state.strategy_successes.get(key, 0) + 1
                self.state.successful_connections += 1
                self.state.last_successful_strategy = strategy
                self.state.last_connection_time = time.time()
                
                # Increase strategy score
                current = self._strategy_scores.get(strategy, 0.5)
                self._strategy_scores[strategy] = min(1.0, current + 0.05)
            else:
                self.state.strategy_failures[key] = \
                    self.state.strategy_failures.get(key, 0) + 1
                
                # Decrease strategy score
                current = self._strategy_scores.get(strategy, 0.5)
                self._strategy_scores[strategy] = max(0.0, current - 0.1)
                
                # If current strategy failed, try next in fallback chain
                if strategy == self.state.active_strategy:
                    self._advance_fallback_chain()
    
    def get_config_for_uri(self, uri: str) -> Dict[str, Any]:
        """
        Enhance a proxy URI with optimal evasion settings.
        
        Takes an existing proxy config URI and applies the current
        optimal evasion strategy settings.
        """
        strategy = self.get_optimal_strategy()
        protocol_info = STRATEGY_PROTOCOL_MAP.get(strategy, {})
        
        enhancements = {
            "strategy": strategy.value,
            "network_type": self.state.network_type.value,
        }
        
        # Add fingerprint
        fp = self.fingerprint_engine.get_rotating_fingerprint(prefer_h2=True)
        enhancements["fingerprint"] = fp.xray_fingerprint
        enhancements["alpn"] = fp.alpn
        
        # Add fragmentation if needed
        if protocol_info.get("fragment") or strategy == EvasionStrategy.DIRECT_FRAGMENT:
            frag_config = self.fragmentation_engine.get_xray_fragment_config()
            enhancements["sockopt"] = frag_config
        
        # Add MTU optimization
        mtu = self.mtu_optimizer.get_optimal_mtu(self.state.network_type)
        enhancements["mtu"] = mtu
        
        return enhancements
    
    def generate_xray_config(self, server_address: str, 
                              server_port: int = 443,
                              user_uuid: str = "",
                              strategy: Optional[EvasionStrategy] = None,
                              **kwargs) -> Dict[str, Any]:
        """
        Generate a complete Xray configuration with all evasion features.
        
        This is the primary config generation endpoint that applies all
        optimizations from the article.
        """
        if strategy is None:
            strategy = self.get_optimal_strategy()
        
        self.logger.info(f"Generating config with strategy: {strategy.value}")
        
        if strategy in (EvasionStrategy.REALITY_VISION, 
                        EvasionStrategy.REALITY_VISION_FRAGMENT):
            network_type = "fiber"
            if self.state.network_type in (NetworkType.MOBILE_4G, NetworkType.MOBILE_5G):
                network_type = self.state.network_type.value
            elif strategy == EvasionStrategy.REALITY_VISION_FRAGMENT:
                network_type = "blackout"
            
            config = self.reality_generator.generate_client_config(
                server_address=server_address,
                server_port=server_port,
                user_uuid=user_uuid,
                enable_fragment=(strategy == EvasionStrategy.REALITY_VISION_FRAGMENT),
                network_type=network_type,
                **{k: v for k, v in kwargs.items() 
                   if k in ('public_key', 'short_id', 'server_name', 'socks_port', 'http_port')}
            )
            
        elif strategy == EvasionStrategy.SPLITHTTP_CDN:
            # Use SplitHTTPTransport for network-aware chunk/timing settings
            network_key = self.state.network_type.value if self.state.network_type else "fiber"
            net_settings = self.splithttp_transport.get_optimal_settings_for_network(network_key)
            self.splithttp_transport.switch_pattern(net_settings["pattern"])
            
            config = self.reality_generator.generate_splithttp_config(
                server_address=server_address,
                server_port=server_port,
                user_uuid=user_uuid,
                **{k: v for k, v in kwargs.items()
                   if k in ('cdn_host', 'path', 'socks_port')}
            )
            
        elif strategy == EvasionStrategy.VLESS_WS_CDN:
            config = self.reality_generator.generate_websocket_cdn_config(
                server_address=server_address,
                server_port=server_port,
                user_uuid=user_uuid,
                **{k: v for k, v in kwargs.items()
                   if k in ('cdn_host', 'ws_path', 'socks_port')}
            )
            
        elif strategy == EvasionStrategy.HYSTERIA2_BRUTAL:
            config = self.udp_manager.generate_hysteria2_client_config(
                server_address=server_address,
                server_port=server_port,
                enable_port_hopping=True,
                **{k: v for k, v in kwargs.items()
                   if k in ('auth_password', 'sni', 'up_mbps', 'down_mbps', 
                           'obfs_password', 'socks_port', 'http_port')}
            )
            config["_config_format"] = "singbox"
            
        elif strategy == EvasionStrategy.TUIC_BBR:
            config = self.udp_manager.generate_tuic_client_config(
                server_address=server_address,
                server_port=server_port,
                **{k: v for k, v in kwargs.items()
                   if k in ('user_uuid', 'auth_password', 'sni', 'socks_port')}
            )
            config["_config_format"] = "singbox"
            
        else:
            # Default: Reality Vision
            config = self.reality_generator.generate_client_config(
                server_address=server_address,
                server_port=server_port,
                user_uuid=user_uuid,
                network_type="fiber",
            )
        
        return config
    
    def enhance_proxy_config(self, xray_config: Dict[str, Any]) -> Dict[str, Any]:
        """
        Enhance an existing Xray config with all evasion features.
        
        Applies: fingerprint randomization, fragmentation, MTU optimization,
        TCP tuning - without changing the protocol/transport.
        """
        network_type = self.state.network_type.value if self.state.network_type else "fiber"
        return self.reality_generator.enhance_existing_config(xray_config, network_type)
    
    def score_proxy_uri(self, uri: str) -> float:
        """
        Score a proxy URI based on current evasion strategy compatibility.
        
        Returns 0.0-1.0 score indicating how well this URI matches
        the current optimal strategy.
        """
        strategy = self.get_optimal_strategy()
        protocol_info = STRATEGY_PROTOCOL_MAP.get(strategy, {})
        
        uri_lower = uri.lower()
        score = 0.0
        
        required_protocol = protocol_info.get("protocol", "")
        required_transport = protocol_info.get("transport", "")
        required_security = protocol_info.get("security", "")
        
        # Protocol match
        if required_protocol and uri_lower.startswith(f"{required_protocol}://"):
            score += 0.3
        
        # Transport match
        if required_transport:
            transport_indicators = {
                "tcp": ["type=tcp", "network=tcp"],
                "ws": ["type=ws", "network=ws", "websocket"],
                "grpc": ["type=grpc", "network=grpc", "gun"],
                "splithttp": ["type=splithttp", "splithttp", "xhttp"],
            }
            for indicator in transport_indicators.get(required_transport, []):
                if indicator in uri_lower:
                    score += 0.2
                    break
        
        # Security match
        if required_security:
            security_indicators = {
                "reality": ["security=reality", "pbk=", "reality"],
                "tls": ["security=tls", "tls"],
            }
            for indicator in security_indicators.get(required_security, []):
                if indicator in uri_lower:
                    score += 0.2
                    break
        
        # Flow match (Vision)
        if protocol_info.get("flow") and "flow=xtls-rprx-vision" in uri_lower:
            score += 0.15
        
        # Fingerprint present
        if "fp=" in uri_lower:
            score += 0.1
        
        # CDN-based
        if protocol_info.get("cdn"):
            cdn_domains = ["cloudflare", "workers.dev", "pages.dev", "gcore", "fastly"]
            for cdn in cdn_domains:
                if cdn in uri_lower:
                    score += 0.1
                    break
        
        # Port 443 (whitelisted)
        if ":443" in uri:
            score += 0.05
        
        return min(1.0, score)
    
    def prioritize_configs_for_strategy(self, uris: List[str]) -> List[str]:
        """
        Prioritize proxy URIs based on current evasion strategy.
        
        Sorts URIs by compatibility with the active strategy.
        """
        scored = [(uri, self.score_proxy_uri(uri)) for uri in uris]
        scored.sort(key=lambda x: x[1], reverse=True)
        return [uri for uri, _ in scored]
    
    def _detect_network(self):
        """Detect current network type and conditions (parallel for speed)."""
        # Run all detection tasks in parallel to minimize total wait time
        results = {}
        
        def _detect_type():
            results["type"] = self.mtu_optimizer.detect_network_type()
        
        def _detect_isp_bg():
            results["isp"] = self._detect_isp()
        
        def _detect_cond():
            results["condition"] = self._detect_network_condition()
        
        threads = [
            threading.Thread(target=_detect_type, daemon=True),
            threading.Thread(target=_detect_isp_bg, daemon=True),
            threading.Thread(target=_detect_cond, daemon=True),
        ]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=3.0)
        
        new_type = results.get("type", NetworkType.UNKNOWN)
        new_isp = results.get("isp", "unknown")
        new_condition = results.get("condition", NetworkCondition.NORMAL)
        
        changed = (
            new_type != self.state.network_type or
            new_condition != self.state.network_condition or
            new_isp != self.state.detected_isp
        )
        
        self.state.network_type = new_type
        self.state.detected_isp = new_isp
        self.state.network_condition = new_condition
        
        if changed:
            self.logger.info(
                f"Network detected: type={new_type.value}, "
                f"condition={new_condition.value}, "
                f"isp={new_isp}"
            )
    
    def _detect_isp(self) -> str:
        """Attempt to detect the ISP."""
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.settimeout(1)
            s.connect(("8.8.8.8", 80))
            local_ip = s.getsockname()[0]
            s.close()
            
            # Check against known Iranian ISP IP ranges
            if local_ip.startswith(("2.176.", "2.177.", "2.178.", "2.179.")):
                return "TCI"
            elif local_ip.startswith(("5.200.", "5.201.", "5.202.")):
                return "Irancell"
            elif local_ip.startswith(("37.191.", "37.192.", "37.193.")):
                return "MCI"
            elif local_ip.startswith(("31.24.232.", "31.24.233.")):
                return "Shatel"
            elif local_ip.startswith(("78.38.", "78.39.")):
                return "Pishgaman"
            elif local_ip.startswith(("5.56.", "5.57.")):
                return "MobinNet"
            
            return "unknown"
        except Exception:
            return "unknown"
    
    def _detect_network_condition(self) -> NetworkCondition:
        """
        Detect current network filtering condition.
        All tests run in parallel with 1.5s timeout each.
        """
        results = {}
        
        def _test_cdn():
            results["cdn"] = self._quick_connectivity_test("1.1.1.1", 443, timeout=1.5)
        
        def _test_google():
            results["google"] = self._quick_connectivity_test("142.250.185.68", 443, timeout=1.5)
        
        def _test_udp():
            results["udp"] = self._test_udp_connectivity()
        
        threads = [
            threading.Thread(target=_test_cdn, daemon=True),
            threading.Thread(target=_test_google, daemon=True),
            threading.Thread(target=_test_udp, daemon=True),
        ]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=2.0)
        
        cdn_reachable = results.get("cdn", False)
        google_reachable = results.get("google", False)
        udp_available = results.get("udp", False)
        
        if not cdn_reachable and not google_reachable:
            return NetworkCondition.BLACKOUT
        if not cdn_reachable:
            return NetworkCondition.HEAVY_DPI
        if not udp_available:
            return NetworkCondition.UDP_BLOCKED
        return NetworkCondition.NORMAL
    
    def _quick_connectivity_test(self, host: str, port: int, 
                                  timeout: float = 1.5) -> bool:
        """Quick TCP connectivity test with short timeout."""
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(timeout)
            result = sock.connect_ex((host, port))
            sock.close()
            return result == 0
        except Exception:
            try:
                sock.close()
            except Exception:
                pass
            return False
    
    def _test_udp_connectivity(self) -> bool:
        """Test if UDP/QUIC is available (not throttled). Timeout: 1.5s."""
        sock = None
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.settimeout(1.5)
            dns_query = (
                b'\x12\x34'  # Transaction ID
                b'\x01\x00'  # Flags: standard query
                b'\x00\x01'  # Questions: 1
                b'\x00\x00\x00\x00'  # Answers, Authority, Additional: 0
                b'\x06google\x03com\x00'  # Query: google.com
                b'\x00\x01'  # Type: A
                b'\x00\x01'  # Class: IN
            )
            sock.sendto(dns_query, ("1.1.1.1", 53))
            data, _ = sock.recvfrom(512)
            sock.close()
            return len(data) > 0
        except Exception:
            if sock:
                try:
                    sock.close()
                except Exception:
                    pass
            return False
    
    def _select_optimal_strategy(self):
        """Select the optimal strategy based on current conditions."""
        condition = self.state.network_condition
        chain = FALLBACK_CHAINS.get(condition, FALLBACK_CHAINS[NetworkCondition.NORMAL])
        
        # Use strategy scores to pick the best from the chain
        best_strategy = None
        best_score = -1
        
        for strategy in chain:
            score = self._strategy_scores.get(strategy, 0.5)
            if score > best_score:
                best_score = score
                best_strategy = strategy
        
        with self._lock:
            self.state.active_strategy = best_strategy
            self.state.fallback_chain_position = 0
        
        self.logger.info(f"Selected strategy: {best_strategy.value} (score: {best_score:.2f})")
    
    def _advance_fallback_chain(self):
        """Move to next strategy in the fallback chain."""
        condition = self.state.network_condition
        chain = FALLBACK_CHAINS.get(condition, FALLBACK_CHAINS[NetworkCondition.NORMAL])
        
        with self._lock:
            self.state.fallback_chain_position += 1
            if self.state.fallback_chain_position < len(chain):
                self.state.active_strategy = chain[self.state.fallback_chain_position]
                self.logger.info(
                    f"Fallback to strategy: {self.state.active_strategy.value} "
                    f"(position {self.state.fallback_chain_position}/{len(chain)})"
                )
            else:
                # Reset and try again
                self.state.fallback_chain_position = 0
                self.state.active_strategy = chain[0]
                self.logger.warning("All strategies exhausted, resetting fallback chain")
    
    def _adaptation_loop(self):
        """Background loop that adapts strategy based on conditions."""
        while self._running:
            try:
                time.sleep(60)  # Re-evaluate every 60 seconds (was 5 minutes)
                
                if not self._running:
                    break
                
                # Re-detect network conditions with timeout
                old_condition = self.state.network_condition
                detect_done = threading.Event()
                def _bg_detect():
                    try:
                        self._detect_network()
                    except Exception:
                        pass
                    finally:
                        detect_done.set()
                
                t = threading.Thread(target=_bg_detect, daemon=True)
                t.start()
                detect_done.wait(timeout=3.0)
                
                # If conditions changed, re-select strategy
                if self.state.network_condition != old_condition:
                    self.logger.info(
                        f"Network condition changed: {old_condition.value} -> "
                        f"{self.state.network_condition.value}"
                    )
                    self._select_optimal_strategy()
                
                # Decay strategy scores toward 0.5 (reset bias)
                for strategy in self._strategy_scores:
                    current = self._strategy_scores[strategy]
                    self._strategy_scores[strategy] = current * 0.95 + 0.5 * 0.05
                
            except Exception as e:
                self.logger.error(f"Adaptation loop error: {e}")
                time.sleep(60)
    
    def get_all_metrics(self) -> Dict[str, Any]:
        """Get comprehensive metrics from all evasion modules."""
        with self._lock:
            success_rate = (
                self.state.successful_connections / 
                max(1, self.state.total_connections)
            ) * 100
            
            return {
                "orchestrator": {
                    "active_strategy": self.state.active_strategy.value if self.state.active_strategy else None,
                    "network_type": self.state.network_type.value,
                    "network_condition": self.state.network_condition.value,
                    "detected_isp": self.state.detected_isp,
                    "total_connections": self.state.total_connections,
                    "successful_connections": self.state.successful_connections,
                    "success_rate": round(success_rate, 1),
                    "fallback_position": self.state.fallback_chain_position,
                    "strategy_scores": {
                        s.value: round(score, 3) 
                        for s, score in self._strategy_scores.items()
                    },
                },
                "fingerprint": self.fingerprint_engine.get_metrics(),
                "fragmentation": self.fragmentation_engine.get_metrics(),
                "mtu": self.mtu_optimizer.get_metrics(),
                "probe_defense": self.probe_defender.get_metrics(),
                "entropy": self.entropy_normalizer.get_metrics(),
                "udp_protocols": self.udp_manager.get_metrics(),
                "splithttp": self.splithttp_transport.get_metrics(),
            }
    
    def get_status_summary(self) -> str:
        """Get a human-readable status summary."""
        metrics = self.get_all_metrics()
        orch = metrics["orchestrator"]
        
        return (
            f"Strategy: {orch['active_strategy']} | "
            f"Network: {orch['network_type']} ({orch['network_condition']}) | "
            f"ISP: {orch['detected_isp']} | "
            f"Success: {orch['success_rate']}% ({orch['successful_connections']}/{orch['total_connections']})"
        )
