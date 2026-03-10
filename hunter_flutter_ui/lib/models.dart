enum HunterRunState { stopped, starting, running, stopping }
enum HunterNavSection { dashboard, configs, logs, docs, advanced, about }
enum HunterConfigListKind { alive, silver, balancer, gemini, allCache, githubCache }

class HunterLogLine {
  HunterLogLine({required this.ts, required this.source, required this.message});
  final DateTime ts;
  final String source;
  final String message;
}

class HunterLatencyConfig {
  HunterLatencyConfig({required this.uri, this.latencyMs, this.firstSeen, this.lastAlive, this.totalTests});
  final String uri;
  final double? latencyMs;
  final double? firstSeen;   // Unix timestamp when first discovered
  final double? lastAlive;   // Unix timestamp when last confirmed alive
  final int? totalTests;     // How many times tested
}
