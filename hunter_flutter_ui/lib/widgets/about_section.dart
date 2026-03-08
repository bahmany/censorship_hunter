import 'package:flutter/material.dart';
import '../theme.dart';

class AboutSection extends StatelessWidget {
  const AboutSection({super.key, required this.bundledConfigsCount});
  final int bundledConfigsCount;

  @override
  Widget build(BuildContext context) {
    return SingleChildScrollView(
      padding: const EdgeInsets.all(40),
      child: Center(
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 500),
          child: Column(
            children: <Widget>[
              const SizedBox(height: 24),
              // Logo
              Container(
                width: 80, height: 80,
                decoration: BoxDecoration(
                  shape: BoxShape.circle,
                  gradient: LinearGradient(
                    colors: <Color>[C.neonCyan.withValues(alpha: 0.2), C.neonGreen.withValues(alpha: 0.2)],
                    begin: Alignment.topLeft,
                    end: Alignment.bottomRight,
                  ),
                  border: Border.all(color: C.neonCyan.withValues(alpha: 0.3), width: 2),
                ),
                child: const Icon(Icons.shield, size: 40, color: C.neonCyan),
              ),
              const SizedBox(height: 20),
              const Text('HUNTER', style: TextStyle(
                color: C.neonCyan,
                fontSize: 28,
                fontWeight: FontWeight.w900,
                letterSpacing: 6,
                fontFamily: 'Consolas',
              )),
              const SizedBox(height: 6),
              const Text('Anti-Censorship Config Discovery', style: TextStyle(color: C.txt3, fontSize: 12, letterSpacing: 1)),
              const SizedBox(height: 32),
              _infoCard(Icons.person_outline, 'Developer', 'bahmanymb@gmail.com'),
              const SizedBox(height: 10),
              _infoCard(Icons.code, 'Repository', 'github.com/bahmany/censorship_hunter'),
              const SizedBox(height: 10),
              _infoCard(Icons.storage_outlined, 'Bundled Seeds', '$bundledConfigsCount configs'),
              const SizedBox(height: 10),
              _infoCard(Icons.speed, 'Engines', 'XRay, Mihomo, Sing-box, Tor'),
              const SizedBox(height: 32),
              Text(
                'Fighting internet censorship in Iran',
                style: TextStyle(color: C.txt3.withValues(alpha: 0.6), fontSize: 11, fontStyle: FontStyle.italic),
              ),
            ],
          ),
        ),
      ),
    );
  }

  Widget _infoCard(IconData icon, String label, String value) {
    return Container(
      width: double.infinity,
      padding: const EdgeInsets.all(14),
      decoration: BoxDecoration(
        color: C.card,
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: C.border),
      ),
      child: Row(
        children: <Widget>[
          Icon(icon, color: C.neonCyan, size: 18),
          const SizedBox(width: 12),
          Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: <Widget>[
              Text(label, style: const TextStyle(color: C.txt3, fontSize: 10, fontWeight: FontWeight.w600)),
              const SizedBox(height: 2),
              SelectableText(value, style: const TextStyle(color: C.txt1, fontSize: 12)),
            ],
          ),
        ],
      ),
    );
  }
}
