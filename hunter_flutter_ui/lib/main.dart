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

    // Try to acquire exclusive lock
    bool acquired = false;
    for (int attempt = 0; attempt < 2; attempt++) {
      try {
        _globalLockHandle = await lockFile.open(mode: FileMode.write);
        await _globalLockHandle!.lock(FileLock.exclusive);
        await _globalLockHandle!.writeString(DateTime.now().toIso8601String());
        await _globalLockHandle!.flush();
        acquired = true;
        break;
      } on FileSystemException {
        // Lock held — on first attempt, try deleting stale lock and retry
        if (attempt == 0) {
          try {
            await _globalLockHandle?.close();
          } catch (_) {}
          _globalLockHandle = null;
          try {
            await lockFile.delete();
          } catch (_) {}
          await Future<void>.delayed(const Duration(milliseconds: 200));
          continue;
        }
        // Second attempt also failed — another instance is truly running
        exit(0);
      }
    }
    if (!acquired) exit(0);
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
  // Auto-adapt to screen resolution: 75% width × 85% height, clamped
  Size screenSize = const Size(1920, 1080);
  try {
    final screen = await windowManager.getBounds();
    if (screen.width > 0 && screen.height > 0) {
      screenSize = Size(screen.width, screen.height);
    }
  } catch (_) {}
  final double w = (screenSize.width * 0.75).clamp(900, 1800);
  final double h = (screenSize.height * 0.85).clamp(600, 1200);
  final WindowOptions opts = WindowOptions(
    size: Size(w, h),
    center: true,
    backgroundColor: Colors.transparent,
    skipTaskbar: false,
    titleBarStyle: TitleBarStyle.hidden,
    title: 'HUNTER',
  );
  await windowManager.waitUntilReadyToShow(opts, () async {
    await windowManager.setMinimumSize(const Size(900, 600));
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
  final TextEditingController _singboxPath = TextEditingController(text: 'bin\\sing-box.exe');
  final TextEditingController _mihomoPath = TextEditingController(text: 'bin\\mihomo-windows-amd64-compatible.exe');
  final TextEditingController _torPath = TextEditingController(text: 'bin\\tor.exe');
  final TextEditingController _multiproxyPort = TextEditingController(text: '10808');
  final TextEditingController _geminiPort = TextEditingController(text: '10809');
  final TextEditingController _maxTotal = TextEditingController(text: '1000');
  final TextEditingController _maxWorkers = TextEditingController(text: '12');
  final TextEditingController _scanLimit = TextEditingController(text: '50');
  final TextEditingController _sleepSeconds = TextEditingController(text: '300');
  final TextEditingController _searchCtl = TextEditingController();
  final TextEditingController _tgApiId = TextEditingController();
  final TextEditingController _tgApiHash = TextEditingController();
  final TextEditingController _tgPhone = TextEditingController();
  final TextEditingController _tgTargets = TextEditingController(text: 'v2rayngvpn\nmitivpn\nv2ray_configs_pool');
  final TextEditingController _tgLimit = TextEditingController(text: '50');
  final TextEditingController _tgTimeout = TextEditingController(text: '12000');
  final TextEditingController _githubUrls = TextEditingController();
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
  StreamSubscription<dynamic>? _controlWsSub;
  StreamSubscription<dynamic>? _monitorWsSub;
  WebSocket? _controlWs;
  WebSocket? _monitorWs;
  bool _realtimeDesired = false;
  bool _realtimeConnecting = false;
  bool _realtimeReconnectScheduled = false;
  int _nextRealtimeRequestId = 1;
  final Map<String, String> _pendingRealtimeCommands = <String, String>{};
  final Map<String, Timer> _pendingRealtimeTimeouts = <String, Timer>{};
  final Set<String> _quietRealtimeCommands = <String>{};
  Timer? _realtimeHeartbeatTimer;
  int _latestStatusTsMs = 0;
  DateTime? _lastRealtimePongAt;
  String? _lastError;
  Timer? _noticeTimer;
  String? _noticeMessage;
  Color _noticeColor = C.neonCyan;
  IconData _noticeIcon = Icons.info_outline;

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
  static const int _realtimeControlPort = 15491;
  static const int _realtimeMonitorPort = 15492;

  // ── Runtime data ──
  List<String> _allCache = const <String>[];
  List<String> _githubCache = const <String>[];
  List<String> _silverConfigs = const <String>[];
  List<String> _goldConfigs = const <String>[];
  List<HunterLatencyConfig> _balancerConfigs = const <HunterLatencyConfig>[];
  List<HunterLatencyConfig> _geminiConfigs = const <HunterLatencyConfig>[];
  List<HunterLatencyConfig> _allRecords = const <HunterLatencyConfig>[];
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
  int? _activeSystemProxyPort;

  // ── Pause / Speed Controls ──
  bool _isPaused = false;
  String _speedProfile = 'medium';
  int _speedThreads = 10;
  int _speedTimeout = 5;
  int _speedChunk = 15;
  String _draftSpeedProfile = 'medium';
  int _draftSpeedThreads = 10;
  int _draftSpeedTimeout = 5;
  bool _speedApplyInFlight = false;
  final TextEditingController _manualConfigCtl = TextEditingController();
  int _clearAgeHours = 168; // 7 days default

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
    _loadConfigSettings();
    _checkSystemProxy();
  }

  @override
  void dispose() {
    _pulseCtl.dispose();
    _ignitionCtl.dispose();
    windowManager.removeListener(this);
    _refreshTimer?.cancel();
    _realtimeHeartbeatTimer?.cancel();
    _noticeTimer?.cancel();
    _stdoutSub?.cancel();
    _stderrSub?.cancel();
    _realtimeDesired = false;
    _controlWsSub?.cancel();
    _monitorWsSub?.cancel();
    _controlWs?.close();
    _monitorWs?.close();
    _clearAllRealtimePendingCommands();
    _process?.kill();
    _systemTray?.destroy();
    for (final TextEditingController c in <TextEditingController>[
      _cliPath, _configPath, _xrayPath, _singboxPath, _mihomoPath, _torPath,
      _multiproxyPort, _geminiPort, _maxTotal, _maxWorkers, _scanLimit, _sleepSeconds,
      _searchCtl,
      _tgApiId, _tgApiHash, _tgPhone, _tgTargets, _tgLimit, _tgTimeout, _githubUrls,
      _manualConfigCtl,
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

  int _statusTimestampMs(Map<String, dynamic>? status) {
    if (status == null) return 0;
    final dynamic tsMs = status['ts_ms'];
    if (tsMs is num) return tsMs.toInt();
    final dynamic ts = status['ts'];
    if (ts is num) return (ts.toDouble() * 1000).round();
    return 0;
  }

  bool _acceptStatusTimestamp(Map<String, dynamic>? status) {
    final int incomingTs = _statusTimestampMs(status);
    if (incomingTs > 0 && incomingTs < _latestStatusTsMs) {
      return false;
    }
    if (incomingTs > _latestStatusTsMs) {
      _latestStatusTsMs = incomingTs;
    }
    return true;
  }

  void _clearRealtimePendingCommand(String requestId) {
    _pendingRealtimeTimeouts.remove(requestId)?.cancel();
    _pendingRealtimeCommands.remove(requestId);
    _quietRealtimeCommands.remove(requestId);
  }

  void _clearAllRealtimePendingCommands() {
    for (final Timer timer in _pendingRealtimeTimeouts.values) {
      timer.cancel();
    }
    _pendingRealtimeTimeouts.clear();
    _pendingRealtimeCommands.clear();
    _quietRealtimeCommands.clear();
  }

  void _registerRealtimePendingCommand(
    String requestId,
    String commandName, {
    bool quiet = false,
    Duration timeout = const Duration(seconds: 8),
  }) {
    _clearRealtimePendingCommand(requestId);
    _pendingRealtimeCommands[requestId] = commandName;
    if (quiet) {
      _quietRealtimeCommands.add(requestId);
    }
    _pendingRealtimeTimeouts[requestId] = Timer(timeout, () {
      final String? pendingCommand = _pendingRealtimeCommands.remove(requestId);
      final bool wasQuiet = _quietRealtimeCommands.remove(requestId);
      _pendingRealtimeTimeouts.remove(requestId);
      if (pendingCommand == null) return;
      if (pendingCommand == 'ping') {
        _log('warn', 'Realtime heartbeat timed out');
        unawaited(_disconnectRealtimeSockets().then((_) => _scheduleRealtimeReconnect()));
        return;
      }
      if (!wasQuiet) {
        _log('warn', 'CLI $pendingCommand [$requestId]: timed out');
        if (mounted) {
          setState(() => _lastError = 'Command timed out: $pendingCommand');
        }
      }
      if (_realtimeDesired && _process != null) {
        _sendCommand(<String, dynamic>{'command': 'get_status'}, expectResponse: false, quiet: true);
      }
    });
  }

  // ━━━━━ Real-time status from C++ stdout ━━━━━
  void _handleStatusLine(String jsonStr) {
    try {
      final Map<String, dynamic> s = jsonDecode(jsonStr) as Map<String, dynamic>;
      if (!mounted) return;
      if (!_acceptStatusTimestamp(s)) return;
      setState(() {
        final Map<String, dynamic> nextStatus = Map<String, dynamic>.from(_status ?? <String, dynamic>{});
        _isPaused = s['paused'] == true;
        _reconcileSpeedState(
          profile: s['speed_profile'] is String ? s['speed_profile'] as String : _speedProfile,
          threads: s['speed_threads'] is num ? (s['speed_threads'] as num).toInt() : _speedThreads,
          timeout: s['speed_timeout'] is num ? (s['speed_timeout'] as num).toInt() : _speedTimeout,
        );
        nextStatus['ts'] = s['ts'];
        nextStatus['ts_ms'] = s['ts_ms'];
        nextStatus['phase'] = s['phase'];
        nextStatus['paused'] = s['paused'];
        nextStatus['uptime_s'] = s['uptime_s'];
        nextStatus['db'] = <String, dynamic>{
          'total': s['db_total'] ?? 0,
          'alive': s['db_alive'] ?? 0,
          'tested_unique': s['db_tested'] ?? 0,
        };
        nextStatus['speed'] = <String, dynamic>{
          'profile': s['speed_profile'],
          'max_threads': s['speed_threads'],
          'test_timeout_s': s['speed_timeout'],
        };
        _status = nextStatus;
        _syncRuntimeControllers(nextStatus['runtime_config'] is Map<String, dynamic>
            ? nextStatus['runtime_config'] as Map<String, dynamic>
            : null);
        _lastActivityAt = DateTime.now();
      });
    } catch (_) {}
  }

  void _applyRealtimeStatusSnapshot(Map<String, dynamic> s) {
    if (!mounted) return;
    if (!_acceptStatusTimestamp(s)) return;
    final Map<String, dynamic>? speed = s['speed'] is Map<String, dynamic>
        ? s['speed'] as Map<String, dynamic>
        : null;
    final Map<String, dynamic>? runtime = s['runtime_config'] is Map<String, dynamic>
        ? s['runtime_config'] as Map<String, dynamic>
        : null;
    setState(() {
      _status = Map<String, dynamic>.from(s);
      _isPaused = s['paused'] == true;
      if (speed != null) {
        _reconcileSpeedState(
          profile: (speed['profile'] as String?) ?? _speedProfile,
          threads: (speed['max_threads'] as num?)?.toInt() ?? _speedThreads,
          timeout: (speed['test_timeout_s'] as num?)?.toInt() ?? _speedTimeout,
          chunk: (speed['chunk_size'] as num?)?.toInt() ?? _speedChunk,
        );
      }
      _syncRuntimeControllers(runtime);
      _lastActivityAt = DateTime.now();
    });
  }

  void _handleRealtimeEvent(String message) {
    try {
      final Map<String, dynamic> event = jsonDecode(message) as Map<String, dynamic>;
      final String type = event['type'] is String ? event['type'] as String : '';
      final dynamic payload = event['payload'];
      if (type == 'status' && payload is Map<String, dynamic>) {
        _applyRealtimeStatusSnapshot(Map<String, dynamic>.from(payload));
        return;
      }
      if (type == 'command_result') {
        final Map<String, dynamic> result = payload is Map<String, dynamic>
            ? Map<String, dynamic>.from(payload)
            : Map<String, dynamic>.from(event);
        final String requestId = result['request_id'] is String ? result['request_id'] as String : '';
        final bool quiet = result['quiet'] == true || (requestId.isNotEmpty && _quietRealtimeCommands.contains(requestId));
        final Map<String, dynamic>? status = event['status'] is Map<String, dynamic>
            ? event['status'] as Map<String, dynamic>
            : (result['status'] is Map<String, dynamic>)
                ? result['status'] as Map<String, dynamic>
            : null;
        final bool ok = result['ok'] == true;
        final String fallbackCommand = requestId.isNotEmpty
            ? (_pendingRealtimeCommands[requestId] ?? 'command')
            : 'command';
        final String command = result['command'] is String
            ? result['command'] as String
            : fallbackCommand;
        if (requestId.isNotEmpty) {
          _clearRealtimePendingCommand(requestId);
        }
        final String resultMessage = result['message'] is String ? result['message'] as String : '';
        if (status != null) {
          _applyRealtimeStatusSnapshot(Map<String, dynamic>.from(status));
        }
        if (command == 'ping' && ok && resultMessage == 'pong') {
          _lastRealtimePongAt = DateTime.now();
        }
        if (!quiet && resultMessage.isNotEmpty) {
          final String prefix = requestId.isEmpty ? 'CLI $command' : 'CLI $command [$requestId]';
          _log(ok ? 'sys' : 'warn', '$prefix: $resultMessage');
        }
        if (!quiet && mounted && (command == 'import_config_file' || command == 'export_config_db')) {
          _showNotice(
            resultMessage.isEmpty ? (ok ? 'Done' : 'Operation failed') : resultMessage,
            color: ok ? C.neonGreen : C.neonRed,
            icon: ok
                ? (command == 'export_config_db' ? Icons.download_done_rounded : Icons.upload_file_rounded)
                : Icons.error_outline,
            duration: const Duration(seconds: 5),
          );
        }
        if (!quiet && !ok && mounted) {
          setState(() => _lastError = resultMessage.isEmpty ? 'Command failed: $command' : resultMessage);
        }
        if (ok && (command == 'import_config_file' || command == 'add_configs')) {
          unawaited(_refresh());
        }
        return;
      }
      if (type == 'logs' && payload is Map<String, dynamic>) {
        final dynamic rawLines = payload['lines'];
        if (rawLines is! List) return;
        final List<MapEntry<String, String>> prepared = <MapEntry<String, String>>[];
        for (final dynamic raw in rawLines) {
          if (raw is! String) continue;
          final MapEntry<String, String>? entry = _prepareLogEntry(_classifyRuntimeLogSource(raw, 'out'), raw);
          if (entry != null) prepared.add(entry);
        }
        _appendPreparedLogEntries(prepared);
      }
    } catch (_) {}
  }

  void _scheduleRealtimeReconnect() {
    if (_realtimeReconnectScheduled || !_realtimeDesired || _process == null) return;
    _realtimeReconnectScheduled = true;
    unawaited(Future<void>.delayed(const Duration(seconds: 1), () async {
      _realtimeReconnectScheduled = false;
      if (!_realtimeDesired || _process == null) return;
      if (_monitorWs == null || _controlWs == null) {
        await _connectRealtimeSockets();
      }
    }));
  }

  Future<void> _disconnectRealtimeSockets() async {
    _realtimeReconnectScheduled = false;
    _realtimeHeartbeatTimer?.cancel();
    _realtimeHeartbeatTimer = null;
    await _controlWsSub?.cancel();
    _controlWsSub = null;
    await _monitorWsSub?.cancel();
    _monitorWsSub = null;
    try { await _monitorWs?.close(); } catch (_) {}
    try { await _controlWs?.close(); } catch (_) {}
    _monitorWs = null;
    _controlWs = null;
  }

  void _sendRealtimeHeartbeat() {
    final WebSocket? ws = _controlWs;
    if (ws == null) return;
    final String requestId = 'hb_${DateTime.now().microsecondsSinceEpoch}';
    _registerRealtimePendingCommand(requestId, 'ping', quiet: true);
    try {
      ws.add(jsonEncode(<String, dynamic>{
        'command': 'ping',
        'request_id': requestId,
        'quiet': true,
      }));
    } catch (_) {
      _clearRealtimePendingCommand(requestId);
      _scheduleRealtimeReconnect();
    }
  }

  void _startRealtimeHeartbeat() {
    _realtimeHeartbeatTimer?.cancel();
    _lastRealtimePongAt = DateTime.now();
    _realtimeHeartbeatTimer = Timer.periodic(const Duration(seconds: 10), (_) {
      if (!_realtimeDesired || _process == null || _controlWs == null) return;
      final DateTime now = DateTime.now();
      final DateTime lastPong = _lastRealtimePongAt ?? now;
      if (now.difference(lastPong) > const Duration(seconds: 25)) {
        _log('warn', 'Realtime heartbeat stale, reconnecting');
        unawaited(_disconnectRealtimeSockets().then((_) => _scheduleRealtimeReconnect()));
        return;
      }
      _sendRealtimeHeartbeat();
    });
  }

  Future<void> _connectRealtimeSockets() async {
    if (_realtimeConnecting || !_realtimeDesired || _process == null) return;
    _realtimeConnecting = true;
    await _disconnectRealtimeSockets();
    for (int attempt = 0; attempt < 25; attempt++) {
      if (!_realtimeDesired || _process == null) {
        _realtimeConnecting = false;
        return;
      }
      try {
        _controlWs = await WebSocket.connect('ws://127.0.0.1:$_realtimeControlPort');
        _monitorWs = await WebSocket.connect('ws://127.0.0.1:$_realtimeMonitorPort');
        _controlWsSub = _controlWs!.listen(
          (dynamic data) {
            if (data is String) _handleRealtimeEvent(data);
          },
          onDone: () {
            _controlWs = null;
            _scheduleRealtimeReconnect();
          },
          onError: (_) {
            _controlWs = null;
            _scheduleRealtimeReconnect();
          },
          cancelOnError: false,
        );
        _monitorWsSub = _monitorWs!.listen(
          (dynamic data) {
            if (data is String) _handleRealtimeEvent(data);
          },
          onDone: () {
            _monitorWs = null;
            _scheduleRealtimeReconnect();
          },
          onError: (_) {
            _monitorWs = null;
            _scheduleRealtimeReconnect();
          },
          cancelOnError: false,
        );
        _startRealtimeHeartbeat();
        _sendCommand(<String, dynamic>{'command': 'get_status'}, expectResponse: false, quiet: true);
        _realtimeConnecting = false;
        _log('sys', 'Realtime WS connected');
        return;
      } catch (_) {
        await _controlWsSub?.cancel();
        _controlWsSub = null;
        try { await _monitorWs?.close(); } catch (_) {}
        try { await _controlWs?.close(); } catch (_) {}
        _monitorWs = null;
        _controlWs = null;
        await Future<void>.delayed(const Duration(milliseconds: 250));
      }
    }
    _realtimeConnecting = false;
    _log('warn', 'Realtime WS unavailable, using stdout/file fallback');
  }

  bool get _hasPendingSpeedChanges {
    final bool presetChanged = _draftSpeedProfile != 'custom' && _draftSpeedProfile != _speedProfile;
    final bool manualChanged = _draftSpeedThreads != _speedThreads || _draftSpeedTimeout != _speedTimeout;
    return presetChanged || manualChanged;
  }

  int _presetThreadsFor(String profile) {
    switch (profile) {
      case 'low':
        return 4;
      case 'high':
        return 20;
      default:
        return 10;
    }
  }

  int _presetTimeoutFor(String profile) {
    switch (profile) {
      case 'low':
        return 8;
      case 'high':
        return 3;
      default:
        return 5;
    }
  }

  int _projectedChunkForDraft() {
    if (_draftSpeedProfile == 'low') {
      return (_draftSpeedThreads < 2 ? 2 : (_draftSpeedThreads > 8 ? 8 : _draftSpeedThreads));
    }
    if (_draftSpeedProfile == 'high') {
      final int doubled = _draftSpeedThreads * 2;
      return doubled < 8 ? 8 : (doubled > 30 ? 30 : doubled);
    }
    if (_draftSpeedProfile == 'medium') {
      return _draftSpeedThreads < 4 ? 4 : (_draftSpeedThreads > 15 ? 15 : _draftSpeedThreads);
    }
    return _draftSpeedThreads < 1 ? 1 : (_draftSpeedThreads > 50 ? 50 : _draftSpeedThreads);
  }

  Duration? _projectedScanTimeForDraft() {
    final int pending = ((_status?['pending_unique'] as num?)?.toInt() ?? 0) > 0
        ? (_status?['pending_unique'] as num).toInt()
        : (((( _status?['db'] as Map<String, dynamic>?)?['total'] as num?)?.toInt() ?? 0) - (((_status?['db'] as Map<String, dynamic>?)?['tested_unique'] as num?)?.toInt() ?? 0));
    if (pending <= 0) return null;
    final int concurrency = _projectedChunkForDraft();
    final double perWaveSeconds = _draftSpeedTimeout + 0.75;
    final double projectedSeconds = (pending / concurrency) * perWaveSeconds;
    final int rounded = projectedSeconds.ceil();
    return Duration(seconds: rounded < 1 ? 1 : rounded);
  }

  void _reconcileSpeedState({required String profile, required int threads, required int timeout, int? chunk}) {
    final bool hadPendingDraft = _hasPendingSpeedChanges;
    _speedProfile = profile;
    _speedThreads = threads;
    _speedTimeout = timeout;
    if (chunk != null) _speedChunk = chunk;
    final bool profileMatch = _draftSpeedProfile != 'custom' && _draftSpeedProfile == _speedProfile;
    final bool manualMatch = _draftSpeedThreads == _speedThreads && _draftSpeedTimeout == _speedTimeout;
    if (_speedApplyInFlight && (profileMatch || manualMatch)) {
      _speedApplyInFlight = false;
      _draftSpeedProfile = _speedProfile;
      _draftSpeedThreads = _speedThreads;
      _draftSpeedTimeout = _speedTimeout;
      return;
    }
    if (!hadPendingDraft) {
      _draftSpeedProfile = _speedProfile;
      _draftSpeedThreads = _speedThreads;
      _draftSpeedTimeout = _speedTimeout;
    }
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
      // Sound notification when new alive configs are found
      final int newAlive = snap.goldConfigs.length + snap.silverConfigs.length;
      final int oldAlive = _goldConfigs.length + _silverConfigs.length;
      if (newAlive > oldAlive && oldAlive > 0) {
        _playNewConfigSound();
      }

      // Parse pause + speed from status
      final Map<String, dynamic>? snapshotStatus = snap.status;
      final bool shouldApplySnapshotStatus = snapshotStatus != null && _acceptStatusTimestamp(snapshotStatus);
      final bool paused = shouldApplySnapshotStatus ? (snapshotStatus['paused'] == true) : _isPaused;
      final Map<String, dynamic>? spd = shouldApplySnapshotStatus ? snapshotStatus['speed'] as Map<String, dynamic>? : null;
      final Map<String, dynamic>? runtime = shouldApplySnapshotStatus ? snapshotStatus['runtime_config'] as Map<String, dynamic>? : null;

      setState(() {
        _allCache = snap.allCache;
        _githubCache = snap.githubCache;
        _silverConfigs = snap.silverConfigs;
        _goldConfigs = snap.goldConfigs;
        _balancerConfigs = snap.balancerConfigs;
        _geminiConfigs = snap.geminiConfigs;
        _allRecords = snap.allRecords;
        if (shouldApplySnapshotStatus) {
          _status = snap.status;
        }
        _history = nextHistory;
        _engines = eng;
        _lastRefresh = now;
        _refreshTick += 1;
        if (shouldApplySnapshotStatus) {
          _isPaused = paused;
        }
        if (spd != null) {
          _reconcileSpeedState(
            profile: (spd['profile'] as String?) ?? _speedProfile,
            threads: (spd['max_threads'] as num?)?.toInt() ?? _speedThreads,
            timeout: (spd['test_timeout_s'] as num?)?.toInt() ?? _speedTimeout,
            chunk: (spd['chunk_size'] as num?)?.toInt() ?? _speedChunk,
          );
        }
        _syncRuntimeControllers(runtime);
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
      _realtimeDesired = true;
      unawaited(_loadDocs());

      _stdoutSub = p.stdout.transform(utf8.decoder).transform(const LineSplitter())
          .listen((String line) {
        if (line.startsWith('##STATUS##')) {
          _handleStatusLine(line.substring(10));
        } else if (line.startsWith('##CMD##')) {
          _handleRealtimeEvent(line.substring(7));
        } else {
          _log('out', line);
        }
      });
      _stderrSub = p.stderr.transform(utf8.decoder).transform(const LineSplitter())
          .listen((String line) => _log('err', line));

      unawaited(p.exitCode.then((int code) {
        _log('sys', 'Process exited: $code');
        _realtimeDesired = false;
        unawaited(_disconnectRealtimeSockets());
        _clearAllRealtimePendingCommands();
        if (mounted) setState(() { _process = null; _workingDir = null; _state = HunterRunState.stopped; });
      }));
      unawaited(_connectRealtimeSockets());
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
      _realtimeDesired = false;
      // Send stop via stdin for instant response
      _sendCommand({'command': 'stop'});
      final Directory wd = _effectiveWd();
      await Directory('${wd.path}\\runtime').create(recursive: true);
      File('${wd.path}\\runtime\\stop.flag').writeAsStringSync('stop\n', flush: true);
      _log('sys', 'Stop command sent');

      final Process? p = _process;
      if (p != null) {
        await p.exitCode.timeout(const Duration(seconds: 15), onTimeout: () {
          _log('sys', 'Timeout, killing...');
          p.kill(ProcessSignal.sigterm);
          return -1;
        });
      }
      await _disconnectRealtimeSockets();
      _clearAllRealtimePendingCommands();
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
    _showNotice(label ?? 'Copied!', color: C.neonCyan, icon: Icons.content_copy_rounded, duration: const Duration(seconds: 2));
  }

  Future<void> _copyLines(List<String> lines, {String? label}) async {
    if (lines.isEmpty) {
      if (!mounted) return;
      _showNotice('Nothing to copy', color: C.neonAmber, icon: Icons.info_outline);
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

  void _showNotice(
    String message, {
    Color color = C.neonCyan,
    IconData icon = Icons.info_outline,
    Duration duration = const Duration(seconds: 3),
  }) {
    _noticeTimer?.cancel();
    if (!mounted) return;
    setState(() {
      _noticeMessage = message;
      _noticeColor = color;
      _noticeIcon = icon;
    });
    _noticeTimer = Timer(duration, () {
      if (!mounted) return;
      setState(() {
        if (_noticeMessage == message) {
          _noticeMessage = null;
        }
      });
    });
  }

  Widget _buildNoticeBanner() {
    if (_noticeMessage == null || _noticeMessage!.trim().isEmpty) {
      return const SizedBox.shrink();
    }
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 10, 16, 0),
      child: Container(
        width: double.infinity,
        padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
        decoration: BoxDecoration(
          color: _noticeColor.withValues(alpha: 0.12),
          borderRadius: BorderRadius.circular(10),
          border: Border.all(color: _noticeColor.withValues(alpha: 0.35)),
        ),
        child: Row(
          children: <Widget>[
            Icon(_noticeIcon, color: _noticeColor, size: 18),
            const SizedBox(width: 10),
            Expanded(
              child: Text(
                _noticeMessage!,
                style: TextStyle(color: _noticeColor, fontSize: 12, fontWeight: FontWeight.w700),
              ),
            ),
            const SizedBox(width: 8),
            InkWell(
              onTap: () {
                _noticeTimer?.cancel();
                if (!mounted) return;
                setState(() => _noticeMessage = null);
              },
              borderRadius: BorderRadius.circular(12),
              child: Padding(
                padding: const EdgeInsets.all(2),
                child: Icon(Icons.close, color: _noticeColor, size: 16),
              ),
            ),
          ],
        ),
      ),
    );
  }

  void _resetFindingsState() {
    _silverConfigs = const <String>[];
    _goldConfigs = const <String>[];
    _balancerConfigs = const <HunterLatencyConfig>[];
    _geminiConfigs = const <HunterLatencyConfig>[];
    _allRecords = const <HunterLatencyConfig>[];
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
  Future<void> _loadConfigSettings() async {
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
        _xrayPath.text = (decoded['xray_path'] ?? 'bin\\xray.exe').toString();
        _singboxPath.text = (decoded['singbox_path'] ?? 'bin\\sing-box.exe').toString();
        _mihomoPath.text = (decoded['mihomo_path'] ?? 'bin\\mihomo-windows-amd64-compatible.exe').toString();
        _torPath.text = (decoded['tor_path'] ?? 'bin\\tor.exe').toString();
        _multiproxyPort.text = (decoded['multiproxy_port'] ?? '10808').toString();
        _geminiPort.text = (decoded['gemini_port'] ?? '10809').toString();
        _maxTotal.text = (decoded['max_total'] ?? '1000').toString();
        _maxWorkers.text = (decoded['max_workers'] ?? '12').toString();
        _scanLimit.text = (decoded['scan_limit'] ?? '50').toString();
        _sleepSeconds.text = (decoded['sleep_seconds'] ?? '300').toString();
        _tgEnabled = decoded['telegram_enabled'] == true;
        _tgApiId.text = (decoded['telegram_api_id'] ?? '').toString();
        _tgApiHash.text = (decoded['telegram_api_hash'] ?? '').toString();
        _tgPhone.text = (decoded['telegram_phone'] ?? '').toString();
        _tgLimit.text = (decoded['telegram_limit'] ?? '50').toString();
        _tgTimeout.text = (decoded['telegram_timeout_ms'] ?? '12000').toString();
        if (decoded['targets'] is List) {
          _tgTargets.text = (decoded['targets'] as List<dynamic>).map((dynamic e) => e.toString()).join('\n');
        }
        if (decoded['github_urls'] is List) {
          _githubUrls.text = (decoded['github_urls'] as List<dynamic>).map((dynamic e) => e.toString()).join('\n');
        } else if (_githubUrls.text.isEmpty) {
          _githubUrls.text = _defaultGithubUrls().join('\n');
        }
      });
    } catch (_) {}
  }

  List<String> _defaultGithubUrls() {
    return const <String>[
      'https://raw.githubusercontent.com/barry-far/V2ray-config/main/All_Configs_Sub.txt',
      'https://raw.githubusercontent.com/ebrasha/free-v2ray-public-list/refs/heads/main/all_extracted_configs.txt',
      'https://raw.githubusercontent.com/miladtahanian/V2RayCFGDumper/refs/heads/main/sub.txt',
      'https://raw.githubusercontent.com/Epodonios/v2ray-configs/main/All_Configs_Sub.txt',
      'https://raw.githubusercontent.com/mahdibland/V2RayAggregator/master/sub/sub_merge.txt',
      'https://raw.githubusercontent.com/coldwater-10/V2ray-Config-Lite/main/All_Configs_Sub.txt',
      'https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/all_sub.txt',
      'https://raw.githubusercontent.com/M-Mashreghi/Free-V2ray-Collector/main/All_Configs_Sub.txt',
      'https://raw.githubusercontent.com/NiREvil/vless/main/subscription.txt',
      'https://raw.githubusercontent.com/ALIILAPRO/v2rayNG-Config/main/sub.txt',
      'https://raw.githubusercontent.com/skywrt/v2ray-configs/main/All_Configs_Sub.txt',
      'https://raw.githubusercontent.com/longlon/v2ray-config/main/All_Configs_Sub.txt',
      'https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/mix',
      'https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/base64/mix',
      'https://raw.githubusercontent.com/mfuu/v2ray/master/v2ray',
      'https://raw.githubusercontent.com/peasoft/NoMoreWalls/master/list_raw.txt',
      'https://raw.githubusercontent.com/freefq/free/master/v2',
      'https://raw.githubusercontent.com/aiboboxx/v2rayfree/main/v2',
      'https://raw.githubusercontent.com/ermaozi/get_subscribe/main/subscribe/v2ray.txt',
      'https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub',
    ];
  }

  Future<void> _saveTelegramSettings() async {
    _sendCommand(<String, dynamic>{
      'command': 'update_runtime_settings',
      'telegram_enabled': _tgEnabled,
      'telegram_api_id': _tgApiId.text.trim(),
      'telegram_api_hash': _tgApiHash.text.trim(),
      'telegram_phone': _tgPhone.text.trim(),
      'telegram_limit': int.tryParse(_tgLimit.text.trim()) ?? 50,
      'telegram_timeout_ms': int.tryParse(_tgTimeout.text.trim()) ?? 12000,
      'targets_text': _tgTargets.text,
    });
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
      _showNotice('Telegram settings saved', color: C.neonGreen, icon: Icons.save_outlined);
    }
  }

  Future<void> _saveRuntimeSettings() async {
    _sendCommand(<String, dynamic>{
      'command': 'update_runtime_settings',
      'xray_path': _xrayPath.text.trim().isEmpty ? 'bin\\xray.exe' : _xrayPath.text.trim(),
      'singbox_path': _singboxPath.text.trim().isEmpty ? 'bin\\sing-box.exe' : _singboxPath.text.trim(),
      'mihomo_path': _mihomoPath.text.trim().isEmpty ? 'bin\\mihomo-windows-amd64-compatible.exe' : _mihomoPath.text.trim(),
      'tor_path': _torPath.text.trim().isEmpty ? 'bin\\tor.exe' : _torPath.text.trim(),
      'multiproxy_port': int.tryParse(_multiproxyPort.text.trim()) ?? 10808,
      'gemini_port': int.tryParse(_geminiPort.text.trim()) ?? 10809,
      'max_total': int.tryParse(_maxTotal.text.trim()) ?? 1000,
      'max_workers': int.tryParse(_maxWorkers.text.trim()) ?? 12,
      'scan_limit': int.tryParse(_scanLimit.text.trim()) ?? 50,
      'sleep_seconds': int.tryParse(_sleepSeconds.text.trim()) ?? 300,
    });
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
    root['xray_path'] = _xrayPath.text.trim().isEmpty ? 'bin\\xray.exe' : _xrayPath.text.trim();
    root['singbox_path'] = _singboxPath.text.trim().isEmpty ? 'bin\\sing-box.exe' : _singboxPath.text.trim();
    root['mihomo_path'] = _mihomoPath.text.trim().isEmpty ? 'bin\\mihomo-windows-amd64-compatible.exe' : _mihomoPath.text.trim();
    root['tor_path'] = _torPath.text.trim().isEmpty ? 'bin\\tor.exe' : _torPath.text.trim();
    root['multiproxy_port'] = int.tryParse(_multiproxyPort.text.trim()) ?? 10808;
    root['gemini_port'] = int.tryParse(_geminiPort.text.trim()) ?? 10809;
    root['max_total'] = int.tryParse(_maxTotal.text.trim()) ?? 1000;
    root['max_workers'] = int.tryParse(_maxWorkers.text.trim()) ?? 12;
    root['scan_limit'] = int.tryParse(_scanLimit.text.trim()) ?? 50;
    root['sleep_seconds'] = int.tryParse(_sleepSeconds.text.trim()) ?? 300;
    await f.parent.create(recursive: true);
    await f.writeAsString(jsonEncode(root), flush: true);
    if (mounted) {
      _showNotice('Runtime settings saved', color: C.neonGreen, icon: Icons.save_outlined);
    }
  }

  Future<void> _saveGithubUrls() async {
    _sendCommand(<String, dynamic>{
      'command': 'update_runtime_settings',
      'github_urls_text': _githubUrls.text,
    });
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
    root['github_urls'] = _githubUrls.text
        .split(RegExp(r'[\r\n]+'))
        .map((String s) => s.trim())
        .where((String s) => s.isNotEmpty && s.startsWith('http'))
        .toList();
    await f.parent.create(recursive: true);
    await f.writeAsString(jsonEncode(root), flush: true);
    if (mounted) {
      _showNotice('Saved ${(root['github_urls'] as List).length} GitHub URLs', color: C.neonGreen, icon: Icons.link);
    }
  }

  void _resetGithubUrls() {
    setState(() {
      _githubUrls.text = _defaultGithubUrls().join('\n');
    });
  }

  void _runCycleNow() {
    _sendCommand(<String, dynamic>{'command': 'run_cycle'});
  }

  void _refreshProvisionedPortsNow() {
    _sendCommand(<String, dynamic>{'command': 'refresh_ports'});
  }

  void _reprovisionPortsNow() {
    _sendCommand(<String, dynamic>{'command': 'reprovision_ports'});
  }

  void _loadRawFilesNow() {
    _sendCommand(<String, dynamic>{'command': 'load_raw_files'});
  }

  void _loadBundleFilesNow() {
    _sendCommand(<String, dynamic>{'command': 'load_bundle_files'});
  }

  void _detectCensorshipNow() {
    _sendCommand(<String, dynamic>{'command': 'detect_censorship'});
  }

  void _syncRuntimeControllers(Map<String, dynamic>? runtime) {
    if (runtime == null) return;
    final String xrayPath = (runtime['xray_path'] ?? '').toString();
    final String singboxPath = (runtime['singbox_path'] ?? '').toString();
    final String mihomoPath = (runtime['mihomo_path'] ?? '').toString();
    final String torPath = (runtime['tor_path'] ?? '').toString();
    final String multiproxyPort = (runtime['multiproxy_port'] ?? '').toString();
    final String geminiPort = (runtime['gemini_port'] ?? '').toString();
    final String maxTotal = (runtime['max_total'] ?? '').toString();
    final String maxWorkers = (runtime['max_workers'] ?? '').toString();
    final String scanLimit = (runtime['scan_limit'] ?? '').toString();
    final String sleepSeconds = (runtime['sleep_seconds'] ?? '').toString();
    if (xrayPath.isNotEmpty) _xrayPath.text = xrayPath;
    if (singboxPath.isNotEmpty) _singboxPath.text = singboxPath;
    if (mihomoPath.isNotEmpty) _mihomoPath.text = mihomoPath;
    if (torPath.isNotEmpty) _torPath.text = torPath;
    if (multiproxyPort.isNotEmpty) _multiproxyPort.text = multiproxyPort;
    if (geminiPort.isNotEmpty) _geminiPort.text = geminiPort;
    if (maxTotal.isNotEmpty) _maxTotal.text = maxTotal;
    if (maxWorkers.isNotEmpty) _maxWorkers.text = maxWorkers;
    if (scanLimit.isNotEmpty) _scanLimit.text = scanLimit;
    if (sleepSeconds.isNotEmpty) _sleepSeconds.text = sleepSeconds;
  }

  // ━━━━━ Sound Notification ━━━━━
  void _playNewConfigSound() {
    // Use Windows system beep via PowerShell (non-blocking)
    Process.start('powershell', <String>['-NoProfile', '-Command', '[console]::beep(800,200);[console]::beep(1000,200)'],
      runInShell: false).then((_) {}).catchError((_) {});
  }

  // ━━━━━ Command Sending (to C++ orchestrator via stdin, file fallback) ━━━━━
  void _sendCommand(Map<String, dynamic> cmd, {bool expectResponse = true, bool quiet = false}) {
    final Map<String, dynamic> enriched = Map<String, dynamic>.from(cmd);
    final String requestId;
    if (enriched['request_id'] is String && (enriched['request_id'] as String).isNotEmpty) {
      requestId = enriched['request_id'] as String;
    } else {
      requestId = 'cmd_${_nextRealtimeRequestId++}';
      enriched['request_id'] = requestId;
    }
    if (quiet) {
      enriched['quiet'] = true;
    }
    final String commandName = enriched['command'] is String ? enriched['command'] as String : 'command';
    if (expectResponse) {
      _registerRealtimePendingCommand(requestId, commandName, quiet: quiet);
    }
    final String json = jsonEncode(enriched);
    if (_controlWs != null) {
      try {
        _controlWs!.add(json);
        return;
      } catch (_) {}
    }
    if (_process != null) {
      try {
        _process!.stdin.writeln(json);
        return;
      } catch (_) {}
    }
    // File fallback for standalone CLI
    try {
      final Directory wd = _effectiveWd();
      File('${wd.path}\\runtime\\hunter_command.json').writeAsStringSync(json);
    } catch (_) {
      _clearRealtimePendingCommand(requestId);
    }
  }

  void _togglePause() {
    if (_isPaused) {
      _sendCommand({'command': 'resume'});
    } else {
      _sendCommand({'command': 'pause'});
    }
  }

  void _setSpeedProfile(String profile) {
    setState(() {
      _draftSpeedProfile = profile;
      _draftSpeedThreads = _presetThreadsFor(profile);
      _draftSpeedTimeout = _presetTimeoutFor(profile);
    });
  }

  void _setThreads(int val) {
    setState(() {
      _draftSpeedThreads = val;
      _draftSpeedProfile = 'custom';
    });
  }

  void _setTimeout(int val) {
    setState(() {
      _draftSpeedTimeout = val;
      _draftSpeedProfile = 'custom';
    });
  }

  void _applySpeedChanges() {
    if (_state != HunterRunState.running || !_hasPendingSpeedChanges || _speedApplyInFlight) return;
    final String draftProfile = _draftSpeedProfile;
    final int draftThreads = _draftSpeedThreads;
    final int draftTimeout = _draftSpeedTimeout;
    final bool applyPreset = draftProfile != 'custom' &&
        draftThreads == _presetThreadsFor(draftProfile) &&
        draftTimeout == _presetTimeoutFor(draftProfile);
    if (applyPreset) {
      _sendCommand({'command': 'speed_profile', 'value': draftProfile});
    } else {
      _sendCommand({
        'command': 'set_speed',
        'threads': draftThreads,
        'timeout': draftTimeout,
        'chunk_size': _projectedChunkForDraft(),
      });
    }
    setState(() => _speedApplyInFlight = true);
    if (mounted) {
      _showNotice('Applying speed changes in CLI...', color: C.neonAmber, icon: Icons.tune);
    }
  }

  void _clearOldConfigs() {
    _sendCommand({'command': 'clear_old', 'hours': _clearAgeHours});
    if (mounted) {
      _showNotice('Clearing configs older than $_clearAgeHours hours...', color: C.neonAmber, icon: Icons.cleaning_services_outlined);
    }
  }

  void _addManualConfigs() {
    final String text = _manualConfigCtl.text.trim();
    if (text.isEmpty) return;
    _sendCommand({'command': 'add_configs', 'configs': text});
    _manualConfigCtl.clear();
    if (mounted) {
      _showNotice('Manual configs submitted for testing', color: C.neonGreen, icon: Icons.playlist_add_check_circle_outlined);
    }
  }

  Future<String?> _promptForPath({
    required String title,
    required String label,
    required String hint,
    required String initialValue,
    required String actionLabel,
  }) async {
    final TextEditingController ctl = TextEditingController(text: initialValue);
    final String? result = await showDialog<String>(
      context: context,
      builder: (BuildContext context) {
        return AlertDialog(
          backgroundColor: C.card,
          title: Text(title, style: const TextStyle(color: C.txt1, fontSize: 16, fontWeight: FontWeight.w700)),
          content: SizedBox(
            width: 640,
            child: TextField(
              controller: ctl,
              autofocus: true,
              style: const TextStyle(color: C.txt1, fontSize: 12, fontFamily: 'Consolas'),
              decoration: InputDecoration(
                labelText: label,
                hintText: hint,
                labelStyle: const TextStyle(color: C.txt3),
                hintStyle: TextStyle(color: C.txt3.withValues(alpha: 0.5)),
                filled: true,
                fillColor: C.surface,
                border: OutlineInputBorder(borderRadius: BorderRadius.circular(10), borderSide: const BorderSide(color: C.border)),
                enabledBorder: OutlineInputBorder(borderRadius: BorderRadius.circular(10), borderSide: const BorderSide(color: C.border)),
                focusedBorder: OutlineInputBorder(borderRadius: BorderRadius.circular(10), borderSide: const BorderSide(color: C.neonCyan)),
              ),
            ),
          ),
          actions: <Widget>[
            TextButton(
              onPressed: () => Navigator.of(context).pop(),
              child: const Text('Cancel'),
            ),
            FilledButton(
              onPressed: () => Navigator.of(context).pop(ctl.text.trim()),
              style: FilledButton.styleFrom(backgroundColor: C.neonCyan, foregroundColor: C.bg),
              child: Text(actionLabel),
            ),
          ],
        );
      },
    );
    ctl.dispose();
    if (result == null || result.trim().isEmpty) return null;
    return result.trim();
  }

  Future<void> _importConfigFile() async {
    final Directory wd = _effectiveWd();
    final String? path = await pickImportTextFile(initialDirectory: '${wd.path}\\config');
    if (path == null) return;
    _sendCommand(<String, dynamic>{'command': 'import_config_file', 'path': path});
    if (mounted) {
      _showNotice('Importing config file into the database...', color: C.neonAmber, icon: Icons.upload_file_rounded);
    }
  }

  Future<void> _exportConfigDb() async {
    final Directory wd = _effectiveWd();
    final String? path = await _promptForPath(
      title: 'Export Config Database',
      label: 'Destination path',
      hint: '${wd.path}\\runtime\\HUNTER_config_db_export.txt',
      initialValue: '${wd.path}\\runtime\\HUNTER_config_db_export.txt',
      actionLabel: 'Export',
    );
    if (path == null) return;
    _sendCommand(<String, dynamic>{'command': 'export_config_db', 'path': path});
    if (mounted) {
      _showNotice('Exporting full config database...', color: C.neonAmber, icon: Icons.file_download_outlined);
    }
  }

  // ━━━━━ System Proxy ━━━━━
  Future<void> _checkSystemProxy() async {
    final int? port = await getWindowsSystemProxyPort();
    if (mounted) setState(() => _activeSystemProxyPort = port);
  }

  Future<void> _setSystemProxy(int port) async {
    // Find the HTTP port for this SOCKS port from provisioned ports
    int httpPort = 0;
    final List<Map<String, dynamic>> ports = _parseProvisionedPorts();
    for (final Map<String, dynamic> p in ports) {
      if ((p['port'] is num) && (p['port'] as num).toInt() == port) {
        httpPort = (p['http_port'] is num) ? (p['http_port'] as num).toInt() : 0;
        break;
      }
    }
    if (httpPort <= 0) httpPort = port;
    final bool ok = await setWindowsSystemProxy(port, httpPort: httpPort);
    if (ok && mounted) {
      setState(() => _activeSystemProxyPort = port);
      final bool mixedPort = httpPort <= 0 || httpPort == port;
      final String msg = mixedPort
          ? 'System proxy: MIXED :$port'
          : 'System proxy: HTTP :$httpPort + SOCKS5 :$port';
      _showNotice(msg, color: C.neonGreen, icon: Icons.vpn_lock_outlined);
    }
  }

  Future<void> _clearSystemProxy() async {
    final bool ok = await clearWindowsSystemProxy();
    if (ok && mounted) {
      setState(() => _activeSystemProxyPort = null);
      _showNotice('System proxy cleared', color: C.neonAmber, icon: Icons.clear_all_rounded);
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
                AnimatedSwitcher(duration: const Duration(milliseconds: 180), child: _buildNoticeBanner()),
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
                    AnimatedSwitcher(duration: const Duration(milliseconds: 180), child: _buildNoticeBanner()),
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
        provisionedPorts: _parseProvisionedPorts(),
        balancers: _parseBalancers(),
        activeSystemProxyPort: _activeSystemProxyPort,
        onSetSystemProxy: _setSystemProxy,
        onClearSystemProxy: _clearSystemProxy,
        // Pause / Speed / Maintenance
        isPaused: _isPaused,
        onTogglePause: _togglePause,
        speedProfile: _speedProfile,
        speedThreads: _speedThreads,
        speedTimeout: _speedTimeout,
        draftSpeedProfile: _draftSpeedProfile,
        draftSpeedThreads: _draftSpeedThreads,
        draftSpeedTimeout: _draftSpeedTimeout,
        hasPendingSpeedChanges: _hasPendingSpeedChanges,
        speedApplyInFlight: _speedApplyInFlight,
        projectedScanDuration: _projectedScanTimeForDraft(),
        onSpeedProfile: _setSpeedProfile,
        onThreadsChanged: _setThreads,
        onTimeoutChanged: _setTimeout,
        onApplySpeedChanges: _applySpeedChanges,
        clearAgeHours: _clearAgeHours,
        onClearAgeChanged: (int v) => setState(() => _clearAgeHours = v),
        onClearOldConfigs: _clearOldConfigs,
        manualConfigCtl: _manualConfigCtl,
        onAddManualConfigs: _addManualConfigs,
        onRunCycle: _runCycleNow,
        onRefreshProvisionedPorts: _refreshProvisionedPortsNow,
        onReprovisionPorts: _reprovisionPortsNow,
        onLoadRawFiles: _loadRawFilesNow,
        onLoadBundleFiles: _loadBundleFilesNow,
        onDetectCensorship: _detectCensorshipNow,
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
        allRecords: _allRecords,
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
        singboxPathController: _singboxPath,
        mihomoPathController: _mihomoPath,
        torPathController: _torPath,
        multiproxyPortController: _multiproxyPort,
        geminiPortController: _geminiPort,
        maxTotalController: _maxTotal,
        maxWorkersController: _maxWorkers,
        scanLimitController: _scanLimit,
        sleepSecondsController: _sleepSeconds,
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
        onSaveRuntimeSettings: _saveRuntimeSettings,
        onSaveTelegram: _saveTelegramSettings,
        onRefresh: _refresh,
        onCopyText: _copyText,
        githubUrlsController: _githubUrls,
        onSaveGithubUrls: _saveGithubUrls,
        onResetGithubUrls: _resetGithubUrls,
        onImportConfigFile: _importConfigFile,
        onExportConfigDb: _exportConfigDb,
      ),
      HunterNavSection.about => AboutSection(bundledConfigsCount: _bundledCount),
    };
  }

  List<Map<String, dynamic>> _parseProvisionedPorts() {
    final dynamic raw = _status?['provisioned_ports'];
    if (raw is List) {
      return raw.whereType<Map<String, dynamic>>().map((Map<String, dynamic> item) {
        final Map<String, dynamic> normalized = Map<String, dynamic>.from(item);
        final int port = (normalized['port'] as num?)?.toInt() ?? 0;
        int httpPort = (normalized['http_port'] as num?)?.toInt() ?? 0;
        if (port > 0) {
          if (httpPort <= 0 || httpPort != port) {
            httpPort = port;
          }
          normalized['port'] = port;
          normalized['http_port'] = httpPort;
          normalized['mixed'] = true;
        }
        return normalized;
      }).where((Map<String, dynamic> item) => ((item['port'] as num?)?.toInt() ?? 0) > 0).toList(growable: false);
    }
    return const <Map<String, dynamic>>[];
  }

  List<Map<String, dynamic>> _parseBalancers() {
    final dynamic raw = _status?['balancers'];
    if (raw is List) {
      return raw.whereType<Map<String, dynamic>>().toList();
    }
    return const <Map<String, dynamic>>[];
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
