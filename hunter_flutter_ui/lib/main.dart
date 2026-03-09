import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:window_manager/window_manager.dart';
import 'package:path_provider/path_provider.dart';
import 'package:system_tray/system_tray.dart';

import 'theme.dart';
import 'models.dart';
import 'services.dart';
import 'widgets/dashboard_section.dart';
import 'widgets/configs_section.dart';
import 'widgets/logs_section.dart';
import 'widgets/docs_section.dart';
import 'widgets/advanced_section.dart';
import 'widgets/about_section.dart';

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Singleton lock
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
File? _globalLockFile;
RandomAccessFile? _globalLockHandle;

Future<void> _ensureSingleInstance() async {
  try {
    final File lockFile = File('${Directory.systemTemp.path}\\hunter_dashboard_singleton.lock');
    _globalLockFile = lockFile;
    try {
      _globalLockHandle = await lockFile.open(mode: FileMode.write);
      await _globalLockHandle!.lock(FileLock.exclusive);
      await _globalLockHandle!.writeString(DateTime.now().toIso8601String());
      await _globalLockHandle!.flush();
    } on FileSystemException {
      exit(0);
    }
  } catch (_) {}
}

Future<void> _cleanupLockFile() async {
  try {
    await _globalLockHandle?.unlock();
    await _globalLockHandle?.close();
    if (_globalLockFile != null && await _globalLockFile!.exists()) {
      await _globalLockFile!.delete();
    }
  } catch (_) {}
}

Future<void> _initWindow() async {
  await windowManager.ensureInitialized();
  const WindowOptions opts = WindowOptions(
    size: Size(1280, 820),
    center: true,
    backgroundColor: Colors.transparent,
    skipTaskbar: false,
    titleBarStyle: TitleBarStyle.hidden,
    title: 'HUNTER',
  );
  await windowManager.waitUntilReadyToShow(opts, () async {
    await windowManager.setPreventClose(true);
    await windowManager.show();
    await windowManager.focus();
  });
}

Future<void> _copyBundledConfigs() async {
  try {
    final Directory appDocDir = await getApplicationDocumentsDirectory();
    final Directory configDir = Directory('${appDocDir.path}\\Hunter\\config');
    await configDir.create(recursive: true);
    for (final String asset in <String>[
      'assets/configs/All_Configs_Sub.txt',
      'assets/configs/all_extracted_configs.txt',
      'assets/configs/sub.txt',
    ]) {
      final File dest = File('${configDir.path}\\${asset.split('/').last}');
      if (!await dest.exists()) {
        final ByteData data = await rootBundle.load(asset);
        await dest.writeAsBytes(data.buffer.asUint8List());
      }
    }
  } catch (_) {}
}

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await _ensureSingleInstance();
  await _copyBundledConfigs();
  await _initWindow();
  runApp(MaterialApp(
    debugShowCheckedModeBanner: false,
    title: 'HUNTER',
    theme: hunterTheme(),
    home: const HunterPage(),
  ));
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Main Page — Orchestrator
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
class HunterPage extends StatefulWidget {
  const HunterPage({super.key});
  @override
  State<HunterPage> createState() => _HunterPageState();
}

class _HunterPageState extends State<HunterPage> with WindowListener, TickerProviderStateMixin {
  // ── Controllers ──
  final TextEditingController _cliPath = TextEditingController(text: 'bin\\hunter_cli.exe');
  final TextEditingController _configPath = TextEditingController(text: 'runtime\\hunter_config.json');
  final TextEditingController _xrayPath = TextEditingController(text: 'bin\\xray.exe');
  final TextEditingController _searchCtl = TextEditingController();
  final TextEditingController _tgApiId = TextEditingController();
  final TextEditingController _tgApiHash = TextEditingController();
  final TextEditingController _tgPhone = TextEditingController();
  final TextEditingController _tgTargets = TextEditingController(text: 'v2rayngvpn\nmitivpn\nv2ray_configs_pool');
  final TextEditingController _tgLimit = TextEditingController(text: '50');
  final TextEditingController _tgTimeout = TextEditingController(text: '12000');
  final ScrollController _logScroll = ScrollController();

  // ── Animations ──
  late final AnimationController _pulseCtl;
  late final AnimationController _ignitionCtl;
  // ignore: unused_field
  late final Animation<double> _pulseAnim;
  // ignore: unused_field
  late final Animation<double> _ignitionAnim;
  // ignore: unused_field
  bool _showIgnition = false;
  // ignore: unused_field
  bool _showShutdown = false;

  // ── State ──
  HunterRunState _state = HunterRunState.stopped;
  HunterNavSection _nav = HunterNavSection.dashboard;
  HunterConfigListKind _configKind = HunterConfigListKind.alive;
  Process? _process;
  Directory? _workingDir;
  StreamSubscription<String>? _stdoutSub;
  StreamSubscription<String>? _stderrSub;
  String? _lastError;

  final List<HunterLogLine> _logs = <HunterLogLine>[];
  int _logBytes = 0;
  bool _autoScroll = true;
  Timer? _refreshTimer;
  bool _refreshing = false;
  DateTime? _lastRefresh;
  final List<String> _recentLogOrder = <String>[];
  final Set<String> _recentLogKeys = <String>{};
  int _hunterLogOffset = 0;
  int _hunterErrOffset = 0;
  int _refreshTick = 0;
  DateTime? _lastActivityAt;
  String _lastSnapshotFingerprint = '';

  // ── Runtime data ──
  List<String> _allCache = const <String>[];
  List<String> _githubCache = const <String>[];
  List<String> _silverConfigs = const <String>[];
  List<String> _goldConfigs = const <String>[];
  List<HunterLatencyConfig> _balancerConfigs = const <HunterLatencyConfig>[];
  List<HunterLatencyConfig> _geminiConfigs = const <HunterLatencyConfig>[];
  Map<String, dynamic>? _status;
  List<Map<String, dynamic>> _history = const <Map<String, dynamic>>[];
  Map<String, int> _engines = const <String, int>{};
  List<ProjectDocFile> _docs = const <ProjectDocFile>[];
  String? _selectedDocPath;
  String _selectedDocContent = '';
  bool _docsLoading = false;
  String? _docsError;
  int _bundledCount = 0;
  bool _tgEnabled = false;

