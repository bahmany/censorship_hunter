import 'package:flutter/material.dart';
import '../models.dart';
import '../theme.dart';
import 'one_button_connect.dart';

class SimpleDashboardSection extends StatelessWidget {
  const SimpleDashboardSection({
    super.key,
    required this.runState,
    required this.onConnect,
    required this.onDisconnect,
    required this.availableConfigsCount,
    required this.checkedConfigsCount,
    this.noticeMessage,
    this.errorMessage,
  });

  final HunterRunState runState;
  final Future<void> Function() onConnect;
  final Future<void> Function() onDisconnect;
  final int availableConfigsCount;
  final int checkedConfigsCount;
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
