import 'package:flutter/material.dart';
import '../theme.dart';

const String kCliVersion = '1.2.2';
const String kDashboardVersion = '1.2.2';

/// Structured changelog entries for display in About and Docs sections.
class ChangelogEntry {
  const ChangelogEntry({required this.version, required this.date, required this.items});
  final String version;
  final String date;
  final List<ChangelogItem> items;
}

class ChangelogItem {
  const ChangelogItem({required this.tag, required this.description});
  final String tag;         // FIX, NEW, PERF, UI
  final String description;
}

const List<ChangelogEntry> kChangelog = <ChangelogEntry>[
  ChangelogEntry(version: '1.2.2', date: '2026-03-14', items: <ChangelogItem>[
    ChangelogItem(tag: 'UI', description: 'Simplified dashboard now shows live Available now and Checked counters for config discovery'),
    ChangelogItem(tag: 'UI', description: 'Main dashboard messaging now clearly states that discovery runs continuously in cycles'),
    ChangelogItem(tag: 'NEW', description: 'Restored a dedicated Advanced workspace that consolidates Stats, Configs, Logs, Docs, and Runtime controls'),
    ChangelogItem(tag: 'FIX', description: 'Advanced tab selection no longer resets during routine live rebuilds and status refreshes'),
    ChangelogItem(tag: 'FIX', description: 'Legacy statistics, logs, and docs navigation now opens the correct Advanced tab reliably'),
  ]),
  ChangelogEntry(version: '1.2.1', date: '2026-03-13', items: <ChangelogItem>[
    ChangelogItem(tag: 'NEW', description: 'Deep LIVE RECHECK workflow — tests live configs sequentially through xray, sing-box, and mihomo using temporary random local ports'),
    ChangelogItem(tag: 'NEW', description: 'Pause-before-test + cleanup decision flow — after recheck you can clear dead configs, clear all tested configs, or continue/resume safely'),
    ChangelogItem(tag: 'FIX', description: 'Realtime WebSocket commands no longer time out aggressively — command timeouts now scale by command type instead of a fixed 8-second limit'),
    ChangelogItem(tag: 'FIX', description: 'Control WebSocket now sends immediate command acknowledgements before long-running command results return'),
    ChangelogItem(tag: 'PERF', description: 'Worker threads now honor orchestrator pause state during maintenance and live validation operations'),
  ]),
  ChangelogEntry(version: '1.2.0', date: '2026-03-13', items: <ChangelogItem>[
    ChangelogItem(tag: 'FIX', description: 'Sequential engine testing (xray \u2192 sing-box \u2192 mihomo) — prevents resource exhaustion from 3 parallel engine processes per config'),
    ChangelogItem(tag: 'FIX', description: 'Accept Telegram-only proxies as alive — critical for Iran where many proxies route TCP to Telegram but fail HTTP due to DPI'),
    ChangelogItem(tag: 'NEW', description: 'Config database persistence — saves health records to disk (HUNTER_config_db.tsv), survives restarts'),
    ChangelogItem(tag: 'FIX', description: 'Import timeout for large config batches — now processes in batches of 5000 with progress reporting'),
    ChangelogItem(tag: 'FIX', description: 'Speed test timeout — uses user-configured timeout instead of hardcoded 3s/5s'),
    ChangelogItem(tag: 'PERF', description: 'Exponential backoff for failed configs — 60s/5min/30min based on consecutive failure count'),
    ChangelogItem(tag: 'NEW', description: 'WebSocket real-time communication (ws://127.0.0.1:15491 control, :15492 monitor)'),
    ChangelogItem(tag: 'NEW', description: 'Mixed inbound ports — single port serves both HTTP and SOCKS (like v2rayNG)'),
    ChangelogItem(tag: 'UI', description: 'Discovery timestamps shown for each config (first seen, last alive, last tested)'),
    ChangelogItem(tag: 'UI', description: 'QR code generation for easy mobile transfer'),
    ChangelogItem(tag: 'UI', description: 'Version display in About section and top bar'),
  ]),
  ChangelogEntry(version: '1.1.0', date: '2026-02-20', items: <ChangelogItem>[
    ChangelogItem(tag: 'NEW', description: 'Multi-engine support: XRay, Sing-box, Mihomo'),
    ChangelogItem(tag: 'NEW', description: 'Continuous background validation with ConfigDatabase'),
    ChangelogItem(tag: 'NEW', description: 'Provisioned proxy ports (2901-2999) with auto health checks'),
    ChangelogItem(tag: 'NEW', description: 'System proxy integration (set/clear per port)'),
    ChangelogItem(tag: 'UI', description: 'Racing Neon dark theme with color psychology palette'),
    ChangelogItem(tag: 'UI', description: 'Real-time dashboard with stats, gauges, sparklines'),
  ]),
  ChangelogEntry(version: '1.0.0', date: '2026-01-15', items: <ChangelogItem>[
    ChangelogItem(tag: 'NEW', description: 'Initial release — Telegram + GitHub + HTTP config scraping'),
    ChangelogItem(tag: 'NEW', description: 'XRay-based proxy testing and load balancing'),
    ChangelogItem(tag: 'NEW', description: 'Flutter desktop dashboard with system tray'),
    ChangelogItem(tag: 'NEW', description: 'Bundled seed configs for first-run bootstrap'),
  ]),
];

class AboutSection extends StatelessWidget {
  const AboutSection({super.key, required this.bundledConfigsCount});
  final int bundledConfigsCount;

