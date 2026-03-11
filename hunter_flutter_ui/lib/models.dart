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
  HunterLatencyConfig({
    required this.uri,
    this.latencyMs,
    this.firstSeen,
    this.lastAlive,
    this.lastTested,
    this.totalTests,
    this.totalPasses,
    this.consecutiveFails,
    this.alive,
    this.tag,
  });
  final String uri;
  final double? latencyMs;
  final double? firstSeen;        // Unix timestamp when first discovered
  final double? lastAlive;        // Unix timestamp when last confirmed alive
  final double? lastTested;       // Unix timestamp of last health check
  final int? totalTests;          // How many times tested
  final int? totalPasses;         // How many times passed
  final int? consecutiveFails;    // Consecutive failures
  final bool? alive;              // Current health status
  final String? tag;              // Source tag (telegram, http, github_bg)
}
