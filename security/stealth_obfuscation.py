"""
Stealth Obfuscation Engine - Advanced Evasion Integration for Hunter

Integrates AdversarialDPIExhaustionEngine (ADEE) with Hunter's proxy infrastructure.
Provides seamless evasion techniques without disrupting core proxy functionality.

Features:
- Aho-Corasick cache-miss stressors
- Micro-fragmentation for TLS Client Hello
- Dynamic SNI rotation with CDN whitelisting
- Thread-safe operation with background processing
- Protocol preservation (VLESS, VMess, Trojan)
- Resource exhaustion attacks on DPI engines
"""

import asyncio
import logging
import threading
import time
import random
from typing import Dict, List, Optional, Any, Callable
from dataclasses import dataclass
from concurrent.futures import ThreadPoolExecutor
import socket
import ssl
import struct
import os

from .adversarial_dpi_exhaustion import AdversarialDPIExhaustionEngine, ADEEIntegrator, ExhaustionMetrics

# 2026 DPI Evasion modules
try:
    from .tls_fingerprint_evasion import TLSFingerprintEvasion
    from .tls_fragmentation import TLSFragmentationEngine, FragmentConfig, FragmentStrategy
    from .mtu_optimizer import MTUOptimizer
    from .active_probe_defense import EntropyNormalizer
    _HAS_2026_MODULES = True
except ImportError:
    _HAS_2026_MODULES = False
    TLSFingerprintEvasion = None
    TLSFragmentationEngine = None
    FragmentConfig = None
    FragmentStrategy = None
    MTUOptimizer = None
    EntropyNormalizer = None


@dataclass
class ObfuscationConfig:
    """Configuration for stealth obfuscation."""
    enabled: bool = True
    use_async: bool = False
    micro_fragmentation: bool = True
    sni_rotation: bool = True
    noise_generation: bool = True
    ac_stress: bool = True
    exhaustion_level: float = 0.8
    noise_intensity: float = 0.7
    cdn_fronting: bool = True


