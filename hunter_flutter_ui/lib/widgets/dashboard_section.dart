import 'dart:math' as math;
import 'package:flutter/material.dart';
import '../theme.dart';
import '../models.dart';
import '../services.dart';
import 'gauge_painter.dart';

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
    required this.runState,
    required this.engines,
    required this.lastRefresh,
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
  final HunterRunState runState;
  final Map<String, int> engines;
  final DateTime? lastRefresh;
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
    final int dbAlive = _num(db, 'alive');
    final int dbTested = _num(db, 'tested_unique');
    final int dbPending = _num(status, 'pending_unique');
    final int dbDead = math.max(0, dbTested - dbAlive);
    final double rate = _dbl(val, 'rate_per_s');
    final double eta = _dbl(status, 'eta_seconds');
    final double uptime = _dbl(status, 'uptime_s');
    final int totalAlive = silverConfigs.length + goldConfigs.length;

    return LayoutBuilder(
      builder: (BuildContext context, BoxConstraints constraints) {
        return SingleChildScrollView(
          padding: const EdgeInsets.all(20),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: <Widget>[
              // ── Hero Stats Row ──
              _buildHeroRow(context, dbTotal, dbAlive, totalAlive, dbTested, dbPending, rate, uptime),
              const SizedBox(height: 20),

              // ── Alive Configs (the star section) ──
              _buildAliveSection(context, totalAlive),
              const SizedBox(height: 20),

              // ── Progress + Sparkline ──
              Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: <Widget>[
                  Expanded(child: _buildProgressCard(context, dbTotal, dbTested, dbAlive, dbDead, dbPending, eta)),
                  const SizedBox(width: 16),
                  Expanded(child: _buildSparklineCard(context)),
                ],
              ),
              const SizedBox(height: 20),

              // ── Engine Status + Quick Stats ──
              Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: <Widget>[
                  Expanded(child: _buildEngineCard(context)),
                  const SizedBox(width: 16),
                  Expanded(child: _buildQuickStatsCard(context)),
                ],
              ),
              const SizedBox(height: 20),

              // ── Live Activity ──
              _buildLiveActivity(context),
            ],
          ),
        );
      },
    );
  }

  Widget _buildHeroRow(BuildContext context, int dbTotal, int dbAlive, int totalAlive,
      int dbTested, int dbPending, double rate, double uptime) {
    return Row(
      children: <Widget>[
        Expanded(child: _heroGauge(
          context, 'ALIVE', totalAlive.toDouble(), math.max(50, dbTested.toDouble()),
          C.neonGreen, Icons.bolt,
        )),
        const SizedBox(width: 12),
        Expanded(child: _heroGauge(
          context, 'TESTED', dbTested.toDouble(), math.max(100, dbTotal.toDouble()),
          C.neonCyan, Icons.speed,
        )),
        const SizedBox(width: 12),
        Expanded(child: _heroGauge(
          context, 'DB', dbTotal.toDouble(), math.max(1000, dbTotal.toDouble()),
          C.neonAmber, Icons.storage,
        )),
        const SizedBox(width: 12),
        Expanded(child: _heroStat(context, 'RATE', rate > 0 ? '${rate.toStringAsFixed(1)}/s' : '--', C.neonPurple, Icons.trending_up)),
        const SizedBox(width: 12),
        Expanded(child: _heroStat(context, 'UPTIME', uptime > 0 ? fmtDuration(Duration(seconds: uptime.round())) : '--', C.neonCyan, Icons.timer)),
      ],
    );
  }

  Widget _heroGauge(BuildContext context, String label, double value, double max, Color color, IconData icon) {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: C.card,
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: color.withValues(alpha: 0.2)),
      ),
      child: Column(
        children: <Widget>[
          SizedBox(
            width: 72, height: 72,
            child: Stack(
              alignment: Alignment.center,
              children: <Widget>[
                CustomPaint(
                  size: const Size(72, 72),
                  painter: ArcGaugePainter(value: value, max: max, color: color),
                ),
                Column(
                  mainAxisSize: MainAxisSize.min,
                  children: <Widget>[
                    Icon(icon, color: color, size: 16),
                    Text(
                      fmtNumber(value.round()),
                      style: TextStyle(color: color, fontWeight: FontWeight.w800, fontSize: 16, fontFamily: 'Consolas'),
                    ),
                  ],
                ),
              ],
            ),
          ),
          const SizedBox(height: 6),
          Text(label, style: const TextStyle(color: C.txt3, fontSize: 10, fontWeight: FontWeight.w600, letterSpacing: 1.5)),
        ],
      ),
    );
  }

  Widget _heroStat(BuildContext context, String label, String value, Color color, IconData icon) {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: C.card,
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: color.withValues(alpha: 0.2)),
      ),
      child: Column(
        children: <Widget>[
          Icon(icon, color: color, size: 28),
          const SizedBox(height: 8),
          Text(value, style: TextStyle(color: color, fontWeight: FontWeight.w800, fontSize: 18, fontFamily: 'Consolas')),
          const SizedBox(height: 4),
          Text(label, style: const TextStyle(color: C.txt3, fontSize: 10, fontWeight: FontWeight.w600, letterSpacing: 1.5)),
        ],
      ),
    );
  }

  Widget _buildAliveSection(BuildContext context, int totalAlive) {
    final List<String> aliveList = <String>[...goldConfigs, ...silverConfigs];
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        gradient: LinearGradient(
          colors: <Color>[C.neonGreen.withValues(alpha: 0.08), C.card],
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
        ),
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: C.neonGreen.withValues(alpha: 0.3)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Row(
            children: <Widget>[
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                decoration: BoxDecoration(
                  color: C.neonGreen.withValues(alpha: 0.15),
                  borderRadius: BorderRadius.circular(20),
                ),
                child: Row(
                  mainAxisSize: MainAxisSize.min,
                  children: <Widget>[
                    Icon(Icons.bolt, color: C.neonGreen, size: 16),
                    const SizedBox(width: 4),
                    Text('ALIVE CONFIGS', style: TextStyle(color: C.neonGreen, fontSize: 12, fontWeight: FontWeight.w700, letterSpacing: 1)),
                  ],
                ),
              ),
              const SizedBox(width: 12),
              Text('$totalAlive', style: const TextStyle(color: C.neonGreen, fontSize: 24, fontWeight: FontWeight.w800, fontFamily: 'Consolas')),
              const Spacer(),
              if (aliveList.isNotEmpty) ...<Widget>[
                _neonButton('COPY ALL', C.neonGreen, () => onCopyLines(aliveList, label: 'Copied $totalAlive alive configs')),
                const SizedBox(width: 8),
                _neonButton('VIEW', C.neonCyan, () => onNavigate(HunterNavSection.configs, kind: HunterConfigListKind.alive)),
              ],
            ],
          ),
          if (aliveList.isNotEmpty) ...<Widget>[
            const SizedBox(height: 12),
            SizedBox(
              height: math.min(aliveList.length * 34.0, 170.0),
              child: ListView.builder(
                itemCount: math.min(aliveList.length, 5),
                itemBuilder: (BuildContext context, int i) {
                  final String uri = aliveList[i];
                  final String proto = uri.contains('://') ? uri.split('://').first.toUpperCase() : '?';
                  final Color protoColor = switch (proto) {
                    'VLESS' || 'VMESS' => C.neonCyan,
                    'SS' || 'SHADOWSOCKS' => C.neonPurple,
                    'TROJAN' => C.neonAmber,
                    _ => C.txt3,
                  };
                  return InkWell(
                    onTap: () => onCopyText(uri, label: 'Config copied!'),
                    borderRadius: BorderRadius.circular(8),
                    child: Padding(
                      padding: const EdgeInsets.symmetric(vertical: 5, horizontal: 4),
                      child: Row(
                        children: <Widget>[
                          Container(
                            padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
                            decoration: BoxDecoration(
                              color: protoColor.withValues(alpha: 0.15),
                              borderRadius: BorderRadius.circular(4),
                            ),
                            child: Text(proto, style: TextStyle(color: protoColor, fontSize: 9, fontWeight: FontWeight.w700, fontFamily: 'Consolas')),
                          ),
                          const SizedBox(width: 8),
                          Expanded(
                            child: Text(uri, maxLines: 1, overflow: TextOverflow.ellipsis,
                              style: const TextStyle(color: C.txt2, fontSize: 11, fontFamily: 'Consolas')),
                          ),
                          Icon(Icons.content_copy, color: C.txt3, size: 14),
                        ],
                      ),
                    ),
                  );
                },
              ),
            ),
            if (aliveList.length > 5)
              Padding(
                padding: const EdgeInsets.only(top: 4),
                child: Text(
                  '+${aliveList.length - 5} more...',
                  style: TextStyle(color: C.neonGreen.withValues(alpha: 0.6), fontSize: 11),
                ),
              ),
          ] else
            Padding(
              padding: const EdgeInsets.only(top: 12),
              child: Text(
                runState == HunterRunState.running
                    ? 'Scanning... configs will appear here as they are validated'
                    : 'Start Hunter to discover alive configs',
                style: const TextStyle(color: C.txt3, fontSize: 12, fontStyle: FontStyle.italic),
              ),
            ),
        ],
      ),
    );
  }

  Widget _buildProgressCard(BuildContext context, int dbTotal, int dbTested, int dbAlive, int dbDead, int dbPending, double eta) {
    final double progress = dbTotal > 0 ? dbTested / dbTotal : 0.0;
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: C.card,
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: C.border),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          const Text('SCAN PROGRESS', style: TextStyle(color: C.txt3, fontSize: 10, fontWeight: FontWeight.w600, letterSpacing: 1.5)),
          const SizedBox(height: 12),
          // Progress bar
          ClipRRect(
            borderRadius: BorderRadius.circular(4),
            child: SizedBox(
              height: 8,
              child: LinearProgressIndicator(
                value: progress.clamp(0.0, 1.0),
                backgroundColor: C.border.withValues(alpha: 0.4),
                valueColor: AlwaysStoppedAnimation<Color>(C.neonCyan),
              ),
            ),
          ),
          const SizedBox(height: 8),
          Text('${(progress * 100).toStringAsFixed(1)}%',
            style: const TextStyle(color: C.neonCyan, fontSize: 20, fontWeight: FontWeight.w800, fontFamily: 'Consolas')),
          const SizedBox(height: 12),
          _statRow('Alive', '$dbAlive', C.neonGreen),
          _statRow('Dead', '$dbDead', C.neonRed),
          _statRow('Pending', fmtNumber(dbPending), C.neonAmber),
          _statRow('ETA', eta > 0 ? fmtDuration(Duration(seconds: eta.round())) : '--', C.txt2),
        ],
      ),
    );
  }

  Widget _statRow(String label, String value, Color valueColor) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 3),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.spaceBetween,
        children: <Widget>[
          Text(label, style: const TextStyle(color: C.txt3, fontSize: 11)),
          Text(value, style: TextStyle(color: valueColor, fontSize: 12, fontWeight: FontWeight.w700, fontFamily: 'Consolas')),
        ],
      ),
    );
  }

  Widget _buildSparklineCard(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: C.card,
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: C.border),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Row(
            children: <Widget>[
              const Text('TREND', style: TextStyle(color: C.txt3, fontSize: 10, fontWeight: FontWeight.w600, letterSpacing: 1.5)),
              const Spacer(),
              _legendDot(C.neonCyan, 'Tested'),
              const SizedBox(width: 10),
              _legendDot(C.neonGreen, 'Alive'),
            ],
          ),
          const SizedBox(height: 12),
          SizedBox(
            height: 120,
            child: history.length >= 2
                ? CustomPaint(
                    size: const Size(double.infinity, 120),
                    painter: SparklinePainter(
                      history: history,
                      line1Key: 'tested_unique',
                      line2Key: 'alive',
                      line1Color: C.neonCyan,
                      line2Color: C.neonGreen,
                    ),
                  )
                : const Center(child: Text('Waiting for data...', style: TextStyle(color: C.txt3, fontSize: 11))),
          ),
        ],
      ),
    );
  }

  Widget _legendDot(Color color, String label) {
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: <Widget>[
        Container(width: 8, height: 8, decoration: BoxDecoration(color: color, shape: BoxShape.circle)),
        const SizedBox(width: 4),
        Text(label, style: const TextStyle(color: C.txt3, fontSize: 10)),
      ],
    );
  }

  Widget _buildEngineCard(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: C.card,
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: C.border),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          const Text('ENGINES', style: TextStyle(color: C.txt3, fontSize: 10, fontWeight: FontWeight.w600, letterSpacing: 1.5)),
          const SizedBox(height: 12),
          ...kEngines.map((EngineInfo e) {
            final bool found = engines.containsKey(e.name);
            return Padding(
              padding: const EdgeInsets.symmetric(vertical: 4),
              child: Row(
                children: <Widget>[
                  Icon(found ? Icons.check_circle : Icons.cancel, color: found ? C.neonGreen : C.neonRed.withValues(alpha: 0.5), size: 16),
                  const SizedBox(width: 8),
                  Text(e.name, style: TextStyle(color: found ? C.txt1 : C.txt3, fontSize: 12, fontWeight: FontWeight.w600)),
                  const Spacer(),
                  Text(
                    found ? '${(engines[e.name]! / 1024 / 1024).toStringAsFixed(1)} MB' : 'not found',
                    style: TextStyle(color: found ? C.txt3 : C.neonRed.withValues(alpha: 0.5), fontSize: 10, fontFamily: 'Consolas'),
                  ),
                ],
              ),
            );
          }),
        ],
      ),
    );
  }

  Widget _buildQuickStatsCard(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: C.card,
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: C.border),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          const Text('SOURCES', style: TextStyle(color: C.txt3, fontSize: 10, fontWeight: FontWeight.w600, letterSpacing: 1.5)),
          const SizedBox(height: 12),
          _sourceRow('All Cache', fmtNumber(allCacheCount), C.neonCyan,
            () => onNavigate(HunterNavSection.configs, kind: HunterConfigListKind.allCache)),
          _sourceRow('GitHub', fmtNumber(githubCacheCount), C.neonPurple,
            () => onNavigate(HunterNavSection.configs, kind: HunterConfigListKind.githubCache)),
          _sourceRow('Balancer', '${balancerConfigs.where((HunterLatencyConfig e) => e.latencyMs != null).length}/${balancerConfigs.length}', C.neonAmber,
            () => onNavigate(HunterNavSection.configs, kind: HunterConfigListKind.balancer)),
          _sourceRow('Gemini', '${geminiConfigs.where((HunterLatencyConfig e) => e.latencyMs != null).length}/${geminiConfigs.length}', C.neonGreen,
            () => onNavigate(HunterNavSection.configs, kind: HunterConfigListKind.gemini)),
          _sourceRow('Silver', '${silverConfigs.length}', C.neonCyan,
            () => onNavigate(HunterNavSection.configs, kind: HunterConfigListKind.silver)),
        ],
      ),
    );
  }

  Widget _sourceRow(String label, String value, Color color, VoidCallback onTap) {
    return InkWell(
      onTap: onTap,
      borderRadius: BorderRadius.circular(6),
      child: Padding(
        padding: const EdgeInsets.symmetric(vertical: 5),
        child: Row(
          children: <Widget>[
            Container(width: 4, height: 16, decoration: BoxDecoration(color: color, borderRadius: BorderRadius.circular(2))),
            const SizedBox(width: 8),
            Text(label, style: const TextStyle(color: C.txt2, fontSize: 12)),
            const Spacer(),
            Text(value, style: TextStyle(color: color, fontSize: 12, fontWeight: FontWeight.w700, fontFamily: 'Consolas')),
            const SizedBox(width: 4),
            Icon(Icons.chevron_right, color: C.txt3, size: 14),
          ],
        ),
      ),
    );
  }

  Widget _buildLiveActivity(BuildContext context) {
    final List<HunterLogLine> recent = logs.length > 15 ? logs.sublist(logs.length - 15) : logs;
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: C.card,
        borderRadius: BorderRadius.circular(14),
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
                  color: runState == HunterRunState.running ? C.neonGreen : C.txt3,
                  shape: BoxShape.circle,
                  boxShadow: runState == HunterRunState.running
                      ? <BoxShadow>[BoxShadow(color: C.neonGreen.withValues(alpha: 0.5), blurRadius: 6)]
                      : null,
                ),
              ),
              const SizedBox(width: 8),
              const Text('LIVE ACTIVITY', style: TextStyle(color: C.txt3, fontSize: 10, fontWeight: FontWeight.w600, letterSpacing: 1.5)),
              const Spacer(),
              if (lastRefresh != null)
                Text(fmtTs(lastRefresh!), style: const TextStyle(color: C.txt3, fontSize: 10, fontFamily: 'Consolas')),
            ],
          ),
          const SizedBox(height: 8),
          if (recent.isEmpty)
            const Padding(
              padding: EdgeInsets.symmetric(vertical: 12),
              child: Text('No activity yet', style: TextStyle(color: C.txt3, fontSize: 11, fontStyle: FontStyle.italic)),
            )
          else
            ...recent.map((HunterLogLine line) {
              final Color lineColor = switch (line.source) {
                'err' => C.neonRed,
                'ui' => C.txt3,
                _ when line.message.contains('working') || line.message.contains('alive') || line.message.contains('Alive') || line.message.contains('Gold') => C.neonGreen,
                _ when line.message.contains('Error') || line.message.contains('error') || line.message.contains('FAIL') => C.neonRed,
                _ when line.message.contains('Bench') || line.message.contains('Testing') => C.neonAmber,
                _ => C.txt3,
              };
              final bool isConfig = line.message.startsWith('vless://') ||
                  line.message.startsWith('vmess://') ||
                  line.message.startsWith('ss://') ||
                  line.message.startsWith('trojan://');
              return InkWell(
                onTap: isConfig ? () => onCopyText(line.message, label: 'Config copied!') : null,
                child: Padding(
                  padding: const EdgeInsets.symmetric(vertical: 1),
                  child: Row(
                    children: <Widget>[
                      Text(fmtTs(line.ts), style: const TextStyle(color: C.txt3, fontSize: 9, fontFamily: 'Consolas')),
                      const SizedBox(width: 6),
                      Expanded(
                        child: Text(
                          line.message,
                          maxLines: 1,
                          overflow: TextOverflow.ellipsis,
                          style: TextStyle(color: lineColor, fontSize: 10, fontFamily: 'Consolas'),
                        ),
                      ),
                      if (isConfig) Icon(Icons.content_copy, color: C.neonGreen, size: 12),
                    ],
                  ),
                ),
              );
            }),
        ],
      ),
    );
  }

  Widget _neonButton(String label, Color color, VoidCallback onPressed) {
    return TextButton(
      onPressed: onPressed,
      style: TextButton.styleFrom(
        foregroundColor: color,
        padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 6),
        shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.circular(8),
          side: BorderSide(color: color.withValues(alpha: 0.4)),
        ),
        backgroundColor: color.withValues(alpha: 0.08),
      ),
      child: Text(label, style: const TextStyle(fontSize: 11, fontWeight: FontWeight.w700, letterSpacing: 0.5)),
    );
  }

  int _num(Map<String, dynamic>? m, String k) => (m?[k] is num) ? (m![k] as num).toInt() : 0;
  double _dbl(Map<String, dynamic>? m, String k) => (m?[k] is num) ? (m![k] as num).toDouble() : 0.0;
}
