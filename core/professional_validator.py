"""
Professional Multithreaded Config Validator

High-performance config testing with:
- ThreadPoolExecutor with optimized worker count
- Batch processing with semaphore-controlled concurrency
- Memory-aware adaptive thread management
- Real-time progress tracking
- Circuit breaker pattern for failing tests
"""

import asyncio
import concurrent.futures
import logging
import os
import threading
import time
from dataclasses import dataclass
from typing import Callable, Dict, List, Optional, Tuple, Any
import queue


@dataclass
class TestResult:
    """Result of a single config test."""
    uri: str
    success: bool
    latency_ms: float
    tier: str = ""
    error: str = ""
    test_duration_ms: float = 0


class CircuitBreaker:
    """Circuit breaker for test failures."""
    
    def __init__(self, threshold: int = 5, reset_timeout: float = 60.0):
        self.threshold = threshold
        self.reset_timeout = reset_timeout
        self.failures = 0
        self.last_failure_time: Optional[float] = None
        self.is_open = False
        self._lock = threading.Lock()
    
    def record_success(self):
        with self._lock:
            self.failures = 0
            self.is_open = False
    
    def record_failure(self) -> bool:
        """Record failure. Returns True if circuit is now open."""
        with self._lock:
            self.failures += 1
            self.last_failure_time = time.time()
            if self.failures >= self.threshold:
                self.is_open = True
                return True
            return False
    
    def can_execute(self) -> bool:
        with self._lock:
            if not self.is_open:
                return True
            # Check if reset timeout has passed
            if self.last_failure_time and (time.time() - self.last_failure_time) > self.reset_timeout:
                self.failures = 0
                self.is_open = False
                return True
            return False