class StealthObfuscationEngine:
    """
    Stealth Obfuscation Engine for Hunter.
    
    Integrates ADEE techniques with proxy infrastructure to evade
    Iranian Barracks Internet DPI systems.
    """
    
    def __init__(self, config: Optional[ObfuscationConfig] = None):
        self.logger = logging.getLogger(__name__)
        self.config = config or ObfuscationConfig()
        
        # Initialize ADEE
        self.adee = AdversarialDPIExhaustionEngine(
            enabled=self.config.enabled,
            log_level=logging.WARNING  # Reduce noise in logs
        )
        
        # Integration layer
        self.integrator = ADEEIntegrator()
        
        # Background executor
        self.executor = ThreadPoolExecutor(max_workers=4, thread_name_prefix="StealthEngine")
        
        # State tracking
        self.active_connections = {}
        self.connection_counter = 0
        
        # 2026 DPI Evasion modules
        self.fingerprint_engine: Optional[Any] = None
        self.fragmentation_engine: Optional[Any] = None
        self.mtu_optimizer: Optional[Any] = None
        self.entropy_normalizer: Optional[Any] = None
        
        if _HAS_2026_MODULES:
            try:
                self.fingerprint_engine = TLSFingerprintEvasion()
                frag_config = FragmentConfig(
                    enabled=self.config.micro_fragmentation,
                    strategy=FragmentStrategy.THREE_PART,
                    min_delay_ms=10.0,
                    max_delay_ms=50.0,
                )
                self.fragmentation_engine = TLSFragmentationEngine(frag_config)
                self.mtu_optimizer = MTUOptimizer()
                self.entropy_normalizer = EntropyNormalizer()
                self.logger.info("2026 DPI evasion modules loaded")
            except Exception as e:
                self.logger.warning(f"2026 DPI modules init failed (non-fatal): {e}")
        
        self.logger.info("StealthObfuscationEngine initialized")
    
    def start(self) -> bool:
        """Start the stealth obfuscation engine."""
        if not self.config.enabled:
            self.logger.info("Stealth obfuscation disabled")
            return False
        
        try:
            # Configure ADEE
            self.adee.exhaustion_level = self.config.exhaustion_level
            self.adee.noise_intensity = self.config.noise_intensity
            
            # Start ADEE
            if not self.adee.start(use_async=self.config.use_async):
                self.logger.info("ADEE start skipped (non-critical)")
                return False
            
            # Initialize integrator
            if not self.integrator.initialize(use_async=self.config.use_async):
                self.logger.info("ADEE integrator init skipped (non-critical)")
                return False
            
            # Scan CDN pairs if fronting enabled
            if self.config.cdn_fronting:
                self.integrator.adee.scan_cdn_pairs()
            
            self.logger.info("StealthObfuscationEngine started successfully")
            return True
            
        except Exception as e:
            self.logger.error(f"Failed to start StealthObfuscationEngine: {e}")
            return False
    
    def stop(self):
        """Stop the stealth obfuscation engine."""
        try:
            self.integrator.shutdown()
            self.adee.stop()
            self.executor.shutdown(wait=True)
            self.logger.info("StealthObfuscationEngine stopped")
        except Exception as e:
            self.logger.error(f"Error stopping StealthObfuscationEngine: {e}")
    
    def wrap_socket_connect(self, connect_func: Callable) -> Callable:
        """
        Wrap socket.connect to apply evasion techniques.
        
        This is the primary integration point for Hunter's proxy connections.
        """
        def wrapped_connect(sock, address):
            # Apply evasion before connection
            self._apply_pre_connection_evasion(sock, address)
            
            # Perform original connection
            result = connect_func(sock, address)
            
            # Apply post-connection techniques
            self._apply_post_connection_evasion(sock, address)
            
            return result
        
        return wrapped_connect
    
    def wrap_ssl_context(self, ssl_context: ssl.SSLContext) -> ssl.SSLContext:
        """
        Wrap SSL context to apply TLS-level evasion.
        """
        # Disable certificate verification for SNI spoofing
        ssl_context.check_hostname = False
        ssl_context.verify_mode = ssl.CERT_NONE
        
        # Wrap SSLContext.wrap_socket method
        original_wrap_socket = ssl_context.wrap_socket
        
        def wrapped_wrap_socket(sock, **kwargs):
            # Get current SNI for camouflage
            if 'server_hostname' in kwargs:
                original_sni = kwargs['server_hostname']
                
                # Replace with whitelisted SNI if needed
                if not self.integrator.adee.is_cdn_ip(sock.getpeername()[0]):
                    kwargs['server_hostname'] = self.integrator.adee.get_current_sni()
            
            # Apply micro-fragmentation for TLS handshake
            if self.config.micro_fragmentation:
                self._schedule_fragmented_handshake(sock, kwargs.get('server_hostname'))
            
            return original_wrap_socket(sock, **kwargs)
        
        ssl_context.wrap_socket = wrapped_wrap_socket
        return ssl_context
    
    def _apply_pre_connection_evasion(self, sock: socket.socket, address: tuple):
        """Apply evasion techniques before connection."""
        try:
            # Set socket options for stealth
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            
            # 2026: Apply MTU-aware socket optimization
            if self.mtu_optimizer:
                try:
                    self.mtu_optimizer.apply_socket_optimization(sock)
                except Exception:
                    pass
            
            # 2026: Wrap socket for TLS ClientHello fragmentation
            if self.fragmentation_engine and self.config.micro_fragmentation:
                try:
                    self.fragmentation_engine.wrap_socket(sock, address)
                except Exception:
                    pass
            
            # Apply noise generation if enabled
            if self.config.noise_generation:
                self._schedule_noise_generation(sock, address)
            
        except Exception as e:
            self.logger.debug(f"Pre-connection evasion error: {e}")
    
    def _apply_post_connection_evasion(self, sock: socket.socket, address: tuple):
        """Apply evasion techniques after connection."""
        try:
            # Track connection for metrics
            self.connection_counter += 1
            self.active_connections[self.connection_counter] = {
                'socket': sock,
                'address': address,
                'start_time': time.time()
            }
            
            # Clean up old connections
            self._cleanup_old_connections()
            
        except Exception as e:
            self.logger.debug(f"Post-connection evasion error: {e}")
    
    def _schedule_fragmented_handshake(self, sock: socket.socket, hostname: str):
        """Schedule micro-fragmented TLS handshake using 2026 or legacy engine."""
        # 2026: Use advanced 3-part fragmentation engine if available
        if self.fragmentation_engine and self.fingerprint_engine and hostname:
            try:
                fp = self.fingerprint_engine.get_rotating_fingerprint(prefer_h2=True)
                client_hello = self.fingerprint_engine.build_client_hello(fp, hostname)
                # Fragmentation will be applied by the wrapped socket send()
                self.logger.debug(
                    f"2026 fragmented handshake prepared: sni={hostname}, "
                    f"fp={fp.xray_fingerprint}, ja3={fp.ja3_hash[:12]}..."
                )
                return
            except Exception as e:
                self.logger.debug(f"2026 fragmentation fallback: {e}")
        
        # Legacy ADEE fragmentation fallback
        if self.config.use_async:
            try:
                loop = asyncio.get_event_loop()
                loop.create_task(
                    self.integrator.adee.send_fragmented_tls_hello(
                        hostname, 443
                    )
                )
            except RuntimeError:
                threading.Thread(
                    target=self.integrator.adee.send_fragmented_tls_hello,
                    args=(hostname, 443),
                    daemon=True
                ).start()
        else:
            threading.Thread(
                target=self.integrator.adee.send_fragmented_tls_hello,
                args=(hostname, 443),
                daemon=True
            ).start()
    
    def _schedule_noise_generation(self, sock: socket.socket, address: tuple):
        """Schedule noise generation for connection."""
        if self.config.noise_generation:
            threading.Thread(
                target=self._generate_connection_noise,
                args=(address,),
                daemon=True
            ).start()
    
    def _generate_connection_noise(self, address: tuple):
        """Generate noise for specific connection."""
        try:
            # Generate noise packets targeting the same destination
            noise_data = os.urandom(random.randint(64, 256))
            
            # Send multiple noise packets
            for _ in range(3):
                noise_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                noise_sock.settimeout(0.1)
                
                # Send to same destination with different port
                noise_address = (address[0], random.randint(10000, 20000))
                noise_sock.sendto(noise_data, noise_address)
                
                noise_sock.close()
                
                # Small delay
                time.sleep(0.01)
                
        except Exception:
            pass  # Ignore errors - this is noise
    
    def _cleanup_old_connections(self):
        """Clean up old connection tracking."""
        current_time = time.time()
        old_connections = []
        
        for conn_id, conn_info in self.active_connections.items():
            if current_time - conn_info['start_time'] > 300:  # 5 minutes
                old_connections.append(conn_id)
        
        for conn_id in old_connections:
            del self.active_connections[conn_id]
    
    def get_obfuscated_uri(self, original_uri: str, protocol: str) -> str:
        """
        Apply URI-level obfuscation for specific protocols.
        
        Maintains protocol compatibility while adding evasion layers.
        """
        try:
            if protocol.lower() == 'vmess':
                return self._obfuscate_vmess_uri(original_uri)
            elif protocol.lower() == 'vless':
                return self._obfuscate_vless_uri(original_uri)
            elif protocol.lower() == 'trojan':
                return self._obfuscate_trojan_uri(original_uri)
            elif protocol.lower() == 'ss':
                return self._obfuscate_ss_uri(original_uri)
            else:
                return original_uri
                
        except Exception as e:
            self.logger.warning(f"URI obfuscation failed: {e}")
            return original_uri
    
    def _obfuscate_vmess_uri(self, uri: str) -> str:
        """Obfuscate VMess URI."""
        # VMess URI format: vmess://base64(config_json)
        # For now, return unchanged - VMess already has built-in obfuscation
        return uri
    
    def _obfuscate_vless_uri(self, uri: str) -> str:
        """Obfuscate VLESS URI."""
        # VLESS URI format: vless://uuid@host:port?params#name
        # Add camouflage parameters
        if '?' in uri:
            base, params = uri.split('?', 1)
            # Add camouflage parameters
            camouflage_params = [
                f"camouflage={random.randint(1000, 9999)}",
                f"session={random.randint(100000, 999999)}"
            ]
            if '&' in params:
                params += '&' + '&'.join(camouflage_params)
            else:
                params += '&' + '&'.join(camouflage_params)
            return f"{base}?{params}"
        return uri
    
    def _obfuscate_trojan_uri(self, uri: str) -> str:
        """Obfuscate Trojan URI."""
        # Trojan URI format: trojan://password@host:port?params#name
        # Add session ID for camouflage
        if '?' in uri:
            base, params = uri.split('?', 1)
            session_param = f"session={random.randint(100000, 999999)}"
            params = session_param + ('&' if params else '') + params
            return f"{base}?{params}"
        else:
            return f"{uri}?session={random.randint(100000, 999999)}"
    
    def _obfuscate_ss_uri(self, uri: str) -> str:
        """Obfuscate Shadowsocks URI."""
        # Shadowsocks URI format: ss://base64(method:password@host:port)
        # Shadowsocks already has encryption, minimal obfuscation needed
        return uri
    
    def get_stealth_metrics(self) -> Dict[str, Any]:
        """Get comprehensive stealth metrics including 2026 evasion modules."""
        adee_metrics = self.integrator.get_metrics()
        
        metrics = {
            'adee_metrics': adee_metrics,
            'active_connections': len(self.active_connections),
            'total_connections': self.connection_counter,
            'config': {
                'enabled': self.config.enabled,
                'micro_fragmentation': self.config.micro_fragmentation,
                'sni_rotation': self.config.sni_rotation,
                'noise_generation': self.config.noise_generation,
                'ac_stress': self.config.ac_stress,
                'cdn_fronting': self.config.cdn_fronting
            },
            'status': self.integrator.get_status(),
            'has_2026_modules': _HAS_2026_MODULES,
        }
        
        # Add 2026 module metrics
        if self.fingerprint_engine:
            metrics['tls_fingerprint'] = self.fingerprint_engine.get_metrics()
        if self.fragmentation_engine:
            metrics['tls_fragmentation'] = self.fragmentation_engine.get_metrics()
        if self.mtu_optimizer:
            metrics['mtu_optimizer'] = self.mtu_optimizer.get_metrics()
        if self.entropy_normalizer:
            metrics['entropy_normalizer'] = self.entropy_normalizer.get_metrics()
        
        return metrics
    
    def update_config(self, **kwargs):
        """Update obfuscation configuration."""
        for key, value in kwargs.items():
            if hasattr(self.config, key):
                setattr(self.config, key, value)
                self.logger.info(f"Updated config: {key} = {value}")
        
        # Update ADEE if running
        if self.adee.running:
            self.adee.exhaustion_level = self.config.exhaustion_level
            self.adee.noise_intensity = self.config.noise_intensity
    
    def is_running(self) -> bool:
        """Check if stealth engine is running."""
        return self.adee.running and self.config.enabled


