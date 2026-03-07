import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:window_manager/window_manager.dart';
import 'package:path_provider/path_provider.dart';
import 'package:system_tray/system_tray.dart';

File? _globalLockFile;
RandomAccessFile? _globalLockHandle;

Future<void> _ensureSingleInstance() async {
  try {
    final Directory appData = Directory.systemTemp;
    final File lockFile = File('${appData.path}\\hunter_dashboard_singleton.lock');
    _globalLockFile = lockFile;
    // Try to open with exclusive lock - if another instance holds it, this fails
    try {
      _globalLockHandle = await lockFile.open(mode: FileMode.write);
      await _globalLockHandle!.lock(FileLock.exclusive);
      await _globalLockHandle!.writeString('${DateTime.now().toIso8601String()}');
      await _globalLockHandle!.flush();
    } on FileSystemException {
      // Another instance holds the lock
      exit(0);
    }
  } catch (e) {
    print('Lock file error: $e');
  }
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

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  
  try {
    await _ensureSingleInstance();
    await _copyBundledConfigsToDataDir();
    await _initWindow();
    runApp(const MyApp());
  } catch (e, stack) {
    print('Startup error: $e');
    print('Stack: $stack');
    rethrow;
  }
}

Future<void> _initWindow() async {
  await windowManager.ensureInitialized();
  WindowOptions windowOptions = const WindowOptions(
    size: Size(1200, 800),
    center: true,
    backgroundColor: Colors.transparent,
    skipTaskbar: false,
    titleBarStyle: TitleBarStyle.normal,
    title: 'Hunter Dashboard',
  );
  await windowManager.waitUntilReadyToShow(windowOptions, () async {
    await windowManager.setPreventClose(true);
    await windowManager.show();
    await windowManager.focus();
  });
}

Future<void> _copyBundledConfigsToDataDir() async {
  try {
    final Directory appDocDir = await getApplicationDocumentsDirectory();
    final Directory hunterDataDir = Directory('${appDocDir.path}\\Hunter');
    await hunterDataDir.create(recursive: true);
    final Directory configDir = Directory('${hunterDataDir.path}\\config');
    await configDir.create(recursive: true);

    final List<String> bundledFiles = <String>[
      'assets/configs/All_Configs_Sub.txt',
      'assets/configs/all_extracted_configs.txt',
      'assets/configs/sub.txt',
    ];

    for (final String assetPath in bundledFiles) {
      final String fileName = assetPath.split('/').last;
      final File dest = File('${configDir.path}\\$fileName');
      if (!await dest.exists()) {
        final ByteData data = await rootBundle.load(assetPath);
        await dest.writeAsBytes(data.buffer.asUint8List());
      }
    }
  } catch (_) {
    // Fallback: ignore bundling errors
  }
}

class MyApp extends StatelessWidget {
  const MyApp({super.key});

  // This widget is the root of your application.
  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      title: 'Hunter Dashboard',
      theme: ThemeData(
        // This is the theme of your application.
        //
        // TRY THIS: Try running your application with "flutter run". You'll see
        // the application has a purple toolbar. Then, without quitting the app,
        // try changing the seedColor in the colorScheme below to Colors.green
        // and then invoke "hot reload" (save your changes or press the "hot
        // reload" button in a Flutter-supported IDE, or press "r" if you used
        // the command line to start the app).
        //
        // Notice that the counter didn't reset back to zero; the application
        // state is not lost during the reload. To reset the state, use hot
        // restart instead.
        //
        // This works for code too, not just values: Most code changes can be
        // tested with just a hot reload.
        useMaterial3: true,
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF7C3AED),
          brightness: Brightness.dark,
        ),
      ),
      home: const HunterDashboardPage(),
    );
  }
}

enum HunterRunState {
  stopped,
  starting,
  running,
  stopping,
}

enum HunterNavSection {
  dashboard,
  configs,
  logs,
  advanced,
  about,
}

enum HunterConfigListKind {
  allCache,
  githubCache,
  balancer,
  gemini,
}

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

class HunterDashboardPage extends StatefulWidget {
  const HunterDashboardPage({super.key});

  @override
  State<HunterDashboardPage> createState() => _HunterDashboardPageState();
}

class _HunterDashboardPageState extends State<HunterDashboardPage> with WindowListener {
  static const int _maxLogLines = 2500;

  final TextEditingController _cliPathController = TextEditingController(
    text: 'bin\\hunter_cli.exe',
  );
  final TextEditingController _configPathController = TextEditingController(
    text: 'runtime\\hunter_config.json',
  );
  final TextEditingController _xrayPathController = TextEditingController(
    text: 'bin\\xray.exe',
  );

  bool _tgEnabled = false;
  final TextEditingController _tgApiIdController = TextEditingController();
  final TextEditingController _tgApiHashController = TextEditingController();
  final TextEditingController _tgPhoneController = TextEditingController();
  final TextEditingController _tgTargetsController = TextEditingController(
    text: 'v2rayngvpn\nmitivpn\nv2ray_configs_pool',
  );
  final TextEditingController _tgLimitController = TextEditingController(text: '50');
  final TextEditingController _tgTimeoutMsController = TextEditingController(text: '12000');

  HunterRunState _state = HunterRunState.stopped;
  Process? _process;
  Directory? _workingDir;
  StreamSubscription<String>? _stdoutSub;
  StreamSubscription<String>? _stderrSub;

  final List<HunterLogLine> _logs = <HunterLogLine>[];
  final ScrollController _logScrollController = ScrollController();
  bool _autoScroll = true;

  String? _lastError;

  HunterNavSection _navSection = HunterNavSection.dashboard;
  HunterConfigListKind _configListKind = HunterConfigListKind.allCache;
  final TextEditingController _configSearchController = TextEditingController();

  Timer? _runtimeRefreshTimer;
  bool _runtimeRefreshing = false;
  DateTime? _lastRuntimeRefresh;

  List<String> _allCacheConfigs = const <String>[];
  List<String> _githubCacheConfigs = const <String>[];
  List<HunterLatencyConfig> _balancerConfigs = const <HunterLatencyConfig>[];
  List<HunterLatencyConfig> _geminiBalancerConfigs = const <HunterLatencyConfig>[];

  Map<String, dynamic>? _hunterStatus;
  List<Map<String, dynamic>> _hunterStatusHistory = const <Map<String, dynamic>>[];

  int _bundledConfigsCount = 0;
  SystemTray? _systemTray;
  int _lastNotifiedConfigCount = 0;
  bool _minimizedToTray = false;
  bool _trayReady = false;

  // System resource monitoring
  int _cpuCores = Platform.numberOfProcessors;
  int _validationBatchSize = 20;
  Timer? _autoValidationTimer;
  bool _autoValidationEnabled = false;
  int _totalValidated = 0;
  int _totalPassed = 0;

  @override
  void initState() {
    super.initState();
    windowManager.addListener(this);
    _runtimeRefreshTimer = Timer.periodic(const Duration(seconds: 3), (_) {
      _refreshRuntime();
    });
    _refreshRuntime();
    _loadBundledConfigsCount();
    _initSystemTray();
    _computeOptimalBatchSize();
    _loadTelegramSettingsFromConfig();
  }

  bool _telegramConfigured() {
    if (!_tgEnabled) return false;
    final String apiId = _tgApiIdController.text.trim();
    final String apiHash = _tgApiHashController.text.trim();
    final String phone = _tgPhoneController.text.trim();
    final List<String> targets = _tgTargetsController.text
        .split(RegExp(r'[\r\n,]+'))
        .map((String s) => s.trim())
        .where((String s) => s.isNotEmpty)
        .toList();
    return apiId.isNotEmpty && apiHash.isNotEmpty && phone.isNotEmpty && targets.isNotEmpty;
  }

