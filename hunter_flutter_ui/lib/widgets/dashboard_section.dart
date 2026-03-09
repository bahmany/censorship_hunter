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
