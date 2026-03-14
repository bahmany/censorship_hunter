import 'package:flutter/material.dart';
import '../services.dart';
import '../theme.dart';

class SimpleConfigsSection extends StatelessWidget {
  const SimpleConfigsSection({
    super.key,
    required this.searchController,
    required this.liveConfigs,
    required this.lastRefresh,
    required this.onSearchChanged,
    required this.onCopyText,
    required this.onCopyLines,
  });

  final TextEditingController searchController;
  final List<String> liveConfigs;
  final DateTime? lastRefresh;
  final VoidCallback onSearchChanged;
  final Future<void> Function(String text, {String? label}) onCopyText;
  final Future<void> Function(List<String> lines, {String? label}) onCopyLines;

  @override
  Widget build(BuildContext context) {
    final String q = searchController.text.trim().toLowerCase();
    final List<String> configs = q.isEmpty
        ? liveConfigs
        : liveConfigs.where((String item) => item.toLowerCase().contains(q)).toList(growable: false);

    return Padding(
      padding: const EdgeInsets.all(20),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Container(
            width: double.infinity,
            padding: const EdgeInsets.all(18),
            decoration: BoxDecoration(
              color: C.card,
              borderRadius: BorderRadius.circular(20),
              border: Border.all(color: C.border),
            ),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                const Text(
                  'Fresh V2Ray Configs',
                  style: TextStyle(
                    color: C.txt1,
                    fontSize: 24,
                    fontWeight: FontWeight.w800,
                  ),
                ),
                const SizedBox(height: 8),
                Text(
                  configs.isEmpty
                      ? 'No fresh live configs discovered yet.'
                      : 'Configs that survived the latest checks for restricted Iranian networks are listed here.',
                  style: const TextStyle(
                    color: C.txt2,
                    fontSize: 13,
                    height: 1.6,
                  ),
                ),
                const SizedBox(height: 14),
                Row(
                  children: <Widget>[
                    Expanded(
                      child: TextField(
                        controller: searchController,
                        onChanged: (_) => onSearchChanged(),
                        decoration: InputDecoration(
                          hintText: 'Search configs...',
                          hintStyle: const TextStyle(color: C.txt3),
                          prefixIcon: const Icon(Icons.search_rounded, color: C.txt3),
                          suffixIcon: searchController.text.isNotEmpty
                              ? IconButton(
                                  onPressed: () {
                                    searchController.clear();
                                    onSearchChanged();
                                  },
                                  icon: const Icon(Icons.close_rounded, color: C.txt3),
                                )
                              : null,
                          filled: true,
                          fillColor: C.surface,
                          border: OutlineInputBorder(
                            borderRadius: BorderRadius.circular(14),
                            borderSide: const BorderSide(color: C.border),
                          ),
                          enabledBorder: OutlineInputBorder(
                            borderRadius: BorderRadius.circular(14),
                            borderSide: const BorderSide(color: C.border),
                          ),
                          focusedBorder: OutlineInputBorder(
                            borderRadius: BorderRadius.circular(14),
                            borderSide: const BorderSide(color: C.neonCyan),
                          ),
                        ),
                      ),
                    ),
                    const SizedBox(width: 12),
                    FilledButton.icon(
                      onPressed: configs.isEmpty ? null : () => onCopyLines(configs, label: 'All configs copied'),
                      style: FilledButton.styleFrom(
                        backgroundColor: C.neonCyan,
                        foregroundColor: C.bg,
                        padding: const EdgeInsets.symmetric(horizontal: 18, vertical: 18),
                        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(14)),
                      ),
                      icon: const Icon(Icons.copy_rounded),
                      label: Text('Copy All (${configs.length})'),
                    ),
                  ],
                ),
                if (lastRefresh != null) ...<Widget>[
                  const SizedBox(height: 10),
                  Text(
                    'Last refresh: ${fmtTs(lastRefresh!)}',
                    style: const TextStyle(
                      color: C.txt3,
                      fontSize: 11,
                    ),
                  ),
                ],
              ],
            ),
          ),
          const SizedBox(height: 16),
          Expanded(
            child: configs.isEmpty
                ? Container(
                    width: double.infinity,
                    decoration: BoxDecoration(
                      color: C.surface,
                      borderRadius: BorderRadius.circular(20),
                      border: Border.all(color: C.border),
                    ),
                    child: const Center(
                      child: Padding(
                        padding: EdgeInsets.all(24),
                        child: Text(
                          'No configs to display yet.',
                          textAlign: TextAlign.center,
                          style: TextStyle(
                            color: C.txt2,
                            fontSize: 15,
                          ),
                        ),
                      ),
                    ),
                  )
                : Container(
                    decoration: BoxDecoration(
                      color: C.surface,
                      borderRadius: BorderRadius.circular(20),
                      border: Border.all(color: C.border),
                    ),
                    child: ListView.separated(
                      padding: const EdgeInsets.all(14),
                      itemCount: configs.length,
                      separatorBuilder: (BuildContext context, int index) => const SizedBox(height: 10),
                      itemBuilder: (BuildContext context, int index) {
                        final String uri = configs[index];
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
                              IconButton(
                                onPressed: () => onCopyText(uri, label: 'Config copied'),
                                icon: const Icon(Icons.copy_rounded, color: C.neonCyan),
                                tooltip: 'Copy',
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
    );
  }
}
