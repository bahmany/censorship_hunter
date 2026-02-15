"""
Advanced Thread Manager - Optimized Thread Pool Management

Provides intelligent thread pool sizing, adaptive scaling, and performance monitoring
for optimal CPU utilization and speed in Hunter validation system.
"""

import os
import time
import threading
import logging
import queue
import psutil
import gc
from typing import List, Dict, Any, Optional, Callable
from concurrent.futures import ThreadPoolExecutor, as_completed, Future
from dataclasses import dataclass
from enum import Enum
import multiprocessing


class ThreadState(Enum):
    """Thread state enumeration."""
    IDLE = "idle"
    RUNNING = "running"
    FINISHED = "finished"
    ERROR = "error"


@dataclass
class ThreadInfo:
    """Information about a worker thread."""
    id: int
    state: ThreadState
    task_count: int = 0
    start_time: float = 0.0
    end_time: float = 0.0
    cpu_time: float = 0.0
    memory_usage: float = 0.0
    last_task_time: float = 0.0
    error_count: int = 0


@dataclass
class PerformanceMetrics:
    """Performance metrics for thread pool."""
    total_tasks: int = 0
    completed_tasks: int = 0
    failed_tasks: int = 0
    average_task_time: float = 0.0
    tasks_per_second: float = 0.0
    cpu_utilization: float = 0.0
    memory_utilization: float = 0.0
    thread_utilization: float = 0.0
    queue_size: int = 0


