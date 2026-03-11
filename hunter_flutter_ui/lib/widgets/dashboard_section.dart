import 'package:flutter/material.dart';
import '../theme.dart';
import '../models.dart';
import '../services.dart';

class DashboardSection extends StatelessWidget {
  const DashboardSection({
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
    this.provisionedPorts = const <Map<String, dynamic>>[],
    this.balancers = const <Map<String, dynamic>>[],
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
    this.manualConfigCtl,
    this.onAddManualConfigs,
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
  final TextEditingController? manualConfigCtl;
  final VoidCallback? onAddManualConfigs;

  @override
  Widget build(BuildContext context) {
    final Map<String, dynamic>? db = status?['db'] is Map<String, dynamic>
        ? status!['db'] as Map<String, dynamic>
        : null;
    final Map<String, dynamic>? val = status?['validator'] is Map<String, dynamic>
        ? status!['validator'] as Map<String, dynamic>
        : null;

    final int dbTotal = _num(db, 'total');
    final int dbTested = _num(db, 'tested_unique');
    final int dbAlive = _num(db, 'alive');
    final double rate = _dbl(val, 'rate_per_s');
    final double eta = _dbl(status, 'eta_seconds');
    final double uptime = _dbl(status, 'uptime_s');
    final int totalFound = goldConfigs.length + silverConfigs.length;
    final int liveBalancer = balancerConfigs.length;
    // Merge file-based configs + balancer URIs, deduplicate
    final Set<String> aliveSet = <String>{...goldConfigs, ...silverConfigs, ...balancerConfigs.map((HunterLatencyConfig e) => e.uri)};
    final List<String> allAlive = aliveSet.toList();
    final int surfacedAlive = dbAlive > 0 ? dbAlive : allAlive.length;
    final bool isRunning = runState == HunterRunState.running;
    final bool isLive = _isLiveNow();

    return SingleChildScrollView(
      padding: const EdgeInsets.all(20),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          // ── Top: Found configs alert (most prominent) ──
          _buildFoundAlert(context, surfacedAlive, totalFound, allAlive, isRunning),
          const SizedBox(height: 16),
          // ── Stats row ──
          _buildStatsRow(context, dbAlive, dbTested, dbTotal, liveBalancer, rate, uptime),
          const SizedBox(height: 16),
          // ── Progress ──
          _buildProgress(context, dbTotal, dbTested, eta, isRunning, isLive),
          const SizedBox(height: 16),
          // ── Pause + Speed Controls ──
          _buildControlsSection(context, isRunning),
          const SizedBox(height: 16),
          // ── Maintenance: clear old + manual add ──
          _buildMaintenanceSection(context, isRunning),
          const SizedBox(height: 16),
          // ── Active Proxy Ports (balancers + provisioned) ──
          if (provisionedPorts.isNotEmpty || balancers.isNotEmpty)
            _buildProvisionedPorts(context),
          if (provisionedPorts.isNotEmpty || balancers.isNotEmpty)
            const SizedBox(height: 16),
          // ── Live configs preview ──
          if (allAlive.isNotEmpty || balancerConfigs.isNotEmpty)
            _buildConfigPreview(context, allAlive, isRunning),
          if (allAlive.isNotEmpty || balancerConfigs.isNotEmpty)
            const SizedBox(height: 16),
          // ── Activity log ──
          _buildActivity(context, isLive),
        ],
      ),
    );
  }

  // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  // TOP ALERT — shows found configs count prominently
  // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  Widget _buildFoundAlert(BuildContext context, int dbAlive, int totalFound, List<String> allAlive, bool isRunning) {
    final bool hasConfigs = dbAlive > 0 || totalFound > 0;
    final bool hasCopyable = allAlive.isNotEmpty;
    final Color accent = hasConfigs ? C.neonGreen : (isRunning ? C.neonAmber : C.txt3);
    final String title = hasConfigs
        ? '\u26A1 $dbAlive Alive Config${dbAlive == 1 ? '' : 's'} Found!'
        : (isRunning ? 'Scanning for configs...' : 'Start Hunter to find configs');
    final String sub = hasConfigs
        ? hasCopyable
            ? '${allAlive.length} ready to copy \u2022 $totalFound in files \u2022 ${balancerConfigs.length} in balancer'
            : 'Alive in validator DB \u2022 syncing runtime files...'
        : (isRunning ? 'Testing configs against censorship filters...' : 'Press START to begin scanning');