  @override
  Widget build(BuildContext context) {
    return SingleChildScrollView(
      padding: const EdgeInsets.all(24),
      child: Center(
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 680),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: <Widget>[
              const SizedBox(height: 16),
              // ── Header ──
              Center(
                child: Column(
                  children: <Widget>[
                    Container(
                      width: 72, height: 72,
                      decoration: BoxDecoration(
                        shape: BoxShape.circle,
                        gradient: LinearGradient(
                          colors: <Color>[C.neonCyan.withValues(alpha: 0.2), C.neonGreen.withValues(alpha: 0.2)],
                          begin: Alignment.topLeft,
                          end: Alignment.bottomRight,
                        ),
                        border: Border.all(color: C.neonCyan.withValues(alpha: 0.3), width: 2),
                      ),
                      child: const Icon(Icons.shield, size: 36, color: C.neonCyan),
                    ),
                    const SizedBox(height: 16),
                    const Text('HUNTER', style: TextStyle(
                      color: C.neonCyan, fontSize: 26, fontWeight: FontWeight.w900, letterSpacing: 6, fontFamily: 'Consolas',
                    )),
                    const SizedBox(height: 4),
                    const Text('Anti-Censorship Config Discovery', style: TextStyle(color: C.txt3, fontSize: 11, letterSpacing: 1)),
                  ],
                ),
              ),
              const SizedBox(height: 24),
              // ── Version + Info Cards ──
              Row(
                children: <Widget>[
                  Expanded(child: _infoCard(Icons.terminal, 'CLI', 'v$kCliVersion')),
                  const SizedBox(width: 10),
                  Expanded(child: _infoCard(Icons.dashboard_outlined, 'Dashboard', 'v$kDashboardVersion')),
                ],
              ),
              const SizedBox(height: 10),
              Row(
                children: <Widget>[
                  Expanded(child: _infoCard(Icons.storage_outlined, 'Seeds', '$bundledConfigsCount')),
                  const SizedBox(width: 10),
                  Expanded(child: _infoCard(Icons.speed, 'Engines', 'XRay, Sing-box, Mihomo')),
                ],
              ),
              const SizedBox(height: 10),
              _infoCard(Icons.code, 'Repository', 'github.com/bahmany/censorship_hunter'),
              const SizedBox(height: 28),
              // ── Changelog ──
              const Text('CHANGELOG', style: TextStyle(
                color: C.txt2, fontSize: 11, fontWeight: FontWeight.w700, letterSpacing: 1.6,
              )),
              const SizedBox(height: 12),
              ...kChangelog.expand((ChangelogEntry entry) => <Widget>[
                _versionHeader(entry.version, entry.date),
                const SizedBox(height: 8),
                ...entry.items.map((ChangelogItem item) => _changelogRow(item)),
                const SizedBox(height: 16),
              ]),
              const SizedBox(height: 12),
              Center(
                child: Text(
                  'Fighting internet censorship in Iran',
                  style: TextStyle(color: C.txt3.withValues(alpha: 0.5), fontSize: 10, fontStyle: FontStyle.italic),
                ),
              ),
              const SizedBox(height: 24),
            ],
          ),
        ),
      ),
    );
  }

  Widget _infoCard(IconData icon, String label, String value) {
    return Container(
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: C.card,
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: C.border),
      ),
      child: Row(
        children: <Widget>[
          Icon(icon, color: C.neonCyan, size: 16),
          const SizedBox(width: 10),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Text(label, style: const TextStyle(color: C.txt3, fontSize: 9, fontWeight: FontWeight.w600)),
                const SizedBox(height: 2),
                Text(value, style: const TextStyle(color: C.txt1, fontSize: 11, fontFamily: 'Consolas')),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _versionHeader(String version, String date) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
      decoration: BoxDecoration(
        color: C.neonCyan.withValues(alpha: 0.06),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: C.neonCyan.withValues(alpha: 0.2)),
      ),
      child: Row(
        children: <Widget>[
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
            decoration: BoxDecoration(
              color: C.neonCyan.withValues(alpha: 0.15),
              borderRadius: BorderRadius.circular(6),
            ),
            child: Text('v$version', style: const TextStyle(color: C.neonCyan, fontSize: 12, fontWeight: FontWeight.w800, fontFamily: 'Consolas')),
          ),
          const SizedBox(width: 10),
          Text(date, style: const TextStyle(color: C.txt3, fontSize: 10, fontFamily: 'Consolas')),
        ],
      ),
    );
  }

  Widget _changelogRow(ChangelogItem item) {
    final Color tagColor = switch (item.tag) {
      'FIX' => C.neonRed,
      'NEW' => C.neonGreen,
      'PERF' => C.neonAmber,
      'UI' => C.neonPurple,
      _ => C.txt3,
    };
    return Padding(
      padding: const EdgeInsets.only(left: 8, bottom: 6),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Container(
            width: 36,
            padding: const EdgeInsets.symmetric(vertical: 2),
            alignment: Alignment.center,
            decoration: BoxDecoration(
              color: tagColor.withValues(alpha: 0.12),
              borderRadius: BorderRadius.circular(4),
            ),
            child: Text(item.tag, style: TextStyle(color: tagColor, fontSize: 8, fontWeight: FontWeight.w800, fontFamily: 'Consolas')),
          ),
          const SizedBox(width: 8),
          Expanded(
            child: Text(item.description, style: const TextStyle(color: C.txt2, fontSize: 11, height: 1.4)),
          ),
        ],
      ),
    );
  }
}
