import 'package:flutter/material.dart';

import '../models.dart';
import 'dashboard_section.dart';

class StatisticsSection extends StatelessWidget {
  const StatisticsSection({
    super.key,
    required this.status,
    required this.history,
    required this.silverConfigs,
    required this.goldConfigs,
    required this.balancerConfigs,
    required this.geminiConfigs,
    required this.allCacheCount,
    required this.githubCacheCount,
    required this.docsCount,
    required this.runState,
    required this.engines,
    required this.lastRefresh,
    required this.lastActivityAt,
    required this.refreshTick,
    required this.logs,
    required this.onNavigate,
    required this.onCopyText,
    required this.onCopyLines,
    this.pulseValue = 0.0,
    required this.provisionedPorts,
    required this.balancers,
    this.activeSystemProxyPort,
    this.onSetSystemProxy,
    this.onClearSystemProxy,
    this.isPaused = false,
    this.onTogglePause,
    this.speedProfile = 'medium',
    this.speedThreads = 10,
    this.speedTimeout = 5,
    this.draftSpeedProfile = 'medium',
    this.draftSpeedThreads = 10,
    this.draftSpeedTimeout = 5,
    this.hasPendingSpeedChanges = false,
    this.speedApplyInFlight = false,
    this.projectedScanDuration,
    this.onSpeedProfile,
    this.onThreadsChanged,
    this.onTimeoutChanged,
    this.onApplySpeedChanges,
    this.clearAgeHours = 168,
    this.onClearAgeChanged,
    this.onClearOldConfigs,
    this.onClearAliveConfigs,
    this.manualConfigCtl,
    this.onAddManualConfigs,
    this.onRunCycle,
    this.onRefreshProvisionedPorts,
    this.onRecheckLivePorts,
    this.onReprovisionPorts,
    this.onLoadRawFiles,
    this.onLoadBundleFiles,
    this.onDetectCensorship,
  });

  final Map<String, dynamic>? status;
  final List<Map<String, dynamic>> history;
  final List<String> silverConfigs;
  final List<String> goldConfigs;
  final List<HunterLatencyConfig> balancerConfigs;
  final List<HunterLatencyConfig> geminiConfigs;
  final int allCacheCount;
  final int githubCacheCount;
  final int docsCount;
  final HunterRunState runState;
  final Map<String, int> engines;
  final DateTime? lastRefresh;
  final DateTime? lastActivityAt;
  final int refreshTick;
  final List<HunterLogLine> logs;
  final void Function(HunterNavSection, {HunterConfigListKind? kind}) onNavigate;
  final Future<void> Function(String text, {String? label}) onCopyText;
  final Future<void> Function(List<String> lines, {String? label}) onCopyLines;
  final double pulseValue;
  final List<Map<String, dynamic>> provisionedPorts;
  final List<Map<String, dynamic>> balancers;
  final int? activeSystemProxyPort;
  final Future<void> Function(int port)? onSetSystemProxy;
  final VoidCallback? onClearSystemProxy;
  final bool isPaused;
  final VoidCallback? onTogglePause;
  final String speedProfile;
  final int speedThreads;
  final int speedTimeout;
  final String draftSpeedProfile;
  final int draftSpeedThreads;
  final int draftSpeedTimeout;
  final bool hasPendingSpeedChanges;
  final bool speedApplyInFlight;
  final Duration? projectedScanDuration;
  final void Function(String)? onSpeedProfile;
  final void Function(int)? onThreadsChanged;
  final void Function(int)? onTimeoutChanged;
  final VoidCallback? onApplySpeedChanges;
  final int clearAgeHours;
  final void Function(int)? onClearAgeChanged;
  final VoidCallback? onClearOldConfigs;
  final VoidCallback? onClearAliveConfigs;
  final TextEditingController? manualConfigCtl;
  final VoidCallback? onAddManualConfigs;
  final VoidCallback? onRunCycle;
  final VoidCallback? onRefreshProvisionedPorts;
  final VoidCallback? onRecheckLivePorts;
  final VoidCallback? onReprovisionPorts;
  final VoidCallback? onLoadRawFiles;
  final VoidCallback? onLoadBundleFiles;
  final VoidCallback? onDetectCensorship;

  @override
  Widget build(BuildContext context) {
    return DashboardSection(
      status: status,
      history: history,
      silverConfigs: silverConfigs,
      goldConfigs: goldConfigs,
      balancerConfigs: balancerConfigs,
      geminiConfigs: geminiConfigs,
      allCacheCount: allCacheCount,
      githubCacheCount: githubCacheCount,
      docsCount: docsCount,
      runState: runState,
      engines: engines,
      lastRefresh: lastRefresh,
      lastActivityAt: lastActivityAt,
      refreshTick: refreshTick,
      logs: logs,
      onNavigate: onNavigate,
      onCopyText: onCopyText,
      onCopyLines: onCopyLines,
      pulseValue: pulseValue,
      statisticsMode: true,
      provisionedPorts: provisionedPorts,
      balancers: balancers,
      activeSystemProxyPort: activeSystemProxyPort,
      onSetSystemProxy: onSetSystemProxy,
      onClearSystemProxy: onClearSystemProxy,
      isPaused: isPaused,
      onTogglePause: onTogglePause,
      speedProfile: speedProfile,
      speedThreads: speedThreads,
      speedTimeout: speedTimeout,
      draftSpeedProfile: draftSpeedProfile,
      draftSpeedThreads: draftSpeedThreads,
      draftSpeedTimeout: draftSpeedTimeout,
      hasPendingSpeedChanges: hasPendingSpeedChanges,
      speedApplyInFlight: speedApplyInFlight,
      projectedScanDuration: projectedScanDuration,
      onSpeedProfile: onSpeedProfile,
      onThreadsChanged: onThreadsChanged,
      onTimeoutChanged: onTimeoutChanged,
      onApplySpeedChanges: onApplySpeedChanges,
      clearAgeHours: clearAgeHours,
      onClearAgeChanged: onClearAgeChanged,
      onClearOldConfigs: onClearOldConfigs,
      onClearAliveConfigs: onClearAliveConfigs,
      manualConfigCtl: manualConfigCtl,
      onAddManualConfigs: onAddManualConfigs,
      onRunCycle: onRunCycle,
      onRefreshProvisionedPorts: onRefreshProvisionedPorts,
      onRecheckLivePorts: onRecheckLivePorts,
      onReprovisionPorts: onReprovisionPorts,
      onLoadRawFiles: onLoadRawFiles,
      onLoadBundleFiles: onLoadBundleFiles,
      onDetectCensorship: onDetectCensorship,
    );
  }
}