  // ── Tray ──
  SystemTray? _systemTray;
  bool _trayReady = false;
  bool _minimizedToTray = false;
  int _lastNotifiedCount = 0;

  @override
  void initState() {
    super.initState();
    windowManager.addListener(this);

    // Pulse animation — continuous glow when running
    _pulseCtl = AnimationController(vsync: this, duration: const Duration(milliseconds: 1400));
    _pulseAnim = Tween<double>(begin: 0.3, end: 1.0).animate(
      CurvedAnimation(parent: _pulseCtl, curve: Curves.easeInOut),
    );

    // Ignition animation — one-shot burst on start/stop
    _ignitionCtl = AnimationController(vsync: this, duration: const Duration(milliseconds: 800));
    _ignitionAnim = Tween<double>(begin: 0.0, end: 1.0).animate(
      CurvedAnimation(parent: _ignitionCtl, curve: Curves.easeOutCubic),
    );
    _ignitionCtl.addStatusListener((AnimationStatus s) {
      if (s == AnimationStatus.completed) {
        Future<void>.delayed(const Duration(milliseconds: 600), () {
          if (mounted) setState(() { _showIgnition = false; _showShutdown = false; });
        });
      }
    });

    _refreshTimer = Timer.periodic(const Duration(seconds: 2), (_) => _refresh());
    _refresh();
    _loadBundled();
    _loadDocs();
    _initTray();
    _loadTelegramSettings();
  }

  @override
  void dispose() {
    _pulseCtl.dispose();
    _ignitionCtl.dispose();
    windowManager.removeListener(this);
    _refreshTimer?.cancel();
    _stdoutSub?.cancel();
    _stderrSub?.cancel();
    _process?.kill();
    _systemTray?.destroy();
    for (final TextEditingController c in <TextEditingController>[
      _cliPath, _configPath, _xrayPath, _searchCtl,
      _tgApiId, _tgApiHash, _tgPhone, _tgTargets, _tgLimit, _tgTimeout,
    ]) {
      c.dispose();
    }
    _logScroll.dispose();
    super.dispose();
  }

