import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'package:flutter/services.dart';

import 'models.dart';

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Binary paths for engines
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
class EngineInfo {
  EngineInfo(this.name, this.relPath);
  final String name;
  final String relPath;
}

final List<EngineInfo> kEngines = <EngineInfo>[
  EngineInfo('XRay', 'bin\\xray.exe'),
  EngineInfo('Mihomo', 'bin\\mihomo-windows-amd64-compatible.exe'),
  EngineInfo('Sing-box', 'bin\\sing-box.exe'),
  EngineInfo('Tor', 'bin\\tor.exe'),
];

const String kSpeedTestUrl = 'http://speed.cloudflare.com/__down?bytes=10000000';
const String kSpeedTestUrlSmall = 'http://speed.cloudflare.com/__down?bytes=1000000';

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// File helpers
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Future<List<String>> readConfigLines(String absPath) async {
  final File f = File(absPath);
  if (!f.existsSync()) return const <String>[];
  try {
    final String data = await f.readAsString();
    final List<String> lines = data.split(RegExp(r'\r?\n'));
    final List<String> out = <String>[];
    final Set<String> seen = <String>{};
    for (final String raw in lines) {
      final String line = raw.trim();
      if (line.isEmpty) continue;
      if (seen.add(line)) out.add(line);
    }
    return out;
  } catch (_) {
    return const <String>[];
  }
}

Future<List<HunterLatencyConfig>> readLatencyConfigsFromJson(String absPath) async {
  final File f = File(absPath);
  if (!f.existsSync()) return const <HunterLatencyConfig>[];
  try {
    final dynamic decoded = jsonDecode(await f.readAsString());
    if (decoded is! Map<String, dynamic>) return const <HunterLatencyConfig>[];
    final dynamic configs = decoded['configs'];
    if (configs is! List) return const <HunterLatencyConfig>[];
    final List<HunterLatencyConfig> out = <HunterLatencyConfig>[];
    for (final dynamic row in configs) {
      if (row is! Map<String, dynamic>) continue;
      final dynamic uri = row['uri'];
      if (uri is! String || uri.trim().isEmpty) continue;
      final dynamic latencyRaw = row['latency_ms'];
      final double? latency = switch (latencyRaw) {
        num v => v.toDouble(),
        String s => double.tryParse(s),
        _ => null,
      };
      out.add(HunterLatencyConfig(uri: uri.trim(), latencyMs: latency));
    }
    return out;
  } catch (_) {
    return const <HunterLatencyConfig>[];
  }
}