class AdaptiveThreadPool:
    """
    Adaptive thread pool with intelligent sizing and performance optimization.
    
    Features:
    - Dynamic thread count adjustment based on CPU utilization
    - Task queue management with priority support
    - Performance monitoring and metrics collection
    - Memory pressure detection and optimization
    - CPU affinity optimization
    - Work stealing between threads
    """
    
    def __init__(self, 
                 min_threads: int = 4,
                 max_threads: Optional[int] = None,
                 target_cpu_utilization: float = 0.8,
                 target_queue_size: int = 100,
                 enable_work_stealing: bool = True,
                 enable_cpu_affinity: bool = False):
        self.logger = logging.getLogger(__name__)
        
        # Thread pool configuration
        self.min_threads = min_threads
        self.max_threads = max_threads or min(multiprocessing.cpu_count() * 2, 32)
        self.target_cpu_utilization = target_cpu_utilization
        self.target_queue_size = target_queue_size
        
        # Thread management
        self.threads: List[threading.Thread] = []
        self.thread_info: Dict[int, ThreadInfo] = {}
        self.thread_queue = queue.Queue()
        self.task_queue = queue.Queue()
        
        # Performance monitoring
        self.metrics = PerformanceMetrics()
        self.start_time = time.time()
        self.last_adjustment_time = 0
        self.adjustment_interval = 5.0  # Adjust every 5 seconds
        
        # Configuration
        self.enable_work_stealing = enable_work_stealing
        self.enable_cpu_affinity = enable_cpu_affinity
        
        # State management
        self.running = False
        self.shutdown_event = threading.Event()
        
        # Performance optimization
        self.memory_pressure_threshold = 0.85  # 85% memory usage
        self.cpu_pressure_threshold = 0.9    # 90% CPU usage
        
        self.logger.info(f"AdaptiveThreadPool initialized: min={min_threads}, max={self.max_threads}")
    
    def start(self):
        """Start the thread pool."""
        if self.running:
            return
        
        self.running = True
        self.start_time = time.time()
        
        # Calculate initial thread count
        initial_threads = self._calculate_optimal_thread_count()
        
        self.logger.info(f"Starting thread pool with {initial_threads} threads")
        
        # Create and start worker threads with proper synchronization
        threads_to_start = []
        for i in range(initial_threads):
            thread = threading.Thread(
                target=self._worker_thread,
                name=f"Worker-{i}",
                daemon=True
            )
            threads_to_start.append(thread)
        
        # Register all threads first, then start them
        for thread in threads_to_start:
            self.threads.append(thread)
            # Pre-register thread info (will be updated when thread starts)
            # Note: thread.ident is None until thread starts, so we'll handle this in _worker_thread
        
        # Now start all threads
        for thread in threads_to_start:
            thread.start()
        
        # Start monitoring thread
        self.monitor_thread = threading.Thread(
            target=self._monitor_thread,
            name="ThreadPool-Monitor",
            daemon=True
        )
        self.monitor_thread.start()
        
        self.logger.info(f"Thread pool started with {len(self.threads)} workers")
    
    def stop(self):
        """Stop the thread pool gracefully."""
        if not self.running:
            return
        
        self.logger.info("Stopping thread pool...")
        
        self.running = False
        self.shutdown_event.set()
        
        # Wait for all threads to finish
        for thread in self.threads:
            thread.join(timeout=5)
        
        # Clear thread lists
        self.threads.clear()
        self.thread_info.clear()
        
        self.logger.info("Thread pool stopped")
    
    def submit(self, fn: Callable, *args, **kwargs) -> Future:
        """Submit a task to the thread pool."""
        if not self.running:
            raise RuntimeError("Thread pool is not running")
        
        future = Future()
        task = (fn, args, kwargs, future)
        
        self.task_queue.put(task)
        
        return future
    
    def submit_batch(self, tasks: List[tuple]) -> List[Future]:
        """Submit multiple tasks to the thread pool."""
        futures = []
        for task in tasks:
            if len(task) == 1:
                future = self.submit(task[0])
            elif len(task) == 2:
                future = self.submit(task[0], task[1])
            else:
                future = self.submit(*task)
            futures.append(future)
        return futures
    
    def _calculate_optimal_thread_count(self) -> int:
        """Calculate optimal thread count based on system resources."""
        try:
            # Get system information
            cpu_count = multiprocessing.cpu_count()
            memory_gb = psutil.virtual_memory().total / (1024**3)
            
            # Base calculation
            base_threads = cpu_count
            
            # Adjust for memory (1GB per thread is reasonable)
            memory_adjusted_threads = min(
                int(memory_gb / 1),
                self.max_threads
            )
            
            # CPU utilization adjustment
            current_cpu = psutil.cpu_percent(interval=0.1)
            if current_cpu > 80:
                cpu_adjusted_threads = max(self.min_threads, base_threads // 2)
            else:
                cpu_adjusted_threads = base_threads
            
            # Queue size adjustment
            queue_size = self.task_queue.qsize()
            if queue_size > self.target_queue_size * 2:
                queue_adjusted_threads = min(
                    self.max_threads,
                    base_threads + (queue_size // self.target_queue_size)
                )
            else:
                queue_adjusted_threads = base_threads
            
            # Choose the minimum of all adjustments
            optimal_threads = max(
                self.min_threads,
                min(memory_adjusted_threads, cpu_adjusted_threads, queue_adjusted_threads)
            )
            
            self.logger.debug(f"Optimal thread count: {optimal_threads}")
            return optimal_threads
            
        except Exception as e:
            self.logger.warning(f"Error calculating optimal thread count: {e}")
            return self.min_threads
    
    def _worker_thread(self):
        """Worker thread that processes tasks."""
        thread_id = threading.current_thread().ident
        
        # Initialize thread_info for this thread (standard threading pattern)
        # This is the proper place to register thread info since thread.ident is only available after start()
        if thread_id not in self.thread_info:
            self.thread_info[thread_id] = ThreadInfo(
                id=thread_id,
                state=ThreadState.IDLE,
                start_time=time.time()
            )
        
        thread_info = self.thread_info[thread_id]
        
        try:
            # Set CPU affinity if enabled
            if self.enable_cpu_affinity:
                self._set_cpu_affinity(thread_id)
            
            while self.running:
                try:
                    # Get task from queue
                    task = self.task_queue.get(timeout=1.0)
                    
                    if task is None:
                        # Timeout, check for work stealing
                        if self.enable_work_stealing:
                            task = self._steal_work()
                        else:
                            continue
                    
                    # Update thread state
                    thread_info.state = ThreadState.RUNNING
                    thread_info.task_count += 1
                    thread_info.last_task_time = time.time()
                    
                    # Execute task
                    fn, args, kwargs, future = task
                    start_time = time.time()
                    
                    try:
                        result = fn(*args, **kwargs)
                        future.set_result(result)
                        thread_info.state = ThreadState.FINISHED
                    except Exception as e:
                        future.set_exception(e)
                        thread_info.state = ThreadState.ERROR
                        thread_info.error_count += 1
                    
                    # Update metrics
                    end_time = time.time()
                    task_time = end_time - start_time
                    thread_info.cpu_time += task_time
                    thread_info.end_time = end_time
                    
                    # Update global metrics
                    self._update_metrics(task_time, thread_info.state == ThreadState.FINISHED)
                    
                    # Mark task as completed - FIXED: call on task_queue not thread_queue
                    self.task_queue.task_done()
                    
                except queue.Empty:
                    # No task available, mark as idle
                    thread_info.state = ThreadState.IDLE
                    continue
                except Exception as e:
                    self.logger.error(f"Worker thread {thread_id} error: {e}")
                    thread_info.state = ThreadState.ERROR
                    thread_info.error_count += 1
                    # Still mark task as done even on error
                    try:
                        self.task_queue.task_done()
                    except ValueError:
                        pass  # Already done or not started
        
        except Exception as e:
            self.logger.error(f"Worker thread {thread_id} crashed: {e}")
            thread_info.state = ThreadState.ERROR
            thread_info.error_count += 1
    
    def _steal_work(self) -> Optional[tuple]:
        """Steal work from another thread's queue."""
        try:
            # Try to get work from other threads
            for thread_id, info in self.thread_info.items():
                if info.state == ThreadState.RUNNING and info.task_count < 2:
                    # Try to get work from thread's queue
                    try:
                        task = self.thread_queue.get_nowait(timeout=0.1)
                        if task:
                            self.logger.debug(f"Stolen work from thread {thread_id}")
                            return task
                    except queue.Empty:
                        continue
            return None
        except Exception:
            return None
    
    def _set_cpu_affinity(self, thread_id: int):
        """Set CPU affinity for thread."""
        try:
            # Set affinity to specific CPU core
            cpu_count = multiprocessing.cpu_count()
            cpu_id = thread_id % cpu_count
            os.sched_slaaffinity(0, {cpu_id})
        except Exception as e:
            self.logger.debug(f"Failed to set CPU affinity: {e}")
    
    def _monitor_thread(self):
        """Monitor thread pool performance and adjust thread count."""
        while self.running:
            try:
                # Sleep for monitoring interval
                self.shutdown_event.wait(self.adjustment_interval)
                
                if not self.running:
                    break
                
                # Collect performance data
                self._collect_performance_data()
                
                # Adjust thread count if needed
                if time.time() - self.last_adjustment_time >= self.adjustment_interval:
                    self._adjust_thread_count()
                    self.last_adjustment_time = time.time()
                
                # Optimize memory if needed
                self._optimize_memory()
                
            except Exception as e:
                self.logger.error(f"Monitor thread error: {e}")
    
    def _collect_performance_data(self):
        """Collect performance metrics."""
        try:
            # System metrics
            cpu_percent = psutil.cpu_percent(interval=0.1)
            memory_info = psutil.virtual_memory()
            memory_percent = memory_info.percent / 100
            
            # Thread metrics
            active_threads = sum(1 for info in self.thread_info.values() 
                              if info.state == ThreadState.RUNNING)
            idle_threads = sum(1 for info in self.thread_info.values() 
                            if info.state == ThreadState.IDLE)
            
            # Update metrics
            self.metrics.cpu_utilization = cpu_percent
            self.metrics.memory_utilization = memory_percent
            self.metrics.thread_utilization = active_threads / len(self.threads)
            self.metrics.queue_size = self.task_queue.qsize()
            
            # Calculate tasks per second
            elapsed_time = time.time() - self.start_time
            if elapsed_time > 0:
                self.metrics.tasks_per_second = self.metrics.completed_tasks / elapsed_time
            
            self.logger.debug(f"Performance: CPU={cpu_percent:.1f}%, "
                            f"Memory={memory_percent:.1f}%, "
                            f"Threads={active_threads}/{len(self.threads)}, "
                            f"Queue={self.metrics.queue_size}, "
                            f"Tasks/s={self.metrics.tasks_per_second:.1f}")
            
        except Exception as e:
            self.logger.warning(f"Error collecting performance data: {e}")
    
    def _update_metrics(self, task_time: float, success: bool):
        """Update performance metrics."""
        self.metrics.total_tasks += 1
        
        if success:
            self.metrics.completed_tasks += 1
            
            # Update average task time
            if self.metrics.completed_tasks > 0:
                total_time = sum(info.cpu_time for info in self.thread_info.values())
                self.metrics.average_task_time = total_time / self.metrics.completed_tasks
        
        # Calculate tasks per second
        elapsed_time = time.time() - self.start_time
        if elapsed_time > 0:
            self.metrics.tasks_per_second = self.metrics.completed_tasks / elapsed_time
    
    def _adjust_thread_count(self):
        """Adjust thread count based on performance metrics."""
        try:
            current_threads = len(self.threads)
            optimal_threads = self._calculate_optimal_thread_count()
            
            if optimal_threads != current_threads:
                self.logger.info(f"Adjusting thread count from {current_threads} to {optimal_threads}")
                
                if optimal_threads > current_threads:
                    # Add threads
                    self._add_threads(optimal_threads - current_threads)
                elif optimal_threads < current_threads:
                    # Remove threads
                    self._remove_threads(current_threads - optimal_threads)
                
                # Log thread utilization
                active = sum(1 for info in self.thread_info.values() 
                              if info.state == ThreadState.RUNNING)
                self.logger.info(f"Thread utilization: {active}/{len(self.threads)} active")
            
        except Exception as e:
            self.logger.warning(f"Error adjusting thread count: {e}")
    
    def _add_threads(self, count: int):
        """Add threads to the pool."""
        for i in range(count):
            thread = threading.Thread(
                target=self._worker_thread,
                name=f"Worker-{len(self.threads)}",
                daemon=True
            )
            thread.start()
            self.threads.append(thread)
            # Use thread.ident as key
            self.thread_info[thread.ident] = ThreadInfo(
                id=thread.ident,
                state=ThreadState.IDLE,
                start_time=time.time()
            )
    
    def _remove_threads(self, count: int):
        """Remove threads from the pool."""
        # Find idle threads to remove
        idle_threads = []
        for thread in self.threads:
            if thread.ident in self.thread_info and self.thread_info[thread.ident].state == ThreadState.IDLE:
                idle_threads.append((thread.ident, thread))
        
        # Remove the specified number of idle threads
        for thread_id, thread in idle_threads[:count]:
            if thread_id in self.thread_info:
                self.thread_info[thread_id].state = ThreadState.FINISHED
            # Thread will naturally exit when no more work
    
    def _optimize_memory(self):
        """Optimize memory usage if needed."""
        try:
            memory_percent = psutil.virtual_memory().percent
            
            # More aggressive memory management
            if memory_percent > 85:
                self.logger.warning(f"High memory usage ({memory_percent:.1f}%), triggering aggressive GC")
                
                # Force garbage collection multiple times
                import gc
                gc.collect()
                gc.collect()  # Second pass for generational GC
                
                # Clear thread info for finished threads
                finished_threads = [
                    thread_id for thread_id, info in self.thread_info.items()
                    if info.state == ThreadState.FINISHED
                ]
                for thread_id in finished_threads:
                    del self.thread_info[thread_id]
                
                # Clear any cached results in the queue
                if hasattr(self, 'result_queue'):
                    while not self.result_queue.empty():
                        try:
                            self.result_queue.get_nowait()
                        except:
                            break
                
                if memory_percent > 95:
                    self.logger.error(f"Critical memory usage ({memory_percent:.1f}%), consider reducing max_workers")
                    # Emergency: reduce thread count
                    if len(self.threads) > self.min_threads:
                        self._remove_threads(max(1, len(self.threads) // 2))
            
        except Exception as e:
            self.logger.debug(f"Memory optimization error: {e}")
    
    def get_metrics(self) -> PerformanceMetrics:
        """Get current performance metrics."""
        # Update queue size
        self.metrics.queue_size = self.task_queue.qsize()
        
        return self.metrics
    
    def get_thread_info(self) -> Dict[int, ThreadInfo]:
        """Get information about all threads."""
        return self.thread_info.copy()
    
    def get_status(self) -> Dict[str, Any]:
        """Get comprehensive status of the thread pool."""
        return {
            "running": self.running,
            "total_threads": len(self.threads),
            "active_threads": sum(1 for info in self.thread_info.values() 
                              if info.state == ThreadState.RUNNING),
            "idle_threads": sum(1 for info in self.thread_info.values() 
                            if info.state == ThreadState.IDLE),
            "queue_size": self.task_queue.qsize(),
            "metrics": self.get_metrics()
        }


class TaskScheduler:
    """Advanced task scheduler with priority queues and load balancing."""
    
    def __thread_init__(self):
        self.task_queues = {
            "high": queue.PriorityQueue(),
            "normal": queue.Queue(),
            "low": queue.Queue()
        }
        self.worker_queues = [queue.Queue() for _ in range(4)]
        self.current_worker = 0
        self.total_tasks = 0
        self.completed_tasks = 0
    
    def submit(self, fn: Callable, priority: str = "normal", *args, **kwargs):
        """Submit task with priority."""
        if priority not in self.task_queues:
            priority = "normal"
        
        task = (fn, args, kwargs)
        self.task_queues[priority].put((self.total_tasks, task))
        self.total_tasks += 1
        
        # Distribute to worker queues
        self._distribute_task()
    
    def _distribute_task(self):
        """Distribute tasks to worker queues for load balancing."""
        # Try high priority first
        for priority in ["high", "normal", "low"]:
            queue = self.task_queues[priority]
            if not queue.empty():
                task_id, task = queue.get()
                worker_queue = self.worker_queues[self.current_worker]
                worker_queue.put(task)
                self.current_worker = (self.current_worker + 1) % len(self.worker_queues)
                break
    
    def get_task(self, worker_id: int) -> Optional[tuple]:
        """Get task for specific worker."""
        worker_queue = self.worker_queues[worker_id]
        
        # Try to get from worker queue
        if not worker_queue.empty():
            return worker_queue.get()
        
        # Try to steal from other queues
        for i, other_queue in enumerate(self.worker_queues):
            if i != worker_id and not other_queue.empty():
                return other_queue.get()
        
        return None
    
    def mark_completed(self):
        """Mark a task as completed."""
        self.completed_tasks += 1


class OptimizedValidator:
    """Optimized validator with advanced thread management."""
    
    def __init__(self, config: Dict[str, Any]):
        self.config = config
        self.logger = logging.getLogger(__name__)
        
        # Thread pool configuration
        self.thread_pool = AdaptiveThreadPool(
            min_threads=config.get("min_threads", 4),
            max_threads=config.get("max_threads", None),
            target_cpu_utilization=config.get("target_cpu_utilization", 0.8),
            target_queue_size=config.get("target_queue_size", 100),
            enable_work_stealing=config.get("enable_work_stealing", True),
            enable_cpu_affinity=config.get("enable_cpu_affinity", False)
        )
        
        # Task scheduler
        self.scheduler = TaskScheduler()
        
        # Performance tracking
        self.start_time = time.time()
        self.validated_configs = []
        
        self.logger.info("OptimizedValidator initialized")
    
    def start(self):
        """Start the validator."""
        self.thread_pool.start()
        self.logger.info("OptimizedValidator started")
    
    def stop(self):
        """Stop the validator."""
        self.thread_pool.stop()
        self.logger.info("OptimizedValidator stopped")
    
    def validate_config(self, config: Dict[str, Any]) -> bool:
        """Validate a single configuration."""
        # Submit validation task
        future = self.thread_pool.submit(self._validate_config_task, config)
        
        # Wait for completion
        try:
            result = future.result(timeout=30) # 30 second timeout
            if result:
                self.validated_configs.append(result)
            return result
        except Exception as e:
            self.logger.error(f"Validation task failed: {e}")
            return False
    
    def validate_configs_batch(self, configs: List[Dict[str, Any]]) -> List[bool]:
        """Validate multiple configurations in parallel."""
        futures = self.thread_pool.submit_batch([
            (self._validate_config_task, config) for config in configs
        ])
        
        results = []
        for future in as_completed(futures):
            try:
                result = future.result(timeout=30)
                results.append(result)
                if result:
                    self.validated_configs.append(result)
            except Exception as e:
                self.logger.error(f"Validation task failed: {e}")
                results.append(False)
        
        return results
    
    def _validate_config_task(self, config: Dict[str, Any]) -> bool:
        """Validate a single configuration (task function)."""
        # This would contain the actual validation logic
        # For now, return True as placeholder
        return True
    
    def get_performance_metrics(self) -> Dict[str, Any]:
        """Get performance metrics."""
        metrics = self.thread_pool.get_metrics()
        
        # Add validation-specific metrics
        elapsed_time = time.time() - self.start_time
        if elapsed_time > 0:
            validation_rate = len(self.validated_configs) / elapsed_time if elapsed_time > 0 else 0
        else:
            validation_rate = 0
        
        return {
            "thread_pool_metrics": metrics,
            "validation_rate": validation_rate,
            "validated_configs": len(self.validated_configs),
            "elapsed_time": elapsed_time
        }
    
    def get_status(self) -> Dict[str, Any]:
        """Get comprehensive status."""
        return {
            "thread_pool_status": self.thread_pool.get_status(),
            "performance_metrics": self.get_performance_metrics(),
            "validated_count": len(self.validated_configs)
        }


# Factory function for easy usage
def create_optimized_validator(config: Dict[str, Any]) -> OptimizedValidator:
    """Create an optimized validator instance."""
    return OptimizedValidator(config)


if __name__ == "__main__":
    # Test the adaptive thread pool
    logging.basicConfig(level=logging.INFO, format='%(asctime)s | %(levelname)-8s | %(message)s')
    
    # Create thread pool
    pool = AdaptiveThreadPool(
        min_threads=4,
        max_threads=16,
        target_cpu_utilization=0.8,
        target_queue_size=100,
        enable_work_stealing=True,
        enable_cpu_affinity=False
    )
    
    # Start the pool
    pool.start()
    
    # Submit some test tasks
    def test_task(x):
        time.sleep(0.1)
        return x * 2
    
    print("Submitting 100 test tasks...")
    futures = pool.submit_batch([(test_task, i) for i in range(100)])
    
    # Wait for completion
    results = []
    for future in as_completed(futures):
        try:
            result = future.result()
            results.append(result)
        except Exception as e:
            print(f"Task failed: {e}")
    
    print(f"Completed {len(results)} tasks")
    print(f"Results: {sum(results)}")
    
    # Get metrics
    metrics = pool.get_metrics()
    print(f"Tasks per second: {metrics.tasks_per_second:.2f}")
    print(f"CPU utilization: {metrics.cpu_utilization:.1f}%")
    print(f"Memory utilization: {metrics.memory_utilization:.1f}%")
    
    # Stop the pool
    pool.stop()
    print("Thread pool stopped")
