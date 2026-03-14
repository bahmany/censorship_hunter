import 'package:flutter/material.dart';
import '../models.dart';
import '../theme.dart';
import 'one_button_connect.dart';
import 'qr_dialog.dart';

class SimpleDashboardSection extends StatelessWidget {
  const SimpleDashboardSection({
    super.key,
    required this.runState,
    required this.onConnect,
    required this.onDisconnect,
    required this.availableConfigsCount,
    required this.checkedConfigsCount,
    required this.liveConfigs,
    required this.onCopyText,
    required this.onCopyLines,
    this.noticeMessage,
    this.errorMessage,
  });

  final HunterRunState runState;
  final Future<void> Function() onConnect;
  final Future<void> Function() onDisconnect;
  final int availableConfigsCount;
  final int checkedConfigsCount;
  final List<String> liveConfigs;
  final Future<void> Function(String text, {String? label}) onCopyText;
  final Future<void> Function(List<String> lines, {String? label}) onCopyLines;
  final String? noticeMessage;
  final String? errorMessage;

  @override
  Widget build(BuildContext context) {
    final bool busy = runState == HunterRunState.starting || runState == HunterRunState.stopping;
    final VoidCallback? action = busy
        ? null
        : runState == HunterRunState.running
            ? () {
                onDisconnect();
              }
            : () {
                onConnect();
              };
    final String title = switch (runState) {
      HunterRunState.stopped => 'Ready to discover fresh V2Ray configs',
      HunterRunState.starting => 'Starting the discovery engine',
      HunterRunState.running => 'Discovering fresh V2Ray configs',
      HunterRunState.stopping => 'Stopping discovery',
    };
    final String subtitle = switch (runState) {
      HunterRunState.stopped => 'Launch the hunt and let Hunter search for working configs under Iranian network restrictions.',
      HunterRunState.starting => 'Preparing workers, engines, and live checks.',
      HunterRunState.running => 'Hunter is searching, validating, and refreshing live configs for restricted networks in Iran in continuous cycles.',
      HunterRunState.stopping => 'Hunter is winding down the current discovery cycle.',
    };
    final bool hasConfigs = liveConfigs.isNotEmpty;

    return Center(
      child: SingleChildScrollView(
        padding: const EdgeInsets.all(24),
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 760),
          child: Column(
            mainAxisAlignment: MainAxisAlignment.center,
            children: <Widget>[
              const SizedBox(height: 12),
              Text(
                'Fresh Config Discovery',
                textAlign: TextAlign.center,
                style: TextStyle(
                  color: C.neonCyan,
                  fontSize: 18,
                  fontWeight: FontWeight.w700,
                ),
              ),
              const SizedBox(height: 10),
              Text(
                title,
                textAlign: TextAlign.center,
                style: const TextStyle(
                  color: C.txt1,
                  fontSize: 34,
                  fontWeight: FontWeight.w900,
                ),
              ),
              const SizedBox(height: 10),
              Text(
                subtitle,
                textAlign: TextAlign.center,
                style: const TextStyle(
                  color: C.txt2,
                  fontSize: 15,
                  height: 1.7,
                ),
              ),
              const SizedBox(height: 32),
              OneButtonConnect(
                runState: runState,
                onPressed: action,
              ),
              const SizedBox(height: 24),
              Wrap(
                alignment: WrapAlignment.center,
                spacing: 12,
                runSpacing: 12,
                children: <Widget>[
                  _MetricCard(
                    label: 'Available now',
                    value: '$availableConfigsCount',
                    color: C.neonGreen,
                  ),
                  _MetricCard(
                    label: 'Checked',
                    value: '$checkedConfigsCount',
                    color: C.neonCyan,
                  ),
                  const _MetricCard(
                    label: 'Mode',
                    value: 'Continuous cycle',
                    color: C.neonAmber,
                  ),
                ],
              ),
              const SizedBox(height: 18),
              Wrap(
                alignment: WrapAlignment.center,
                spacing: 12,
                runSpacing: 12,
                children: <Widget>[
                  FilledButton.icon(
                    onPressed: hasConfigs
                        ? () => onCopyLines(liveConfigs, label: 'Copied ${liveConfigs.length} fresh configs')
                        : null,
                    style: FilledButton.styleFrom(
                      backgroundColor: C.neonCyan,
                      foregroundColor: C.bg,
                      padding: const EdgeInsets.symmetric(horizontal: 18, vertical: 16),
                      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(14)),
                    ),
                    icon: const Icon(Icons.copy_rounded),
                    label: Text('Copy Fresh Configs${hasConfigs ? ' (${liveConfigs.length})' : ''}'),
                  ),
                  OutlinedButton.icon(
                    onPressed: hasConfigs
                        ? () => _showTransferConfigsDialog(
                              context,
                              liveConfigs: liveConfigs,
                              onCopyText: onCopyText,
                              onCopyLines: onCopyLines,
                            )
                        : null,
                    style: OutlinedButton.styleFrom(
                      foregroundColor: C.neonPurple,
                      side: const BorderSide(color: C.neonPurple),
                      padding: const EdgeInsets.symmetric(horizontal: 18, vertical: 16),
                      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(14)),
                    ),
                    icon: const Icon(Icons.qr_code_rounded),
                    label: const Text('Transfer to Mobile'),
                  ),
                ],
              ),
              const SizedBox(height: 28),
              _InfoCard(
                color: errorMessage == null ? C.neonCyan : C.neonRed,
                icon: errorMessage == null ? Icons.info_outline_rounded : Icons.error_outline_rounded,
                text: errorMessage ?? noticeMessage ?? 'Hunter continuously looks for fresh V2Ray configs that still work under Iranian network constraints. After each completed cycle, it automatically starts the next pass.',
              ),
              const SizedBox(height: 16),
              Wrap(
                alignment: WrapAlignment.center,
                spacing: 12,
                runSpacing: 12,
                children: const <Widget>[
                  _SmallStep(text: 'Launch Hunter'),
                  _SmallStep(text: 'Start discovery'),
                  _SmallStep(text: 'Copy fresh configs'),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }
}

void _showTransferConfigsDialog(
  BuildContext context, {
  required List<String> liveConfigs,
  required Future<void> Function(String text, {String? label}) onCopyText,
  required Future<void> Function(List<String> lines, {String? label}) onCopyLines,
}) {
  showDialog<void>(
    context: context,
    builder: (BuildContext ctx) {
      return Dialog(
        backgroundColor: C.card,
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(20)),
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 760, maxHeight: 680),
          child: Padding(
            padding: const EdgeInsets.all(20),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Row(
                  children: <Widget>[
                    const Icon(Icons.qr_code_rounded, color: C.neonPurple),
                    const SizedBox(width: 10),
                    Expanded(
                      child: Text(
                        'Transfer Fresh Configs',
                        style: const TextStyle(
                          color: C.txt1,
                          fontSize: 18,
                          fontWeight: FontWeight.w800,
                        ),
                      ),
                    ),
                    IconButton(
                      onPressed: () => Navigator.of(ctx).pop(),
                      icon: const Icon(Icons.close_rounded, color: C.txt3),
                    ),
                  ],
                ),
                const SizedBox(height: 8),
                Text(
                  'Copy all fresh configs or open a QR code for any single config to import it on your phone.',
                  style: const TextStyle(
                    color: C.txt2,
                    fontSize: 13,
                    height: 1.6,
                  ),
                ),
                const SizedBox(height: 16),
                Row(
                  children: <Widget>[
                    FilledButton.icon(
                      onPressed: () => onCopyLines(liveConfigs, label: 'Copied ${liveConfigs.length} fresh configs'),
                      style: FilledButton.styleFrom(
                        backgroundColor: C.neonCyan,
                        foregroundColor: C.bg,
                        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
                        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(14)),
                      ),
                      icon: const Icon(Icons.copy_rounded),
                      label: Text('Copy All (${liveConfigs.length})'),
                    ),
                  ],
                ),
                const SizedBox(height: 16),
                Expanded(
                  child: Container(
                    decoration: BoxDecoration(
                      color: C.surface,
                      borderRadius: BorderRadius.circular(18),
                      border: Border.all(color: C.border),
                    ),
                    child: ListView.separated(
                      padding: const EdgeInsets.all(14),
                      itemCount: liveConfigs.length,
                      separatorBuilder: (BuildContext context, int index) => const SizedBox(height: 10),
                      itemBuilder: (BuildContext context, int index) {
                        final String uri = liveConfigs[index];
                        final String proto = uri.contains('://') ? uri.split('://').first.toUpperCase() : 'CONFIG';
                        final Color color = switch (proto) {
                          'VLESS' || 'VMESS' => C.neonCyan,
                          'SS' || 'SHADOWSOCKS' => C.neonPurple,
                          'TROJAN' => C.neonAmber,
                          _ => C.neonGreen,
                        };
                        return Container(
                          padding: const EdgeInsets.all(14),
                          decoration: BoxDecoration(
                            color: C.card,
                            borderRadius: BorderRadius.circular(16),
                            border: Border.all(color: C.border),
                          ),
                          child: Row(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: <Widget>[
                              Container(
                                padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
                                decoration: BoxDecoration(
                                  color: color.withValues(alpha: 0.16),
                                  borderRadius: BorderRadius.circular(999),
                                ),
                                child: Text(
                                  proto,
                                  style: TextStyle(
                                    color: color,
                                    fontSize: 11,
                                    fontWeight: FontWeight.w800,
                                  ),
                                ),
                              ),
                              const SizedBox(width: 12),
                              Expanded(
                                child: SelectableText(
                                  uri,
                                  style: const TextStyle(
                                    color: C.txt1,
                                    fontSize: 12,
                                    height: 1.5,
                                  ),
                                ),
                              ),
                              const SizedBox(width: 12),
                              Column(
                                mainAxisSize: MainAxisSize.min,
                                children: <Widget>[
                                  IconButton(
                                    onPressed: () => showQrDialog(context, uri, title: '$proto Config'),
                                    icon: const Icon(Icons.qr_code_rounded, color: C.neonPurple),
                                    tooltip: 'Show QR',
                                  ),
                                  IconButton(
                                    onPressed: () => onCopyText(uri, label: 'Config copied'),
                                    icon: const Icon(Icons.copy_rounded, color: C.neonCyan),
                                    tooltip: 'Copy',
                                  ),
                                ],
                              ),
                            ],
                          ),
                        );
                      },
                    ),
                  ),
                ),
              ],
            ),
          ),
        ),
      );
    },
  );
}

