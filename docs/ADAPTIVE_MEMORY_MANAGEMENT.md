# Adaptive Memory Management System

## Overview

Hunter now includes a comprehensive **Adaptive Memory Management System** that monitors system memory at every stage of operation and automatically adjusts execution parameters to prevent crashes and ensure stable operation.

## Architecture

The memory management system operates at **three critical levels**:

### 1. **Startup Check** (`main.py`)
- Runs **before** the application starts
- Checks memory usage at application launch
- Forces aggressive garbage collection if memory > 80%
- **Prevents startup** if memory remains > 85% after cleanup

### 2. **Cycle Start Check** (`orchestrator.py` - `run_cycle()`)
- Runs at the **beginning of every hunter cycle**
- Monitors memory and adjusts all operation parameters dynamically
- Implements 5-tier adaptive configuration based on memory pressure
- Logs memory status at cycle start and end

### 3. **Benchmark Protection** (`testing/benchmark.py`)
- Emergency stop at **85%** memory (down from 90%)
- Warning + cleanup at **80%** memory (down from 85%)
- Memory-aware batch chunking (50 configs max per batch)
- Reduced max threads to **8** (down from 32)

## Adaptive Configuration Tiers

The system automatically adjusts parameters based on current memory usage:

| Memory Level | Status | max_total | max_workers | scan_limit | Action |
|--------------|--------|-----------|-------------|------------|--------|
| **≥ 85%** | CRITICAL | - | - | - | **Skip cycle entirely** |
| **80-84%** | SEVERE | 200 | 5 | 20 | Minimal operation |
| **70-79%** | HIGH | 400 | 8 | 30 | Reduced operation |
| **60-69%** | MODERATE | 600 | 10 | 40 | Conservative operation |
| **< 60%** | NORMAL | 800 | 10 | 50 | Full operation |

## Memory Cleanup Strategy

### At Startup (≥ 80% memory)
```python
1. Force 3 rounds of gc.collect() with 0.5s delays
2. Check memory again
3. If still ≥ 85%: Exit with error message
```

### At Cycle Start (≥ 75% memory)
```python
1. Force gc.collect()
2. Wait 0.5s
3. Re-check memory
4. Apply adaptive configuration based on new level
```

### During Benchmark (≥ 80% memory)
```python
1. Log warning
2. Force gc.collect()
3. Continue with caution
4. If ≥ 85%: Emergency stop
```

## Configuration Parameters

### Startup Memory Thresholds
- **Cleanup trigger**: 80%
- **Exit threshold**: 85%
- **GC rounds**: 3

### Cycle Memory Thresholds
- **Cleanup trigger**: 75%
- **Skip cycle**: 85%
- **Adaptive tiers**: 60%, 70%, 80%, 85%

### Benchmark Memory Thresholds
- **Warning**: 80%
- **Emergency stop**: 85%
- **Batch chunk size**: 50 configs
- **Max threads**: 8

## Logging

The system provides detailed logging at each stage:

### Startup
```
Memory status: 92.6% used (22.0GB / 23.7GB)
High memory at startup (92.6%), forcing aggressive cleanup...
After cleanup: 78.3% used
```

### Cycle Start
```
Starting hunter cycle #1
Memory status: 72.4% used (17.2GB / 23.7GB)
HIGH memory pressure (72.4%) - Reducing: max_total=400, workers=8
```

### Cycle End
```
Cycle completed in 45.2 seconds
Cycle end memory: 68.9% (16.3GB / 23.7GB)
```

### Benchmark
```
Memory warning: 82.1% - forcing cleanup
Memory after cleanup: 79.5%
Emergency stop: 86.2% - stopping benchmark to prevent crash
```

## Benefits

1. **Prevents System Crashes**: Never exceeds safe memory limits
2. **Automatic Adaptation**: No manual intervention required
3. **Graceful Degradation**: Reduces workload instead of failing
4. **Transparent Operation**: Clear logging of all decisions
5. **Configurable**: All thresholds can be adjusted if needed

## Technical Implementation

### Memory Monitoring
```python
import psutil
mem = psutil.virtual_memory()
mem_percent = mem.percent  # Current usage percentage
```

### Adaptive Configuration
```python
# Temporarily override config for this cycle
self.config.set("max_total", adaptive_max_total)
self.config.set("max_workers", adaptive_max_workers)
self.config.set("scan_limit", adaptive_scan_limit)

# ... run cycle operations ...

# Restore original values
self.config.set("max_total", original_max_total)
```

### Garbage Collection
```python
import gc
gc.collect()  # Force immediate garbage collection
```

## Troubleshooting

### Application Won't Start
**Symptom**: "Cannot start Hunter due to insufficient memory"

**Solution**:
1. Close other applications
2. Restart your system
3. Increase system RAM if consistently hitting this limit

### Cycles Being Skipped
**Symptom**: "CRITICAL MEMORY: 87.2% - Skipping this cycle"

**Solution**:
1. Wait for memory to free up naturally
2. Close unnecessary applications
3. Consider reducing base `max_total` in config

### Frequent Warnings
**Symptom**: "HIGH memory pressure" messages in every cycle

**Solution**:
1. This is normal - system is adapting automatically
2. Consider increasing system RAM for better performance
3. Reduce `max_total` in config to lower base memory usage

## Performance Impact

The adaptive system has **minimal performance overhead**:
- Memory check: < 0.01s per check
- Garbage collection: 0.5-1s when triggered
- Configuration adjustment: Negligible

The benefits far outweigh the minimal overhead:
- **No crashes** vs potential system freeze
- **Stable operation** vs unpredictable behavior
- **Automatic recovery** vs manual intervention

## Future Enhancements

Potential improvements for future versions:
1. Machine learning-based memory prediction
2. Per-protocol memory profiling
3. Dynamic batch size adjustment during benchmark
4. Memory usage history and trend analysis
5. Automatic config optimization based on system specs

## Related Files

- `main.py`: Startup memory check
- `orchestrator.py`: Cycle-level adaptive management
- `testing/benchmark.py`: Benchmark-level protection
- `MEMORY_LEAK_FIX.md`: Original memory leak fix documentation

## Version History

- **v2.1** (2026-02-15): Added cycle-level adaptive management
- **v2.0** (2026-02-15): Added startup and benchmark memory checks
- **v1.0** (2026-02-14): Initial memory leak fix