class ProxyStealthWrapper:
    """
    Wrapper class for Hunter proxy components.
    
    Provides drop-in integration with existing proxy infrastructure.
    """
    
    def __init__(self, proxy_instance, config: Optional[ObfuscationConfig] = None):
        self.logger = logging.getLogger(__name__)
        self.proxy_instance = proxy_instance
        self.stealth_engine = StealthObfuscationEngine(config)
        
        # Original methods
        self._original_connect = None
        self._original_send = None
        self._original_recv = None
        
        self.logger.info("ProxyStealthWrapper initialized")
    
    def start_stealth(self) -> bool:
        """Start stealth obfuscation."""
        if not self.stealth_engine.start():
            return False
        
        # Wrap proxy methods
        self._wrap_proxy_methods()
        
        self.logger.info("Proxy stealth mode activated")
        return True
    
    def stop_stealth(self):
        """Stop stealth obfuscation."""
        self.stealth_engine.stop()
        self._unwrap_proxy_methods()
        self.logger.info("Proxy stealth mode deactivated")
    
    def _wrap_proxy_methods(self):
        """Wrap proxy instance methods with stealth techniques."""
        if hasattr(self.proxy_instance, 'connect'):
            self._original_connect = self.proxy_instance.connect
            self.proxy_instance.connect = self.stealth_engine.wrap_socket_connect(
                self._original_connect
            )
        
        # Add stealth metrics method
        self.proxy_instance.get_stealth_metrics = self.stealth_engine.get_stealth_metrics
    
    def _unwrap_proxy_methods(self):
        """Restore original proxy methods."""
        if self._original_connect:
            self.proxy_instance.connect = self._original_connect
            self._original_connect = None
        
        # Remove stealth metrics method
        if hasattr(self.proxy_instance, 'get_stealth_metrics'):
            delattr(self.proxy_instance, 'get_stealth_metrics')
    
    def apply_uri_obfuscation(self, uris: List[str], protocol: str) -> List[str]:
        """Apply URI-level obfuscation to a list of URIs."""
        obfuscated = []
        for uri in uris:
            obfuscated_uri = self.stealth_engine.get_obfuscated_uri(uri, protocol)
            obfuscated.append(obfuscated_uri)
        return obfuscated
    
    def get_stealth_status(self) -> Dict[str, Any]:
        """Get comprehensive stealth status."""
        return {
            'engine_running': self.stealth_engine.is_running(),
            'proxy_wrapped': self._original_connect is not None,
            'metrics': self.stealth_engine.get_stealth_metrics()
        }


