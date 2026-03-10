import 'package:flutter/material.dart';
import '../theme.dart';
import '../models.dart';
import '../services.dart';
import 'qr_dialog.dart';

class ConfigsSection extends StatelessWidget {
  const ConfigsSection({
    super.key,
    required this.listKind,
    required this.searchController,
    required this.allCacheConfigs,
    required this.githubCacheConfigs,
    required this.silverConfigs,
    required this.goldConfigs,
    required this.balancerConfigs,
    required this.geminiConfigs,
    required this.lastRefresh,
    required this.onKindChanged,
    required this.onSearchChanged,
    required this.onRefresh,
    required this.onCopyText,
    required this.onCopyLines,
    required this.onSpeedTest,
  });

  final HunterConfigListKind listKind;
  final TextEditingController searchController;
  final List<String> allCacheConfigs;
  final List<String> githubCacheConfigs;
  final List<String> silverConfigs;
  final List<String> goldConfigs;
  final List<HunterLatencyConfig> balancerConfigs;
  final List<HunterLatencyConfig> geminiConfigs;
  final DateTime? lastRefresh;
  final ValueChanged<HunterConfigListKind> onKindChanged;
  final VoidCallback onSearchChanged;
  final VoidCallback onRefresh;
  final Future<void> Function(String text, {String? label}) onCopyText;
  final Future<void> Function(List<String> lines, {String? label}) onCopyLines;
  final Future<void> Function(String configUri) onSpeedTest;

  List<String> get _aliveConfigs {
    final Set<String> seen = <String>{};
    final List<String> result = <String>[];
    for (final String uri in <String>[...goldConfigs, ...silverConfigs]) {
      if (seen.add(uri)) result.add(uri);
    }
    return result;
  }

  bool get _isLatencyKind => listKind == HunterConfigListKind.balancer || listKind == HunterConfigListKind.gemini;

  List<String> _baseStrings() {
    return switch (listKind) {
      HunterConfigListKind.alive => _aliveConfigs,
      HunterConfigListKind.silver => silverConfigs,
      HunterConfigListKind.allCache => allCacheConfigs,
      HunterConfigListKind.githubCache => githubCacheConfigs,
      _ => const <String>[],
    };
  }

  List<HunterLatencyConfig> _baseLatency() {
    return switch (listKind) {
      HunterConfigListKind.balancer => balancerConfigs,
      HunterConfigListKind.gemini => geminiConfigs,
      _ => const <HunterLatencyConfig>[],
    };
  }

  String _kindLabel(HunterConfigListKind k) {
    return switch (k) {
      HunterConfigListKind.alive => 'Found (${_aliveConfigs.length})',
      HunterConfigListKind.silver => 'Silver (${silverConfigs.length})',
      HunterConfigListKind.balancer => 'Live Xray (${balancerConfigs.length})',
      HunterConfigListKind.gemini => 'Live Gemini (${geminiConfigs.length})',
      HunterConfigListKind.allCache => 'All Cache (${fmtNumber(allCacheConfigs.length)})',
      HunterConfigListKind.githubCache => 'GitHub (${fmtNumber(githubCacheConfigs.length)})',
    };
  }

