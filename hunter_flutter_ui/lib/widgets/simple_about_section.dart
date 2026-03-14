import 'package:flutter/material.dart';
import '../theme.dart';
import 'about_section.dart';

class SimpleAboutSection extends StatelessWidget {
  const SimpleAboutSection({
    super.key,
    required this.bundledConfigsCount,
  });

  final int bundledConfigsCount;

  @override
  Widget build(BuildContext context) {
    return Center(
      child: SingleChildScrollView(
        padding: const EdgeInsets.all(24),
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 760),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: <Widget>[
              Container(
                width: double.infinity,
                padding: const EdgeInsets.all(24),
                decoration: BoxDecoration(
                  color: C.card,
                  borderRadius: BorderRadius.circular(24),
                  border: Border.all(color: C.border),
                ),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: <Widget>[
                    Row(
                      children: <Widget>[
                        Container(
                          width: 58,
                          height: 58,
                          decoration: BoxDecoration(
                            shape: BoxShape.circle,
                            color: C.neonCyan.withValues(alpha: 0.12),
                            border: Border.all(color: C.neonCyan.withValues(alpha: 0.3)),
                          ),
                          child: const Icon(Icons.shield_rounded, color: C.neonCyan, size: 30),
                        ),
                        const SizedBox(width: 16),
                        const Expanded(
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: <Widget>[
                              Text(
                                'HUNTER',
                                style: TextStyle(
                                  color: C.txt1,
                                  fontSize: 28,
                                  fontWeight: FontWeight.w900,
                                ),
                              ),
                              SizedBox(height: 4),
                              Text(
                                'Desktop discovery dashboard for fresh V2Ray configs in restricted Iranian networks',
                                style: TextStyle(
                                  color: C.txt2,
                                  fontSize: 14,
                                  height: 1.6,
                                ),
                              ),
                            ],
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(height: 24),
                    Wrap(
                      spacing: 12,
                      runSpacing: 12,
                      children: <Widget>[
                        _MetaCard(title: 'Dashboard Version', value: 'v$kDashboardVersion'),
                        _MetaCard(title: 'CLI Version', value: 'v$kCliVersion'),
                        _MetaCard(title: 'Bundled Seeds', value: '$bundledConfigsCount'),
                      ],
                    ),
                    const SizedBox(height: 18),
                    const Text(
                      'Hunter is built to discover, validate, and export fresh V2Ray configs that still survive difficult network conditions in Iran.',
                      style: TextStyle(
                        color: C.txt1,
                        fontSize: 14,
                        height: 1.8,
                      ),
                    ),
                    const SizedBox(height: 16),
                    Container(
                      width: double.infinity,
                      padding: const EdgeInsets.all(16),
                      decoration: BoxDecoration(
                        color: C.surface,
                        borderRadius: BorderRadius.circular(18),
                        border: Border.all(color: C.border),
                      ),
                      child: const Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: <Widget>[
                          Text(
                            'Repository',
                            style: TextStyle(
                              color: C.txt2,
                              fontSize: 12,
                              fontWeight: FontWeight.w700,
                            ),
                          ),
                          SizedBox(height: 6),
                          SelectableText(
                            'github.com/bahmany/censorship_hunter',
                            style: TextStyle(
                              color: C.neonCyan,
                              fontSize: 14,
                              fontWeight: FontWeight.w600,
                            ),
                          ),
                        ],
                      ),
                    ),
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _MetaCard extends StatelessWidget {
  const _MetaCard({
    required this.title,
    required this.value,
  });

  final String title;
  final String value;

  @override
  Widget build(BuildContext context) {
    return Container(
      constraints: const BoxConstraints(minWidth: 180),
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
      decoration: BoxDecoration(
        color: C.surface,
        borderRadius: BorderRadius.circular(18),
        border: Border.all(color: C.border),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Text(
            title,
            style: const TextStyle(
              color: C.txt3,
              fontSize: 11,
              fontWeight: FontWeight.w700,
            ),
          ),
          const SizedBox(height: 6),
          Text(
            value,
            style: const TextStyle(
              color: C.txt1,
              fontSize: 18,
              fontWeight: FontWeight.w800,
            ),
          ),
        ],
      ),
    );
  }
}
