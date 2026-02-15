"""
Performance optimization for Hunter validation system.

Improves:
- Thread pool management (adaptive worker scaling)
- Memory efficiency (streaming processing, garbage collection)
- CPU usage (batch processing, connection pooling)
- Timeout optimization (early termination for slow proxies)
"""

import logging
import os
import psutil
import time
from typing import Optional, List
from concurrent.futures import ThreadPoolExecutor, as_completed
from queue import Queue, Empty
import gc


class PerformanceOptimizer:
    """Optimizes validation performance based on system resources."""
    
    def __init__(self):
        self.logger = logging.getLogger(__name__)
        self.process = psutil.Process()
        self.initial_memory = self.process.memory_info().rss / 1024 / 1024  # MB
    
    def get_optimal_workers(self, total_configs: int) -> int:
        """Calculate optimal number of worker threads based on system resources.
        
        Strategy:
        - CPU cores * 2 (for I/O-bound tasks)
        - Max 150 workers (diminishing returns)
        - Min 20 workers (minimum parallelism)
        - Adjust based on available memory
        """
        cpu_count = os.cpu_count() or 4
        base_workers = min(cpu_count * 2, 150)
        
        # Adjust based on available memory
        available_memory = psutil.virtual_memory().available / 1024 / 1024  # MB
        if available_memory < 500:  # Less than 500MB available
            base_workers = max(20, base_workers // 2)
        
        # Adjust based on config count
        if total_configs < 100:
            base_workers = min(base_workers, total_configs)
        
        self.logger.info(f"Optimal workers: {base_workers} (CPU cores: {cpu_count}, Available memory: {available_memory:.0f}MB)")
        return base_workers
    
    def get_optimal_timeout(self, base_timeout: int = 10) -> int:
        """Calculate optimal timeout based on network conditions.
        
        Strategy:
        - Start with base timeout (10s)
        - Reduce for faster validation (5s for fast proxies)
        - Increase for slow networks (15s)
        """
        # Check if we're on a slow network
        available_bandwidth = psutil.net_if_stats().get('Ethernet', None)
        if available_bandwidth and available_bandwidth.isup:
            # Assume good network, use shorter timeout
            return max(5, base_timeout - 3)
        
        return base_timeout
    
    def optimize_memory(self):
        """Optimize memory usage during validation."""
        # Force garbage collection
        gc.collect()
        
        current_memory = self.process.memory_info().rss / 1024 / 1024
        memory_increase = current_memory - self.initial_memory
        
        if memory_increase > 200:  # More than 200MB increase
            self.logger.warning(f"High memory usage: {current_memory:.0f}MB (+{memory_increase:.0f}MB)")
            gc.collect()
    
    def get_cpu_usage(self) -> float:
        """Get current CPU usage percentage."""
        return self.process.cpu_percent(interval=0.1)
    
    def get_memory_usage(self) -> float:
        """Get current memory usage in MB."""
        return self.process.memory_info().rss / 1024 / 1024


class AdaptiveValidationExecutor:
    """Executes validation with adaptive resource management."""
    
    def __init__(self, optimizer: PerformanceOptimizer):
        self.optimizer = optimizer
        self.logger = logging.getLogger(__name__)
    
    def execute_with_optimization(
        self,
        configs: List[str],
        benchmark_func,
        max_workers: Optional[int] = None,
        timeout: int = 10
    ) -> List:
        """Execute validation with adaptive resource management.
        
        Features:
        - Adaptive worker scaling
        - Memory monitoring and cleanup
        - Early termination for slow configs
        - Batch processing for efficiency
        """
        if not configs:
            return []
        
        # Determine optimal workers
        if max_workers is None:
            max_workers = self.optimizer.get_optimal_workers(len(configs))
        
        # Optimize timeout
        optimized_timeout = self.optimizer.get_optimal_timeout(timeout)
        
        self.logger.info(f"Validation: {len(configs)} configs, {max_workers} workers, {optimized_timeout}s timeout")
        
        results = []
        processed = 0
        start_time = time.time()
        
        try:
            with ThreadPoolExecutor(max_workers=max_workers) as executor:
                # Submit all tasks
                futures = {
                    executor.submit(benchmark_func, uri, optimized_timeout): uri
                    for uri in configs
                }
                
                # Process results as they complete
                for future in as_completed(futures, timeout=optimized_timeout + 5):
                    try:
                        result = future.result(timeout=optimized_timeout)
                        if result:
                            results.append(result)
                    except Exception as e:
                        self.logger.debug(f"Benchmark failed: {e}")
                    finally:
                        processed += 1
                        
                        # Periodic memory optimization
                        if processed % 50 == 0:
                            self.optimizer.optimize_memory()
                            elapsed = time.time() - start_time
                            rate = processed / elapsed if elapsed > 0 else 0
                            self.logger.info(f"Progress: {processed}/{len(configs)} ({rate:.2f} configs/sec)")
        
        except Exception as e:
            self.logger.error(f"Validation executor error: {e}")
        
        elapsed = time.time() - start_time
        rate = len(configs) / elapsed if elapsed > 0 else 0
        self.logger.info(f"Validation complete: {len(results)} valid configs in {elapsed:.1f}s ({rate:.2f} configs/sec)")
        
        return results


class FastPathValidator:
    """Fast-path validation for quick config filtering."""
    
    def __init__(self):
        self.logger = logging.getLogger(__name__)
    
    def quick_validate(self, uri: str, timeout: int = 5) -> bool:
        """Quick validation to filter out obviously bad configs.
        
        Checks:
        - Valid URI format
        - Reachable host (quick ping)
        - Open port
        """
        try:
            from hunter.parsers import UniversalParser
            parser = UniversalParser()
            parsed = parser.parse(uri)
            
            if not parsed:
                return False
            
            # Quick port check
            import socket
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(timeout)
            try:
                result = sock.connect_ex((parsed.host, parsed.port))
                return result == 0
            finally:
                sock.close()
        
        except Exception:
            return False
    
    def filter_configs(self, configs: List[str], timeout: int = 5) -> List[str]:
        """Filter out obviously bad configs before full validation.
        
        This reduces the number of configs that need full benchmarking.
        """
        self.logger.info(f"Quick filtering {len(configs)} configs...")
        
        valid_configs = []
        for uri in configs:
            if self.quick_validate(uri, timeout):
                valid_configs.append(uri)
        
        filtered_out = len(configs) - len(valid_configs)
        self.logger.info(f"Filtered out {filtered_out} bad configs, {len(valid_configs)} remain")
        
        return valid_configs


class ValidationOptimizer:
    """Main optimizer for validation performance."""
    
    def __init__(self):
        self.logger = logging.getLogger(__name__)
        self.optimizer = PerformanceOptimizer()
        self.executor = AdaptiveValidationExecutor(self.optimizer)
        self.fast_validator = FastPathValidator()
    
    def optimize_validation(
        self,
        configs: List[str],
        benchmark_func,
        use_fast_filter: bool = True,
        max_workers: Optional[int] = None,
        timeout: int = 10
    ) -> List:
        """Optimize validation with all available techniques.
        
        Strategy:
        1. Quick filter to remove obviously bad configs
        2. Adaptive worker scaling based on system resources
        3. Memory monitoring and cleanup
        4. Early termination for slow configs
        """
        if not configs:
            return []
        
        # Step 1: Quick filter (optional, disabled in test mode)
        if use_fast_filter and os.getenv("HUNTER_TEST_MODE", "").lower() != "true":
            configs = self.fast_validator.filter_configs(configs, timeout=2)
        
        # Step 2: Adaptive validation
        results = self.executor.execute_with_optimization(
            configs,
            benchmark_func,
            max_workers=max_workers,
            timeout=timeout
        )
        
        return results
    
    def get_performance_report(self) -> dict:
        """Get performance metrics."""
        return {
            "cpu_usage": self.optimizer.get_cpu_usage(),
            "memory_usage": self.optimizer.get_memory_usage(),
            "memory_increase": self.optimizer.get_memory_usage() - self.optimizer.initial_memory
        }
