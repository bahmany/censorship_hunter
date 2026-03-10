import 'dart:io';
import 'package:flutter/material.dart';
import '../theme.dart';
import '../services.dart';

class AdvancedSection extends StatelessWidget {
  const AdvancedSection({
    super.key,
    required this.cliPathController,
    required this.configPathController,
    required this.xrayPathController,
    required this.tgEnabled,
    required this.tgApiIdController,
    required this.tgApiHashController,
    required this.tgPhoneController,
    required this.tgTargetsController,
    required this.tgLimitController,
    required this.tgTimeoutMsController,
    required this.workingDir,
    required this.processInfo,
    required this.engines,
    required this.onTgEnabledChanged,
    required this.onSaveTelegram,
    required this.onRefresh,
    required this.onCopyText,
    required this.githubUrlsController,
    required this.onSaveGithubUrls,
    required this.onResetGithubUrls,
  });

  final TextEditingController cliPathController;
  final TextEditingController configPathController;
  final TextEditingController xrayPathController;
  final bool tgEnabled;
  final TextEditingController tgApiIdController;
  final TextEditingController tgApiHashController;
  final TextEditingController tgPhoneController;
  final TextEditingController tgTargetsController;
  final TextEditingController tgLimitController;
  final TextEditingController tgTimeoutMsController;
  final Directory workingDir;
  final String processInfo;
  final Map<String, int> engines;
  final ValueChanged<bool> onTgEnabledChanged;
  final VoidCallback onSaveTelegram;
  final VoidCallback onRefresh;
  final Future<void> Function(String text, {String? label}) onCopyText;
  final TextEditingController githubUrlsController;
  final VoidCallback onSaveGithubUrls;
  final VoidCallback onResetGithubUrls;