  Future<void> _loadTelegramSettingsFromConfig() async {
    try {
      final Directory wd = _effectiveWorkingDir();
      final String configPath = _absPathInWorkingDir(wd, _configPathController.text);
      if (configPath.isEmpty) return;
      final File f = File(configPath);
      if (!f.existsSync()) return;
      final dynamic decoded = jsonDecode(await f.readAsString());
      if (decoded is! Map<String, dynamic>) return;

      final bool enabled = decoded['telegram_enabled'] == true || decoded['telegram_enabled'] == 'true';
      final String apiId = (decoded['telegram_api_id'] ?? '').toString();
      final String apiHash = (decoded['telegram_api_hash'] ?? '').toString();
      final String phone = (decoded['telegram_phone'] ?? '').toString();
      final String limit = (decoded['telegram_limit'] ?? '50').toString();
      final String timeoutMs = (decoded['telegram_timeout_ms'] ?? '12000').toString();
      String targetsText = _tgTargetsController.text;
      final dynamic targets = decoded['targets'];
      if (targets is List) {
        targetsText = targets.map((dynamic e) => e.toString()).join('\n');
      }

      if (!mounted) return;
      setState(() {
        _tgEnabled = enabled;
        _tgApiIdController.text = apiId;
        _tgApiHashController.text = apiHash;
        _tgPhoneController.text = phone;
        _tgLimitController.text = limit;
        _tgTimeoutMsController.text = timeoutMs;
        _tgTargetsController.text = targetsText;
      });
    } catch (_) {
      // ignore
    }
  }

