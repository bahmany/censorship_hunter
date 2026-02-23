"""
Adaptive Strategy Selector - Intelligent fetching strategy selection.

This module analyzes network conditions and historical performance
to select the optimal fetching strategy dynamically.
"""

import time
import random
from enum import Enum, auto
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Any
from collections import deque


class NetworkCondition(Enum):
    """Detected network conditions."""
    NORMAL = auto()
    SLOW = auto()
    CENSORED = auto()
    OFFLINE = auto()
    UNSTABLE = auto()


class FetchPriority(Enum):
    """Fetch source priorities."""
    TELEGRAM_FIRST = "telegram_first"      # Always try Telegram first
    HTTP_FIRST = "http_first"              # HTTP sources first
    PARALLEL = "parallel"                   # All at once
    CONSERVATIVE = "conservative"           # Most reliable only
    AGGRESSIVE = "aggressive"               # Everything possible


@dataclass
class StrategyDecision:
    """Decision result from strategy selector."""
    priority: FetchPriority
    telegram_channels: List[str]
    http_sources: List[str]
    timeout_telegram: float
    timeout_http: float
    parallel_mode: bool
    retry_aggressive: bool
    reason: str


class AdaptiveStrategySelector:
    """
    Adaptive strategy selector that analyzes conditions and
    chooses the best fetching approach.
    """
    
    def __init__(self, logger=None):
        self.logger = logger
        
        # Network condition tracking
        self._condition_history: deque = deque(maxlen=10)
        self._last_fetch_success = False
        self._consecutive_failures = 0
        self._success_history: deque = deque(maxlen=20)
        
        # Channel reliability scores
        self._channel_scores: Dict[str, float] = {}
        
        # Default timeouts per condition
        self._timeouts = {
            NetworkCondition.NORMAL: {
                'telegram': 60,
                'http': 15,
                'connect': 10
            },
            NetworkCondition.SLOW: {
                'telegram': 120,
                'http': 30,
                'connect': 20
            },
            NetworkCondition.CENSORED: {
                'telegram': 90,
                'http': 45,
                'connect': 15
            },
            NetworkCondition.UNSTABLE: {
                'telegram': 45,
                'http': 10,
                'connect': 5
            },
            NetworkCondition.OFFLINE: {
                'telegram': 30,
                'http': 5,
                'connect': 3
            }
        }
        
        # Channel prioritization by reliability
        self._default_channels = [
            "v2rayngvpn",      # Usually reliable
            "mitivpn",         # Good uptime
            "proxymtprotoir",  # Iran-specific
            "Porteqal3",       # Active
            "v2ray_configs_pool",  # Large pool
            "vmessorg",
            "V2rayNGn",
            "v2ray_swhil",
            "VmessProtocol",
            "PrivateVPNs",
            "DirectVPN",
            "v2rayNG_Matsuri",
            "FalconPolV2rayNG",
            "ShadowSocks_s",
            "napsternetv_config",
            "VlessConfig",
            "iP_CF",
            "ConfigsHUB",
        ]
    
    def record_fetch_result(
        self,
        success: bool,
        source: str,
        config_count: int = 0,
        duration: float = 0
    ):
        """Record the result of a fetch operation."""
        self._success_history.append({
            'success': success,
            'source': source,
            'count': config_count,
            'duration': duration,
            'time': time.time()
        })
        
        if success:
            self._last_fetch_success = True
            self._consecutive_failures = 0
            
            # Update channel score if it's a Telegram channel
            if source in self._channel_scores:
                # Exponential moving average
                old_score = self._channel_scores[source]
                new_score = 0.7 * old_score + 0.3 * min(config_count / 50, 1.0)
                self._channel_scores[source] = new_score
        else:
            self._consecutive_failures += 1
            self._last_fetch_success = False
    
    def detect_network_condition(
        self,
        recent_results: List[Dict[str, Any]]
    ) -> NetworkCondition:
        """
        Detect current network condition based on recent results.
        
        Analyzes:
        - Success/failure ratio
        - Response times
        - Error patterns
        """
        if not recent_results:
            return NetworkCondition.NORMAL
        
        # Calculate metrics
        successes = sum(1 for r in recent_results if r.get('success', False))
        total = len(recent_results)
        success_rate = successes / total if total > 0 else 0
        
        # Average response time
        durations = [
            r.get('duration_ms', 0) 
            for r in recent_results 
            if r.get('success', False)
        ]
        avg_duration = sum(durations) / len(durations) if durations else 0
        
        # Check error patterns
        errors = [r.get('error', '') for r in recent_results if not r.get('success', False)]
        timeout_count = sum(1 for e in errors if 'timeout' in str(e).lower())
        connection_count = sum(1 for e in errors if 'connection' in str(e).lower())
        
        # Determine condition
        if success_rate < 0.3:
            if timeout_count > len(errors) * 0.7:
                return NetworkCondition.SLOW
            elif connection_count > len(errors) * 0.5:
                return NetworkCondition.CENSORED
            else:
                return NetworkCondition.UNSTABLE
        
        elif success_rate < 0.7:
            if avg_duration > 30000:  # > 30s average
                return NetworkCondition.SLOW
            else:
                return NetworkCondition.UNSTABLE
        
        elif avg_duration > 20000:  # > 20s even with good success rate
            return NetworkCondition.SLOW
        
        else:
            return NetworkCondition.NORMAL
    
    def select_channels_by_priority(
        self,
        all_channels: List[str],
        condition: NetworkCondition,
        max_channels: int = 10
    ) -> List[str]:
        """
        Select channels to fetch from based on priority and condition.
        
        For censored networks: fewer channels, most reliable
        For slow networks: fewer channels, longer timeout
        For normal: all channels
        """
        # Score all channels
        scored = []
        for ch in all_channels:
            base_score = self._channel_scores.get(ch, 0.5)
            # Boost known reliable channels
            if ch in self._default_channels[:5]:
                base_score += 0.2
            scored.append((ch, base_score))
        
        # Sort by score
        scored.sort(key=lambda x: x[1], reverse=True)
        
        # Select based on condition
        if condition in (NetworkCondition.CENSORED, NetworkCondition.OFFLINE):
            # Use only top 3 most reliable
            return [ch for ch, _ in scored[:3]]
        
        elif condition == NetworkCondition.SLOW:
            # Use top 5, but with longer timeout
            return [ch for ch, _ in scored[:5]]
        
        elif condition == NetworkCondition.UNSTABLE:
            # Use reliable ones, but try more
            return [ch for ch, _ in scored[:7]]
        
        else:  # NORMAL
            # Use up to max_channels
            return [ch for ch, _ in scored[:max_channels]]
    
    def decide_strategy(
        self,
        available_channels: List[str],
        fetch_history: Optional[List[Dict]] = None,
        memory_pressure: float = 0.0,
        force_strategy: Optional[FetchPriority] = None
    ) -> StrategyDecision:
        """
        Make a strategy decision based on current conditions.
        
        This is the main entry point for strategy selection.
        """
        history = fetch_history or list(self._success_history)
        condition = self.detect_network_condition(history)
        
        # Log detection
        if self.logger:
            self.logger.info(
                f"[StrategySelector] Detected condition: {condition.name}, "
                f"consecutive failures: {self._consecutive_failures}"
            )
        
        # Force strategy if specified
        if force_strategy:
            priority = force_strategy
        else:
            # Automatic selection based on condition
            priority = self._auto_select_priority(condition)
        
        # Get timeouts for condition
        timeouts = self._timeouts.get(condition, self._timeouts[NetworkCondition.NORMAL])
        
        # Select channels
        selected_channels = self.select_channels_by_priority(
            available_channels, condition, max_channels=15
        )
        
        # Build decision
        if priority == FetchPriority.TELEGRAM_FIRST:
            decision = StrategyDecision(
                priority=priority,
                telegram_channels=selected_channels,
                http_sources=['iran_priority'],  # Only most relevant
                timeout_telegram=timeouts['telegram'],
                timeout_http=timeouts['http'],
                parallel_mode=False,
                retry_aggressive=condition != NetworkCondition.OFFLINE,
                reason=f"Condition={condition.name}, prioritize Telegram"
            )
        
        elif priority == FetchPriority.PARALLEL:
            decision = StrategyDecision(
                priority=priority,
                telegram_channels=selected_channels[:5],  # Limit for parallel
                http_sources=['github', 'anti_censorship', 'iran_priority'],
                timeout_telegram=timeouts['telegram'] * 0.7,  # Shorter for parallel
                timeout_http=timeouts['http'] * 0.7,
                parallel_mode=True,
                retry_aggressive=False,
                reason=f"Condition={condition.name}, parallel for speed"
            )
        
        elif priority == FetchPriority.CONSERVATIVE:
            # Only most reliable sources
            decision = StrategyDecision(
                priority=priority,
                telegram_channels=selected_channels[:3],
                http_sources=['iran_priority'],
                timeout_telegram=timeouts['telegram'] * 1.5,
                timeout_http=timeouts['http'] * 1.5,
                parallel_mode=False,
                retry_aggressive=True,
                reason=f"Condition={condition.name}, conservative mode"
            )
        
        elif priority == FetchPriority.AGGRESSIVE:
            decision = StrategyDecision(
                priority=priority,
                telegram_channels=selected_channels,
                http_sources=['github', 'anti_censorship', 'iran_priority'],
                timeout_telegram=timeouts['telegram'],
                timeout_http=timeouts['http'],
                parallel_mode=True,
                retry_aggressive=True,
                reason=f"Condition={condition.name}, aggressive mode"
            )
        
        else:  # HTTP_FIRST
            decision = StrategyDecision(
                priority=priority,
                telegram_channels=selected_channels[:5],
                http_sources=['github', 'anti_censorship', 'iran_priority'],
                timeout_telegram=timeouts['telegram'],
                timeout_http=timeouts['http'],
                parallel_mode=False,
                retry_aggressive=False,
                reason=f"Condition={condition.name}, HTTP first"
            )
        
        # Adjust for memory pressure
        if memory_pressure > 85:
            # Reduce everything
            decision.telegram_channels = decision.telegram_channels[:3]
            decision.http_sources = decision.http_sources[:1]
            decision.reason += f", memory pressure {memory_pressure:.0f}%"
        
        if self.logger:
            self.logger.info(
                f"[StrategySelector] Decision: {decision.priority.value}, "
                f"channels={len(decision.telegram_channels)}, "
                f"parallel={decision.parallel_mode}"
            )
        
        return decision
    
    def _auto_select_priority(self, condition: NetworkCondition) -> FetchPriority:
        """Automatically select priority based on network condition."""
        if condition == NetworkCondition.CENSORED:
            return FetchPriority.TELEGRAM_FIRST
        elif condition == NetworkCondition.SLOW:
            return FetchPriority.CONSERVATIVE
        elif condition == NetworkCondition.UNSTABLE:
            return FetchPriority.PARALLEL
        elif condition == NetworkCondition.OFFLINE:
            return FetchPriority.CONSERVATIVE
        else:  # NORMAL
            return FetchPriority.TELEGRAM_FIRST
    
    def should_retry_with_different_strategy(
        self,
        last_result: Dict[str, Any],
        attempts_made: int
    ) -> Optional[FetchPriority]:
        """
        Decide if we should retry with a different strategy.
        
        Returns new priority or None if should stop.
        """
        if attempts_made >= 3:
            return None  # Stop after 3 attempts
        
        last_strategy = last_result.get('strategy', '')
        
        # If Telegram failed, try HTTP
        if 'telegram' in last_strategy.lower():
            return FetchPriority.HTTP_FIRST
        
        # If HTTP failed, try parallel everything
        if 'http' in last_strategy.lower():
            return FetchPriority.AGGRESSIVE
        
        # If parallel failed, try conservative
        if 'aggressive' in last_strategy.lower() or 'parallel' in last_strategy.lower():
            return FetchPriority.CONSERVATIVE
        
        return None
    
    def get_stats(self) -> Dict[str, Any]:
        """Get selector statistics."""
        return {
            'consecutive_failures': self._consecutive_failures,
            'last_success': self._last_fetch_success,
            'channel_scores': dict(self._channel_scores),
            'history_size': len(self._success_history),
            'condition_history': [c.name for c in self._condition_history]
        }


# Convenience function
def select_fetch_strategy(
    channels: List[str],
    recent_results: Optional[List[Dict]] = None,
    logger=None
) -> StrategyDecision:
    """
    Quick function to select a fetching strategy.
    
    Example:
        decision = select_fetch_strategy(
            channels=["v2rayngvpn", "mitivpn"],
            recent_results=[{'success': True, 'source': 'telegram', 'count': 50}]
        )
        print(f"Strategy: {decision.priority.value}")
        print(f"Channels: {decision.telegram_channels}")
    """
    selector = AdaptiveStrategySelector(logger=logger)
    if recent_results:
        for r in recent_results:
            selector.record_fetch_result(
                success=r.get('success', False),
                source=r.get('source', 'unknown'),
                config_count=r.get('count', 0),
                duration=r.get('duration_ms', 0)
            )
    
    return selector.decide_strategy(channels)