    return Container(
      width: double.infinity,
      padding: const EdgeInsets.all(20),
      decoration: BoxDecoration(
        gradient: LinearGradient(
          colors: <Color>[accent.withValues(alpha: hasConfigs ? 0.15 : 0.05), C.card],
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
        ),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: accent.withValues(alpha: hasConfigs ? 0.5 : 0.2), width: hasConfigs ? 2 : 1),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Row(
            children: <Widget>[
              Expanded(
                child: Text(title, style: TextStyle(color: accent, fontSize: 20, fontWeight: FontWeight.w800)),
              ),
              if (hasCopyable)
                _actionBtn('COPY ALL', C.neonGreen, () => onCopyLines(allAlive, label: 'Copied ${allAlive.length} configs')),
            ],
          ),
          const SizedBox(height: 8),
          Text(sub, style: TextStyle(color: C.txt2, fontSize: 13, height: 1.4)),
          if (hasConfigs && allAlive.isNotEmpty) ...<Widget>[
            const SizedBox(height: 14),
            ...allAlive.take(3).map((String uri) {
              final String proto = uri.contains('://') ? uri.split('://').first.toUpperCase() : '?';
              final Color pc = _protoColor(proto);
              return Padding(
                padding: const EdgeInsets.only(bottom: 6),
                child: InkWell(
                  onTap: () => onCopyText(uri, label: 'Config copied!'),
                  borderRadius: BorderRadius.circular(6),
                  child: Container(
                    padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
                    decoration: BoxDecoration(
                      color: C.surface,
                      borderRadius: BorderRadius.circular(6),
                      border: Border.all(color: pc.withValues(alpha: 0.2)),
                    ),
                    child: Row(
                      children: <Widget>[
                        Container(
                          padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
                          decoration: BoxDecoration(color: pc.withValues(alpha: 0.15), borderRadius: BorderRadius.circular(4)),
                          child: Text(proto, style: TextStyle(color: pc, fontSize: 10, fontWeight: FontWeight.w700, fontFamily: 'Consolas')),
                        ),
                        const SizedBox(width: 10),
                        Expanded(child: Text(uri, maxLines: 1, overflow: TextOverflow.ellipsis, style: const TextStyle(color: C.txt2, fontSize: 11, fontFamily: 'Consolas'))),
                        const Icon(Icons.content_copy, color: C.txt3, size: 14),
                      ],
                    ),
                  ),
                ),
              );
            }),
            if (allAlive.length > 3)
              Padding(
                padding: const EdgeInsets.only(top: 4),
                child: Row(
                  children: <Widget>[
                    Text('+${allAlive.length - 3} more', style: TextStyle(color: accent.withValues(alpha: 0.7), fontSize: 12)),
                    const Spacer(),
                    _actionBtn('VIEW ALL', C.neonCyan, () => onNavigate(HunterNavSection.configs, kind: HunterConfigListKind.alive)),
                  ],
                ),
              ),
          ],
        ],
      ),
    );
  }

  // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  // STATS ROW — compact key metrics
  // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  Widget _buildStatsRow(BuildContext context, int alive, int tested, int total, int balancer, double rate, double uptime) {
    return Wrap(
      spacing: 12,
      runSpacing: 12,
      children: <Widget>[
        _statChip('ALIVE', '$alive', C.neonGreen),
        _statChip('TESTED', fmtNumber(tested), C.neonCyan),
        _statChip('TOTAL DB', fmtNumber(total), C.neonAmber),
        _statChip('BALANCER', '$balancer', C.neonPurple),
        _statChip('RATE', rate > 0.01 ? '${rate.toStringAsFixed(1)}/s' : '--', C.neonCyan),
        _statChip('UPTIME', uptime > 0 ? fmtDuration(Duration(seconds: uptime.round())) : '--', C.txt2),
      ],
    );
  }

  Widget _statChip(String label, String value, Color color) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
      decoration: BoxDecoration(
        color: C.card,
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: color.withValues(alpha: 0.2)),
      ),
      child: Column(
        children: <Widget>[
          Text(value, style: TextStyle(color: color, fontSize: 18, fontWeight: FontWeight.w800, fontFamily: 'Consolas')),
          const SizedBox(height: 2),
          Text(label, style: const TextStyle(color: C.txt3, fontSize: 9, fontWeight: FontWeight.w600, letterSpacing: 1)),
        ],
      ),
    );
  }

  // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  // PROGRESS BAR
  // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  Widget _buildProgress(BuildContext context, int total, int tested, double eta, bool isRunning, bool isLive) {
    final double pct = total > 0 ? (tested / total).clamp(0.0, 1.0) : 0.0;
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: C.card,
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: C.border),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Row(
            children: <Widget>[
              const Text('SCAN PROGRESS', style: TextStyle(color: C.txt3, fontSize: 10, fontWeight: FontWeight.w600, letterSpacing: 1.2)),
              const Spacer(),
              Text('${(pct * 100).toStringAsFixed(1)}%', style: const TextStyle(color: C.neonCyan, fontSize: 16, fontWeight: FontWeight.w800, fontFamily: 'Consolas')),
            ],
          ),
          const SizedBox(height: 10),
          ClipRRect(
            borderRadius: BorderRadius.circular(6),
            child: LinearProgressIndicator(
              value: pct,
              minHeight: 8,
              backgroundColor: C.surface,
              valueColor: const AlwaysStoppedAnimation<Color>(C.neonCyan),
            ),
          ),
          const SizedBox(height: 10),
          Row(
            children: <Widget>[
              Text('${fmtNumber(tested)} / ${fmtNumber(total)}', style: const TextStyle(color: C.txt2, fontSize: 12)),
              const Spacer(),
              if (isLive)
                Container(
                  padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
                  decoration: BoxDecoration(color: C.neonGreen.withValues(alpha: 0.12), borderRadius: BorderRadius.circular(20)),
                  child: const Text('LIVE', style: TextStyle(color: C.neonGreen, fontSize: 9, fontWeight: FontWeight.w700)),
                )
              else if (isRunning)
                Container(
                  padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
                  decoration: BoxDecoration(color: C.neonAmber.withValues(alpha: 0.12), borderRadius: BorderRadius.circular(20)),
                  child: const Text('SCANNING', style: TextStyle(color: C.neonAmber, fontSize: 9, fontWeight: FontWeight.w700)),
                ),
              if (eta > 0) ...<Widget>[
                const SizedBox(width: 10),
                Text('ETA ${fmtDuration(Duration(seconds: eta.round()))}', style: const TextStyle(color: C.txt3, fontSize: 11)),
              ],
            ],
          ),
        ],
      ),
    );
  }

  // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  // CONFIG PREVIEW — show found configs with copy buttons
  // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  Widget _buildConfigPreview(BuildContext context, List<String> allAlive, bool isRunning) {
    final List<HunterLatencyConfig> topBalancer = balancerConfigs.take(5).toList();
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: C.card,
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: C.neonCyan.withValues(alpha: 0.25)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Row(
            children: <Widget>[
              const Icon(Icons.bolt, color: C.neonCyan, size: 18),
              const SizedBox(width: 8),
              const Text('LIVE BALANCER', style: TextStyle(color: C.txt3, fontSize: 10, fontWeight: FontWeight.w600, letterSpacing: 1.2)),
              const SizedBox(width: 8),
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
                decoration: BoxDecoration(
                  color: balancerConfigs.isNotEmpty ? C.neonGreen.withValues(alpha: 0.12) : C.neonAmber.withValues(alpha: 0.12),
                  borderRadius: BorderRadius.circular(20),
                ),
                child: Text(
                  balancerConfigs.isNotEmpty ? '${balancerConfigs.length} READY' : 'BUILDING',
                  style: TextStyle(
                    color: balancerConfigs.isNotEmpty ? C.neonGreen : C.neonAmber,
                    fontSize: 9, fontWeight: FontWeight.w700,
                  ),
                ),
              ),
              const Spacer(),
              if (balancerConfigs.isNotEmpty)
                _actionBtn('COPY ALL', C.neonCyan, () => onCopyLines(balancerConfigs.map((HunterLatencyConfig e) => e.uri).toList(), label: 'Copied ${balancerConfigs.length} balancer configs')),
            ],
          ),
          const SizedBox(height: 12),
          if (topBalancer.isEmpty)
            Padding(
              padding: const EdgeInsets.symmetric(vertical: 8),
              child: Text(
                isRunning ? 'Testing configs... live balancer will populate soon.' : 'Start Hunter to build the balancer pool.',
                style: const TextStyle(color: C.txt3, fontSize: 12, fontStyle: FontStyle.italic),
              ),
            )
          else
            ...topBalancer.map((HunterLatencyConfig cfg) {
              final String proto = cfg.uri.contains('://') ? cfg.uri.split('://').first.toUpperCase() : '?';
              final Color pc = _protoColor(proto);
              final String lat = cfg.latencyMs != null ? '${cfg.latencyMs!.toStringAsFixed(0)}ms' : '--';
              return Padding(
                padding: const EdgeInsets.only(bottom: 4),
                child: InkWell(
                  onTap: () => onCopyText(cfg.uri, label: 'Config copied!'),
                  borderRadius: BorderRadius.circular(6),
                  child: Padding(
                    padding: const EdgeInsets.symmetric(vertical: 4, horizontal: 4),
                    child: Row(
                      children: <Widget>[
                        SizedBox(
                          width: 56,
                          child: Text(lat, style: const TextStyle(color: C.neonCyan, fontSize: 11, fontWeight: FontWeight.w700, fontFamily: 'Consolas')),
                        ),
                        Container(
                          padding: const EdgeInsets.symmetric(horizontal: 5, vertical: 1),
                          decoration: BoxDecoration(color: pc.withValues(alpha: 0.15), borderRadius: BorderRadius.circular(3)),
                          child: Text(proto, style: TextStyle(color: pc, fontSize: 9, fontWeight: FontWeight.w700, fontFamily: 'Consolas')),
                        ),
                        const SizedBox(width: 8),
                        Expanded(child: Text(cfg.uri, maxLines: 1, overflow: TextOverflow.ellipsis, style: const TextStyle(color: C.txt2, fontSize: 11, fontFamily: 'Consolas'))),
                        const Icon(Icons.content_copy, color: C.txt3, size: 13),
                      ],
                    ),
                  ),
                ),
              );
            }),
          if (balancerConfigs.length > 5)
            Padding(
              padding: const EdgeInsets.only(top: 6),
              child: InkWell(
                onTap: () => onNavigate(HunterNavSection.configs, kind: HunterConfigListKind.balancer),
                child: Text('+${balancerConfigs.length - 5} more \u2192', style: TextStyle(color: C.neonCyan.withValues(alpha: 0.7), fontSize: 12)),
              ),
            ),
        ],
      ),
    );
  }

  // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  // ACTIVE PROXY PORTS — balancers + provisioned with system proxy
  // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  Widget _buildProvisionedPorts(BuildContext context) {
    final int aliveCount = provisionedPorts.where((Map<String, dynamic> p) => p['alive'] == true).length;
    final int balAlive = balancers.where((Map<String, dynamic> b) => b['running'] == true).length;
    final bool hasProxy = activeSystemProxyPort != null;

    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: C.card,
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: C.neonPurple.withValues(alpha: 0.25)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          // ── Header row ──
          Row(
            children: <Widget>[
              const Icon(Icons.dns, color: C.neonPurple, size: 18),
              const SizedBox(width: 8),
              const Text('ACTIVE PORTS', style: TextStyle(color: C.txt3, fontSize: 10, fontWeight: FontWeight.w600, letterSpacing: 1.2)),
              const SizedBox(width: 8),
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
                decoration: BoxDecoration(
                  color: (balAlive + aliveCount) > 0 ? C.neonGreen.withValues(alpha: 0.12) : C.neonRed.withValues(alpha: 0.12),
                  borderRadius: BorderRadius.circular(20),
                ),
                child: Text(
                  '${balAlive + aliveCount} ALIVE',
                  style: TextStyle(color: (balAlive + aliveCount) > 0 ? C.neonGreen : C.neonRed, fontSize: 9, fontWeight: FontWeight.w700),
                ),
              ),
              const Spacer(),
              if (hasProxy)
                InkWell(
                  onTap: onClearSystemProxy,
                  borderRadius: BorderRadius.circular(6),
                  child: Container(
                    padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                    decoration: BoxDecoration(
                      color: C.neonRed.withValues(alpha: 0.12),
                      borderRadius: BorderRadius.circular(6),
                      border: Border.all(color: C.neonRed.withValues(alpha: 0.3)),
                    ),
                    child: Row(mainAxisSize: MainAxisSize.min, children: <Widget>[
                      const Icon(Icons.link_off, color: C.neonRed, size: 12),
                      const SizedBox(width: 4),
                      Text('CLEAR PROXY :$activeSystemProxyPort', style: const TextStyle(color: C.neonRed, fontSize: 9, fontWeight: FontWeight.w700)),
                    ]),
                  ),
                ),
            ],
          ),
          const SizedBox(height: 12),

          // ── Balancer ports ──
          ...balancers.map((Map<String, dynamic> b) {
            final int port = (b['port'] is num) ? (b['port'] as num).toInt() : 0;
            final String type = (b['type'] is String) ? (b['type'] as String).toUpperCase() : '?';
            final bool running = b['running'] == true;
            final int backends = (b['backends'] is num) ? (b['backends'] as num).toInt() : 0;
            final int healthy = (b['healthy'] is num) ? (b['healthy'] as num).toInt() : 0;
            final bool isActive = activeSystemProxyPort == port;
            return _buildPortRow(
              port: port,
              label: '$type BALANCER',
              alive: running,
              detail: '$healthy/$backends backends',
              isSystemProxy: isActive,
              onSetProxy: running && onSetSystemProxy != null ? () => onSetSystemProxy!(port) : null,
              onCopy: () => onCopyText('socks5://127.0.0.1:$port', label: 'Copied SOCKS5 :$port'),
              badgeColor: C.neonCyan,
            );
          }),

          if (balancers.isNotEmpty && provisionedPorts.isNotEmpty)
            Padding(
              padding: const EdgeInsets.symmetric(vertical: 6),
              child: Row(children: <Widget>[
                Expanded(child: Divider(color: C.border.withValues(alpha: 0.5), height: 1)),
                Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 8),
                  child: Text('INDIVIDUAL PROXIES', style: TextStyle(color: C.txt3.withValues(alpha: 0.5), fontSize: 8, fontWeight: FontWeight.w600, letterSpacing: 1)),
                ),
                Expanded(child: Divider(color: C.border.withValues(alpha: 0.5), height: 1)),
              ]),
            ),

          // ── Provisioned proxy ports (dual-protocol: SOCKS + HTTP) ──
          ...provisionedPorts.map((Map<String, dynamic> p) {
            final int port = (p['port'] is num) ? (p['port'] as num).toInt() : 0;
            final int httpPort = (p['http_port'] is num) ? (p['http_port'] as num).toInt() : 0;
            final String uri = (p['uri'] is String) ? p['uri'] as String : '';
            final bool alive = p['alive'] == true;
            final double latency = (p['latency_ms'] is num) ? (p['latency_ms'] as num).toDouble() : 0.0;
            final int failures = (p['consecutive_failures'] is num) ? (p['consecutive_failures'] as num).toInt() : 0;
            final String proto = uri.contains('://') ? uri.split('://').first.toUpperCase() : '?';
            final bool isActive = activeSystemProxyPort == port;
            return _buildDualPortRow(
              socksPort: port,
              httpPort: httpPort,
              label: proto,
              alive: alive,
              detail: latency > 0 ? '${latency.toStringAsFixed(0)}ms' : (failures > 0 ? '×$failures' : '--'),
              isSystemProxy: isActive,
              onSetProxy: alive && onSetSystemProxy != null ? () => onSetSystemProxy!(port) : null,
              onCopySocks: () => onCopyText('socks5://127.0.0.1:$port', label: 'Copied SOCKS5 :$port'),
              onCopyHttp: httpPort > 0 ? () => onCopyText('http://127.0.0.1:$httpPort', label: 'Copied HTTP :$httpPort') : null,
              badgeColor: _protoColor(proto),
              uri: uri,
            );
          }),
        ],
      ),
    );
  }

  Widget _buildPortRow({
    required int port,
    required String label,
    required bool alive,
    required String detail,
    required bool isSystemProxy,
    VoidCallback? onSetProxy,
    required VoidCallback onCopy,
    required Color badgeColor,
    String uri = '',
  }) {
    final Color statusColor = alive ? C.neonGreen : C.neonRed;
    return Padding(
      padding: const EdgeInsets.only(bottom: 3),
      child: Row(
        children: <Widget>[
          // Status dot
          Container(
            width: 8, height: 8,
            decoration: BoxDecoration(
              color: statusColor, shape: BoxShape.circle,
              boxShadow: alive ? <BoxShadow>[BoxShadow(color: statusColor.withValues(alpha: 0.4), blurRadius: 4)] : null,
            ),
          ),
          const SizedBox(width: 8),
          // Port
          SizedBox(
            width: 46,
            child: Text(':$port', style: TextStyle(color: alive ? C.neonCyan : C.txt3, fontSize: 12, fontWeight: FontWeight.w700, fontFamily: 'Consolas')),
          ),
          // Badge
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 5, vertical: 1),
            decoration: BoxDecoration(color: badgeColor.withValues(alpha: 0.15), borderRadius: BorderRadius.circular(3)),
            child: Text(label, style: TextStyle(color: badgeColor, fontSize: 9, fontWeight: FontWeight.w700, fontFamily: 'Consolas')),
          ),
          const SizedBox(width: 6),
          // Detail (latency or backends)
          SizedBox(
            width: 70,
            child: Text(detail, style: TextStyle(color: alive ? C.neonGreen : C.txt3, fontSize: 10, fontFamily: 'Consolas')),
          ),
          // URI (only for provisioned)
          if (uri.isNotEmpty)
            Expanded(child: Text(uri, maxLines: 1, overflow: TextOverflow.ellipsis, style: TextStyle(color: alive ? C.txt2 : C.txt3, fontSize: 9, fontFamily: 'Consolas')))
          else
            const Spacer(),
          // System proxy indicator or button
          if (isSystemProxy)
            Container(
              padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
              decoration: BoxDecoration(
                color: C.neonGreen.withValues(alpha: 0.15),
                borderRadius: BorderRadius.circular(4),
                border: Border.all(color: C.neonGreen.withValues(alpha: 0.4)),
              ),
              child: const Text('✓ SYSTEM', style: TextStyle(color: C.neonGreen, fontSize: 8, fontWeight: FontWeight.w800)),
            )
          else if (onSetProxy != null)
            InkWell(
              onTap: onSetProxy,
              borderRadius: BorderRadius.circular(4),
              child: Container(
                padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
                decoration: BoxDecoration(
                  color: C.neonCyan.withValues(alpha: 0.08),
                  borderRadius: BorderRadius.circular(4),
                  border: Border.all(color: C.neonCyan.withValues(alpha: 0.2)),
                ),
                child: const Text('USE', style: TextStyle(color: C.neonCyan, fontSize: 8, fontWeight: FontWeight.w700)),
              ),
            ),
          const SizedBox(width: 4),
          // Copy button
          InkWell(
            onTap: onCopy,
            borderRadius: BorderRadius.circular(4),
            child: const Padding(
              padding: EdgeInsets.all(2),
              child: Icon(Icons.content_copy, color: C.txt3, size: 12),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildDualPortRow({
    required int socksPort,
    required int httpPort,
    required String label,
    required bool alive,
    required String detail,
    required bool isSystemProxy,
    VoidCallback? onSetProxy,
    required VoidCallback onCopySocks,
    VoidCallback? onCopyHttp,
    required Color badgeColor,
    String uri = '',
  }) {
    final Color statusColor = alive ? C.neonGreen : C.neonRed;
    return Tooltip(
      message: 'SOCKS5  127.0.0.1:$socksPort\n'
          '${httpPort > 0 ? 'HTTP     127.0.0.1:$httpPort\n' : ''}'
          'Smart routing: Iranian sites direct, DNS through proxy, TLS fragment anti-DPI',
      child: Padding(
        padding: const EdgeInsets.only(bottom: 3),
        child: Row(
          children: <Widget>[
            // Status dot
            Container(
              width: 8, height: 8,
              decoration: BoxDecoration(
                color: statusColor, shape: BoxShape.circle,
                boxShadow: alive ? <BoxShadow>[BoxShadow(color: statusColor.withValues(alpha: 0.4), blurRadius: 4)] : null,
              ),
            ),
            const SizedBox(width: 6),
            // Dual port display
            SizedBox(
              width: 88,
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: <Widget>[
                  Text('S:$socksPort', style: TextStyle(color: alive ? C.neonCyan : C.txt3, fontSize: 10, fontWeight: FontWeight.w700, fontFamily: 'Consolas')),
                  if (httpPort > 0)
                    Text('H:$httpPort', style: TextStyle(color: alive ? C.neonAmber : C.txt3, fontSize: 10, fontWeight: FontWeight.w700, fontFamily: 'Consolas')),
                ],
              ),
            ),
            // Badge
            Container(
              padding: const EdgeInsets.symmetric(horizontal: 5, vertical: 1),
              decoration: BoxDecoration(color: badgeColor.withValues(alpha: 0.15), borderRadius: BorderRadius.circular(3)),
              child: Text(label, style: TextStyle(color: badgeColor, fontSize: 9, fontWeight: FontWeight.w700, fontFamily: 'Consolas')),
            ),
            const SizedBox(width: 6),
            // Detail (latency)
            SizedBox(
              width: 50,
              child: Text(detail, style: TextStyle(color: alive ? C.neonGreen : C.txt3, fontSize: 10, fontFamily: 'Consolas')),
            ),
            // URI
            if (uri.isNotEmpty)
              Expanded(child: Text(uri, maxLines: 1, overflow: TextOverflow.ellipsis, style: TextStyle(color: alive ? C.txt2 : C.txt3, fontSize: 9, fontFamily: 'Consolas')))
            else
              const Spacer(),
            // System proxy indicator or button
            if (isSystemProxy)
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
                decoration: BoxDecoration(
                  color: C.neonGreen.withValues(alpha: 0.15),
                  borderRadius: BorderRadius.circular(4),
                  border: Border.all(color: C.neonGreen.withValues(alpha: 0.4)),
                ),
                child: const Text('✓ SYSTEM', style: TextStyle(color: C.neonGreen, fontSize: 8, fontWeight: FontWeight.w800)),
              )
            else if (onSetProxy != null)
              InkWell(
                onTap: onSetProxy,
                borderRadius: BorderRadius.circular(4),
                child: Container(
                  padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
                  decoration: BoxDecoration(
                    color: C.neonCyan.withValues(alpha: 0.08),
                    borderRadius: BorderRadius.circular(4),
                    border: Border.all(color: C.neonCyan.withValues(alpha: 0.2)),
                  ),
                  child: const Text('USE', style: TextStyle(color: C.neonCyan, fontSize: 8, fontWeight: FontWeight.w700)),
                ),
              ),
            const SizedBox(width: 4),
            // Copy SOCKS button
            InkWell(
              onTap: onCopySocks,
              borderRadius: BorderRadius.circular(4),
              child: Padding(
                padding: const EdgeInsets.all(2),
                child: Text('S', style: TextStyle(color: C.neonCyan.withValues(alpha: 0.7), fontSize: 10, fontWeight: FontWeight.w800, fontFamily: 'Consolas')),
              ),
            ),
            // Copy HTTP button
            if (onCopyHttp != null) ...<Widget>[
              const SizedBox(width: 2),
              InkWell(
                onTap: onCopyHttp,
                borderRadius: BorderRadius.circular(4),
                child: Padding(
                  padding: const EdgeInsets.all(2),
                  child: Text('H', style: TextStyle(color: C.neonAmber.withValues(alpha: 0.7), fontSize: 10, fontWeight: FontWeight.w800, fontFamily: 'Consolas')),
                ),
              ),
            ],
          ],
        ),
      ),
    );
  }

  // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  // ACTIVITY LOG — last 15 lines
  // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  Widget _buildActivity(BuildContext context, bool isLive) {
    final List<HunterLogLine> filtered = logs.where((HunterLogLine l) {
      final String m = l.message.toLowerCase();
      return !m.contains('connection pool is full') && !m.contains('added to the connection pool');
    }).toList(growable: false);
    final List<HunterLogLine> recent = filtered.length > 15 ? filtered.sublist(filtered.length - 15) : filtered;
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: C.card,
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: C.border),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Row(
            children: <Widget>[
              Container(
                width: 8, height: 8,
                decoration: BoxDecoration(
                  color: isLive ? C.neonGreen : C.txt3,
                  shape: BoxShape.circle,
                  boxShadow: isLive ? <BoxShadow>[BoxShadow(color: C.neonGreen.withValues(alpha: 0.4), blurRadius: 6)] : null,
                ),
              ),
              const SizedBox(width: 8),
              const Text('LIVE ACTIVITY', style: TextStyle(color: C.txt3, fontSize: 10, fontWeight: FontWeight.w600, letterSpacing: 1.2)),
              const Spacer(),
              if (lastRefresh != null)
                Text(fmtTs(lastRefresh!), style: const TextStyle(color: C.txt3, fontSize: 10, fontFamily: 'Consolas')),
            ],
          ),
          const SizedBox(height: 10),
          if (recent.isEmpty)
            const Padding(
              padding: EdgeInsets.symmetric(vertical: 12),
              child: Text('No activity yet', style: TextStyle(color: C.txt3, fontSize: 12, fontStyle: FontStyle.italic)),
            )
          else
            ...recent.map((HunterLogLine line) {
              final Color lc = _logColor(line);
              final bool isUri = line.message.startsWith('vless://') ||
                  line.message.startsWith('vmess://') ||
                  line.message.startsWith('ss://') ||
                  line.message.startsWith('trojan://');
              return InkWell(
                onTap: isUri ? () => onCopyText(line.message, label: 'Config copied!') : null,
                child: Padding(
                  padding: const EdgeInsets.symmetric(vertical: 1.5),
                  child: Row(
                    children: <Widget>[
                      Text(fmtTs(line.ts), style: const TextStyle(color: C.txt3, fontSize: 9, fontFamily: 'Consolas')),
                      const SizedBox(width: 6),
                      Expanded(
                        child: Text(line.message, maxLines: 1, overflow: TextOverflow.ellipsis,
                          style: TextStyle(color: lc, fontSize: 10, fontFamily: 'Consolas')),
                      ),
                      if (isUri) const Icon(Icons.content_copy, color: C.neonGreen, size: 12),
                    ],
                  ),
                ),
              );
            }),
        ],
      ),
    );
  }

  // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  // CONTROLS — Pause + Speed
  // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  Widget _buildControlsSection(BuildContext context, bool isRunning) {
    final Color pauseColor = isPaused ? C.neonAmber : (isRunning ? C.neonGreen : C.txt3);
    final bool canEditSpeed = isRunning && !speedApplyInFlight;
    final String draftLabel = draftSpeedProfile == 'custom' ? 'CUSTOM' : draftSpeedProfile.toUpperCase();
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: C.card,
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: isPaused ? C.neonAmber.withValues(alpha: 0.4) : C.border),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Row(
            children: <Widget>[
              Icon(isPaused ? Icons.pause_circle : Icons.speed, color: pauseColor, size: 18),
              const SizedBox(width: 8),
              Text(isPaused ? 'PAUSED' : 'SPEED CONTROLS', style: TextStyle(color: pauseColor, fontSize: 13, fontWeight: FontWeight.w800, letterSpacing: 1)),
              const Spacer(),
              // Pause/Resume button
              if (!isRunning)
                Container(
                  padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                  decoration: BoxDecoration(color: C.txt3.withValues(alpha: 0.08), borderRadius: BorderRadius.circular(6)),
                  child: const Text('OFFLINE', style: TextStyle(color: C.txt3, fontSize: 9, fontWeight: FontWeight.w700)),
                ),
              SizedBox(
                height: 32,
                child: ElevatedButton.icon(
                  onPressed: isRunning ? onTogglePause : null,
                  icon: Icon(isPaused ? Icons.play_arrow : Icons.pause, size: 16),
                  label: Text(isPaused ? 'RESUME' : 'PAUSE', style: const TextStyle(fontSize: 11, fontWeight: FontWeight.w700)),
                  style: ElevatedButton.styleFrom(
                    backgroundColor: pauseColor.withValues(alpha: 0.15),
                    foregroundColor: pauseColor,
                    shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8), side: BorderSide(color: pauseColor.withValues(alpha: 0.4))),
                    padding: const EdgeInsets.symmetric(horizontal: 14),
                  ),
                ),
              ),
            ],
          ),
          if (!isPaused) ...<Widget>[
            const SizedBox(height: 14),
            Wrap(
              spacing: 8,
              runSpacing: 8,
              children: <Widget>[
                Container(
                  padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
                  decoration: BoxDecoration(color: C.surface, borderRadius: BorderRadius.circular(8), border: Border.all(color: C.border)),
                  child: Text('Applied: ${speedProfile.toUpperCase()}  |  $speedThreads threads  |  ${speedTimeout}s', style: const TextStyle(color: C.txt2, fontSize: 11, fontWeight: FontWeight.w600)),
                ),
                Container(
                  padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
                  decoration: BoxDecoration(
                    color: hasPendingSpeedChanges ? C.neonAmber.withValues(alpha: 0.08) : C.surface,
                    borderRadius: BorderRadius.circular(8),
                    border: Border.all(color: hasPendingSpeedChanges ? C.neonAmber.withValues(alpha: 0.35) : C.border),
                  ),
                  child: Text('Draft: $draftLabel  |  $draftSpeedThreads threads  |  ${draftSpeedTimeout}s', style: TextStyle(color: hasPendingSpeedChanges ? C.neonAmber : C.txt2, fontSize: 11, fontWeight: FontWeight.w700)),
                ),
              ],
            ),
            const SizedBox(height: 12),
            // Auto profiles
            Row(
              children: <Widget>[
                const Text('Profile:', style: TextStyle(color: C.txt2, fontSize: 11)),
                const SizedBox(width: 8),
                for (final String p in <String>['low', 'medium', 'high'])
                  Padding(
                    padding: const EdgeInsets.only(right: 6),
                    child: ChoiceChip(
                      label: Text(p.toUpperCase(), style: TextStyle(fontSize: 10, fontWeight: FontWeight.w700, color: draftSpeedProfile == p ? C.bg : C.txt2)),
                      selected: draftSpeedProfile == p,
                      onSelected: canEditSpeed ? (_) => onSpeedProfile?.call(p) : null,
                      selectedColor: C.neonCyan,
                      backgroundColor: C.surface,
                      side: BorderSide(color: draftSpeedProfile == p ? C.neonCyan : C.border),
                      visualDensity: VisualDensity.compact,
                      padding: const EdgeInsets.symmetric(horizontal: 6),
                    ),
                  ),
                if (draftSpeedProfile == 'custom')
                  Container(
                    padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 6),
                    decoration: BoxDecoration(color: C.neonPurple.withValues(alpha: 0.12), borderRadius: BorderRadius.circular(20), border: Border.all(color: C.neonPurple.withValues(alpha: 0.35))),
                    child: const Text('CUSTOM', style: TextStyle(color: C.neonPurple, fontSize: 9, fontWeight: FontWeight.w800)),
                  ),
              ],
            ),
            const SizedBox(height: 10),
            // Threads slider
            Row(
              children: <Widget>[
                SizedBox(width: 90, child: Text('Threads: $draftSpeedThreads', style: const TextStyle(color: C.txt2, fontSize: 11))),
                Expanded(
                  child: SliderTheme(
                    data: SliderThemeData(overlayShape: SliderComponentShape.noOverlay, trackHeight: 3),
                    child: Slider(
                      value: draftSpeedThreads.toDouble(),
                      min: 1, max: 50,
                      divisions: 49,
                      activeColor: C.neonCyan,
                      inactiveColor: C.surface,
                      onChanged: canEditSpeed ? (double v) => onThreadsChanged?.call(v.round()) : null,
                    ),
                  ),
                ),
              ],
            ),
            // Timeout slider
            Row(
              children: <Widget>[
                SizedBox(width: 90, child: Text('Timeout: ${draftSpeedTimeout}s', style: const TextStyle(color: C.txt2, fontSize: 11))),
                Expanded(
                  child: SliderTheme(
                    data: SliderThemeData(overlayShape: SliderComponentShape.noOverlay, trackHeight: 3),
                    child: Slider(
                      value: draftSpeedTimeout.toDouble(),
                      min: 1, max: 10,
                      divisions: 9,
                      activeColor: C.neonPurple,
                      inactiveColor: C.surface,
                      onChanged: canEditSpeed ? (double v) => onTimeoutChanged?.call(v.round()) : null,
                    ),
                  ),
                ),
              ],
            ),
            const SizedBox(height: 10),
            Wrap(
              alignment: WrapAlignment.spaceBetween,
              runSpacing: 8,
              spacing: 8,
              crossAxisAlignment: WrapCrossAlignment.center,
              children: <Widget>[
                if (projectedScanDuration != null)
                  Container(
                    padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
                    decoration: BoxDecoration(color: C.neonCyan.withValues(alpha: 0.08), borderRadius: BorderRadius.circular(8), border: Border.all(color: C.neonCyan.withValues(alpha: 0.25))),
                    child: Text('Projected scan time: ${fmtDuration(projectedScanDuration!)}', style: const TextStyle(color: C.neonCyan, fontSize: 11, fontWeight: FontWeight.w700)),
                  ),
                if (speedApplyInFlight)
                  Container(
                    padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
                    decoration: BoxDecoration(color: C.neonAmber.withValues(alpha: 0.1), borderRadius: BorderRadius.circular(8), border: Border.all(color: C.neonAmber.withValues(alpha: 0.35))),
                    child: const Row(
                      mainAxisSize: MainAxisSize.min,
                      children: <Widget>[
                        SizedBox(width: 12, height: 12, child: CircularProgressIndicator(strokeWidth: 1.6, color: C.neonAmber)),
                        SizedBox(width: 8),
                        Text('PLEASE WAIT... applying speed', style: TextStyle(color: C.neonAmber, fontSize: 11, fontWeight: FontWeight.w800)),
                      ],
                    ),
                  )
                else if (hasPendingSpeedChanges)
                  Container(
                    padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
                    decoration: BoxDecoration(color: C.neonAmber.withValues(alpha: 0.08), borderRadius: BorderRadius.circular(8), border: Border.all(color: C.neonAmber.withValues(alpha: 0.3))),
                    child: const Text('Speed changed. Press APPLY and wait for sync.', style: TextStyle(color: C.neonAmber, fontSize: 11, fontWeight: FontWeight.w700)),
                  )
                else
                  Container(
                    padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
                    decoration: BoxDecoration(color: C.neonGreen.withValues(alpha: 0.08), borderRadius: BorderRadius.circular(8), border: Border.all(color: C.neonGreen.withValues(alpha: 0.25))),
                    child: const Text('Speed is synced with CLI', style: TextStyle(color: C.neonGreen, fontSize: 11, fontWeight: FontWeight.w700)),
                  ),
                SizedBox(
                  height: 34,
                  child: ElevatedButton.icon(
                    onPressed: canEditSpeed && hasPendingSpeedChanges ? onApplySpeedChanges : null,
                    icon: speedApplyInFlight
                        ? const SizedBox(width: 14, height: 14, child: CircularProgressIndicator(strokeWidth: 1.8, color: C.bg))
                        : const Icon(Icons.check_circle_outline, size: 16),
                    label: Text(speedApplyInFlight ? 'APPLYING...' : 'APPLY SPEED', style: const TextStyle(fontSize: 11, fontWeight: FontWeight.w800)),
                    style: ElevatedButton.styleFrom(
                      backgroundColor: C.neonCyan,
                      foregroundColor: C.bg,
                      disabledBackgroundColor: C.surface,
                      disabledForegroundColor: C.txt3,
                      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
                      padding: const EdgeInsets.symmetric(horizontal: 14),
                    ),
                  ),
                ),
              ],
            ),
          ],
        ],
      ),
    );
  }

  // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  // MAINTENANCE — Clear old + Manual add
  // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  Widget _buildMaintenanceSection(BuildContext context, bool isRunning) {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: C.card,
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: C.border),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          const Row(
            children: <Widget>[
              Icon(Icons.build_circle_outlined, color: C.txt3, size: 16),
              SizedBox(width: 8),
              Text('MAINTENANCE', style: TextStyle(color: C.txt2, fontSize: 12, fontWeight: FontWeight.w800, letterSpacing: 1)),
            ],
          ),
          const SizedBox(height: 12),
          // Clear old configs
          Row(
            children: <Widget>[
              const Text('Clear configs offline >', style: TextStyle(color: C.txt2, fontSize: 11)),
              const SizedBox(width: 6),
              SizedBox(
                width: 60,
                height: 28,
                child: DropdownButtonFormField<int>(
                  initialValue: clearAgeHours,
                  decoration: const InputDecoration(
                    border: OutlineInputBorder(),
                    contentPadding: EdgeInsets.symmetric(horizontal: 6, vertical: 0),
                    isDense: true,
                  ),
                  style: const TextStyle(color: C.txt1, fontSize: 11),
                  dropdownColor: C.card,
                  items: const <DropdownMenuItem<int>>[
                    DropdownMenuItem<int>(value: 1, child: Text('1h')),
                    DropdownMenuItem<int>(value: 6, child: Text('6h')),
                    DropdownMenuItem<int>(value: 24, child: Text('1d')),
                    DropdownMenuItem<int>(value: 72, child: Text('3d')),
                    DropdownMenuItem<int>(value: 168, child: Text('7d')),
                    DropdownMenuItem<int>(value: 720, child: Text('30d')),
                  ],
                  onChanged: (int? v) { if (v != null) onClearAgeChanged?.call(v); },
                ),
              ),
              const SizedBox(width: 8),
              SizedBox(
                height: 28,
                child: ElevatedButton(
                  onPressed: isRunning ? onClearOldConfigs : null,
                  style: ElevatedButton.styleFrom(
                    backgroundColor: C.neonRed.withValues(alpha: 0.12),
                    foregroundColor: C.neonRed,
                    shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(6), side: BorderSide(color: C.neonRed.withValues(alpha: 0.3))),
                    padding: const EdgeInsets.symmetric(horizontal: 12),
                  ),
                  child: const Text('CLEAR', style: TextStyle(fontSize: 10, fontWeight: FontWeight.w700)),
                ),
              ),
            ],
          ),
          const SizedBox(height: 12),
          // Manual config add
          const Text('Add configs manually:', style: TextStyle(color: C.txt2, fontSize: 11)),
          const SizedBox(height: 6),
          Row(
            children: <Widget>[
              Expanded(
                child: SizedBox(
                  height: 32,
                  child: TextField(
                    controller: manualConfigCtl,
                    style: const TextStyle(color: C.txt1, fontSize: 11, fontFamily: 'Consolas'),
                    decoration: InputDecoration(
                      hintText: 'vless://... or vmess://... (one per line)',
                      hintStyle: TextStyle(color: C.txt3.withValues(alpha: 0.5), fontSize: 11),
                      border: OutlineInputBorder(borderRadius: BorderRadius.circular(6)),
                      contentPadding: const EdgeInsets.symmetric(horizontal: 10, vertical: 0),
                      isDense: true,
                    ),
                    maxLines: 1,
                  ),
                ),
              ),
              const SizedBox(width: 8),
              SizedBox(
                height: 32,
                child: ElevatedButton.icon(
                  onPressed: isRunning ? onAddManualConfigs : null,
                  icon: const Icon(Icons.add, size: 14),
                  label: const Text('ADD', style: TextStyle(fontSize: 10, fontWeight: FontWeight.w700)),
                  style: ElevatedButton.styleFrom(
                    backgroundColor: C.neonCyan.withValues(alpha: 0.12),
                    foregroundColor: C.neonCyan,
                    shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(6), side: BorderSide(color: C.neonCyan.withValues(alpha: 0.3))),
                    padding: const EdgeInsets.symmetric(horizontal: 12),
                  ),
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }

  // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  // Helpers
  // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  Widget _actionBtn(String label, Color color, VoidCallback onPressed) {
    return TextButton(
      onPressed: onPressed,
      style: TextButton.styleFrom(
        foregroundColor: color,
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(6), side: BorderSide(color: color.withValues(alpha: 0.3))),
        backgroundColor: color.withValues(alpha: 0.08),
      ),
      child: Text(label, style: const TextStyle(fontSize: 11, fontWeight: FontWeight.w700, letterSpacing: 0.5)),
    );
  }

  Color _protoColor(String proto) {
    return switch (proto) {
      'VLESS' || 'VMESS' => C.neonCyan,
      'SS' || 'SHADOWSOCKS' => C.neonPurple,
      'TROJAN' => C.neonAmber,
      _ => C.txt3,
    };
  }

  Color _logColor(HunterLogLine line) {
    return switch (line.source) {
      'err' => C.neonRed,
      'warn' => C.neonAmber,
      'ui' || 'sys' => C.txt3,
      _ when line.message.contains('alive') || line.message.contains('Gold') || line.message.contains('working') => C.neonGreen,
      _ when line.message.contains('Error') || line.message.contains('FAIL') => C.neonRed,
      _ => C.txt3,
    };
  }

  bool _isLiveNow() {
    if (runState != HunterRunState.running || lastActivityAt == null) return false;
    return DateTime.now().difference(lastActivityAt!).inSeconds <= 5;
  }

  int _num(Map<String, dynamic>? m, String k) => (m?[k] is num) ? (m![k] as num).toInt() : 0;
  double _dbl(Map<String, dynamic>? m, String k) => (m?[k] is num) ? (m![k] as num).toDouble() : 0.0;
}
