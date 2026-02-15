"""
Security module - 2026 Iranian DPI Evasion Suite

Comprehensive anti-censorship toolkit designed for Iran's TSG (Traffic Secure Gateway)
and Whitelist-based filtering architecture. Implements all evasion strategies from the
2026 technical report.

Modules:
- tls_fingerprint_evasion: JA3/JA4 spoofing, uTLS identity randomization
- tls_fragmentation: Advanced ClientHello fragmentation with timing jitter
- reality_config_generator: VLESS-Reality-Vision + SplitHTTP config generation
- udp_protocols: Hysteria2/TUIC with port hopping and BBR/Brutal congestion
- mtu_optimizer: 5G PMTUD attack mitigation, TCP buffer tuning
- active_probe_defense: Active probe detection/deflection, entropy normalization
- dpi_evasion_orchestrator: Adaptive strategy selection based on network conditions
- adversarial_dpi_exhaustion: ADEE engine (Aho-Corasick stress, noise generation)
- stealth_obfuscation: Integration layer for ADEE with proxy infrastructure
"""

from .adversarial_dpi_exhaustion import (
    AdversarialDPIExhaustionEngine,
    ADEEIntegrator,
    ExhaustionMetrics,
)
from .stealth_obfuscation import (
    StealthObfuscationEngine,
    ObfuscationConfig,
    ProxyStealthWrapper,
)

# 2026 DPI Evasion modules - imported with graceful fallback
try:
    from .tls_fingerprint_evasion import TLSFingerprintEvasion, BrowserProfile
except ImportError:
    TLSFingerprintEvasion = None
    BrowserProfile = None

try:
    from .tls_fragmentation import TLSFragmentationEngine, FragmentConfig, FragmentStrategy
except ImportError:
    TLSFragmentationEngine = None
    FragmentConfig = None
    FragmentStrategy = None

try:
    from .reality_config_generator import RealityConfigGenerator
except ImportError:
    RealityConfigGenerator = None

try:
    from .udp_protocols import UDPProtocolManager
except ImportError:
    UDPProtocolManager = None

try:
    from .mtu_optimizer import MTUOptimizer, NetworkType
except ImportError:
    MTUOptimizer = None
    NetworkType = None

try:
    from .active_probe_defense import ActiveProbeDefender, EntropyNormalizer
except ImportError:
    ActiveProbeDefender = None
    EntropyNormalizer = None

try:
    from .split_http_transport import SplitHTTPTransport
except ImportError:
    SplitHTTPTransport = None

try:
    from .dpi_evasion_orchestrator import DPIEvasionOrchestrator, EvasionStrategy
except ImportError:
    DPIEvasionOrchestrator = None
    EvasionStrategy = None

__all__ = [
    # Legacy ADEE
    "AdversarialDPIExhaustionEngine",
    "ADEEIntegrator",
    "ExhaustionMetrics",
    "StealthObfuscationEngine",
    "ObfuscationConfig",
    "ProxyStealthWrapper",
    # 2026 DPI Evasion
    "TLSFingerprintEvasion",
    "BrowserProfile",
    "TLSFragmentationEngine",
    "FragmentConfig",
    "FragmentStrategy",
    "RealityConfigGenerator",
    "UDPProtocolManager",
    "MTUOptimizer",
    "NetworkType",
    "ActiveProbeDefender",
    "EntropyNormalizer",
    "SplitHTTPTransport",
    "DPIEvasionOrchestrator",
    "EvasionStrategy",
]