  @override
  Widget build(BuildContext context) {
    final String q = searchController.text.trim().toLowerCase();
    final bool latency = _isLatencyKind;

    List<String> visibleUris;
    List<HunterLatencyConfig> visibleLatency;

    if (latency) {
      final List<HunterLatencyConfig> base = _baseLatency();
      visibleLatency = q.isEmpty ? base : base.where((HunterLatencyConfig c) => c.uri.toLowerCase().contains(q)).toList();
      visibleUris = visibleLatency.map((HunterLatencyConfig e) => e.uri).toList();
    } else {
      final List<String> base = _baseStrings();
      visibleUris = q.isEmpty ? base : base.where((String s) => s.toLowerCase().contains(q)).toList();
      visibleLatency = const <HunterLatencyConfig>[];
    }

    return Column(
      children: <Widget>[
        // ── Header bar ──
        Container(
          padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
          decoration: BoxDecoration(
            color: C.card,
            borderRadius: const BorderRadius.vertical(top: Radius.circular(14)),
            border: Border.all(color: C.border),
          ),
          child: Column(
            children: <Widget>[
              Row(
                children: <Widget>[
                  // Kind selector chips
                  Expanded(
                    child: SingleChildScrollView(
                      scrollDirection: Axis.horizontal,
                      child: Row(
                        children: HunterConfigListKind.values.map((HunterConfigListKind k) {
                          final bool selected = k == listKind;
                          final Color chipColor = switch (k) {
                            HunterConfigListKind.alive => C.neonGreen,
                            HunterConfigListKind.silver => C.neonCyan,
                            HunterConfigListKind.balancer => C.neonAmber,
                            HunterConfigListKind.gemini => C.neonPurple,
                            _ => C.txt3,
                          };
                          return Padding(
                            padding: const EdgeInsets.only(right: 6),
                            child: ChoiceChip(
                              label: Text(_kindLabel(k), style: TextStyle(
                                color: selected ? C.bg : chipColor,
                                fontSize: 11,
                                fontWeight: FontWeight.w600,
                              )),
                              selected: selected,
                              selectedColor: chipColor,
                              backgroundColor: chipColor.withValues(alpha: 0.1),
                              side: BorderSide(color: chipColor.withValues(alpha: selected ? 0.8 : 0.3)),
                              onSelected: (_) => onKindChanged(k),
                              visualDensity: VisualDensity.compact,
                              padding: const EdgeInsets.symmetric(horizontal: 6),
                            ),
                          );
                        }).toList(),
                      ),
                    ),
                  ),
                ],
              ),
              const SizedBox(height: 10),
              Row(
                children: <Widget>[
                  Expanded(
                    child: SizedBox(
                      height: 38,
                      child: TextField(
                        controller: searchController,
                        onChanged: (_) => onSearchChanged(),
                        style: const TextStyle(fontSize: 12, color: C.txt1),
                        decoration: InputDecoration(
                          hintText: 'Search configs...',
                          hintStyle: const TextStyle(color: C.txt3, fontSize: 12),
                          prefixIcon: const Icon(Icons.search, size: 18, color: C.txt3),
                          suffixIcon: searchController.text.isNotEmpty
                              ? IconButton(
                                  icon: const Icon(Icons.clear, size: 16, color: C.txt3),
                                  onPressed: () { searchController.clear(); onSearchChanged(); },
                                )
                              : null,
                          isDense: true,
                          contentPadding: const EdgeInsets.symmetric(horizontal: 12),
                          filled: true,
                          fillColor: C.surface,
                          border: OutlineInputBorder(borderRadius: BorderRadius.circular(8), borderSide: BorderSide(color: C.border)),
                          enabledBorder: OutlineInputBorder(borderRadius: BorderRadius.circular(8), borderSide: BorderSide(color: C.border)),
                          focusedBorder: OutlineInputBorder(borderRadius: BorderRadius.circular(8), borderSide: const BorderSide(color: C.neonCyan)),
                        ),
                      ),
                    ),
                  ),
                  const SizedBox(width: 8),
                  _neonBtn('COPY ${visibleUris.length}', C.neonCyan,
                    visibleUris.isEmpty ? null : () => onCopyLines(visibleUris, label: 'Copied ${visibleUris.length} configs')),
                  const SizedBox(width: 6),
                  IconButton(
                    icon: const Icon(Icons.refresh, size: 18, color: C.neonCyan),
                    onPressed: onRefresh,
                    tooltip: 'Refresh',
                    visualDensity: VisualDensity.compact,
                  ),
                ],
              ),
            ],
          ),
        ),
        // ── Info bar ──
        Container(
          padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 6),
          decoration: const BoxDecoration(
            color: C.surface,
            border: Border(left: BorderSide(color: C.border), right: BorderSide(color: C.border)),
          ),
          child: Row(
            children: <Widget>[
              Text('${visibleUris.length} shown', style: const TextStyle(color: C.txt2, fontSize: 11, fontWeight: FontWeight.w600)),
              const Spacer(),
              if (lastRefresh != null) Text(fmtTs(lastRefresh!), style: const TextStyle(color: C.txt3, fontSize: 10, fontFamily: 'Consolas')),
            ],
          ),
        ),
        // ── Config list ──
        Expanded(
          child: Container(
            decoration: BoxDecoration(
              color: C.surface,
              borderRadius: const BorderRadius.vertical(bottom: Radius.circular(14)),
              border: Border.all(color: C.border),
            ),
            child: ClipRRect(
              borderRadius: const BorderRadius.vertical(bottom: Radius.circular(14)),
              child: ListView.builder(
                padding: EdgeInsets.zero,
                itemCount: latency ? visibleLatency.length : visibleUris.length,
                itemBuilder: (BuildContext context, int i) {
                  if (latency) {
                    return _latencyRow(context, visibleLatency[i]);
                  }
                  return _configRow(context, visibleUris[i]);
                },
              ),
            ),
          ),
        ),
      ],
    );
  }

  Widget _configRow(BuildContext context, String uri) {
    final String proto = uri.contains('://') ? uri.split('://').first.toUpperCase() : '?';
    final Color protoColor = switch (proto) {
      'VLESS' || 'VMESS' => C.neonCyan,
      'SS' || 'SHADOWSOCKS' => C.neonPurple,
      'TROJAN' => C.neonAmber,
      _ => C.txt3,
    };
    final bool isAlive = listKind == HunterConfigListKind.alive || listKind == HunterConfigListKind.silver;

    return InkWell(
      onTap: () => onCopyText(uri, label: 'Config copied!'),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 8),
        decoration: const BoxDecoration(
          border: Border(bottom: BorderSide(color: C.border, width: 0.5)),
        ),
        child: Row(
          children: <Widget>[
            Container(
              padding: const EdgeInsets.symmetric(horizontal: 5, vertical: 2),
              decoration: BoxDecoration(
                color: protoColor.withValues(alpha: 0.12),
                borderRadius: BorderRadius.circular(4),
              ),
              child: Text(proto, style: TextStyle(color: protoColor, fontSize: 9, fontWeight: FontWeight.w700, fontFamily: 'Consolas')),
            ),
            const SizedBox(width: 8),
            Expanded(
              child: Tooltip(
                message: uri,
                child: Text(uri, maxLines: 1, overflow: TextOverflow.ellipsis,
                  style: const TextStyle(color: C.txt2, fontSize: 11, fontFamily: 'Consolas', height: 1.2)),
              ),
            ),
            if (isAlive)
              IconButton(
                icon: const Icon(Icons.speed, size: 16, color: C.neonAmber),
                onPressed: () => onSpeedTest(uri),
                tooltip: 'Speed test',
                visualDensity: VisualDensity.compact,
                padding: EdgeInsets.zero,
                constraints: const BoxConstraints(minWidth: 28, minHeight: 28),
              ),
            IconButton(
              icon: const Icon(Icons.qr_code, size: 14, color: C.neonPurple),
              onPressed: () => showQrDialog(context, uri, title: '$proto Config'),
              tooltip: 'QR Code for mobile',
              visualDensity: VisualDensity.compact,
              padding: EdgeInsets.zero,
              constraints: const BoxConstraints(minWidth: 28, minHeight: 28),
            ),
            IconButton(
              icon: const Icon(Icons.content_copy, size: 14, color: C.txt3),
              onPressed: () => onCopyText(uri, label: 'Config copied!'),
              tooltip: 'Copy',
              visualDensity: VisualDensity.compact,
              padding: EdgeInsets.zero,
              constraints: const BoxConstraints(minWidth: 28, minHeight: 28),
            ),
          ],
        ),
      ),
    );
  }

  Widget _latencyRow(BuildContext context, HunterLatencyConfig cfg) {
    final String latencyText = (cfg.latencyMs == null || !cfg.latencyMs!.isFinite) ? '--' : '${cfg.latencyMs!.toStringAsFixed(0)}ms';
    final Color latColor = cfg.latencyMs != null
        ? (cfg.latencyMs! < 500 ? C.neonGreen : (cfg.latencyMs! < 2000 ? C.neonAmber : C.neonRed))
        : C.txt3;
    final String proto = cfg.uri.contains('://') ? cfg.uri.split('://').first.toUpperCase() : '?';
    final Color protoColor = switch (proto) {
      'VLESS' || 'VMESS' => C.neonCyan,
      'SS' || 'SHADOWSOCKS' => C.neonPurple,
      'TROJAN' => C.neonAmber,
      _ => C.txt3,
    };
    final String discoveredAt = _fmtUnixTs(cfg.firstSeen);
    final String lastAliveAt = _fmtUnixTs(cfg.lastAlive);

    return InkWell(
      onTap: () => onCopyText(cfg.uri, label: 'Config copied!'),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 8),
        decoration: const BoxDecoration(
          border: Border(bottom: BorderSide(color: C.border, width: 0.5)),
        ),
        child: Row(
          children: <Widget>[
            SizedBox(
              width: 60,
              child: Text(latencyText, textAlign: TextAlign.right,
                style: TextStyle(color: latColor, fontSize: 11, fontWeight: FontWeight.w700, fontFamily: 'Consolas')),
            ),
            const SizedBox(width: 10),
            Container(
              padding: const EdgeInsets.symmetric(horizontal: 5, vertical: 2),
              decoration: BoxDecoration(
                color: protoColor.withValues(alpha: 0.12),
                borderRadius: BorderRadius.circular(4),
              ),
              child: Text(proto, style: TextStyle(color: protoColor, fontSize: 9, fontWeight: FontWeight.w700, fontFamily: 'Consolas')),
            ),
            const SizedBox(width: 6),
            // Discovery time
            if (discoveredAt.isNotEmpty)
              Tooltip(
                message: 'Found: $discoveredAt\nLast alive: $lastAliveAt\nTests: ${cfg.totalTests ?? 0}',
                child: Text(discoveredAt, style: TextStyle(color: C.txt3.withValues(alpha: 0.6), fontSize: 9, fontFamily: 'Consolas')),
              ),
            const SizedBox(width: 6),
            Expanded(
              child: Tooltip(
                message: cfg.uri,
                child: Text(cfg.uri, maxLines: 1, overflow: TextOverflow.ellipsis,
                  style: const TextStyle(color: C.txt2, fontSize: 11, fontFamily: 'Consolas', height: 1.2)),
              ),
            ),
            IconButton(
              icon: const Icon(Icons.speed, size: 16, color: C.neonAmber),
              onPressed: () => onSpeedTest(cfg.uri),
              tooltip: 'Speed test',
              visualDensity: VisualDensity.compact,
              padding: EdgeInsets.zero,
              constraints: const BoxConstraints(minWidth: 28, minHeight: 28),
            ),
            IconButton(
              icon: const Icon(Icons.qr_code, size: 14, color: C.neonPurple),
              onPressed: () => showQrDialog(context, cfg.uri, title: '$proto Config'),
              tooltip: 'QR Code for mobile',
              visualDensity: VisualDensity.compact,
              padding: EdgeInsets.zero,
              constraints: const BoxConstraints(minWidth: 28, minHeight: 28),
            ),
            IconButton(
              icon: const Icon(Icons.content_copy, size: 14, color: C.txt3),
              onPressed: () => onCopyText(cfg.uri, label: 'Config copied!'),
              tooltip: 'Copy',
              visualDensity: VisualDensity.compact,
              padding: EdgeInsets.zero,
              constraints: const BoxConstraints(minWidth: 28, minHeight: 28),
            ),
          ],
        ),
      ),
    );
  }

  static String _fmtUnixTs(double? ts) {
    if (ts == null || ts <= 0) return '';
    try {
      final DateTime dt = DateTime.fromMillisecondsSinceEpoch((ts * 1000).toInt(), isUtc: true).toLocal();
      final DateTime now = DateTime.now();
      final Duration diff = now.difference(dt);
      if (diff.inMinutes < 1) return 'now';
      if (diff.inMinutes < 60) return '${diff.inMinutes}m ago';
      if (diff.inHours < 24) return '${diff.inHours}h ago';
      return '${dt.month}/${dt.day} ${dt.hour.toString().padLeft(2, '0')}:${dt.minute.toString().padLeft(2, '0')}';
    } catch (_) {
      return '';
    }
  }

  Widget _neonBtn(String label, Color color, VoidCallback? onPressed) {
    return TextButton(
      onPressed: onPressed,
      style: TextButton.styleFrom(
        foregroundColor: color,
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
        shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.circular(8),
          side: BorderSide(color: color.withValues(alpha: onPressed != null ? 0.4 : 0.15)),
        ),
        backgroundColor: color.withValues(alpha: onPressed != null ? 0.08 : 0.03),
      ),
      child: Text(label, style: TextStyle(fontSize: 11, fontWeight: FontWeight.w700, color: onPressed != null ? color : C.txt3)),
    );
  }
}