class _InfoCard extends StatelessWidget {
  const _InfoCard({
    required this.color,
    required this.icon,
    required this.text,
  });

  final Color color;
  final IconData icon;
  final String text;

  @override
  Widget build(BuildContext context) {
    return Container(
      width: double.infinity,
      padding: const EdgeInsets.symmetric(horizontal: 18, vertical: 16),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.08),
        borderRadius: BorderRadius.circular(18),
        border: Border.all(color: color.withValues(alpha: 0.28)),
      ),
      child: Row(
        children: <Widget>[
          Icon(icon, color: color),
          const SizedBox(width: 12),
          Expanded(
            child: Text(
              text,
              style: const TextStyle(
                color: C.txt1,
                fontSize: 14,
                height: 1.6,
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class _MetricCard extends StatelessWidget {
  const _MetricCard({
    required this.label,
    required this.value,
    required this.color,
  });

  final String label;
  final String value;
  final Color color;

  @override
  Widget build(BuildContext context) {
    return Container(
      constraints: const BoxConstraints(minWidth: 150),
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
      decoration: BoxDecoration(
        color: C.card,
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: color.withValues(alpha: 0.28)),
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: <Widget>[
          Text(
            label,
            style: TextStyle(
              color: color,
              fontSize: 11,
              fontWeight: FontWeight.w700,
              letterSpacing: 0.4,
            ),
          ),
          const SizedBox(height: 8),
          Text(
            value,
            style: const TextStyle(
              color: C.txt1,
              fontSize: 20,
              fontWeight: FontWeight.w900,
            ),
          ),
        ],
      ),
    );
  }
}

class _SmallStep extends StatelessWidget {
  const _SmallStep({required this.text});

  final String text;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
      decoration: BoxDecoration(
        color: C.card,
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: C.border),
      ),
      child: Text(
        text,
        style: const TextStyle(
          color: C.txt2,
          fontSize: 13,
          fontWeight: FontWeight.w600,
        ),
      ),
    );
  }
}