  // ━━━━━ Logging (100KB cap) ━━━━━
  void _log(String source, String message) {
    final MapEntry<String, String>? entry = _prepareLogEntry(source, message);
    if (entry == null) return;
    _appendPreparedLogEntries(<MapEntry<String, String>>[entry]);
    if (_autoScroll) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (_logScroll.hasClients) {
          _logScroll.jumpTo(_logScroll.position.maxScrollExtent);
        }
      });
    }
  }

  MapEntry<String, String>? _prepareLogEntry(String source, String message) {
    final String clean = stripAnsi(message).trimRight();
    if (clean.isEmpty) return null;
    final String key = '$source|$clean';
    if (_recentLogKeys.contains(key)) return null;
    _recentLogKeys.add(key);
    _recentLogOrder.add(key);
    while (_recentLogOrder.length > 240) {
      final String removed = _recentLogOrder.removeAt(0);
      _recentLogKeys.remove(removed);
    }
    return MapEntry<String, String>(source, clean);
  }

  void _appendPreparedLogEntries(List<MapEntry<String, String>> entries) {
    if (entries.isEmpty || !mounted) return;
    final DateTime now = DateTime.now();
    setState(() {
      for (final MapEntry<String, String> entry in entries) {
        _logs.add(HunterLogLine(ts: now, source: entry.key, message: entry.value));
        _logBytes += entry.value.length;
      }
      while (_logBytes > 100 * 1024 && _logs.length > 1) {
        _logBytes -= _logs.first.message.length;
        _logs.removeAt(0);
      }
      _lastActivityAt = now;
    });
  }

  String _classifyRuntimeLogSource(String line, String fallback) {
    final String lower = line.toLowerCase();
    if (fallback == 'err') return 'err';
    if (lower.contains('warning')) return 'warn';
    if (lower.contains(' error') || lower.contains('| error') || lower.contains('failed') || lower.contains('exception')) {
      return 'err';
    }
    return fallback;
  }

  List<MapEntry<String, String>> _prepareRuntimeLogBatch(List<String> lines, String fallback) {
    final List<MapEntry<String, String>> out = <MapEntry<String, String>>[];
    final List<String> tail = lines.length > 120 ? lines.sublist(lines.length - 120) : lines;
    for (final String line in tail) {
      final MapEntry<String, String>? entry = _prepareLogEntry(_classifyRuntimeLogSource(line, fallback), line);
      if (entry != null) out.add(entry);
    }
    return out;
  }

  Future<bool> _pullRuntimeLogs(String rtDir) async {
    final LogTailResult mainTail = await readNewLogLines('$rtDir\\hunter.log', _hunterLogOffset, maxBytes: 24 * 1024);
    final LogTailResult errTail = await readNewLogLines('$rtDir\\hunter_stderr.log', _hunterErrOffset, maxBytes: 16 * 1024);
    _hunterLogOffset = mainTail.nextOffset;
    _hunterErrOffset = errTail.nextOffset;
    final List<MapEntry<String, String>> prepared = <MapEntry<String, String>>[
      ..._prepareRuntimeLogBatch(mainTail.lines, 'out'),
      ..._prepareRuntimeLogBatch(errTail.lines, 'err'),
    ];
    if (prepared.isEmpty) return false;
    _appendPreparedLogEntries(prepared);
    return true;
  }

  String _snapshotFingerprint(RuntimeSnapshot snap) {
    final Map<String, dynamic>? db = snap.status?['db'] is Map<String, dynamic>
        ? snap.status!['db'] as Map<String, dynamic>
        : null;
    final Map<String, dynamic>? validator = snap.status?['validator'] is Map<String, dynamic>
        ? snap.status!['validator'] as Map<String, dynamic>
        : null;
    return <Object?>[
      snap.status?['phase'],
      db?['total'],
      db?['alive'],
      db?['tested_unique'],
      validator?['rate_per_s'],
      snap.goldConfigs.length,
      snap.silverConfigs.length,
      snap.balancerConfigs.length,
      snap.geminiConfigs.length,
      snap.allCache.length,
      snap.githubCache.length,
    ].join('|');
  }

  List<Map<String, dynamic>> _nextHistory(RuntimeSnapshot snap, DateTime now) {
    final List<Map<String, dynamic>> base = snap.history.isNotEmpty
        ? snap.history.map((Map<String, dynamic> row) => Map<String, dynamic>.from(row)).toList()
        : _history.map((Map<String, dynamic> row) => Map<String, dynamic>.from(row)).toList();
    final Map<String, dynamic>? db = snap.status?['db'] is Map<String, dynamic>
        ? snap.status!['db'] as Map<String, dynamic>
        : null;
    if (db == null) return base;
    final Map<String, dynamic> point = <String, dynamic>{
      'ts': now.millisecondsSinceEpoch ~/ 1000,
      'tested_unique': db['tested_unique'] is num ? (db['tested_unique'] as num).toInt() : 0,
      'alive': db['alive'] is num ? (db['alive'] as num).toInt() : 0,
    };
    if (base.isEmpty || base.last['tested_unique'] != point['tested_unique'] || base.last['alive'] != point['alive']) {
      base.add(point);
    }
    if (base.length > 90) {
      base.removeRange(0, base.length - 90);
    }
    return base;
  }

  // ━━━━━ Runtime refresh ━━━━━
  Future<void> _refresh() async {
    if (_refreshing) return;
    _refreshing = true;
    try {
      final Directory wd = _effectiveWd();
      final String rtDir = '${wd.path}\\runtime';
      final RuntimeSnapshot snap = await loadRuntimeSnapshot(rtDir);
      final Map<String, int> eng = detectEngines(wd.path);
      final bool sawNewLogs = await _pullRuntimeLogs(rtDir);
      final DateTime now = DateTime.now();
      final String fingerprint = _snapshotFingerprint(snap);
      final bool snapshotChanged = fingerprint != _lastSnapshotFingerprint;
      final List<Map<String, dynamic>> nextHistory = _nextHistory(snap, now);

      if (!mounted) return;
      final int newTotal = snap.allCache.length;
      final int oldTotal = _allCache.length;
      if (newTotal > oldTotal && _lastNotifiedCount != newTotal && _minimizedToTray) {
        _lastNotifiedCount = newTotal;
        _sendTrayNotification('${newTotal - oldTotal} new configs! Total: $newTotal');
      }

      setState(() {
        _allCache = snap.allCache;
        _githubCache = snap.githubCache;
        _silverConfigs = snap.silverConfigs;
        _goldConfigs = snap.goldConfigs;
        _balancerConfigs = snap.balancerConfigs;
        _geminiConfigs = snap.geminiConfigs;
        _status = snap.status;
        _history = nextHistory;
        _engines = eng;
        _lastRefresh = now;
        _refreshTick += 1;
        if (sawNewLogs || snapshotChanged) {
          _lastActivityAt = now;
        }
      });
      _lastSnapshotFingerprint = fingerprint;
    } catch (e) {
      // Silently handle refresh errors - don't spam logs
    } finally {
      _refreshing = false;
    }
  }

  Future<void> _loadBundled() async {
    final int count = await loadBundledConfigsCount();
    if (mounted) setState(() => _bundledCount = count);
  }

  Future<void> _loadDocs() async {
    if (_docsLoading) return;
    setState(() {
      _docsLoading = true;
      _docsError = null;
    });
    try {
      final Directory wd = _effectiveWd();
      final List<ProjectDocFile> docs = await listProjectDocs('${wd.path}\\docs');
      String? selectedPath = _selectedDocPath;
      String content = _selectedDocContent;
      if (docs.isEmpty) {
        selectedPath = null;
        content = '';
      } else {
        final bool selectedStillExists = selectedPath != null && docs.any((ProjectDocFile d) => d.absPath == selectedPath);
        if (!selectedStillExists) {
          selectedPath = docs.first.absPath;
          content = await readProjectDoc(selectedPath);
        }
      }
      if (!mounted) return;
      setState(() {
        _docs = docs;
        _selectedDocPath = selectedPath;
        _selectedDocContent = content;
      });
    } catch (e) {
      if (!mounted) return;
      setState(() => _docsError = '$e');
    } finally {
      if (mounted) setState(() => _docsLoading = false);
    }
  }

  Future<void> _openDoc(ProjectDocFile doc) async {
    final String content = await readProjectDoc(doc.absPath);
    if (!mounted) return;
    setState(() {
      _selectedDocPath = doc.absPath;
      _selectedDocContent = content;
      _nav = HunterNavSection.docs;
    });
  }

  // ━━━━━ CLI Process ━━━━━
  Directory _effectiveWd() => _workingDir ?? resolveWorkingDirForCli(_cliPath.text);

  Future<void> _start() async {
    if (_state == HunterRunState.starting || _state == HunterRunState.running) return;
    setState(() {
      _state = HunterRunState.starting;
      _lastError = null;
      _showIgnition = true;
      _showShutdown = false;
    });
    _ignitionCtl.forward(from: 0);

    try {
      final Directory wd = resolveWorkingDirForCli(_cliPath.text);
      final String cliAbs = absPathInWorkingDir(wd, _cliPath.text);
      if (cliAbs.isEmpty || !File(cliAbs).existsSync()) {
        throw Exception('hunter_cli.exe not found: $cliAbs');
      }
      await Directory('${wd.path}\\runtime').create(recursive: true);
      try { File('${wd.path}\\runtime\\stop.flag').deleteSync(); } catch (_) {}

      final List<String> args = <String>[];
      final String cfg = _configPath.text.trim();
      if (cfg.isNotEmpty) args.addAll(<String>['--config', cfg]);
      final String xray = _xrayPath.text.trim();
      if (xray.isNotEmpty) args.addAll(<String>['--xray', xray]);

      _log('sys', 'Starting: $cliAbs ${args.join(' ')}');
      final Process p = await Process.start(cliAbs, args, workingDirectory: wd.path, runInShell: false);
      _process = p;
      _workingDir = wd;
      unawaited(_loadDocs());

      _stdoutSub = p.stdout.transform(utf8.decoder).transform(const LineSplitter())
          .listen((String line) => _log('out', line));
      _stderrSub = p.stderr.transform(utf8.decoder).transform(const LineSplitter())
          .listen((String line) => _log('err', line));

      unawaited(p.exitCode.then((int code) {
        _log('sys', 'Process exited: $code');
        if (mounted) setState(() { _process = null; _workingDir = null; _state = HunterRunState.stopped; });
      }));
      setState(() => _state = HunterRunState.running);
    } catch (e) {
      _pulseCtl.stop();
      setState(() { _state = HunterRunState.stopped; _workingDir = null; _lastError = '$e'; });
      _log('err', 'Start failed: $e');
    }
  }

  Future<void> _stop() async {
    if (_state == HunterRunState.stopping || _state == HunterRunState.stopped) return;
    _pulseCtl.stop();
    setState(() {
      _state = HunterRunState.stopping;
      _lastError = null;
      _showShutdown = true;
      _showIgnition = false;
    });
    _ignitionCtl.forward(from: 0);
    try {
      final Directory wd = _effectiveWd();
      await Directory('${wd.path}\\runtime').create(recursive: true);
      File('${wd.path}\\runtime\\stop.flag').writeAsStringSync('stop\n', flush: true);
      _log('sys', 'stop.flag created');

      final Process? p = _process;
      if (p != null) {
        await p.exitCode.timeout(const Duration(seconds: 15), onTimeout: () {
          _log('sys', 'Timeout, killing...');
          p.kill(ProcessSignal.sigterm);
          return -1;
        });
      }
      setState(() { _process = null; _workingDir = null; _state = HunterRunState.stopped; });
    } catch (e) {
      setState(() { _state = HunterRunState.stopped; _workingDir = null; _lastError = '$e'; });
      _log('err', 'Stop failed: $e');
    }
  }

  // ━━━━━ Copy helpers ━━━━━
  Future<void> _copyText(String text, {String? label}) async {
    if (text.trim().isEmpty) return;
    await Clipboard.setData(ClipboardData(text: text.trim()));
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(
        content: Text(label ?? 'Copied!', style: const TextStyle(fontSize: 12)),
        backgroundColor: C.card,
        behavior: SnackBarBehavior.floating,
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
        duration: const Duration(seconds: 2),
      ),
    );
  }

  Future<void> _copyLines(List<String> lines, {String? label}) async {
    if (lines.isEmpty) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Nothing to copy')),
      );
      return;
    }
    await _copyText(lines.join('\n'), label: label ?? 'Copied ${lines.length} configs');
  }

  Future<void> _copyLogs() async {
    if (_logs.isEmpty) return;
    final StringBuffer buf = StringBuffer();
    for (final HunterLogLine l in _logs) {
      buf.writeln('${fmtTs(l.ts)} [${l.source}] ${l.message}');
    }
    await _copyText(buf.toString(), label: 'Copied ${_logs.length} log lines');
  }

  void _clearLogs() => setState(() {
    _logs.clear();
    _logBytes = 0;
    _recentLogKeys.clear();
    _recentLogOrder.clear();
  });

  void _resetFindingsState() {
    _silverConfigs = const <String>[];
    _goldConfigs = const <String>[];
    _balancerConfigs = const <HunterLatencyConfig>[];
    _geminiConfigs = const <HunterLatencyConfig>[];
    _status = null;
    _history = const <Map<String, dynamic>>[];
    _lastRefresh = null;
    _lastActivityAt = null;
    _lastSnapshotFingerprint = '';
    _hunterLogOffset = 0;
    _hunterErrOffset = 0;
    _refreshTick = 0;
    _logs.clear();
    _logBytes = 0;
    _recentLogKeys.clear();
    _recentLogOrder.clear();
  }

  Future<void> _deleteFileIfExists(String path) async {
    final File file = File(path);
    if (await file.exists()) {
      await file.delete();
    }
  }

  Future<void> _resetFindingsAndRestart() async {
    if (_state == HunterRunState.starting || _state == HunterRunState.stopping) return;

    if (_state == HunterRunState.running) {
      await _stop();
    }

    final Directory wd = _effectiveWd();
    final String runtimeDir = '${wd.path}\\runtime';
    await Directory(runtimeDir).create(recursive: true);

    for (final String fileName in <String>[
      'HUNTER_status.json',
      'HUNTER_gold.txt',
      'HUNTER_silver.txt',
      'gold.txt',
      'silver.txt',
      'HUNTER_balancer_cache.json',
      'HUNTER_gemini_balancer_cache.json',
      'HUNTER_working_cache.txt',
      'hunter_state.json',
      'hunter.log',
      'hunter_stderr.log',
    ]) {
      await _deleteFileIfExists('$runtimeDir\\$fileName');
    }

    if (!mounted) return;
    setState(() {
      _lastError = null;
      _resetFindingsState();
    });
    _log('sys', 'Findings reset. Cached/downloaded configs were preserved.');
    await _refresh();
    await _start();
  }

  // ━━━━━ Speed test ━━━━━
  Future<void> _speedTest(String configUri) async {
    final Directory wd = _effectiveWd();
    final String xray = absPathInWorkingDir(wd, _xrayPath.text);
    if (!File(xray).existsSync()) {
      _log('err', 'XRay not found at: $xray');
      return;
    }
    _log('sys', 'Speed test: $configUri');
    final SpeedTestResult result = await runConfigSpeedTest(
      xrayPath: xray,
      configUri: configUri,
      workingDir: wd.path,
    );
    if (result.error != null && !result.isAlive) {
      _log('err', 'Speed test: ${result.error}');
    } else if (result.isAlive) {
      final StringBuffer msg = StringBuffer('✓ ALIVE');
      if (result.latencyMs != null) msg.write(' | Latency: ${result.latencyMs}ms');
      if (result.speedMbps != null) msg.write(' | Speed: ${result.speedMbps!.toStringAsFixed(2)} Mbps');
      if (result.downloadBytes != null) msg.write(' | Downloaded: ${(result.downloadBytes! / 1024).toStringAsFixed(0)}KB');
      _log('sys', msg.toString());
    } else if (result.telegramOk) {
      _log('sys', 'Telegram-only: proxy can reach Telegram but HTTP download failed');
    } else {
      _log('err', 'Speed test: All tests failed');
    }
  }

  // ━━━━━ Telegram settings ━━━━━
  Future<void> _loadTelegramSettings() async {
    try {
      final Directory wd = _effectiveWd();
      final String path = absPathInWorkingDir(wd, _configPath.text);
      if (path.isEmpty) return;
      final File f = File(path);
      if (!f.existsSync()) return;
      final dynamic decoded = jsonDecode(await f.readAsString());
      if (decoded is! Map<String, dynamic>) return;
      if (!mounted) return;
      setState(() {
        _tgEnabled = decoded['telegram_enabled'] == true;
        _tgApiId.text = (decoded['telegram_api_id'] ?? '').toString();
        _tgApiHash.text = (decoded['telegram_api_hash'] ?? '').toString();
        _tgPhone.text = (decoded['telegram_phone'] ?? '').toString();
        _tgLimit.text = (decoded['telegram_limit'] ?? '50').toString();
        _tgTimeout.text = (decoded['telegram_timeout_ms'] ?? '12000').toString();
        if (decoded['targets'] is List) {
          _tgTargets.text = (decoded['targets'] as List<dynamic>).map((dynamic e) => e.toString()).join('\n');
        }
      });
    } catch (_) {}
  }

  Future<void> _saveTelegramSettings() async {
    final Directory wd = _effectiveWd();
    final String path = absPathInWorkingDir(wd, _configPath.text);
    if (path.isEmpty) return;
    Map<String, dynamic> root = <String, dynamic>{};
    final File f = File(path);
    try {
      if (f.existsSync()) {
        final dynamic d = jsonDecode(await f.readAsString());
        if (d is Map<String, dynamic>) root = d;
      }
    } catch (_) {}
    root['telegram_enabled'] = _tgEnabled;
    root['telegram_api_id'] = _tgApiId.text.trim();
    root['telegram_api_hash'] = _tgApiHash.text.trim();
    root['telegram_phone'] = _tgPhone.text.trim();
    root['telegram_limit'] = int.tryParse(_tgLimit.text.trim()) ?? 50;
    root['telegram_timeout_ms'] = int.tryParse(_tgTimeout.text.trim()) ?? 12000;
    root['targets'] = _tgTargets.text.split(RegExp(r'[\r\n,]+')).map((String s) => s.trim()).where((String s) => s.isNotEmpty).toList();
    await f.parent.create(recursive: true);
    await f.writeAsString(jsonEncode(root), flush: true);
    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Telegram settings saved')));
    }
  }

  // ━━━━━ System Tray ━━━━━
  Future<void> _initTray() async {
    try {
      final String? iconPath = await _findTrayIcon();
      if (iconPath == null) { _trayReady = false; return; }
      _systemTray = SystemTray();
      await _systemTray!.initSystemTray(title: 'HUNTER', iconPath: iconPath, toolTip: 'Hunter Dashboard');
      _trayReady = true;
      _systemTray!.registerSystemTrayEventHandler((String e) {
        if (e == kSystemTrayEventClick || e == kSystemTrayEventRightClick) _restoreFromTray();
      });
      final Menu menu = Menu();
      await menu.buildFrom(<MenuItemBase>[
        MenuItemLabel(label: 'Show', onClicked: (_) => _restoreFromTray()),
        MenuSeparator(),
        MenuItemLabel(label: 'Start', onClicked: (_) { if (_state == HunterRunState.stopped) _start(); }),
        MenuItemLabel(label: 'Stop', onClicked: (_) { if (_state == HunterRunState.running) _stop(); }),
        MenuSeparator(),
        MenuItemLabel(label: 'Exit', onClicked: (_) async { await _cleanupLockFile(); exit(0); }),
      ]);
      await _systemTray!.setContextMenu(menu);
    } catch (_) { _trayReady = false; }
  }

  Future<String?> _findTrayIcon() async {
    if (!Platform.isWindows) return null;
    String exeDir;
    try { exeDir = File(Platform.resolvedExecutable).parent.path; } catch (_) { exeDir = Directory.current.path; }
    for (final String p in <String>[
      '$exeDir\\app_icon.ico',
      '$exeDir\\resources\\app_icon.ico',
      '${Directory.current.path}\\windows\\runner\\resources\\app_icon.ico',
    ]) {
      try { if (File(p).existsSync()) return p; } catch (_) {}
    }
    return null;
  }

  Future<void> _restoreFromTray() async {
    _minimizedToTray = false;
    await windowManager.show();
    await windowManager.setSkipTaskbar(false);
    await windowManager.focus();
  }

  Future<void> _minimizeToTray() async {
    if (!_trayReady) { await _initTray(); }
    if (!_trayReady) return;
    _minimizedToTray = true;
    await windowManager.hide();
    await windowManager.setSkipTaskbar(true);
  }

  @override
  Future<void> onWindowClose() async {
    await windowManager.setPreventClose(true);
    await _minimizeToTray();
  }

  void _sendTrayNotification(String msg) {
    try { if (_trayReady) _systemTray?.setToolTip('Hunter: $msg'); } catch (_) {}
  }

  // ━━━━━ Navigation ━━━━━
  void _navigate(HunterNavSection section, {HunterConfigListKind? kind}) {
    setState(() {
      _nav = section;
      if (kind != null) _configKind = kind;
      _searchCtl.clear();
    });
  }

  // ━━━━━ Build ━━━━━
  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: LayoutBuilder(
        builder: (BuildContext context, BoxConstraints shellConstraints) {
          final bool compactShell = shellConstraints.maxWidth < 1100;
          if (compactShell) {
            return Column(
              children: <Widget>[
                _buildTopBar(context),
                Expanded(child: _buildContent(context, 0.0)),
                _buildCompactNavBar(context),
              ],
            );
          }
          return Row(
            children: <Widget>[
              _buildSidebar(context),
              Expanded(
                child: Column(
                  children: <Widget>[
                    _buildTopBar(context),
                    Expanded(child: _buildContent(context, 0.0)),
                  ],
                ),
              ),
            ],
          );
        },
      ),
    );
  }

  List<(HunterNavSection, IconData, String)> _navEntries() {
    return <(HunterNavSection, IconData, String)>[
      (HunterNavSection.dashboard, Icons.speed, 'Dashboard'),
      (HunterNavSection.configs, Icons.vpn_key, 'Configs'),
      (HunterNavSection.logs, Icons.terminal, 'Logs'),
      (HunterNavSection.docs, Icons.menu_book_rounded, 'Docs'),
      (HunterNavSection.advanced, Icons.tune, 'Advanced'),
      (HunterNavSection.about, Icons.info_outline, 'About'),
    ];
  }

  Widget _buildSidebar(BuildContext context) {
    final double pulseVal = 0.0;
    return Container(
      width: 56,
      decoration: BoxDecoration(
        color: C.surface,
        border: const Border(right: BorderSide(color: C.border, width: 0.5)),
        // Subtle side glow when running
        boxShadow: _state == HunterRunState.running
            ? <BoxShadow>[BoxShadow(color: C.neonGreen.withValues(alpha: 0.06 * pulseVal), blurRadius: 20, spreadRadius: 4)]
            : null,
      ),
      child: Column(
        children: <Widget>[
          const SizedBox(height: 12),
          // Logo with animated glow
          AnimatedContainer(
            duration: const Duration(milliseconds: 400),
            width: 36, height: 36,
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              border: Border.all(
                color: _state == HunterRunState.running
                    ? C.neonGreen.withValues(alpha: 0.4 + 0.4 * pulseVal)
                    : C.neonCyan.withValues(alpha: 0.3),
                width: _state == HunterRunState.running ? 2 : 1,
              ),
              boxShadow: _state == HunterRunState.running
                  ? <BoxShadow>[BoxShadow(color: C.neonGreen.withValues(alpha: 0.3 * pulseVal), blurRadius: 12)]
                  : null,
            ),
            child: Icon(Icons.shield,
              color: _state == HunterRunState.running ? C.neonGreen : C.neonCyan, size: 18),
          ),
          const SizedBox(height: 16),
          ..._navItems(),
          const Spacer(),
          // Animated status indicator
          Padding(
            padding: const EdgeInsets.only(bottom: 16),
            child: AnimatedContainer(
              duration: const Duration(milliseconds: 300),
              width: _state == HunterRunState.running ? 12 : 10,
              height: _state == HunterRunState.running ? 12 : 10,
              decoration: BoxDecoration(
                shape: BoxShape.circle,
                color: _stateColor(),
                boxShadow: _state == HunterRunState.running
                    ? <BoxShadow>[BoxShadow(color: C.neonGreen.withValues(alpha: 0.5 + 0.3 * pulseVal), blurRadius: 10 + 6 * pulseVal)]
                    : _state == HunterRunState.starting
                        ? <BoxShadow>[BoxShadow(color: C.neonAmber.withValues(alpha: 0.4), blurRadius: 8)]
                        : null,
              ),
            ),
          ),
        ],
      ),
    );
  }

  List<Widget> _navItems() {
    return _navEntries().map(((HunterNavSection, IconData, String) item) {
      final bool selected = _nav == item.$1;
      return Tooltip(
        message: item.$3,
        preferBelow: false,
        child: InkWell(
          onTap: () => setState(() => _nav = item.$1),
          child: Container(
            width: 56, height: 48,
            decoration: BoxDecoration(
              border: Border(
                left: BorderSide(
                  color: selected ? C.neonCyan : Colors.transparent,
                  width: 2,
                ),
              ),
            ),
            child: Icon(item.$2, color: selected ? C.neonCyan : C.txt3, size: 20),
          ),
        ),
      );
    }).toList();
  }

  Widget _buildCompactNavBar(BuildContext context) {
    final double pulseVal = 0.0;
    return SafeArea(
      top: false,
      child: Container(
        height: 62,
        decoration: BoxDecoration(
          color: C.surface,
          border: const Border(top: BorderSide(color: C.border, width: 0.5)),
          boxShadow: _state == HunterRunState.running
              ? <BoxShadow>[BoxShadow(color: C.neonGreen.withValues(alpha: 0.05 * pulseVal), blurRadius: 18, offset: const Offset(0, -4))]
              : null,
        ),
        child: LayoutBuilder(
          builder: (BuildContext context, BoxConstraints navConstraints) {
            final bool iconOnly = navConstraints.maxWidth < 560;
            return Row(
              children: _navEntries().map(((HunterNavSection, IconData, String) item) {
                final bool selected = _nav == item.$1;
                return Expanded(
                  child: InkWell(
                    onTap: () => setState(() => _nav = item.$1),
                    child: AnimatedContainer(
                      duration: const Duration(milliseconds: 220),
                      curve: Curves.easeOutCubic,
                      padding: EdgeInsets.symmetric(vertical: iconOnly ? 8 : 6),
                      decoration: BoxDecoration(
                        border: Border(
                          top: BorderSide(color: selected ? C.neonCyan : Colors.transparent, width: 2),
                        ),
                        color: selected ? C.neonCyan.withValues(alpha: 0.08) : Colors.transparent,
                      ),
                      child: Column(
                        mainAxisAlignment: MainAxisAlignment.center,
                        children: <Widget>[
                          Icon(item.$2, color: selected ? C.neonCyan : C.txt3, size: iconOnly ? 20 : 18),
                          if (!iconOnly) ...<Widget>[
                            const SizedBox(height: 2),
                            Text(
                              item.$3,
                              maxLines: 1,
                              overflow: TextOverflow.ellipsis,
                              style: TextStyle(color: selected ? C.neonCyan : C.txt3, fontSize: 9, fontWeight: FontWeight.w600),
                            ),
                          ],
                        ],
                      ),
                    ),
                  ),
                );
              }).toList(),
            );
          },
        ),
      ),
    );
  }

  Widget _buildTopBar(BuildContext context) {
    final String title = switch (_nav) {
      HunterNavSection.dashboard => 'DASHBOARD',
      HunterNavSection.configs => 'CONFIGS',
      HunterNavSection.logs => 'LOGS',
      HunterNavSection.docs => 'DOCS',
      HunterNavSection.advanced => 'SETTINGS',
      HunterNavSection.about => 'ABOUT',
    };
    final double pulseVal = 0.0;

    return GestureDetector(
      onPanStart: (_) => windowManager.startDragging(),
      onDoubleTap: () async {
        if (await windowManager.isMaximized()) {
          await windowManager.unmaximize();
        } else {
          await windowManager.maximize();
        }
      },
      child: Container(
        constraints: const BoxConstraints(minHeight: 40),
        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 6),
        decoration: BoxDecoration(
          color: C.surface,
          border: const Border(bottom: BorderSide(color: C.border, width: 0.5)),
          // Top bar glow when running
          boxShadow: _state == HunterRunState.running
              ? <BoxShadow>[BoxShadow(color: C.neonGreen.withValues(alpha: 0.04 * pulseVal), blurRadius: 12, offset: const Offset(0, 2))]
              : null,
        ),
        child: LayoutBuilder(
          builder: (BuildContext context, BoxConstraints topBarConstraints) {
            final bool compact = topBarConstraints.maxWidth < 1040;
            final Widget titleBlock = Row(
              children: <Widget>[
                AnimatedSwitcher(
                  duration: const Duration(milliseconds: 200),
                  child: Text(title, key: ValueKey<String>(title),
                    style: const TextStyle(color: C.txt3, fontSize: 10, fontWeight: FontWeight.w600, letterSpacing: 2)),
                ),
                if (_lastError != null) ...<Widget>[
                  const SizedBox(width: 12),
                  Expanded(
                    child: Container(
                      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
                      decoration: BoxDecoration(
                        color: C.neonRed.withValues(alpha: 0.1),
                        borderRadius: BorderRadius.circular(6),
                        border: Border.all(color: C.neonRed.withValues(alpha: 0.3)),
                      ),
                      child: Text(_lastError!, maxLines: 1, overflow: TextOverflow.ellipsis,
                        style: const TextStyle(color: C.neonRed, fontSize: 10)),
                    ),
                  ),
                ],
              ],
            );
            final Widget actionBlock = Wrap(
              alignment: WrapAlignment.end,
              crossAxisAlignment: WrapCrossAlignment.center,
              spacing: 6,
              runSpacing: 6,
              children: <Widget>[
                _animatedActionButton(
                  icon: Icons.play_arrow,
                  label: 'START',
                  color: C.neonGreen,
                  isActive: _state == HunterRunState.running,
                  isLoading: _state == HunterRunState.starting,
                  onPressed: _state == HunterRunState.stopped ? _start : null,
                ),
                _animatedActionButton(
                  icon: Icons.stop,
                  label: 'STOP',
                  color: C.neonRed,
                  isActive: false,
                  isLoading: _state == HunterRunState.stopping,
                  onPressed: _state == HunterRunState.running ? _stop : null,
                ),
                Container(
                  decoration: BoxDecoration(
                    borderRadius: BorderRadius.circular(8),
                    border: Border.all(color: C.neonAmber.withValues(alpha: 0.5), width: 2),
                    color: C.neonAmber.withValues(alpha: 0.1),
                  ),
                  child: Material(
                    color: Colors.transparent,
                    child: InkWell(
                      onTap: (_state == HunterRunState.starting || _state == HunterRunState.stopping) ? null : _resetFindingsAndRestart,
                      borderRadius: BorderRadius.circular(8),
                      child: Padding(
                        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
                        child: Row(
                          mainAxisSize: MainAxisSize.min,
                          children: <Widget>[
                            Icon(Icons.refresh, color: C.neonAmber, size: 18),
                            const SizedBox(width: 8),
                            Text('RESET', style: TextStyle(color: C.neonAmber, fontSize: 12, fontWeight: FontWeight.bold, letterSpacing: 0.5)),
                          ],
                        ),
                      ),
                    ),
                  ),
                ),
                AnimatedContainer(
                  duration: const Duration(milliseconds: 400),
                  curve: Curves.easeOutCubic,
                  padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 3),
                  decoration: BoxDecoration(
                    color: _stateColor().withValues(alpha: 0.08 + 0.06 * pulseVal),
                    borderRadius: BorderRadius.circular(12),
                    border: Border.all(color: _stateColor().withValues(alpha: 0.25 + 0.15 * pulseVal)),
                    boxShadow: _state == HunterRunState.running
                        ? <BoxShadow>[BoxShadow(color: C.neonGreen.withValues(alpha: 0.15 * pulseVal), blurRadius: 8)]
                        : null,
                  ),
                  child: Row(
                    mainAxisSize: MainAxisSize.min,
                    children: <Widget>[
                      AnimatedContainer(
                        duration: const Duration(milliseconds: 300),
                        width: 6, height: 6,
                        decoration: BoxDecoration(
                          shape: BoxShape.circle,
                          color: _stateColor(),
                          boxShadow: <BoxShadow>[BoxShadow(color: _stateColor().withValues(alpha: 0.4 + 0.3 * pulseVal), blurRadius: 4 + 4 * pulseVal)],
                        ),
                      ),
                      const SizedBox(width: 6),
                      AnimatedSwitcher(
                        duration: const Duration(milliseconds: 200),
                        child: Text(_stateLabel(), key: ValueKey<String>(_stateLabel()),
                          style: TextStyle(color: _stateColor(), fontSize: 9, fontWeight: FontWeight.w700, letterSpacing: 0.5)),
                      ),
                    ],
                  ),
                ),
                _windowButton(Icons.remove, 'Minimize', () => windowManager.minimize()),
                _windowButton(Icons.crop_square, 'Maximize', () async {
                  if (await windowManager.isMaximized()) { await windowManager.unmaximize(); } else { await windowManager.maximize(); }
                }),
                _windowButton(Icons.close, 'Exit', () async { await _cleanupLockFile(); exit(0); }, hoverColor: C.neonRed),
              ],
            );
            if (compact) {
              return Column(
                mainAxisAlignment: MainAxisAlignment.center,
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: <Widget>[
                  titleBlock,
                  const SizedBox(height: 6),
                  Align(alignment: Alignment.centerRight, child: actionBlock),
                ],
              );
            }
            return Row(
              children: <Widget>[
                Expanded(child: titleBlock),
                const SizedBox(width: 12),
                actionBlock,
              ],
            );
          },
        ),
      ),
    );
  }

  Widget _animatedActionButton({
    required IconData icon,
    required String label,
    required Color color,
    required bool isActive,
    required bool isLoading,
    VoidCallback? onPressed,
  }) {
    final bool enabled = onPressed != null;
    final double pulseVal = 0.0;
    return AnimatedContainer(
      duration: const Duration(milliseconds: 300),
      curve: Curves.easeOutCubic,
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: (enabled || isActive ? color : C.txt3).withValues(alpha: enabled || isActive ? 0.35 + 0.2 * pulseVal : 0.1)),
        color: isActive ? color.withValues(alpha: 0.1 + 0.06 * pulseVal) : (enabled ? color.withValues(alpha: 0.06) : Colors.transparent),
        boxShadow: isActive
            ? <BoxShadow>[BoxShadow(color: color.withValues(alpha: 0.15 * pulseVal), blurRadius: 10)]
            : null,
      ),
      child: Material(
        color: Colors.transparent,
        child: InkWell(
          onTap: onPressed,
          borderRadius: BorderRadius.circular(8),
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 5),
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: <Widget>[
                if (isLoading)
                  SizedBox(width: 12, height: 12, child: CircularProgressIndicator(strokeWidth: 1.5, color: color))
                else
                  Icon(icon, size: 13, color: enabled || isActive ? color : C.txt3.withValues(alpha: 0.3)),
                const SizedBox(width: 5),
                Text(label, style: TextStyle(
                  fontSize: 9, fontWeight: FontWeight.w700,
                  color: enabled || isActive ? color : C.txt3.withValues(alpha: 0.3),
                  letterSpacing: 0.5,
                )),
              ],
            ),
          ),
        ),
      ),
    );
  }

  Widget _windowButton(IconData icon, String tooltip, VoidCallback onPressed, {Color? hoverColor}) {
    return Tooltip(
      message: tooltip,
      child: Material(
        color: Colors.transparent,
        child: InkWell(
          onTap: onPressed,
          borderRadius: BorderRadius.circular(4),
          hoverColor: (hoverColor ?? C.txt3).withValues(alpha: 0.15),
          child: SizedBox(
            width: 32, height: 28,
            child: Icon(icon, size: 14, color: C.txt3),
          ),
        ),
      ),
    );
  }


  Widget _buildContent(BuildContext context, double pulseValue) {
    return switch (_nav) {
      HunterNavSection.dashboard => DashboardSection(
        status: _status,
        history: _history,
        silverConfigs: _silverConfigs,
        goldConfigs: _goldConfigs,
        balancerConfigs: _balancerConfigs,
        geminiConfigs: _geminiConfigs,
        allCacheCount: _allCache.length,
        githubCacheCount: _githubCache.length,
        docsCount: _docs.length,
        runState: _state,
        engines: _engines,
        lastRefresh: _lastRefresh,
        lastActivityAt: _lastActivityAt,
        refreshTick: _refreshTick,
        logs: _logs,
        onNavigate: _navigate,
        onCopyText: _copyText,
        onCopyLines: _copyLines,
      ),
      HunterNavSection.configs => ConfigsSection(
        listKind: _configKind,
        searchController: _searchCtl,
        allCacheConfigs: _allCache,
        githubCacheConfigs: _githubCache,
        silverConfigs: _silverConfigs,
        goldConfigs: _goldConfigs,
        balancerConfigs: _balancerConfigs,
        geminiConfigs: _geminiConfigs,
        lastRefresh: _lastRefresh,
        onKindChanged: (HunterConfigListKind k) => setState(() { _configKind = k; _searchCtl.clear(); }),
        onSearchChanged: () => setState(() {}),
        onRefresh: _refresh,
        onCopyText: _copyText,
        onCopyLines: _copyLines,
        onSpeedTest: _speedTest,
      ),
      HunterNavSection.logs => LogsSection(
        logs: _logs,
        logScrollController: _logScroll,
        autoScroll: _autoScroll,
        onAutoScrollChanged: (bool v) => setState(() => _autoScroll = v),
        onCopyLogs: _copyLogs,
        onClearLogs: _clearLogs,
        logMemoryBytes: _logBytes,
      ),
      HunterNavSection.docs => DocsSection(
        docs: _docs,
        selectedDocPath: _selectedDocPath,
        selectedDocContent: _selectedDocContent,
        loading: _docsLoading,
        error: _docsError,
        onRefresh: _loadDocs,
        onOpenDoc: _openDoc,
        onCopyText: _copyText,
      ),
      HunterNavSection.advanced => AdvancedSection(
        cliPathController: _cliPath,
        configPathController: _configPath,
        xrayPathController: _xrayPath,
        tgEnabled: _tgEnabled,
        tgApiIdController: _tgApiId,
        tgApiHashController: _tgApiHash,
        tgPhoneController: _tgPhone,
        tgTargetsController: _tgTargets,
        tgLimitController: _tgLimit,
        tgTimeoutMsController: _tgTimeout,
        workingDir: _effectiveWd(),
        processInfo: _process == null ? 'Not running' : 'PID ${_process!.pid}',
        engines: _engines,
        onTgEnabledChanged: (bool v) => setState(() => _tgEnabled = v),
        onSaveTelegram: _saveTelegramSettings,
        onRefresh: _refresh,
        onCopyText: _copyText,
      ),
      HunterNavSection.about => AboutSection(bundledConfigsCount: _bundledCount),
    };
  }

  Color _stateColor() {
    return switch (_state) {
      HunterRunState.running => C.neonGreen,
      HunterRunState.starting || HunterRunState.stopping => C.neonAmber,
      HunterRunState.stopped => C.txt3,
    };
  }

  String _stateLabel() {
    return switch (_state) {
      HunterRunState.stopped => 'OFFLINE',
      HunterRunState.starting => 'STARTING',
      HunterRunState.running => 'RUNNING',
      HunterRunState.stopping => 'STOPPING',
    };
  }
}
