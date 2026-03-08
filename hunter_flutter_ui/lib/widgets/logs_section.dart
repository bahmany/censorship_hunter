import 'package:flutter/material.dart';
import '../theme.dart';
import '../models.dart';
import '../services.dart';

class LogsSection extends StatelessWidget {
  const LogsSection({
    super.key,
    required this.logs,
    required this.logScrollController,
    required this.autoScroll,
    required this.onAutoScrollChanged,
    required this.onCopyLogs,
    required this.onClearLogs,
    required this.logMemoryBytes,
  });

  final List<HunterLogLine> logs;
  final ScrollController logScrollController;
  final bool autoScroll;
  final ValueChanged<bool> onAutoScrollChanged;
  final VoidCallback onCopyLogs;
  final VoidCallback onClearLogs;
  final int logMemoryBytes;

  @override
  Widget build(BuildContext context) {
    final String memText = logMemoryBytes < 1024
        ? '$logMemoryBytes B'
        : '${(logMemoryBytes / 1024).toStringAsFixed(1)} KB';

    return Column(
      children: <Widget>[
        // Header
        Container(
          padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
          decoration: BoxDecoration(
            color: C.card,
            borderRadius: const BorderRadius.vertical(top: Radius.circular(14)),
            border: Border.all(color: C.border),
          ),
          child: Row(
            children: <Widget>[
              const Icon(Icons.terminal, color: C.neonCyan, size: 16),
              const SizedBox(width: 8),
              const Text('LOGS', style: TextStyle(color: C.txt3, fontSize: 10, fontWeight: FontWeight.w600, letterSpacing: 1.5)),
              const SizedBox(width: 12),
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
                decoration: BoxDecoration(
                  color: C.surface,
                  borderRadius: BorderRadius.circular(10),
                ),
                child: Text('${logs.length}', style: const TextStyle(color: C.neonCyan, fontSize: 10, fontFamily: 'Consolas', fontWeight: FontWeight.w700)),
              ),
              const SizedBox(width: 8),
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
                decoration: BoxDecoration(
                  color: logMemoryBytes > 90 * 1024
                      ? C.neonAmber.withValues(alpha: 0.15)
                      : C.surface,
                  borderRadius: BorderRadius.circular(10),
                ),
                child: Text(memText, style: TextStyle(
                  color: logMemoryBytes > 90 * 1024 ? C.neonAmber : C.txt3,
                  fontSize: 10, fontFamily: 'Consolas',
                )),
              ),
              const Spacer(),
              _miniBtn(Icons.content_copy, 'Copy all', onCopyLogs),
              const SizedBox(width: 4),
              _miniBtn(Icons.delete_outline, 'Clear', onClearLogs),
              const SizedBox(width: 12),
              Text('Auto', style: const TextStyle(color: C.txt3, fontSize: 10)),
              const SizedBox(width: 4),
              SizedBox(
                height: 20,
                child: Switch(
                  value: autoScroll,
                  onChanged: onAutoScrollChanged,
                  activeTrackColor: C.neonCyan.withValues(alpha: 0.5),
                  activeThumbColor: C.neonCyan,
                  materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
                ),
              ),
            ],
          ),
        ),
        // Log lines
        Expanded(
          child: Container(
            decoration: BoxDecoration(
              color: C.bg,
              borderRadius: const BorderRadius.vertical(bottom: Radius.circular(14)),
              border: Border.all(color: C.border),
            ),
            child: ClipRRect(
              borderRadius: const BorderRadius.vertical(bottom: Radius.circular(14)),
              child: ListView.builder(
                controller: logScrollController,
                padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
                itemCount: logs.length,
                itemBuilder: (BuildContext context, int i) {
                  final HunterLogLine line = logs[i];
                  final Color srcColor = switch (line.source) {
                    'err' => C.neonRed,
                    'ui' => C.txt3.withValues(alpha: 0.6),
                    _ when line.message.contains('working') || line.message.contains('Gold') || line.message.contains('alive') => C.neonGreen,
                    _ when line.message.contains('Error') || line.message.contains('error') || line.message.contains('FAIL') => C.neonRed,
                    _ when line.message.contains('Bench') || line.message.contains('Testing') || line.message.contains('Cycle') => C.neonAmber,
                    _ => C.txt3,
                  };
                  return Padding(
                    padding: const EdgeInsets.symmetric(vertical: 0.5),
                    child: Text(
                      '${fmtTs(line.ts)} ${line.message}',
                      style: TextStyle(
                        fontFamily: 'Consolas',
                        fontSize: 10,
                        height: 1.3,
                        color: srcColor,
                      ),
                    ),
                  );
                },
              ),
            ),
          ),
        ),
      ],
    );
  }

  Widget _miniBtn(IconData icon, String tooltip, VoidCallback onPressed) {
    return IconButton(
      icon: Icon(icon, size: 14, color: C.txt3),
      onPressed: onPressed,
      tooltip: tooltip,
      visualDensity: VisualDensity.compact,
      padding: EdgeInsets.zero,
      constraints: const BoxConstraints(minWidth: 28, minHeight: 28),
    );
  }
}