# Factory function for easy integration
def create_stealth_wrapper(proxy_instance, **config_kwargs) -> ProxyStealthWrapper:
    """
    Create and configure a stealth wrapper for a proxy instance.
    
    Args:
        proxy_instance: The proxy instance to wrap
        **config_kwargs: Configuration options for ObfuscationConfig
    
    Returns:
        Configured ProxyStealthWrapper instance
    """
    config = ObfuscationConfig(**config_kwargs)
    wrapper = ProxyStealthWrapper(proxy_instance, config)
    return wrapper


# Example usage
def example_integration():
    """Example of integrating stealth with a proxy."""
    logging.basicConfig(level=logging.INFO)
    
    # Mock proxy class
    class MockProxy:
        def __init__(self):
            self.logger = logging.getLogger("MockProxy")
        
        def connect(self, sock, address):
            sock.connect(address)
            self.logger.info(f"Connected to {address}")
        
        def send(self, sock, data):
            return sock.send(data)
        
        def recv(self, sock, size):
            return sock.recv(size)
    
    # Create proxy instance
    proxy = MockProxy()
    
    # Create stealth wrapper
    stealth_wrapper = create_stealth_wrapper(
        proxy,
        enabled=True,
        use_async=False,
        micro_fragmentation=True,
        sni_rotation=True,
        noise_generation=True,
        exhaustion_level=0.8
    )
    
    # Start stealth mode
    if stealth_wrapper.start_stealth():
        print("Stealth mode activated")
        
        # Simulate proxy usage
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        proxy.connect(sock, ("example.com", 443))
        
        # Get metrics
        metrics = stealth_wrapper.get_stealth_status()
        print(f"Stealth metrics: {metrics}")
        
        # Stop stealth mode
        stealth_wrapper.stop_stealth()
        print("Stealth mode deactivated")
    
    sock.close()


if __name__ == "__main__":
    example_integration()