  @override
  Widget build(BuildContext context) {
    return SingleChildScrollView(
      padding: const EdgeInsets.all(20),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          // ── Paths ──
          _sectionHeader('PATHS'),
          const SizedBox(height: 10),
          _cardWrap(
            child: Column(
              children: <Widget>[
                Row(
                  children: <Widget>[
                    Expanded(child: _field('CLI Executable', cliPathController, 'bin\\hunter_cli.exe')),
                    const SizedBox(width: 12),
                    Expanded(child: _field('Config File', configPathController, 'runtime\\hunter_config.json')),
                    const SizedBox(width: 12),
                    Expanded(child: _field('XRay Path', xrayPathController, 'bin\\xray.exe')),
                  ],
                ),
                const SizedBox(height: 12),
                Row(
                  children: <Widget>[
                    _infoPill('Working dir', workingDir.path),
                    const SizedBox(width: 8),
                    _infoPill('Process', processInfo),
                    const Spacer(),
                    TextButton.icon(
                      onPressed: onRefresh,
                      icon: const Icon(Icons.refresh, size: 14),
                      label: const Text('Refresh', style: TextStyle(fontSize: 11)),
                      style: TextButton.styleFrom(foregroundColor: C.neonCyan),
                    ),
                  ],
                ),
              ],
            ),
          ),
          const SizedBox(height: 24),

          // ── Engines ──
          _sectionHeader('ENGINES'),
          const SizedBox(height: 10),
          _cardWrap(
            child: Column(
              children: kEngines.map((EngineInfo e) {
                final bool found = engines.containsKey(e.name);
                final String fullPath = '${workingDir.path}\\${e.relPath}';
                return Padding(
                  padding: const EdgeInsets.symmetric(vertical: 4),
                  child: Row(
                    children: <Widget>[
                      Icon(found ? Icons.check_circle : Icons.cancel,
                        color: found ? C.neonGreen : C.neonRed.withValues(alpha: 0.5), size: 16),
                      const SizedBox(width: 10),
                      Text(e.name, style: TextStyle(color: found ? C.txt1 : C.txt3, fontSize: 12, fontWeight: FontWeight.w600)),
                      const SizedBox(width: 12),
                      Expanded(
                        child: Text(e.relPath, style: const TextStyle(color: C.txt3, fontSize: 10, fontFamily: 'Consolas')),
                      ),
                      if (found)
                        Text('${(engines[e.name]! / 1024 / 1024).toStringAsFixed(1)} MB',
                          style: const TextStyle(color: C.txt3, fontSize: 10, fontFamily: 'Consolas')),
                      const SizedBox(width: 8),
                      IconButton(
                        icon: const Icon(Icons.content_copy, size: 14, color: C.txt3),
                        onPressed: () => onCopyText(fullPath, label: 'Copied ${e.name} path'),
                        tooltip: 'Copy path',
                        visualDensity: VisualDensity.compact,
                        padding: EdgeInsets.zero,
                        constraints: const BoxConstraints(minWidth: 24, minHeight: 24),
                      ),
                    ],
                  ),
                );
              }).toList(),
            ),
          ),
          const SizedBox(height: 24),

          // ── GitHub Config Sources ──
          _sectionHeader('GITHUB CONFIG SOURCES'),
          const SizedBox(height: 10),
          _cardWrap(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Row(
                  children: <Widget>[
                    const Icon(Icons.link, color: C.neonCyan, size: 16),
                    const SizedBox(width: 8),
                    const Text('Subscription URLs', style: TextStyle(color: C.txt2, fontSize: 12, fontWeight: FontWeight.w600)),
                    const Spacer(),
                    TextButton.icon(
                      onPressed: onResetGithubUrls,
                      icon: const Icon(Icons.restore, size: 14),
                      label: const Text('Defaults', style: TextStyle(fontSize: 11)),
                      style: TextButton.styleFrom(foregroundColor: C.neonAmber),
                    ),
                    const SizedBox(width: 6),
                    TextButton.icon(
                      onPressed: onSaveGithubUrls,
                      icon: const Icon(Icons.save, size: 14),
                      label: const Text('Save', style: TextStyle(fontSize: 11)),
                      style: TextButton.styleFrom(
                        foregroundColor: C.neonCyan,
                        backgroundColor: C.neonCyan.withValues(alpha: 0.08),
                        shape: RoundedRectangleBorder(
                          borderRadius: BorderRadius.circular(8),
                          side: BorderSide(color: C.neonCyan.withValues(alpha: 0.3)),
                        ),
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 8),
                const Text(
                  'One URL per line. These are fetched periodically for proxy configs.\n'
                  'Supports raw GitHub links, subscription URLs, and base64 encoded feeds.',
                  style: TextStyle(color: C.txt3, fontSize: 10),
                ),
                const SizedBox(height: 8),
                TextField(
                  controller: githubUrlsController,
                  minLines: 6,
                  maxLines: 15,
                  style: const TextStyle(fontSize: 10, fontFamily: 'Consolas', color: C.txt1),
                  decoration: _inputDeco('URLs', 'https://raw.githubusercontent.com/...'),
                ),
              ],
            ),
          ),
          const SizedBox(height: 24),

          // ── Telegram ──
          _sectionHeader('TELEGRAM SCRAPE'),
          const SizedBox(height: 10),
          _cardWrap(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Row(
                  children: <Widget>[
                    SizedBox(
                      height: 24,
                      child: Switch(
                        value: tgEnabled,
                        onChanged: onTgEnabledChanged,
                        activeTrackColor: C.neonGreen.withValues(alpha: 0.5),
                        activeThumbColor: C.neonGreen,
                        materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
                      ),
                    ),
                    const SizedBox(width: 8),
                    Text(
                      tgEnabled ? 'Enabled' : 'Disabled',
                      style: TextStyle(color: tgEnabled ? C.neonGreen : C.txt3, fontSize: 12, fontWeight: FontWeight.w600),
                    ),
                    const Spacer(),
                    TextButton.icon(
                      onPressed: onSaveTelegram,
                      icon: const Icon(Icons.save, size: 14),
                      label: const Text('Save', style: TextStyle(fontSize: 11)),
                      style: TextButton.styleFrom(
                        foregroundColor: C.neonCyan,
                        backgroundColor: C.neonCyan.withValues(alpha: 0.08),
                        shape: RoundedRectangleBorder(
                          borderRadius: BorderRadius.circular(8),
                          side: BorderSide(color: C.neonCyan.withValues(alpha: 0.3)),
                        ),
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 12),
                Row(
                  children: <Widget>[
                    Expanded(child: _field('API ID', tgApiIdController, 'from my.telegram.org')),
                    const SizedBox(width: 10),
                    Expanded(child: _field('API Hash', tgApiHashController, 'from my.telegram.org')),
                    const SizedBox(width: 10),
                    Expanded(child: _field('Phone', tgPhoneController, '+98...')),
                  ],
                ),
                const SizedBox(height: 10),
                Row(
                  children: <Widget>[
                    Expanded(child: _field('Limit', tgLimitController, '50')),
                    const SizedBox(width: 10),
                    Expanded(child: _field('Timeout (ms)', tgTimeoutMsController, '12000')),
                    const SizedBox(width: 10),
                    Expanded(
                      flex: 2,
                      child: TextField(
                        controller: tgTargetsController,
                        minLines: 2,
                        maxLines: 5,
                        style: const TextStyle(fontSize: 11, fontFamily: 'Consolas', color: C.txt1),
                        decoration: _inputDeco('Targets (channels)', 'One per line'),
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 10),
                ExpansionTile(
                  title: const Text('How to activate Telegram scraping', style: TextStyle(color: C.txt2, fontSize: 12)),
                  tilePadding: EdgeInsets.zero,
                  childrenPadding: const EdgeInsets.only(bottom: 8),
                  iconColor: C.txt3,
                  collapsedIconColor: C.txt3,
                  children: <Widget>[
                    Text(
                      '1) Go to https://my.telegram.org and login with your phone.\n'
                      '2) Open API development tools and create an application.\n'
                      '3) Copy API ID and API Hash here.\n'
                      '4) Enter your phone in international format (+98...).\n'
                      '5) Add channel usernames in Targets (without @).\n'
                      '6) Click Save. Then Start Hunter.',
                      style: const TextStyle(color: C.txt3, fontSize: 11),
                    ),
                  ],
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _sectionHeader(String text) {
    return Text(text, style: const TextStyle(color: C.txt3, fontSize: 10, fontWeight: FontWeight.w600, letterSpacing: 1.5));
  }

  Widget _cardWrap({required Widget child}) {
    return Container(
      width: double.infinity,
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: C.card,
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: C.border),
      ),
      child: child,
    );
  }

  Widget _field(String label, TextEditingController controller, String hint) {
    return TextField(
      controller: controller,
      style: const TextStyle(fontSize: 11, fontFamily: 'Consolas', color: C.txt1),
      decoration: _inputDeco(label, hint),
    );
  }

  InputDecoration _inputDeco(String label, String hint) {
    return InputDecoration(
      labelText: label,
      labelStyle: const TextStyle(color: C.txt3, fontSize: 11),
      hintText: hint,
      hintStyle: TextStyle(color: C.txt3.withValues(alpha: 0.5), fontSize: 10),
      isDense: true,
      contentPadding: const EdgeInsets.symmetric(horizontal: 10, vertical: 10),
      filled: true,
      fillColor: C.surface,
      border: OutlineInputBorder(borderRadius: BorderRadius.circular(8), borderSide: const BorderSide(color: C.border)),
      enabledBorder: OutlineInputBorder(borderRadius: BorderRadius.circular(8), borderSide: const BorderSide(color: C.border)),
      focusedBorder: OutlineInputBorder(borderRadius: BorderRadius.circular(8), borderSide: const BorderSide(color: C.neonCyan)),
    );
  }

  Widget _infoPill(String label, String value) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
      decoration: BoxDecoration(
        color: C.surface,
        borderRadius: BorderRadius.circular(6),
        border: Border.all(color: C.border),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: <Widget>[
          Text('$label: ', style: const TextStyle(color: C.txt3, fontSize: 10)),
          Flexible(child: Text(value, style: const TextStyle(color: C.txt2, fontSize: 10, fontFamily: 'Consolas'), overflow: TextOverflow.ellipsis)),
        ],
      ),
    );
  }
}