Future<int> loadBundledConfigsCount() async {
  int count = 0;
  final List<String> bundledFiles = <String>[
    'assets/configs/All_Configs_Sub.txt',
    'assets/configs/all_extracted_configs.txt',
    'assets/configs/sub.txt',
  ];
  for (final String assetPath in bundledFiles) {
    try {
      final String content = await rootBundle.loadString(assetPath);
      final List<String> lines = content.split('\n');
      count += lines.where((String l) => l.trim().isNotEmpty && !l.trim().startsWith('#')).length;
    } catch (_) {}
  }
  return count;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Working dir resolution
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Iterable<Directory> _startDirs() sync* {
  yield Directory.current;
  try {
    yield File(Platform.resolvedExecutable).parent;
  } catch (_) {}
}

Directory resolveWorkingDirForCli(String cliInput) {
  final String trimmed = cliInput.trim();
  if (trimmed.isEmpty) return Directory.current;
  if (trimmed.contains(':\\') || trimmed.startsWith('\\\\') || trimmed.startsWith('/')) {
    try {
      return File(trimmed).parent;
    } catch (_) {
      return Directory.current;
    }
  }
  final Set<String> visited = <String>{};
  for (final Directory start in _startDirs()) {
    Directory cur = start;
    while (true) {
      final String key = cur.path.toLowerCase();
      if (!visited.add(key)) break;
      final String candidate = '${cur.path}\\$trimmed';
      if (File(candidate).existsSync()) return cur;
      if (cur.parent.path == cur.path) break;
      cur = cur.parent;
    }
  }
  return Directory.current;
}

String absPathInWorkingDir(Directory wd, String relOrAbs) {
  final String trimmed = relOrAbs.trim();
  if (trimmed.isEmpty) return '';
  if (trimmed.contains(':\\') || trimmed.startsWith('\\\\') || trimmed.startsWith('/')) {
    return trimmed;
  }
  return '${wd.path}\\$trimmed';
}

class LogTailResult {
  const LogTailResult({required this.nextOffset, required this.lines});
  final int nextOffset;
  final List<String> lines;
}

String stripAnsi(String input) {
  return input.replaceAll(RegExp(r'\x1B\[[0-9;?]*[ -/]*[@-~]'), '');
}

Future<LogTailResult> readNewLogLines(String absPath, int offset, {int maxBytes = 64 * 1024}) async {
  final File f = File(absPath);
  if (!f.existsSync()) {
    return const LogTailResult(nextOffset: 0, lines: <String>[]);
  }
  RandomAccessFile? raf;
  try {
    final int fileLength = await f.length();
    int start = offset;
    if (start < 0 || start > fileLength) start = 0;
    if (fileLength - start > maxBytes) {
      start = fileLength - maxBytes;
    }
    raf = await f.open();
    await raf.setPosition(start);
    final List<int> bytes = await raf.read(fileLength - start);
    String text = utf8.decode(bytes, allowMalformed: true);
    if (start > 0 && text.isNotEmpty && !text.startsWith('\n') && !text.startsWith('\r')) {
      final int nl = text.indexOf('\n');
      text = nl >= 0 ? text.substring(nl + 1) : '';
    }
    final List<String> lines = text
        .split(RegExp(r'\r?\n'))
        .map((String line) => stripAnsi(line).trimRight())
        .where((String line) => line.trim().isNotEmpty)
        .toList(growable: false);
    return LogTailResult(nextOffset: fileLength, lines: lines);
  } catch (_) {
    return const LogTailResult(nextOffset: 0, lines: <String>[]);
  } finally {
    await raf?.close();
  }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Runtime snapshot (what the CLI writes to disk)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
class RuntimeSnapshot {
  List<String> allCache = const <String>[];
  List<String> githubCache = const <String>[];
  List<String> silverConfigs = const <String>[];
  List<String> goldConfigs = const <String>[];
  List<HunterLatencyConfig> balancerConfigs = const <HunterLatencyConfig>[];
  List<HunterLatencyConfig> geminiConfigs = const <HunterLatencyConfig>[];
  Map<String, dynamic>? status;
  List<Map<String, dynamic>> history = const <Map<String, dynamic>>[];
}

Future<RuntimeSnapshot> loadRuntimeSnapshot(String runtimeDir) async {
  final RuntimeSnapshot s = RuntimeSnapshot();
  final results = await Future.wait(<Future<dynamic>>[
    readConfigLines('$runtimeDir\\HUNTER_all_cache.txt'),
    readConfigLines('$runtimeDir\\HUNTER_github_configs_cache.txt'),
    readConfigLines('$runtimeDir\\HUNTER_silver.txt'),
    readConfigLines('$runtimeDir\\HUNTER_gold.txt'),
    readLatencyConfigsFromJson('$runtimeDir\\HUNTER_balancer_cache.json'),
    readLatencyConfigsFromJson('$runtimeDir\\HUNTER_gemini_balancer_cache.json'),
  ]);
  s.allCache = results[0] as List<String>;
  s.githubCache = results[1] as List<String>;
  s.silverConfigs = results[2] as List<String>;
  s.goldConfigs = results[3] as List<String>;
  s.balancerConfigs = results[4] as List<HunterLatencyConfig>;
  s.geminiConfigs = results[5] as List<HunterLatencyConfig>;

  try {
    final File statusFile = File('$runtimeDir\\HUNTER_status.json');
    if (statusFile.existsSync()) {
      final dynamic decoded = jsonDecode(await statusFile.readAsString());
      if (decoded is Map<String, dynamic>) {
        s.status = decoded;
        final dynamic h = decoded['history'];
        if (h is List) {
          s.history = h.whereType<Map<String, dynamic>>().toList();
        }
      }
    }
  } catch (_) {}
  return s;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Speed test — v2rayNG-style comprehensive test via SOCKS proxy
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// Multi-tier test URLs (same as C++ CLI)
const List<String> kTestUrls = <String>[
  'http://cp.cloudflare.com/generate_204',
  'http://1.1.1.1/generate_204',
  'http://www.gstatic.com/generate_204',
];
// Telegram DC IPs for connectivity fallback
const List<String> kTelegramDCs = <String>[
  '149.154.175.50', '149.154.167.51', '149.154.175.100',
  '149.154.167.91', '91.108.56.130',
];

class SpeedTestResult {
  SpeedTestResult({this.speedMbps, this.latencyMs, this.downloadBytes, this.httpOk = false, this.telegramOk = false, this.error});
  final double? speedMbps;
  final int? latencyMs;
  final int? downloadBytes;
  final bool httpOk;
  final bool telegramOk;
  final String? error;
  bool get isAlive => httpOk;
}

/// Comprehensive v2rayNG-style test: HTTP download + Telegram DC connectivity
/// Tests through an already-running SOCKS proxy (balancer port 10808 or individual config port)
Future<SpeedTestResult> runSpeedTest({
  required String workingDir,
  int socksPort = 10808,
  int timeoutSec = 15,
}) async {
  // ── Tier 1: HTTP 204 tests (fast connectivity check) ──
  for (final String url in kTestUrls) {
    final _CurlResult cr = await _curlViaSocks(socksPort, url, workingDir, timeoutSec: 10);
    if (cr.exitCode == 0 && cr.httpCode >= 200 && cr.httpCode < 400) {
      return SpeedTestResult(
        latencyMs: (cr.timeTotal * 1000).round(),
        httpOk: true,
        telegramOk: true,
      );
    }
  }

  // ── Tier 2: HTTPS download test (measures actual speed) ──
  final _CurlResult dlResult = await _curlViaSocks(socksPort, kSpeedTestUrlSmall, workingDir, timeoutSec: timeoutSec);
  if (dlResult.exitCode == 0 && dlResult.bytesDownloaded > 0) {
    return SpeedTestResult(
      speedMbps: dlResult.speedBps * 8 / 1000000,
      latencyMs: (dlResult.timeTotal * 1000).round(),
      downloadBytes: dlResult.bytesDownloaded,
      httpOk: true,
      telegramOk: true,
    );
  }

  // ── Tier 3: Telegram DC TCP connectivity test ──
  final bool tgOk = await _testTelegramDC(socksPort);
  if (tgOk) {
    return SpeedTestResult(
      telegramOk: true,
      httpOk: false,
      error: 'Telegram-only (HTTP download failed)',
    );
  }

  return SpeedTestResult(error: 'All tests failed');
}

/// Run a single config through xray SOCKS and test comprehensively
Future<SpeedTestResult> runConfigSpeedTest({
  required String xrayPath,
  required String configUri,
  required String workingDir,
  int timeoutSec = 15,
}) async {
  // Parse the config URI and generate XRay JSON using the C++ CLI's test approach
  // We write the URI to a temp file and use xray with a generated config
  final int testPort = 21199 + (DateTime.now().millisecondsSinceEpoch % 800);
  final String configFile = '$workingDir\\runtime\\temp_speedtest_$testPort.json';

  // Generate minimal xray config - we use xray's built-in URI support via outbound
  final Map<String, dynamic> xrayConfig = _buildXrayConfigFromUri(configUri, testPort);
  if (xrayConfig.isEmpty) {
    return SpeedTestResult(error: 'Unsupported config protocol');
  }

  try {
    await Directory('$workingDir\\runtime').create(recursive: true);
    await File(configFile).writeAsString(jsonEncode(xrayConfig));
  } catch (e) {
    return SpeedTestResult(error: 'Failed to write config: $e');
  }

  Process? xrayProcess;
  try {
    xrayProcess = await Process.start(xrayPath, <String>['-c', configFile],
        workingDirectory: workingDir);

    // Wait for xray to start listening
    bool portAlive = false;
    for (int i = 0; i < 12; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 300));
      try {
        final Socket sock = await Socket.connect('127.0.0.1', testPort,
            timeout: const Duration(milliseconds: 500));
        await sock.close();
        portAlive = true;
        break;
      } catch (_) {}
    }

    if (!portAlive) {
      return SpeedTestResult(error: 'XRay port not listening');
    }

    // Run comprehensive test through this proxy
    return await runSpeedTest(
      workingDir: workingDir,
      socksPort: testPort,
      timeoutSec: timeoutSec,
    );
  } catch (e) {
    return SpeedTestResult(error: '$e');
  } finally {
    xrayProcess?.kill();
    try { File(configFile).deleteSync(); } catch (_) {}
  }
}

/// Build XRay JSON config from a config URI
Map<String, dynamic> _buildXrayConfigFromUri(String uri, int socksPort) {
  final String proto = uri.contains('://') ? uri.split('://').first.toLowerCase() : '';
  if (!<String>['vless', 'vmess', 'trojan', 'ss', 'shadowsocks'].contains(proto)) {
    return const <String, dynamic>{};
  }

  // Parse URI components
  Map<String, dynamic>? outbound;
  try {
    if (proto == 'vmess') {
      outbound = _parseVmessUri(uri, socksPort);
    } else if (proto == 'vless') {
      outbound = _parseVlessUri(uri);
    } else if (proto == 'trojan') {
      outbound = _parseTrojanUri(uri);
    } else if (proto == 'ss' || proto == 'shadowsocks') {
      outbound = _parseShadowsocksUri(uri);
    }
  } catch (_) {
    return const <String, dynamic>{};
  }

  if (outbound == null) return const <String, dynamic>{};

  return <String, dynamic>{
    'log': <String, dynamic>{'loglevel': 'none'},
    'dns': <String, dynamic>{
      'servers': <dynamic>['1.1.1.1', '8.8.8.8'],
      'queryStrategy': 'UseIPv4',
    },
    'inbounds': <Map<String, dynamic>>[
      <String, dynamic>{
        'port': socksPort,
        'listen': '127.0.0.1',
        'protocol': 'socks',
        'settings': <String, dynamic>{'udp': true},
        'tag': 'socks-in',
      }
    ],
    'outbounds': <Map<String, dynamic>>[outbound],
  };
}

Map<String, dynamic>? _parseVmessUri(String uri, int socksPort) {
  // vmess:// is base64-encoded JSON
  final String b64 = uri.substring('vmess://'.length);
  final String decoded = utf8.decode(base64Decode(b64.padRight((b64.length + 3) & ~3, '=')));
  final Map<String, dynamic> cfg = jsonDecode(decoded) as Map<String, dynamic>;
  final String address = (cfg['add'] ?? '') as String;
  final int port = int.tryParse('${cfg['port']}') ?? 443;
  final String id = (cfg['id'] ?? '') as String;
  final String net = (cfg['net'] ?? 'tcp') as String;
  final String tls = (cfg['tls'] ?? '') as String;
  final String sni = (cfg['sni'] ?? cfg['host'] ?? '') as String;
  final String host = (cfg['host'] ?? '') as String;
  final String path = (cfg['path'] ?? '/') as String;
  final int aid = int.tryParse('${cfg['aid']}') ?? 0;

  final Map<String, dynamic> ob = <String, dynamic>{
    'protocol': 'vmess',
    'tag': 'proxy',
    'settings': <String, dynamic>{
      'vnext': <Map<String, dynamic>>[
        <String, dynamic>{
          'address': address,
          'port': port,
          'users': <Map<String, dynamic>>[
            <String, dynamic>{'id': id, 'alterId': aid, 'security': 'auto'},
          ],
        }
      ],
    },
  };

  // Stream settings
  final Map<String, dynamic> ss = <String, dynamic>{'network': net};
  if (tls == 'tls') {
    ss['security'] = 'tls';
    ss['tlsSettings'] = <String, dynamic>{'serverName': sni.isNotEmpty ? sni : address, 'allowInsecure': true};
  }
  if (net == 'ws') {
    ss['wsSettings'] = <String, dynamic>{'path': path, 'host': host.isNotEmpty ? host : address};
  } else if (net == 'grpc') {
    ss['grpcSettings'] = <String, dynamic>{'serviceName': path};
  }
  ob['streamSettings'] = ss;
  return ob;
}

Map<String, dynamic>? _parseVlessUri(String uri) {
  // vless://uuid@host:port?params#name
  final Uri parsed = Uri.parse(uri);
  final String id = parsed.userInfo;
  final String address = parsed.host;
  final int port = parsed.port;
  final Map<String, String> params = parsed.queryParameters;

  final String flow = params['flow'] ?? '';
  final String encryption = params['encryption'] ?? 'none';
  final String net = params['type'] ?? 'tcp';
  final String security = params['security'] ?? '';
  final String sni = params['sni'] ?? '';
  final String host = params['host'] ?? '';
  final String path = params['path'] ?? '/';
  final String fp = params['fp'] ?? '';
  final String pbk = params['pbk'] ?? '';
  final String sid = params['sid'] ?? '';

  final Map<String, dynamic> ob = <String, dynamic>{
    'protocol': 'vless',
    'tag': 'proxy',
    'settings': <String, dynamic>{
      'vnext': <Map<String, dynamic>>[
        <String, dynamic>{
          'address': address,
          'port': port,
          'users': <Map<String, dynamic>>[
            <String, dynamic>{'id': id, 'encryption': encryption, if (flow.isNotEmpty) 'flow': flow},
          ],
        }
      ],
    },
  };

  final Map<String, dynamic> ss = <String, dynamic>{'network': net};
  if (security == 'tls') {
    ss['security'] = 'tls';
    final Map<String, dynamic> tls = <String, dynamic>{
      'serverName': sni.isNotEmpty ? sni : address,
      'allowInsecure': true,
    };
    if (fp.isNotEmpty) tls['fingerprint'] = fp;
    ss['tlsSettings'] = tls;
  } else if (security == 'reality') {
    ss['security'] = 'reality';
    final Map<String, dynamic> rs = <String, dynamic>{
      'serverName': sni.isNotEmpty ? sni : address,
      'publicKey': pbk,
      'shortId': sid,
    };
    if (fp.isNotEmpty) rs['fingerprint'] = fp;
    ss['realitySettings'] = rs;
  }
  if (net == 'ws') {
    ss['wsSettings'] = <String, dynamic>{'path': path, 'host': host.isNotEmpty ? host : address};
  } else if (net == 'grpc') {
    ss['grpcSettings'] = <String, dynamic>{'serviceName': params['serviceName'] ?? path};
  } else if (net == 'httpupgrade') {
    ss['httpupgradeSettings'] = <String, dynamic>{'path': path, 'host': host.isNotEmpty ? host : address};
  } else if (net == 'splithttp') {
    ss['splithttpSettings'] = <String, dynamic>{'path': path, 'host': host.isNotEmpty ? host : address};
  }
  ob['streamSettings'] = ss;
  return ob;
}

Map<String, dynamic>? _parseTrojanUri(String uri) {
  final Uri parsed = Uri.parse(uri);
  final String password = parsed.userInfo;
  final String address = parsed.host;
  final int port = parsed.port;
  final Map<String, String> params = parsed.queryParameters;
  final String sni = params['sni'] ?? '';
  final String net = params['type'] ?? 'tcp';
  final String path = params['path'] ?? '/';
  final String host = params['host'] ?? '';
  final String fp = params['fp'] ?? '';

  final Map<String, dynamic> ob = <String, dynamic>{
    'protocol': 'trojan',
    'tag': 'proxy',
    'settings': <String, dynamic>{
      'servers': <Map<String, dynamic>>[
        <String, dynamic>{'address': address, 'port': port, 'password': password},
      ],
    },
  };

  final Map<String, dynamic> ss = <String, dynamic>{'network': net, 'security': 'tls'};
  final Map<String, dynamic> tls = <String, dynamic>{
    'serverName': sni.isNotEmpty ? sni : address,
    'allowInsecure': true,
  };
  if (fp.isNotEmpty) tls['fingerprint'] = fp;
  ss['tlsSettings'] = tls;
  if (net == 'ws') {
    ss['wsSettings'] = <String, dynamic>{'path': path, 'host': host.isNotEmpty ? host : address};
  } else if (net == 'grpc') {
    ss['grpcSettings'] = <String, dynamic>{'serviceName': params['serviceName'] ?? path};
  }
  ob['streamSettings'] = ss;
  return ob;
}

Map<String, dynamic>? _parseShadowsocksUri(String uri) {
  // ss://base64(method:password)@host:port#name  OR  ss://base64(method:password@host:port)#name
  String body = uri.substring(uri.indexOf('://') + 3);
  // Remove fragment
  final int hashIdx = body.indexOf('#');
  if (hashIdx >= 0) body = body.substring(0, hashIdx);

  String method, password, address;
  int port;

  if (body.contains('@')) {
    // Standard format: base64(method:password)@host:port
    final List<String> parts = body.split('@');
    String userInfo = parts[0];
    final String hostPort = parts[1];
    // Decode userInfo
    try {
      userInfo = utf8.decode(base64Decode(userInfo.padRight((userInfo.length + 3) & ~3, '=')));
    } catch (_) {}
    final int colonIdx = userInfo.indexOf(':');
    method = userInfo.substring(0, colonIdx);
    password = userInfo.substring(colonIdx + 1);
    final List<String> hp = hostPort.split(':');
    address = hp[0];
    port = int.tryParse(hp.length > 1 ? hp[1] : '443') ?? 443;
  } else {
    // All-in-one base64
    try {
      final String decoded = utf8.decode(base64Decode(body.padRight((body.length + 3) & ~3, '=')));
      final int atIdx = decoded.indexOf('@');
      final String userInfo = decoded.substring(0, atIdx);
      final String hostPort = decoded.substring(atIdx + 1);
      final int colonIdx = userInfo.indexOf(':');
      method = userInfo.substring(0, colonIdx);
      password = userInfo.substring(colonIdx + 1);
      final List<String> hp = hostPort.split(':');
      address = hp[0];
      port = int.tryParse(hp.length > 1 ? hp[1] : '443') ?? 443;
    } catch (_) {
      return null;
    }
  }

  return <String, dynamic>{
    'protocol': 'shadowsocks',
    'tag': 'proxy',
    'settings': <String, dynamic>{
      'servers': <Map<String, dynamic>>[
        <String, dynamic>{'address': address, 'port': port, 'method': method, 'password': password},
      ],
    },
  };
}

class _CurlResult {
  double timeTotal = 0;
  int bytesDownloaded = 0;
  double speedBps = 0;
  int httpCode = 0;
  int exitCode = -1;
}

Future<_CurlResult> _curlViaSocks(int socksPort, String url, String workingDir, {int timeoutSec = 10}) async {
  final _CurlResult cr = _CurlResult();
  try {
    final ProcessResult r = await Process.run(
      'curl',
      <String>[
        '--socks5-hostname', '127.0.0.1:$socksPort',
        '-o', 'NUL',
        '-w', '%{http_code},%{time_total},%{size_download},%{speed_download}',
        '--max-time', '$timeoutSec',
        '--connect-timeout', '8',
        '-s',
        '-L',
        url,
      ],
      workingDirectory: workingDir,
    );
    cr.exitCode = r.exitCode;
    final String out = r.stdout.toString().trim();
    if (out.isNotEmpty) {
      final List<String> parts = out.split(',');
      if (parts.length >= 4) {
        cr.httpCode = int.tryParse(parts[0]) ?? 0;
        cr.timeTotal = double.tryParse(parts[1]) ?? 0;
        cr.bytesDownloaded = (double.tryParse(parts[2]) ?? 0).toInt();
        cr.speedBps = double.tryParse(parts[3]) ?? 0;
      }
    }
  } catch (_) {}
  return cr;
}

Future<bool> _testTelegramDC(int socksPort) async {
  for (final String dc in kTelegramDCs) {
    try {
      // Connect to SOCKS5 proxy
      final Socket sock = await Socket.connect('127.0.0.1', socksPort,
          timeout: const Duration(seconds: 5));
      // SOCKS5 handshake
      sock.add(<int>[0x05, 0x01, 0x00]);
      await sock.flush();
      final List<int> authResp = await sock.first.timeout(const Duration(seconds: 5));
      if (authResp.length < 2 || authResp[0] != 0x05 || authResp[1] != 0x00) {
        await sock.close();
        continue;
      }
      // SOCKS5 CONNECT to DC:443
      final List<int> ipParts = dc.split('.').map(int.parse).toList();
      sock.add(<int>[0x05, 0x01, 0x00, 0x01, ...ipParts, 0x01, 0xBB]);
      await sock.flush();
      final List<int> connResp = await sock.first.timeout(const Duration(seconds: 8));
      await sock.close();
      if (connResp.length >= 2 && connResp[0] == 0x05 && connResp[1] == 0x00) {
        return true;
      }
    } catch (_) {}
  }
  return false;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Helpers
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
String fmtTs(DateTime dt) {
  String two(int x) => x.toString().padLeft(2, '0');
  return '${two(dt.hour)}:${two(dt.minute)}:${two(dt.second)}';
}

String fmtDuration(Duration d) {
  int s = d.inSeconds;
  if (s <= 0) return '0s';
  final int h = s ~/ 3600;
  s -= h * 3600;
  final int m = s ~/ 60;
  s -= m * 60;
  if (h > 0) return '${h}h ${m}m';
  if (m > 0) return '${m}m ${s}s';
  return '${s}s';
}

String fmtNumber(int n) {
  if (n < 1000) return '$n';
  if (n < 1000000) return '${(n / 1000).toStringAsFixed(1)}K';
  return '${(n / 1000000).toStringAsFixed(1)}M';
}

double? avgLatencyMs(List<HunterLatencyConfig> items) {
  double sum = 0;
  int n = 0;
  for (final HunterLatencyConfig cfg in items) {
    if (cfg.latencyMs == null || !cfg.latencyMs!.isFinite) continue;
    sum += cfg.latencyMs!;
    n += 1;
  }
  return n == 0 ? null : sum / n;
}

double? minLatencyMs(List<HunterLatencyConfig> items) {
  double? best;
  for (final HunterLatencyConfig cfg in items) {
    if (cfg.latencyMs == null || !cfg.latencyMs!.isFinite) continue;
    if (best == null || cfg.latencyMs! < best) best = cfg.latencyMs;
  }
  return best;
}

Map<String, int> detectEngines(String workingDir) {
  final Map<String, int> found = <String, int>{};
  for (final EngineInfo e in kEngines) {
    final String path = '$workingDir\\${e.relPath}';
    if (File(path).existsSync()) {
      try {
        found[e.name] = File(path).lengthSync();
      } catch (_) {
        found[e.name] = 0;
      }
    }
  }
  return found;
}