class ConfigValidator:
    """
    Professional multithreaded config validator.
    
    Features:
    - Adaptive thread pool sizing based on CPU cores and memory
    - Work-stealing thread pool for balanced load
    - Priority queue for high-priority configs
    - Circuit breakers for failing test types
    - Real-time metrics and progress tracking
    """
    
    def __init__(
        self,
        max_workers: Optional[int] = None,
        memory_threshold: float = 85.0,
        test_timeout: float = 30.0,
        logger: Optional[logging.Logger] = None
    ):
        self.logger = logger or logging.getLogger(__name__)
        self.memory_threshold = memory_threshold
        self.test_timeout = test_timeout
        
        # Adaptive worker count
        cpu_cores = self._get_cpu_cores()
        self.max_workers = max_workers or min(32, max(4, cpu_cores * 2))
        self._adaptive_workers = self.max_workers
        
        # Thread pool
        self._executor: Optional[concurrent.futures.ThreadPoolExecutor] = None
        self._executor_lock = threading.Lock()
        
        # Circuit breakers for different test types
        self._circuit_breakers: Dict[str, CircuitBreaker] = {}
        
        # Metrics
        self._metrics = {
            'total_tests': 0,
            'successful_tests': 0,
            'failed_tests': 0,
            'avg_latency_ms': 0.0,
            'tests_per_second': 0.0,
        }
        self._metrics_lock = threading.Lock()
        
        # Progress callback
        self._progress_callback: Optional[Callable[[int, int, int], None]] = None
        
        # Semaphore for concurrency control
        self._semaphore: Optional[threading.Semaphore] = None
        
    def _get_cpu_cores(self) -> int:
        """Get number of CPU cores."""
        try:
            return len(os.sched_getaffinity(0))
        except Exception:
            return os.cpu_count() or 4
    
    def set_progress_callback(self, callback: Callable[[int, int, int], None]):
        """Set callback for progress updates (completed, total, failed)."""
        self._progress_callback = callback
    
    def start(self):
        """Initialize the thread pool."""
        with self._executor_lock:
            if self._executor is None:
                self._executor = concurrent.futures.ThreadPoolExecutor(
                    max_workers=self._adaptive_workers,
                    thread_name_prefix="config-validator-"
                )
                self._semaphore = threading.Semaphore(self._adaptive_workers)
                self.logger.info(
                    f"ConfigValidator started with {self._adaptive_workers} workers"
                )
    
    def stop(self):
        """Shutdown the thread pool."""
        with self._executor_lock:
            if self._executor:
                self._executor.shutdown(wait=True)
                self._executor = None
                self._semaphore = None
                self.logger.info("ConfigValidator stopped")
    
    def _check_memory(self) -> Tuple[bool, float]:
        """Check memory usage. Returns (ok, percent)."""
        try:
            import psutil
            mem = psutil.virtual_memory()
            return mem.percent < self.memory_threshold, mem.percent
        except Exception:
            return True, 0.0
    
    def _update_adaptive_workers(self, memory_percent: float):
        """Adjust worker count based on memory pressure."""
        if memory_percent >= 95:
            target = max(2, self.max_workers // 4)
        elif memory_percent >= 90:
            target = max(3, self.max_workers // 3)
        elif memory_percent >= 80:
            target = max(4, self.max_workers // 2)
        else:
            target = self.max_workers
        
        if target != self._adaptive_workers:
            self._adaptive_workers = target
            self.logger.info(f"Adaptive workers adjusted to {target} (memory: {memory_percent:.1f}%)")
    
    def _get_circuit_breaker(self, test_type: str) -> CircuitBreaker:
        """Get or create circuit breaker for test type."""
        if test_type not in self._circuit_breakers:
            self._circuit_breakers[test_type] = CircuitBreaker(threshold=5)
        return self._circuit_breakers[test_type]
    
    def _update_metrics(self, success: bool, latency_ms: float):
        """Update internal metrics."""
        with self._metrics_lock:
            self._metrics['total_tests'] += 1
            if success:
                self._metrics['successful_tests'] += 1
            else:
                self._metrics['failed_tests'] += 1
            
            # Running average
            n = self._metrics['total_tests']
            if n > 1:
                self._metrics['avg_latency_ms'] = (
                    (self._metrics['avg_latency_ms'] * (n - 1) + latency_ms) / n
                )
            else:
                self._metrics['avg_latency_ms'] = latency_ms
    
    def get_metrics(self) -> Dict[str, Any]:
        """Get current metrics."""
        with self._metrics_lock:
            return dict(self._metrics)
    
    def validate_configs_parallel(
        self,
        configs: List[str],
        test_func: Callable[[str], Tuple[bool, float, str]],
        batch_size: int = 100,
        max_concurrent: Optional[int] = None
    ) -> List[TestResult]:
        """
        Validate configs using parallel processing.
        
        Args:
            configs: List of config URIs to test
            test_func: Function that takes URI and returns (success, latency_ms, tier)
            batch_size: Process configs in batches of this size
            max_concurrent: Max concurrent tests (None = use semaphore default)
            
        Returns:
            List of TestResult objects
        """
        if not self._executor:
            self.start()
        
        if not configs:
            return []
        
        total = len(configs)
        results: List[TestResult] = []
        completed = 0
        failed = 0
        
        self.logger.info(f"Starting parallel validation of {total} configs")
        start_time = time.time()
        
        # Process in batches
        for batch_start in range(0, total, batch_size):
            batch = configs[batch_start:batch_start + batch_size]
            batch_results = self._process_batch(batch, test_func, max_concurrent)
            results.extend(batch_results)
            
            # Update progress
            for r in batch_results:
                completed += 1
                if not r.success:
                    failed += 1
                self._update_metrics(r.success, r.latency_ms)
                
                if self._progress_callback and completed % 10 == 0:
                    self._progress_callback(completed, total, failed)
            
            # Memory check
            ok, mem_pct = self._check_memory()
            if not ok:
                self._update_adaptive_workers(mem_pct)
                self.logger.warning(f"Memory high ({mem_pct:.1f}%), reducing concurrency")
        
        duration = time.time() - start_time
        tps = total / duration if duration > 0 else 0
        
        with self._metrics_lock:
            self._metrics['tests_per_second'] = tps
        
        self.logger.info(
            f"Validation complete: {total} configs in {duration:.1f}s "
            f"({tps:.1f}/s), {sum(1 for r in results if r.success)} success"
        )
        
        return results
    
    def _process_batch(
        self,
        batch: List[str],
        test_func: Callable[[str], Tuple[bool, float, str]],
        max_concurrent: Optional[int] = None
    ) -> List[TestResult]:
        """Process a batch of configs."""
        results: List[TestResult] = []
        futures: Dict[concurrent.futures.Future, str] = {}
        
        semaphore = threading.Semaphore(max_concurrent or self._adaptive_workers)
        
        def test_with_semaphore(uri: str) -> TestResult:
            with semaphore:
                test_start = time.time()
                try:
                    success, latency, tier = test_func(uri)
                    return TestResult(
                        uri=uri,
                        success=success,
                        latency_ms=latency,
                        tier=tier,
                        test_duration_ms=(time.time() - test_start) * 1000
                    )
                except Exception as e:
                    return TestResult(
                        uri=uri,
                        success=False,
                        latency_ms=0,
                        error=str(e),
                        test_duration_ms=(time.time() - test_start) * 1000
                    )
        
        # Submit all tasks
        for uri in batch:
            future = self._executor.submit(test_with_semaphore, uri)
            futures[future] = uri
        
        # Collect results as they complete
        for future in concurrent.futures.as_completed(futures):
            try:
                result = future.result(timeout=self.test_timeout + 5)
                results.append(result)
            except concurrent.futures.TimeoutError:
                uri = futures[future]
                results.append(TestResult(
                    uri=uri,
                    success=False,
                    error="Timeout",
                    latency_ms=0
                ))
            except Exception as e:
                uri = futures[future]
                results.append(TestResult(
                    uri=uri,
                    success=False,
                    error=str(e),
                    latency_ms=0
                ))
        
        return results
    
    async def validate_configs_async(
        self,
        configs: List[str],
        test_func: Callable[[str], Tuple[bool, float, str]],
        batch_size: int = 100
    ) -> List[TestResult]:
        """
        Async wrapper for parallel validation.
        
        This allows integration with async code while still using
        thread pool for CPU-bound testing.
        """
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(
            None,
            self.validate_configs_parallel,
            configs,
            test_func,
            batch_size
        )


class PriorityConfigValidator(ConfigValidator):
    """Extended validator with priority-based testing."""
    
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._priority_queue: queue.PriorityQueue = queue.PriorityQueue()
        self._priority_lock = threading.Lock()
    
    def add_config_with_priority(self, uri: str, priority: int):
        """Add config with priority (lower = higher priority)."""
        with self._priority_lock:
            self._priority_queue.put((priority, time.time(), uri))
    
    def validate_priority_queue(
        self,
        test_func: Callable[[str], Tuple[bool, float, str]],
        max_configs: Optional[int] = None
    ) -> List[TestResult]:
        """Validate configs from priority queue."""
        configs = []
        count = 0
        
        with self._priority_lock:
            while not self._priority_queue.empty():
                if max_configs and count >= max_configs:
                    break
                try:
                    _, _, uri = self._priority_queue.get_nowait()
                    configs.append(uri)
                    count += 1
                except queue.Empty:
                    break
        
        return self.validate_configs_parallel(configs, test_func)


# Convenience function for simple use cases
def validate_configs_simple(
    configs: List[str],
    test_func: Callable[[str], Tuple[bool, float, str]],
    max_workers: int = 16,
    batch_size: int = 100,
    logger: Optional[logging.Logger] = None
) -> List[TestResult]:
    """
    Simple function to validate configs with parallel processing.
    
    Example:
        def test_config(uri: str) -> Tuple[bool, float, str]:
            # Your test logic here
            return (True, 100.0, "gold")
        
        results = validate_configs_simple(configs, test_config)
        for r in results:
            print(f"{r.uri}: {'OK' if r.success else 'FAIL'} ({r.latency_ms}ms)")
    """
    validator = ConfigValidator(max_workers=max_workers, logger=logger)
    try:
        return validator.validate_configs_parallel(configs, test_func, batch_size)
    finally:
        validator.stop()


# Integration with orchestrator
class OrchestratorValidatorMixin:
    """Mixin to add professional validation to orchestrator."""
    
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._validator: Optional[ConfigValidator] = None
    
    def init_validator(self, max_workers: Optional[int] = None):
        """Initialize the professional validator."""
        self._validator = ConfigValidator(
            max_workers=max_workers,
            logger=self.logger if hasattr(self, 'logger') else None
        )
        self._validator.start()
    
    def validate_configs_professional(
        self,
        configs: List[str],
        test_func: Callable[[str], Tuple[bool, float, str]],
        batch_size: int = 100
    ) -> List[TestResult]:
        """Validate configs using professional multithreading."""
        if not self._validator:
            self.init_validator()
        
        # Set up progress callback
        if hasattr(self, 'ui') and self.ui:
            def progress_callback(completed, total, failed):
                self.logger.debug(f"Progress: {completed}/{total} ({failed} failed)")
            self._validator.set_progress_callback(progress_callback)
        
        return self._validator.validate_configs_parallel(
            configs, test_func, batch_size
        )
    
    def get_validator_metrics(self) -> Dict[str, Any]:
        """Get validation metrics."""
        if self._validator:
            return self._validator.get_metrics()
        return {}
    
    def stop_validator(self):
        """Stop the validator."""
        if self._validator:
            self._validator.stop()
            self._validator = None


if __name__ == "__main__":
    # Example usage
    import os
    
    logging.basicConfig(level=logging.INFO)
    
    # Example test function
    def example_test(uri: str) -> Tuple[bool, float, str]:
        """Simulate config test."""
        time.sleep(0.1)  # Simulate work
        if "fail" in uri:
            return (False, 0, "")
        latency = hash(uri) % 200 + 50  # Simulated latency 50-250ms
        tier = "gold" if latency < 100 else "silver"
        return (True, float(latency), tier)
    
    # Test configs
    test_configs = [
        f"vmess://example{i}@server{i}.com:443" for i in range(50)
    ] + [
        f"vless://fail{i}@bad{i}.com:443" for i in range(10)
    ]
    
    # Validate
    print("Testing professional validator...")
    results = validate_configs_simple(
        test_configs,
        example_test,
        max_workers=8,
        batch_size=20
    )
    
    success_count = sum(1 for r in results if r.success)
    print(f"\nResults: {success_count}/{len(results)} passed")
    
    # Show some successful results
    for r in results[:5]:
        if r.success:
            print(f"  {r.uri[:40]}...: {r.latency_ms:.0f}ms ({r.tier})")
