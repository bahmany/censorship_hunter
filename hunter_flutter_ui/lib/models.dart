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
  HunterLatencyConfig({required this.uri, this.latencyMs});
  final String uri;
  final double? latencyMs;
}