  Future<void> _saveTelegramSettingsToConfig() async {
    final Directory wd = _effectiveWorkingDir();
    final String configPath = _absPathInWorkingDir(wd, _configPathController.text);
    if (configPath.isEmpty) return;

    Map<String, dynamic> root = <String, dynamic>{};
    final File f = File(configPath);
    try {
      if (f.existsSync()) {
        final dynamic decoded = jsonDecode(await f.readAsString());
        if (decoded is Map<String, dynamic>) root = decoded;
      }
    } catch (_) {}

    final List<String> targets = _tgTargetsController.text
        .split(RegExp(r'[\r\n,]+'))
        .map((String s) => s.trim())
        .where((String s) => s.isNotEmpty)
        .toList();

    int limit = int.tryParse(_tgLimitController.text.trim()) ?? 50;
    if (limit < 0) limit = 0;
    if (limit > 500) limit = 500;

    int timeoutMs = int.tryParse(_tgTimeoutMsController.text.trim()) ?? 12000;
    if (timeoutMs < 3000) timeoutMs = 3000;
    if (timeoutMs > 25000) timeoutMs = 25000;

    root['telegram_enabled'] = _tgEnabled;
    root['telegram_api_id'] = _tgApiIdController.text.trim();
    root['telegram_api_hash'] = _tgApiHashController.text.trim();
    root['telegram_phone'] = _tgPhoneController.text.trim();
    root['telegram_limit'] = limit;
    root['telegram_timeout_ms'] = timeoutMs;
    root['targets'] = targets;

    await f.parent.create(recursive: true);
    await f.writeAsString(jsonEncode(root), flush: true);

    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text('Saved Telegram settings to ${f.path}')),
    );
  }

  Future<void> _loadBundledConfigsCount() async {
    try {
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
      if (mounted) {
        setState(() {
          _bundledConfigsCount = count;
        });
      }
    } catch (_) {}
  }

  Future<void> _initSystemTray() async {
    try {
      final String? iconPath = await _resolveTrayIconPath();
      if (iconPath == null || iconPath.trim().isEmpty) {
        _trayReady = false;
        _appendLog('ui', 'System tray init skipped: icon not found');
        return;
      }

      _systemTray = SystemTray();
      await _systemTray!.initSystemTray(
        title: 'Hunter Dashboard',
        iconPath: iconPath,
        toolTip: 'Hunter - Anti-Censorship Config Discovery',
      );
      _trayReady = true;

      _systemTray!.registerSystemTrayEventHandler((String eventName) {
        if (eventName == kSystemTrayEventClick || eventName == kSystemTrayEventRightClick) {
          _restoreFromTray();
        }
      });
      final Menu menu = Menu();
      await menu.buildFrom(<MenuItemBase>[
        MenuItemLabel(label: 'Show Dashboard', onClicked: (_) async {
          _restoreFromTray();
        }),
        MenuSeparator(),
        MenuItemLabel(label: 'Start Hunter', onClicked: (_) async {
          if (_state == HunterRunState.stopped) _start();
        }),
        MenuItemLabel(label: 'Stop Hunter', onClicked: (_) async {
          if (_state == HunterRunState.running) _stop();
        }),
        MenuSeparator(),
        MenuItemLabel(label: 'Exit', onClicked: (_) async {
          await _cleanupLockFile();
          exit(0);
        }),
      ]);
      await _systemTray!.setContextMenu(menu);
    } catch (e) {
      _trayReady = false;
      print('System tray init error: $e');
      _appendLog('ui', 'System tray init error: $e');
    }
  }

  Future<String?> _resolveTrayIconPath() async {
    if (!Platform.isWindows) return null;

    // system_tray expects a real file path (absolute is safest)
    String exeDir;
    try {
      exeDir = File(Platform.resolvedExecutable).parent.path;
    } catch (_) {
      exeDir = Directory.current.path;
    }

    final List<String> candidates = <String>[
      '$exeDir\\app_icon.ico',
      // In some Flutter builds, the exe lives under runner/Release or runner/Debug
      // and resources may be adjacent.
      '$exeDir\\resources\\app_icon.ico',
      // As a dev fallback (running from repo)
      '${Directory.current.path}\\windows\\runner\\resources\\app_icon.ico',
      '${Directory.current.path}\\build\\windows\\x64\\runner\\Release\\app_icon.ico',
      '${Directory.current.path}\\build\\windows\\x64\\runner\\Debug\\app_icon.ico',
    ];

    for (final String p in candidates) {
      try {
        if (File(p).existsSync()) return p;
      } catch (_) {}
    }
    return null;
  }

  Future<void> _restoreFromTray() async {
    if (!_trayReady) {
      await _initSystemTray();
    }
    _minimizedToTray = false;
    await windowManager.show();
    await windowManager.setSkipTaskbar(false);
    await windowManager.focus();
  }

  Future<void> _minimizeToTray() async {
    if (!_trayReady) {
      await _initSystemTray();
    }
    if (!_trayReady) {
      // Do not hide the window if tray is not available; user would think app disappeared.
      _appendLog('ui', 'Minimize-to-tray requested but tray is not ready (missing icon).');
      return;
    }

    _minimizedToTray = true;
    await windowManager.hide();
    await windowManager.setSkipTaskbar(true);
  }

  @override
  Future<void> onWindowClose() async {
    // Prevent actual close - minimize to tray instead
    await windowManager.setPreventClose(true);
    await _minimizeToTray();
  }

  void _computeOptimalBatchSize() {
    // Scale batch size with CPU cores: min 10, max 100
    _validationBatchSize = math.max(10, math.min(100, _cpuCores * 5));
    _appendLog('ui', 'System: $_cpuCores CPU cores, validation batch size: $_validationBatchSize');
  }

  void _toggleAutoValidation() {
    setState(() {
      _autoValidationEnabled = !_autoValidationEnabled;
    });
    if (_autoValidationEnabled) {
      _appendLog('ui', 'Auto-validation started (batch: $_validationBatchSize)');
      _autoValidationTimer?.cancel();
      _autoValidationTimer = Timer.periodic(const Duration(seconds: 10), (_) {
        _runOneValidationBatch();
      });
      _runOneValidationBatch();
    } else {
      _appendLog('ui', 'Auto-validation stopped');
      _autoValidationTimer?.cancel();
      _autoValidationTimer = null;
    }
  }

  Future<void> _runOneValidationBatch() async {
    if (_state != HunterRunState.running) return;
    // Backend handles actual validation; UI just triggers a refresh
    await _refreshRuntime();
  }

  void _sendTrayNotification(String message) {
    try {
      if (!_trayReady) return;
      _systemTray?.setToolTip('Hunter: $message');
    } catch (_) {}
  }

  @override
  void dispose() {
    windowManager.removeListener(this);
    _runtimeRefreshTimer?.cancel();
    _autoValidationTimer?.cancel();
    _stdoutSub?.cancel();
    _stderrSub?.cancel();
    _process?.kill();
    _systemTray?.destroy();

    _cliPathController.dispose();
    _configPathController.dispose();
    _xrayPathController.dispose();
    _configSearchController.dispose();
    _logScrollController.dispose();

    _tgApiIdController.dispose();
    _tgApiHashController.dispose();
    _tgPhoneController.dispose();
    _tgTargetsController.dispose();
    _tgLimitController.dispose();
    _tgTimeoutMsController.dispose();
    super.dispose();
  }

  void _appendLog(String source, String message) {
    final String clean = message.trimRight();
    if (clean.isEmpty) return;

    setState(() {
      _logs.add(HunterLogLine(ts: DateTime.now(), source: source, message: clean));
      if (_logs.length > _maxLogLines) {
        _logs.removeRange(0, _logs.length - _maxLogLines);
      }
    });

    if (_autoScroll) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (!_logScrollController.hasClients) return;
        _logScrollController.jumpTo(_logScrollController.position.maxScrollExtent);
      });
    }
  }

  Iterable<Directory> _startDirs() sync* {
    yield Directory.current;
    try {
      final File exe = File(Platform.resolvedExecutable);
      yield exe.parent;
    } catch (_) {
      // ignore
    }
  }

  Directory _resolveWorkingDirForCli(String cliInput) {
    final String trimmed = cliInput.trim();
    if (trimmed.isEmpty) return Directory.current;

    // If absolute, use its parent.
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
        if (File(candidate).existsSync()) {
          return cur;
        }
        if (cur.parent.path == cur.path) break;
        cur = cur.parent;
      }
    }

    return Directory.current;
  }

  String _absPathInWorkingDir(Directory wd, String relOrAbs) {
    final String trimmed = relOrAbs.trim();
    if (trimmed.isEmpty) return '';
    if (trimmed.contains(':\\') || trimmed.startsWith('\\\\') || trimmed.startsWith('/')) {
      return trimmed;
    }
    return '${wd.path}\\$trimmed';
  }

  Future<List<String>> _readConfigLines(String absPath) async {
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

  Future<List<HunterLatencyConfig>> _readLatencyConfigsFromJson(String absPath) async {
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

  Future<void> _refreshRuntime() async {
    if (_runtimeRefreshing) return;
    _runtimeRefreshing = true;

    try {
      final Directory wd = _workingDir ?? _resolveWorkingDirForCli(_cliPathController.text);
      final String runtimeDir = '${wd.path}\\runtime';

      _appendLog('ui', 'Refreshing runtime from: $runtimeDir');

      final List<String> allCache = await _readConfigLines('$runtimeDir\\HUNTER_all_cache.txt');
      final List<String> githubCache =
          await _readConfigLines('$runtimeDir\\HUNTER_github_configs_cache.txt');
      final List<HunterLatencyConfig> balancer =
          await _readLatencyConfigsFromJson('$runtimeDir\\HUNTER_balancer_cache.json');
      final List<HunterLatencyConfig> gemini =
          await _readLatencyConfigsFromJson('$runtimeDir\\HUNTER_gemini_balancer_cache.json');

      Map<String, dynamic>? status;
      List<Map<String, dynamic>> history = const <Map<String, dynamic>>[];
      try {
        final File statusFile = File('$runtimeDir\\HUNTER_status.json');
        _appendLog('ui', 'Status file: ${statusFile.path} exists=${statusFile.existsSync()}');
        if (statusFile.existsSync()) {
          final String content = await statusFile.readAsString();
          final dynamic decoded = jsonDecode(content);
          if (decoded is Map<String, dynamic>) {
            status = decoded;
            final dynamic h = decoded['history'];
            if (h is List) {
              final List<Map<String, dynamic>> rows = <Map<String, dynamic>>[];
              for (final dynamic item in h) {
                if (item is Map<String, dynamic>) rows.add(item);
              }
              history = rows;
            }
            _appendLog('ui', 'Status loaded: db=${status?['db']?['total'] ?? 'n/a'} entries');
          }
        } else {
          _appendLog('ui', 'Status file not found - backend may not be writing it');
        }
      } catch (e) {
        _appendLog('ui', 'Error reading status: $e');
      }

      if (!mounted) return;

      final int newTotal = allCache.length;
      final int oldTotal = _allCacheConfigs.length;
      final int diff = newTotal - oldTotal;
      if (diff > 0 && _lastNotifiedConfigCount != newTotal && _minimizedToTray) {
        _lastNotifiedConfigCount = newTotal;
        _sendTrayNotification('$diff new configs discovered! Total: $newTotal');
      }

      setState(() {
        _allCacheConfigs = allCache;
        _githubCacheConfigs = githubCache;
        _balancerConfigs = balancer;
        _geminiBalancerConfigs = gemini;
        _hunterStatus = status;
        _hunterStatusHistory = history;
        _lastRuntimeRefresh = DateTime.now();
      });

      _appendLog('ui', 'Runtime refreshed: $newTotal configs');
    } catch (e) {
      _appendLog('ui', 'Refresh error: $e');
      if (!mounted) return;
      setState(() {
        _lastRuntimeRefresh = DateTime.now();
      });
    } finally {
      _runtimeRefreshing = false;
    }
  }

  Future<void> _start() async {
    if (_state == HunterRunState.starting || _state == HunterRunState.running) return;
    setState(() {
      _state = HunterRunState.starting;
      _lastError = null;
    });

    try {
      final Directory wd = _resolveWorkingDirForCli(_cliPathController.text);
      final String cliPath = _absPathInWorkingDir(wd, _cliPathController.text);

      if (cliPath.isEmpty || !File(cliPath).existsSync()) {
        throw Exception('hunter_cli.exe not found: $cliPath');
      }

      await Directory('${wd.path}\\runtime').create(recursive: true);
      try {
        final File stopFlag = File('${wd.path}\\runtime\\stop.flag');
        if (stopFlag.existsSync()) {
          stopFlag.deleteSync();
        }
      } catch (_) {
        // ignore
      }

      final List<String> args = <String>[];
      final String configPath = _configPathController.text.trim();
      if (configPath.isNotEmpty) {
        args.addAll(<String>['--config', configPath]);
      }
      final String xrayPath = _xrayPathController.text.trim();
      if (xrayPath.isNotEmpty) {
        args.addAll(<String>['--xray', xrayPath]);
      }

      _appendLog('ui', 'Working dir: ${wd.path}');
      _appendLog('ui', 'Starting: $cliPath ${args.join(' ')}');

      final Process p = await Process.start(
        cliPath,
        args,
        workingDirectory: wd.path,
        runInShell: false,
      );
      _process = p;
      _workingDir = wd;

      _stdoutSub = p.stdout
          .transform(utf8.decoder)
          .transform(const LineSplitter())
          .listen((String line) => _appendLog('out', line));
      _stderrSub = p.stderr
          .transform(utf8.decoder)
          .transform(const LineSplitter())
          .listen((String line) => _appendLog('err', line));

      // Capture exit
      unawaited(p.exitCode.then((int code) {
        _appendLog('ui', 'Process exited with code $code');
        if (mounted) {
          setState(() {
            _process = null;
            _workingDir = null;
            _state = HunterRunState.stopped;
          });
        }
      }));

      setState(() {
        _state = HunterRunState.running;
      });
    } catch (e) {
      setState(() {
        _state = HunterRunState.stopped;
        _workingDir = null;
        _lastError = e.toString();
      });
      _appendLog('ui', 'Start failed: $e');
    }
  }

  Future<void> _stop() async {
    if (_state == HunterRunState.stopping || _state == HunterRunState.stopped) return;
    setState(() {
      _state = HunterRunState.stopping;
      _lastError = null;
    });

    try {
      final Directory wd = _workingDir ?? _resolveWorkingDirForCli(_cliPathController.text);
      await Directory('${wd.path}\\runtime').create(recursive: true);

      final File stopFlag = File('${wd.path}\\runtime\\stop.flag');
      stopFlag.writeAsStringSync('stop\n', mode: FileMode.write, flush: true);
      _appendLog('ui', 'stop.flag created: ${stopFlag.path}');

      final Process? p = _process;
      if (p != null) {
        final int code = await p.exitCode.timeout(
          const Duration(seconds: 15),
          onTimeout: () {
            _appendLog('ui', 'Graceful stop timeout, killing process...');
            p.kill(ProcessSignal.sigterm);
            return -1;
          },
        );
        _appendLog('ui', 'Stop result code: $code');
      }

      setState(() {
        _process = null;
        _workingDir = null;
        _state = HunterRunState.stopped;
      });
    } catch (e) {
      setState(() {
        _state = HunterRunState.stopped;
        _workingDir = null;
        _lastError = e.toString();
      });
      _appendLog('ui', 'Stop failed: $e');
    }
  }

  void _clearLogs() {
    setState(() {
      _logs.clear();
    });
  }

  Future<void> _copyLogs() async {
    if (_logs.isEmpty) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('No logs to copy')),
      );
      return;
    }
    final StringBuffer buffer = StringBuffer();
    for (final HunterLogLine line in _logs) {
      buffer.writeln('[${_fmtTs(line.ts)}] ${line.source.toUpperCase()}  ${line.message}');
    }
    await Clipboard.setData(ClipboardData(text: buffer.toString()));
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text('Copied ${_logs.length} log lines')),
    );
  }

  Color _stateColor(BuildContext context) {
    switch (_state) {
      case HunterRunState.running:
        return Colors.greenAccent;
      case HunterRunState.starting:
      case HunterRunState.stopping:
        return Colors.amberAccent;
      case HunterRunState.stopped:
        return Theme.of(context).colorScheme.onSurfaceVariant;
    }
  }

  String _stateText() {
    switch (_state) {
      case HunterRunState.stopped:
        return 'Stopped';
      case HunterRunState.starting:
        return 'Starting...';
      case HunterRunState.running:
        return 'Running';
      case HunterRunState.stopping:
        return 'Stopping...';
    }
  }

  String _navTitle() {
    return switch (_navSection) {
      HunterNavSection.dashboard => 'Hunter Dashboard',
      HunterNavSection.configs => 'Configs',
      HunterNavSection.logs => 'Logs',
      HunterNavSection.advanced => 'Advanced',
      HunterNavSection.about => 'About',
    };
  }

  String _configListKindLabel(HunterConfigListKind kind) {
    return switch (kind) {
      HunterConfigListKind.allCache => 'All cache',
      HunterConfigListKind.githubCache => 'GitHub cache',
      HunterConfigListKind.balancer => 'Balancer',
      HunterConfigListKind.gemini => 'Gemini balancer',
    };
  }

  bool _isLatencyKind(HunterConfigListKind kind) {
    return kind == HunterConfigListKind.balancer || kind == HunterConfigListKind.gemini;
  }

  Directory _effectiveWorkingDir() {
    return _workingDir ?? _resolveWorkingDirForCli(_cliPathController.text);
  }

  Future<void> _copyText(String text, {String? label}) async {
    final String clean = text.trim();
    if (clean.isEmpty) return;

    await Clipboard.setData(ClipboardData(text: clean));
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(label ?? 'Copied to clipboard')),
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

  Future<void> _useFirstConfig(List<String> configs, String label) async {
    if (configs.isEmpty) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('No $label configs available')),
      );
      return;
    }
    await _copyText(configs.first, label: 'Ready to use: $label config copied!');
  }

  Future<void> _useFirstLatencyConfig(List<HunterLatencyConfig> configs, String label) async {
    if (configs.isEmpty) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('No $label configs available')),
      );
      return;
    }
    final HunterLatencyConfig best = configs.first;
    final String latencyInfo = best.latencyMs != null 
        ? ' (${best.latencyMs!.toStringAsFixed(0)}ms)' 
        : '';
    await _copyText(best.uri, label: 'Ready to use: $label$latencyInfo');
  }

  Widget _errorBanner(TextTheme t, ColorScheme c) {
    if (_lastError == null) return const SizedBox.shrink();
    return Container(
      width: double.infinity,
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: c.errorContainer.withValues(alpha: 0.55),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: c.error.withValues(alpha: 0.35)),
      ),
      child: Text(
        _lastError!,
        style: t.bodyMedium?.copyWith(color: c.onErrorContainer),
      ),
    );
  }

  Widget _miniStat({
    required TextTheme t,
    required ColorScheme c,
    required String label,
    required String value,
  }) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
      decoration: BoxDecoration(
        color: c.surface.withValues(alpha: 0.25),
        borderRadius: BorderRadius.circular(999),
        border: Border.all(color: c.outlineVariant.withValues(alpha: 0.6)),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: <Widget>[
          Text(label, style: t.labelMedium?.copyWith(color: c.onSurfaceVariant)),
          const SizedBox(width: 8),
          Text(value, style: t.labelLarge?.copyWith(fontWeight: FontWeight.w700)),
        ],
      ),
    );
  }

  Widget _statCard({
    required TextTheme t,
    required ColorScheme c,
    required String title,
    required String value,
    String? subtitle,
    VoidCallback? onView,
    VoidCallback? onCopy,
    VoidCallback? onUse,
  }) {
    final TextStyle? valueStyle = t.headlineMedium?.copyWith(fontWeight: FontWeight.w700);
    return SizedBox(
      width: 260,
      child: Container(
        padding: const EdgeInsets.all(14),
        decoration: BoxDecoration(
          color: c.surfaceContainerHighest.withValues(alpha: 0.6),
          borderRadius: BorderRadius.circular(16),
          border: Border.all(color: c.outlineVariant.withValues(alpha: 0.6)),
        ),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Text(title, style: t.labelLarge),
            const SizedBox(height: 6),
            if (value.trim().isNotEmpty) Text(value, style: valueStyle),
            if (subtitle != null && subtitle.trim().isNotEmpty) ...<Widget>[
              const SizedBox(height: 4),
              Text(subtitle, style: t.bodySmall?.copyWith(color: c.onSurfaceVariant)),
            ],
            if (onView != null || onCopy != null || onUse != null) ...<Widget>[
              const SizedBox(height: 10),
              Wrap(
                spacing: 4,
                runSpacing: 4,
                children: <Widget>[
                  if (onView != null)
                    TextButton(
                      onPressed: onView,
                      child: const Text('View'),
                    ),
                  if (onUse != null)
                    FilledButton.icon(
                      onPressed: onUse,
                      icon: const Icon(Icons.play_arrow, size: 18),
                      label: const Text('Use'),
                      style: FilledButton.styleFrom(
                        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
                      ),
                    ),
                  if (onCopy != null)
                    TextButton.icon(
                      onPressed: onCopy,
                      icon: const Icon(Icons.content_copy, size: 18),
                      label: const Text('Copy'),
                    ),
                ],
              ),
            ],
          ],
        ),
      ),
    );
  }

  double? _avgLatencyMs(List<HunterLatencyConfig> items) {
    double sum = 0;
    int n = 0;
    for (final HunterLatencyConfig cfg in items) {
      final double? v = cfg.latencyMs;
      if (v == null || !v.isFinite) continue;
      sum += v;
      n += 1;
    }
    if (n == 0) return null;
    return sum / n;
  }

  double? _minLatencyMs(List<HunterLatencyConfig> items) {
    double? best;
    for (final HunterLatencyConfig cfg in items) {
      final double? v = cfg.latencyMs;
      if (v == null || !v.isFinite) continue;
      if (best == null || v < best) best = v;
    }
    return best;
  }

  Widget _configRow({
    required TextTheme t,
    required ColorScheme c,
    required String uri,
    double? latencyMs,
    bool showLatencyColumn = false,
  }) {
    final String latencyText = (latencyMs == null || !latencyMs.isFinite)
        ? '—'
        : '${latencyMs.toStringAsFixed(0)} ms';

    return InkWell(
      onTap: () => _copyText(uri, label: 'Copied 1 config'),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
        decoration: BoxDecoration(
          border: Border(
            bottom: BorderSide(color: c.outlineVariant.withValues(alpha: 0.45)),
          ),
        ),
        child: Row(
          children: <Widget>[
            if (showLatencyColumn)
              SizedBox(
                width: 92,
                child: Text(
                  latencyText,
                  style: t.labelMedium?.copyWith(color: c.onSurfaceVariant),
                  textAlign: TextAlign.right,
                ),
              ),
            if (showLatencyColumn) const SizedBox(width: 12),
            Expanded(
              child: Tooltip(
                message: uri,
                child: Text(
                  uri,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: t.bodySmall?.copyWith(fontFamily: 'Consolas', height: 1.15),
                ),
              ),
            ),
            IconButton(
              tooltip: 'Copy',
              onPressed: () => _copyText(uri, label: 'Copied 1 config'),
              icon: const Icon(Icons.content_copy, size: 18),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildDashboardSection(TextTheme t, ColorScheme c) {
    final int total = _allCacheConfigs.length;
    final int github = _githubCacheConfigs.length;
    final int balancerTotal = _balancerConfigs.length;
    final int geminiTotal = _geminiBalancerConfigs.length;

    final int balancerAlive =
        _balancerConfigs.where((HunterLatencyConfig e) => e.latencyMs != null).length;
    final int geminiAlive =
        _geminiBalancerConfigs.where((HunterLatencyConfig e) => e.latencyMs != null).length;

    final double? balancerAvg = _avgLatencyMs(_balancerConfigs);
    final double? geminiAvg = _avgLatencyMs(_geminiBalancerConfigs);
    final double? balancerMin = _minLatencyMs(_balancerConfigs);
    final double? geminiMin = _minLatencyMs(_geminiBalancerConfigs);

    final String refreshText = (_lastRuntimeRefresh == null) ? '—' : _fmtTs(_lastRuntimeRefresh!);
    final String bundledText = _bundledConfigsCount > 0 ? '$_bundledConfigsCount configs ready' : 'Loading...';

    final Map<String, dynamic>? hs = _hunterStatus;
    final Map<String, dynamic>? db = (hs?['db'] is Map<String, dynamic>) ? (hs!['db'] as Map<String, dynamic>) : null;
    final Map<String, dynamic>? val = (hs?['validator'] is Map<String, dynamic>) ? (hs!['validator'] as Map<String, dynamic>) : null;
    final int dbTotal = (db?['total'] is num) ? (db!['total'] as num).toInt() : 0;
    final int dbAlive = (db?['alive'] is num) ? (db!['alive'] as num).toInt() : 0;
    final int dbTestedUnique = (db?['tested_unique'] is num) ? (db!['tested_unique'] as num).toInt() : 0;
    final int dbPendingUnique = (hs?['pending_unique'] is num) ? (hs!['pending_unique'] as num).toInt() : 0;
    final int dbDead = math.max(0, dbTestedUnique - dbAlive);
    final int lastBatchTested = (val?['last_tested'] is num) ? (val!['last_tested'] as num).toInt() : 0;
    final int lastBatchPassed = (val?['last_passed'] is num) ? (val!['last_passed'] as num).toInt() : 0;
    final double rate = (val?['rate_per_s'] is num) ? (val!['rate_per_s'] as num).toDouble() : 0.0;
    final double etaSeconds = (hs?['eta_seconds'] is num) ? (hs!['eta_seconds'] as num).toDouble() : 0.0;
    final String etaText = (etaSeconds <= 0)
        ? '—'
        : _fmtDuration(Duration(seconds: etaSeconds.round()));

    return SingleChildScrollView(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          _errorBanner(t, c),
          if (_lastError != null) const SizedBox(height: 12),
          Row(
            children: <Widget>[
              Text('Runtime refresh: $refreshText', style: t.labelLarge),
              const SizedBox(width: 16),
              Chip(
                avatar: const Icon(Icons.memory, size: 16),
                label: Text('$_cpuCores cores / batch: $_validationBatchSize'),
              ),
              const Spacer(),
              OutlinedButton.icon(
                onPressed: _refreshRuntime,
                icon: const Icon(Icons.refresh),
                label: const Text('Refresh'),
              ),
            ],
          ),
          const SizedBox(height: 12),
          Container(
            padding: const EdgeInsets.all(14),
            decoration: BoxDecoration(
              color: c.surfaceContainerHighest.withValues(alpha: 0.6),
              borderRadius: BorderRadius.circular(16),
              border: Border.all(color: c.outlineVariant.withValues(alpha: 0.6)),
            ),
            child: Row(
              children: <Widget>[
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: <Widget>[
                      Text('Scanner status', style: t.titleMedium),
                      const SizedBox(height: 6),
                      Wrap(
                        spacing: 10,
                        runSpacing: 8,
                        children: <Widget>[
                          _miniStat(t: t, c: c, label: 'DB', value: '$dbTotal'),
                          _miniStat(t: t, c: c, label: 'Alive', value: '$dbAlive'),
                          _miniStat(t: t, c: c, label: 'Dead', value: '$dbDead'),
                          _miniStat(t: t, c: c, label: 'Tested', value: '$dbTestedUnique'),
                          _miniStat(t: t, c: c, label: 'Pending', value: '$dbPendingUnique'),
                          _miniStat(t: t, c: c, label: 'Last', value: lastBatchTested == 0 ? '—' : '$lastBatchPassed/$lastBatchTested'),
                          _miniStat(t: t, c: c, label: 'Rate', value: rate <= 0 ? '—' : '${rate.toStringAsFixed(1)}/s'),
                          _miniStat(t: t, c: c, label: 'ETA', value: etaText),
                        ],
                      ),
                    ],
                  ),
                ),
                const SizedBox(width: 12),
                SizedBox(
                  width: 220,
                  height: 64,
                  child: HunterSparkline(
                    history: _hunterStatusHistory,
                    line1Key: 'tested_unique',
                    line2Key: 'alive',
                    c: c,
                  ),
                ),
              ],
            ),
          ),
          const SizedBox(height: 12),
          Wrap(
            spacing: 12,
            runSpacing: 12,
            children: <Widget>[
              _statCard(
                t: t,
                c: c,
                title: 'Bundled seeds',
                value: bundledText,
                subtitle: 'Initial config database',
                onView: () {
                  setState(() {
                    _navSection = HunterNavSection.about;
                  });
                },
              ),
              _statCard(
                t: t,
                c: c,
                title: 'Total cache',
                value: '$total',
                subtitle: 'HUNTER_all_cache.txt',
                onView: () {
                  setState(() {
                    _navSection = HunterNavSection.configs;
                    _configListKind = HunterConfigListKind.allCache;
                    _configSearchController.clear();
                  });
                },
                onCopy: () => _copyLines(_allCacheConfigs, label: 'Copied $total configs'),
              ),
              _statCard(
                t: t,
                c: c,
                title: 'GitHub cache',
                value: '$github',
                subtitle: 'HUNTER_github_configs_cache.txt',
                onView: () {
                  setState(() {
                    _navSection = HunterNavSection.configs;
                    _configListKind = HunterConfigListKind.githubCache;
                    _configSearchController.clear();
                  });
                },
                onCopy: () =>
                    _copyLines(_githubCacheConfigs, label: 'Copied $github configs'),
              ),
              _statCard(
                t: t,
                c: c,
                title: 'Balancer',
                value: '$balancerAlive / $balancerTotal',
                subtitle: 'avg: ${balancerAvg?.toStringAsFixed(0) ?? '—'} ms, best: ${balancerMin?.toStringAsFixed(0) ?? '—'}',
                onView: () {
                  setState(() {
                    _navSection = HunterNavSection.configs;
                    _configListKind = HunterConfigListKind.balancer;
                    _configSearchController.clear();
                  });
                },
                onUse: balancerAlive > 0 ? () => _useFirstLatencyConfig(_balancerConfigs, 'Balancer') : null,
                onCopy: () => _copyLines(
                  _balancerConfigs.map((HunterLatencyConfig e) => e.uri).toList(),
                  label: 'Copied $balancerTotal configs',
                ),
              ),
              _statCard(
                t: t,
                c: c,
                title: 'Gemini balancer',
                value: '$geminiAlive / $geminiTotal',
                subtitle: 'avg: ${geminiAvg?.toStringAsFixed(0) ?? '—'} ms, best: ${geminiMin?.toStringAsFixed(0) ?? '—'}',
                onView: () {
                  setState(() {
                    _navSection = HunterNavSection.configs;
                    _configListKind = HunterConfigListKind.gemini;
                    _configSearchController.clear();
                  });
                },
                onUse: geminiAlive > 0 ? () => _useFirstLatencyConfig(_geminiBalancerConfigs, 'Gemini') : null,
                onCopy: () => _copyLines(
                  _geminiBalancerConfigs.map((HunterLatencyConfig e) => e.uri).toList(),
                  label: 'Copied $geminiTotal configs',
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }

  List<String> _filterStrings(List<String> items, String q) {
    final String query = q.trim().toLowerCase();
    if (query.isEmpty) return items;
    return items.where((String s) => s.toLowerCase().contains(query)).toList();
  }

  List<HunterLatencyConfig> _filterLatency(List<HunterLatencyConfig> items, String q) {
    final String query = q.trim().toLowerCase();
    if (query.isEmpty) return items;
    return items
        .where((HunterLatencyConfig s) => s.uri.toLowerCase().contains(query))
        .toList();
  }

  Widget _buildConfigsSection(TextTheme t, ColorScheme c) {
    final String q = _configSearchController.text;
    final bool latency = _isLatencyKind(_configListKind);

    final List<String> baseStrings = switch (_configListKind) {
      HunterConfigListKind.allCache => _allCacheConfigs,
      HunterConfigListKind.githubCache => _githubCacheConfigs,
      _ => const <String>[],
    };

    final List<HunterLatencyConfig> baseLatency = switch (_configListKind) {
      HunterConfigListKind.balancer => _balancerConfigs,
      HunterConfigListKind.gemini => _geminiBalancerConfigs,
      _ => const <HunterLatencyConfig>[],
    };

    final List<String> visibleUris;
    final List<HunterLatencyConfig> visibleLatency;
    if (latency) {
      visibleLatency = _filterLatency(baseLatency, q);
      visibleUris = visibleLatency.map((HunterLatencyConfig e) => e.uri).toList();
    } else {
      visibleUris = _filterStrings(baseStrings, q);
      visibleLatency = const <HunterLatencyConfig>[];
    }

    final int total = latency ? baseLatency.length : baseStrings.length;
    final String refreshText = (_lastRuntimeRefresh == null) ? '—' : _fmtTs(_lastRuntimeRefresh!);

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: <Widget>[
        _errorBanner(t, c),
        if (_lastError != null) const SizedBox(height: 12),
        Row(
          children: <Widget>[
            SizedBox(
              width: 220,
              child: DropdownButtonFormField<HunterConfigListKind>(
                value: _configListKind,
                items: HunterConfigListKind.values
                    .map(
                      (HunterConfigListKind k) => DropdownMenuItem<HunterConfigListKind>(
                        value: k,
                        child: Text(_configListKindLabel(k)),
                      ),
                    )
                    .toList(),
                onChanged: (HunterConfigListKind? v) {
                  if (v == null) return;
                  setState(() {
                    _configListKind = v;
                  });
                },
                decoration: InputDecoration(
                  labelText: 'List',
                  isDense: true,
                  border: OutlineInputBorder(borderRadius: BorderRadius.circular(12)),
                ),
              ),
            ),
            const SizedBox(width: 12),
            Expanded(
              child: TextField(
                controller: _configSearchController,
                onChanged: (_) => setState(() {}),
                decoration: InputDecoration(
                  labelText: 'Search',
                  isDense: true,
                  prefixIcon: const Icon(Icons.search),
                  suffixIcon: (_configSearchController.text.isEmpty)
                      ? null
                      : IconButton(
                          tooltip: 'Clear',
                          onPressed: () {
                            _configSearchController.clear();
                            setState(() {});
                          },
                          icon: const Icon(Icons.clear),
                        ),
                  border: OutlineInputBorder(borderRadius: BorderRadius.circular(12)),
                ),
              ),
            ),
            const SizedBox(width: 8),
            IconButton(
              tooltip: 'Refresh runtime',
              onPressed: _refreshRuntime,
              icon: const Icon(Icons.refresh),
            ),
            const SizedBox(width: 8),
            FilledButton.icon(
              onPressed: visibleUris.isEmpty
                  ? null
                  : () => _copyLines(visibleUris, label: 'Copied ${visibleUris.length} configs'),
              icon: const Icon(Icons.content_copy),
              label: const Text('Copy visible'),
            ),
          ],
        ),
        const SizedBox(height: 10),
        Row(
          children: <Widget>[
            Text('${visibleUris.length} shown', style: t.labelLarge),
            const SizedBox(width: 12),
            Text('Total: $total', style: t.labelLarge?.copyWith(color: c.onSurfaceVariant)),
            const Spacer(),
            Text('Runtime: $refreshText', style: t.labelMedium?.copyWith(color: c.onSurfaceVariant)),
          ],
        ),
        const SizedBox(height: 10),
        Expanded(
          child: Container(
            decoration: BoxDecoration(
              color: c.surfaceContainerHighest.withValues(alpha: 0.6),
              borderRadius: BorderRadius.circular(16),
              border: Border.all(color: c.outlineVariant.withValues(alpha: 0.6)),
            ),
            child: ClipRRect(
              borderRadius: BorderRadius.circular(16),
              child: ListView.builder(
                padding: EdgeInsets.zero,
                itemCount: latency ? visibleLatency.length : visibleUris.length,
                itemBuilder: (BuildContext context, int i) {
                  if (latency) {
                    final HunterLatencyConfig cfg = visibleLatency[i];
                    return _configRow(
                      t: t,
                      c: c,
                      uri: cfg.uri,
                      latencyMs: cfg.latencyMs,
                      showLatencyColumn: true,
                    );
                  }
                  return _configRow(
                    t: t,
                    c: c,
                    uri: visibleUris[i],
                  );
                },
              ),
            ),
          ),
        ),
      ],
    );
  }

  Widget _buildLogsSection(TextTheme t, ColorScheme c) {
    return Column(
      children: <Widget>[
        _errorBanner(t, c),
        if (_lastError != null) const SizedBox(height: 12),
        Row(
          children: <Widget>[
            Text('Logs', style: t.titleMedium),
            const SizedBox(width: 12),
            Text('${_logs.length}', style: t.labelLarge?.copyWith(color: c.onSurfaceVariant)),
            const Spacer(),
            TextButton.icon(
              onPressed: _copyLogs,
              icon: const Icon(Icons.content_copy),
              label: const Text('Copy all'),
            ),
            const SizedBox(width: 8),
            TextButton.icon(
              onPressed: _clearLogs,
              icon: const Icon(Icons.delete_outline),
              label: const Text('Clear'),
            ),
            const SizedBox(width: 12),
            Row(
              children: <Widget>[
                Text('Auto-scroll', style: t.labelLarge),
                const SizedBox(width: 8),
                Switch(
                  value: _autoScroll,
                  onChanged: (bool v) => setState(() => _autoScroll = v),
                ),
              ],
            ),
          ],
        ),
        const SizedBox(height: 12),
        Expanded(
          child: Container(
            decoration: BoxDecoration(
              color: c.surfaceContainerHighest.withValues(alpha: 0.6),
              borderRadius: BorderRadius.circular(16),
              border: Border.all(color: c.outlineVariant.withValues(alpha: 0.6)),
            ),
            child: ClipRRect(
              borderRadius: BorderRadius.circular(16),
              child: ListView.builder(
                controller: _logScrollController,
                padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
                itemCount: _logs.length,
                itemBuilder: (BuildContext context, int i) {
                  final HunterLogLine line = _logs[i];
                  final Color srcColor = switch (line.source) {
                    'err' => Colors.redAccent,
                    'ui' => Colors.blueAccent,
                    _ => c.onSurfaceVariant,
                  };
                  return Padding(
                    padding: const EdgeInsets.symmetric(vertical: 2),
                    child: Text(
                      '[${_fmtTs(line.ts)}] ${line.source.toUpperCase()}  ${line.message}',
                      style: t.bodySmall?.copyWith(
                        fontFamily: 'Consolas',
                        height: 1.25,
                        color: srcColor,
                      ),
                    ),
                  );
                },
              ),
            ),
          ),
        ),
      ],
    );
  }

  Widget _buildAdvancedSection(TextTheme t, ColorScheme c) {
    final Directory wd = _effectiveWorkingDir();
    final String runtimeDir = '${wd.path}\\runtime';
    final String refreshText = (_lastRuntimeRefresh == null) ? '—' : _fmtTs(_lastRuntimeRefresh!);

    return SingleChildScrollView(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          _errorBanner(t, c),
          if (_lastError != null) const SizedBox(height: 12),
          Text('Paths', style: t.titleMedium),
          const SizedBox(height: 8),
          Row(
            children: <Widget>[
              Expanded(
                child: _PathField(
                  label: 'CLI',
                  controller: _cliPathController,
                  hintText: 'bin\\hunter_cli.exe',
                ),
              ),
              const SizedBox(width: 12),
              Expanded(
                child: _PathField(
                  label: 'Config',
                  controller: _configPathController,
                  hintText: 'runtime\\hunter_config.json',
                ),
              ),
              const SizedBox(width: 12),
              Expanded(
                child: _PathField(
                  label: 'XRay',
                  controller: _xrayPathController,
                  hintText: 'bin\\xray.exe',
                ),
              ),
            ],
          ),
          const SizedBox(height: 12),
          Row(
            children: <Widget>[
              OutlinedButton.icon(
                onPressed: _refreshRuntime,
                icon: const Icon(Icons.refresh),
                label: const Text('Refresh runtime'),
              ),
              const SizedBox(width: 12),
              Text('Runtime refresh: $refreshText', style: t.labelLarge),
            ],
          ),
          const SizedBox(height: 12),
          Wrap(
            spacing: 12,
            runSpacing: 12,
            children: <Widget>[
              _statCard(
                t: t,
                c: c,
                title: 'Working dir',
                value: ' ',
                subtitle: wd.path,
                onCopy: () => _copyText(wd.path, label: 'Copied working dir'),
              ),
              _statCard(
                t: t,
                c: c,
                title: 'Runtime dir',
                value: ' ',
                subtitle: runtimeDir,
                onCopy: () => _copyText(runtimeDir, label: 'Copied runtime dir'),
              ),
              _statCard(
                t: t,
                c: c,
                title: 'Process',
                value: _stateText(),
                subtitle: _process == null ? '—' : 'pid: ${_process!.pid}',
              ),
            ],
          ),
          const SizedBox(height: 16),
          Text('Telegram (scrape channels)', style: t.titleMedium),
          const SizedBox(height: 8),
          Container(
            padding: const EdgeInsets.all(14),
            decoration: BoxDecoration(
              color: c.surfaceContainerHighest.withValues(alpha: 0.6),
              borderRadius: BorderRadius.circular(16),
              border: Border.all(color: c.outlineVariant.withValues(alpha: 0.6)),
            ),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Row(
                  children: <Widget>[
                    Switch(
                      value: _tgEnabled,
                      onChanged: (bool v) {
                        setState(() {
                          _tgEnabled = v;
                        });
                      },
                    ),
                    const SizedBox(width: 8),
                    Expanded(
                      child: Text(
                        _telegramConfigured() ? 'Enabled (configured)' : (_tgEnabled ? 'Enabled (needs setup)' : 'Disabled'),
                        style: t.titleSmall,
                      ),
                    ),
                    FilledButton.icon(
                      onPressed: _saveTelegramSettingsToConfig,
                      icon: const Icon(Icons.save),
                      label: const Text('Save'),
                    ),
                  ],
                ),
                const SizedBox(height: 10),
                Row(
                  children: <Widget>[
                    Expanded(
                      child: TextField(
                        controller: _tgApiIdController,
                        decoration: const InputDecoration(
                          labelText: 'API ID',
                          hintText: 'from my.telegram.org',
                          border: OutlineInputBorder(),
                        ),
                      ),
                    ),
                    const SizedBox(width: 12),
                    Expanded(
                      child: TextField(
                        controller: _tgApiHashController,
                        decoration: const InputDecoration(
                          labelText: 'API Hash',
                          hintText: 'from my.telegram.org',
                          border: OutlineInputBorder(),
                        ),
                      ),
                    ),
                    const SizedBox(width: 12),
                    Expanded(
                      child: TextField(
                        controller: _tgPhoneController,
                        decoration: const InputDecoration(
                          labelText: 'Phone',
                          hintText: '+98...',
                          border: OutlineInputBorder(),
                        ),
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 10),
                Row(
                  children: <Widget>[
                    Expanded(
                      child: TextField(
                        controller: _tgLimitController,
                        decoration: const InputDecoration(
                          labelText: 'Telegram limit',
                          hintText: '50',
                          border: OutlineInputBorder(),
                        ),
                      ),
                    ),
                    const SizedBox(width: 12),
                    Expanded(
                      child: TextField(
                        controller: _tgTimeoutMsController,
                        decoration: const InputDecoration(
                          labelText: 'Telegram timeout (ms)',
                          hintText: '12000',
                          border: OutlineInputBorder(),
                        ),
                      ),
                    ),
                    const SizedBox(width: 12),
                    Expanded(
                      flex: 2,
                      child: TextField(
                        controller: _tgTargetsController,
                        minLines: 2,
                        maxLines: 6,
                        decoration: const InputDecoration(
                          labelText: 'Targets (channels)',
                          hintText: 'One per line (e.g. v2rayngvpn)',
                          border: OutlineInputBorder(),
                        ),
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 8),
                ExpansionTile(
                  title: const Text('How to activate Telegram scraping'),
                  childrenPadding: const EdgeInsets.only(left: 8, right: 8, bottom: 8),
                  children: <Widget>[
                    Align(
                      alignment: Alignment.centerLeft,
                      child: Text(
                        '1) Go to https://my.telegram.org and login with your phone.\n'
                        '2) Open API development tools and create an application.\n'
                        '3) Copy API ID and API Hash here.\n'
                        '4) Enter your phone in international format (+98...).\n'
                        '5) Add channel usernames in Targets (without @).\n'
                        '6) Click Save. Then Start Hunter.\n\n'
                        'If Telegram is blocked on your network, make sure a local SOCKS proxy is running (Hunter uses your local ports like 10808/10809).',
                        style: t.bodyMedium?.copyWith(color: c.onSurfaceVariant),
                      ),
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

  Widget _buildAboutSection(TextTheme t, ColorScheme c) {
    return SingleChildScrollView(
      padding: const EdgeInsets.all(24),
      child: Center(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: <Widget>[
            const SizedBox(height: 48),
            Icon(Icons.shield, size: 120, color: c.primary),
            const SizedBox(height: 24),
            Text('Hunter Dashboard', style: t.displaySmall?.copyWith(fontWeight: FontWeight.bold)),
            const SizedBox(height: 8),
            Text('Censorship Hunter - Anti-Censorship Proxy Config Discovery', style: t.titleMedium?.copyWith(color: c.onSurfaceVariant)),
            const SizedBox(height: 32),
            Card(
              child: Padding(
                padding: const EdgeInsets.all(24),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: <Widget>[
                    Row(
                      children: <Widget>[
                        Icon(Icons.person, color: c.primary),
                        const SizedBox(width: 12),
                        Expanded(
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: <Widget>[
                              Text('Developed by', style: t.labelMedium?.copyWith(color: c.onSurfaceVariant)),
                              const SizedBox(height: 4),
                              SelectableText('bahmanymb@gmail.com', style: t.titleMedium),
                            ],
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(height: 24),
                    Row(
                      children: <Widget>[
                        Icon(Icons.code, color: c.primary),
                        const SizedBox(width: 12),
                        Expanded(
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: <Widget>[
                              Text('GitHub Repository', style: t.labelMedium?.copyWith(color: c.onSurfaceVariant)),
                              const SizedBox(height: 4),
                              SelectableText('https://github.com/bahmany/censorship_hunter', style: t.titleMedium),
                            ],
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(height: 24),
                    Row(
                      children: <Widget>[
                        Icon(Icons.storage, color: c.primary),
                        const SizedBox(width: 12),
                        Expanded(
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: <Widget>[
                              Text('Bundled Seed Configs', style: t.labelMedium?.copyWith(color: c.onSurfaceVariant)),
                              const SizedBox(height: 4),
                              Text('${_bundledConfigsCount.toString()} configs available', style: t.titleMedium),
                            ],
                          ),
                        ),
                      ],
                    ),
                  ],
                ),
              ),
            ),
            const SizedBox(height: 32),
            Text('Fighting internet censorship in Iran', style: t.bodyLarge?.copyWith(fontStyle: FontStyle.italic, color: c.onSurfaceVariant)),
          ],
        ),
      ),
    );
  }

  Widget _buildSection(TextTheme t, ColorScheme c) {
    return switch (_navSection) {
      HunterNavSection.dashboard => _buildDashboardSection(t, c),
      HunterNavSection.configs => _buildConfigsSection(t, c),
      HunterNavSection.logs => _buildLogsSection(t, c),
      HunterNavSection.advanced => _buildAdvancedSection(t, c),
      HunterNavSection.about => _buildAboutSection(t, c),
    };
  }

  @override
  Widget build(BuildContext context) {
    final TextTheme t = Theme.of(context).textTheme;
    final ColorScheme c = Theme.of(context).colorScheme;

    return Scaffold(
      appBar: AppBar(
        title: Text(_navTitle()),
        actions: <Widget>[
          FilledButton.icon(
            onPressed: (_state == HunterRunState.stopped) ? _start : null,
            icon: const Icon(Icons.play_arrow),
            label: const Text('Start'),
          ),
          const SizedBox(width: 8),
          OutlinedButton.icon(
            onPressed: (_state == HunterRunState.running) ? _stop : null,
            icon: const Icon(Icons.stop),
            label: const Text('Stop'),
          ),
          const SizedBox(width: 12),
          Row(
            children: <Widget>[
              Container(
                width: 10,
                height: 10,
                decoration: BoxDecoration(
                  color: _stateColor(context),
                  borderRadius: BorderRadius.circular(99),
                ),
              ),
              const SizedBox(width: 8),
              Text(_stateText(), style: t.labelLarge),
            ],
          ),
          const SizedBox(width: 12),
          IconButton(
            tooltip: 'Minimize to tray',
            onPressed: _minimizeToTray,
            icon: const Icon(Icons.minimize),
          ),
          IconButton(
            tooltip: 'Exit',
            onPressed: () async {
              await _cleanupLockFile();
              exit(0);
            },
            icon: const Icon(Icons.close),
          ),
          const SizedBox(width: 8),
        ],
      ),
      body: LayoutBuilder(
        builder: (BuildContext context, BoxConstraints constraints) {
          final bool extended = constraints.maxWidth >= 1080;
          return Row(
            children: <Widget>[
              NavigationRail(
                extended: extended,
                selectedIndex: HunterNavSection.values.indexOf(_navSection),
                onDestinationSelected: (int i) {
                  setState(() {
                    _navSection = HunterNavSection.values[i];
                  });
                },
                destinations: const <NavigationRailDestination>[
                  NavigationRailDestination(
                    icon: Icon(Icons.dashboard_outlined),
                    selectedIcon: Icon(Icons.dashboard),
                    label: Text('Dashboard'),
                  ),
                  NavigationRailDestination(
                    icon: Icon(Icons.storage_outlined),
                    selectedIcon: Icon(Icons.storage),
                    label: Text('Configs'),
                  ),
                  NavigationRailDestination(
                    icon: Icon(Icons.article_outlined),
                    selectedIcon: Icon(Icons.article),
                    label: Text('Logs'),
                  ),
                  NavigationRailDestination(
                    icon: Icon(Icons.tune_outlined),
                    selectedIcon: Icon(Icons.tune),
                    label: Text('Advanced'),
                  ),
                  NavigationRailDestination(
                    icon: Icon(Icons.info_outlined),
                    selectedIcon: Icon(Icons.info),
                    label: Text('About'),
                  ),
                ],
              ),
              const VerticalDivider(width: 1, thickness: 1),
              Expanded(
                child: Padding(
                  padding: const EdgeInsets.all(16),
                  child: _buildSection(t, c),
                ),
              ),
            ],
          );
        },
      ),
    );
  }
}

String _fmtTs(DateTime dt) {
  String two(int x) => x.toString().padLeft(2, '0');
  return '${two(dt.hour)}:${two(dt.minute)}:${two(dt.second)}';
}

String _fmtDuration(Duration d) {
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

class HunterSparkline extends StatelessWidget {
  const HunterSparkline({
    super.key,
    required this.history,
    required this.line1Key,
    required this.line2Key,
    required this.c,
  });

  final List<Map<String, dynamic>> history;
  final String line1Key;
  final String line2Key;
  final ColorScheme c;

  @override
  Widget build(BuildContext context) {
    return CustomPaint(
      painter: _HunterSparklinePainter(
        history: history,
        line1Key: line1Key,
        line2Key: line2Key,
        line1Color: c.primary,
        line2Color: c.tertiary,
        gridColor: c.outlineVariant.withValues(alpha: 0.35),
      ),
    );
  }
}

class _HunterSparklinePainter extends CustomPainter {
  _HunterSparklinePainter({
    required this.history,
    required this.line1Key,
    required this.line2Key,
    required this.line1Color,
    required this.line2Color,
    required this.gridColor,
  });

  final List<Map<String, dynamic>> history;
  final String line1Key;
  final String line2Key;
  final Color line1Color;
  final Color line2Color;
  final Color gridColor;

  List<double> _extract(String key) {
    final List<double> out = <double>[];
    for (final Map<String, dynamic> row in history) {
      final dynamic v = row[key];
      if (v is num) out.add(v.toDouble());
    }
    return out;
  }

  @override
  void paint(Canvas canvas, Size size) {
    final List<double> a = _extract(line1Key);
    final List<double> b = _extract(line2Key);
    final int n = math.max(a.length, b.length);
    if (n < 2) return;

    double maxV = 0;
    for (final double v in a) {
      if (v.isFinite) maxV = math.max(maxV, v);
    }
    for (final double v in b) {
      if (v.isFinite) maxV = math.max(maxV, v);
    }
    if (maxV <= 0) return;

    final Paint gridPaint = Paint()
      ..color = gridColor
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1;

    canvas.drawLine(Offset(0, size.height - 1), Offset(size.width, size.height - 1), gridPaint);

    void drawLine(List<double> vals, Color color) {
      if (vals.length < 2) return;
      final Paint p = Paint()
        ..color = color
        ..style = PaintingStyle.stroke
        ..strokeWidth = 2
        ..strokeCap = StrokeCap.round;
      final Path path = Path();
      for (int i = 0; i < vals.length; i++) {
        final double x = (i / (vals.length - 1)) * size.width;
        final double y = size.height - ((vals[i] / maxV) * size.height);
        if (i == 0) {
          path.moveTo(x, y);
        } else {
          path.lineTo(x, y);
        }
      }
      canvas.drawPath(path, p);
    }

    drawLine(a, line1Color);
    drawLine(b, line2Color);
  }

  @override
  bool shouldRepaint(covariant _HunterSparklinePainter oldDelegate) {
    return oldDelegate.history != history ||
        oldDelegate.line1Key != line1Key ||
        oldDelegate.line2Key != line2Key ||
        oldDelegate.line1Color != line1Color ||
        oldDelegate.line2Color != line2Color ||
        oldDelegate.gridColor != gridColor;
  }
}

class _PathField extends StatelessWidget {
  const _PathField({
    required this.label,
    required this.controller,
    required this.hintText,
  });

  final String label;
  final TextEditingController controller;
  final String hintText;

  @override
  Widget build(BuildContext context) {
    return TextField(
      controller: controller,
      decoration: InputDecoration(
        labelText: label,
        hintText: hintText,
        isDense: true,
        border: OutlineInputBorder(borderRadius: BorderRadius.circular(12)),
      ),
    );
  }
}

class MyHomePage extends StatefulWidget {
  const MyHomePage({super.key, required this.title});

  // This widget is the home page of your application. It is stateful, meaning
  // that it has a State object (defined below) that contains fields that affect
  // how it looks.

  // This class is the configuration for the state. It holds the values (in this
  // case the title) provided by the parent (in this case the App widget) and
  // used by the build method of the State. Fields in a Widget subclass are
  // always marked "final".

  final String title;

  @override
  State<MyHomePage> createState() => _MyHomePageState();
}

class _MyHomePageState extends State<MyHomePage> {
  int _counter = 0;

  void _incrementCounter() {
    setState(() {
      // This call to setState tells the Flutter framework that something has
      // changed in this State, which causes it to rerun the build method below
      // so that the display can reflect the updated values. If we changed
      // _counter without calling setState(), then the build method would not be
      // called again, and so nothing would appear to happen.
      _counter++;
    });
  }

  @override
  Widget build(BuildContext context) {
    // This method is rerun every time setState is called, for instance as done
    // by the _incrementCounter method above.
    //
    // The Flutter framework has been optimized to make rerunning build methods
    // fast, so that you can just rebuild anything that needs updating rather
    // than having to individually change instances of widgets.
    return Scaffold(
      appBar: AppBar(
        // TRY THIS: Try changing the color here to a specific color (to
        // Colors.amber, perhaps?) and trigger a hot reload to see the AppBar
        // change color while the other colors stay the same.
        backgroundColor: Theme.of(context).colorScheme.inversePrimary,
        // Here we take the value from the MyHomePage object that was created by
        // the App.build method, and use it to set our appbar title.
        title: Text(widget.title),
      ),
      body: Center(
        // Center is a layout widget. It takes a single child and positions it
        // in the middle of the parent.
        child: Column(
          // Column is also a layout widget. It takes a list of children and
          // arranges them vertically. By default, it sizes itself to fit its
          // children horizontally, and tries to be as tall as its parent.
          //
          // Column has various properties to control how it sizes itself and
          // how it positions its children. Here we use mainAxisAlignment to
          // center the children vertically; the main axis here is the vertical
          // axis because Columns are vertical (the cross axis would be
          // horizontal).
          //
          // TRY THIS: Invoke "debug painting" (choose the "Toggle Debug Paint"
          // action in the IDE, or press "p" in the console), to see the
          // wireframe for each widget.
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            const Text('You have pushed the button this many times:'),
            Text(
              '$_counter',
              style: Theme.of(context).textTheme.headlineMedium,
            ),
          ],
        ),
      ),
      floatingActionButton: FloatingActionButton(
        onPressed: _incrementCounter,
        tooltip: 'Increment',
        child: const Icon(Icons.add),
      ),
    );
  }
}
