import 'package:flutter_test/flutter_test.dart';
import 'package:flutter/material.dart';

import 'package:hunter_dashboard/models.dart';
import 'package:hunter_dashboard/services.dart';
import 'package:hunter_dashboard/widgets/advanced_workbench_section.dart';

void main() {
  test('fmtDuration formats correctly', () {
    expect(fmtDuration(const Duration(seconds: 0)), '0s');
    expect(fmtDuration(const Duration(seconds: 90)), '1m 30s');
    expect(fmtDuration(const Duration(hours: 2, minutes: 5)), '2h 5m');
  });

  test('fmtNumber formats correctly', () {
    expect(fmtNumber(42), '42');
    expect(fmtNumber(1500), '1.5K');
    expect(fmtNumber(2500000), '2.5M');
  });

  test('HunterLogLine stores fields', () {
    final HunterLogLine line = HunterLogLine(ts: DateTime(2025), source: 'out', message: 'test');
    expect(line.source, 'out');
    expect(line.message, 'test');
  });

  testWidgets('Advanced workbench keeps selected tab on rebuild and follows external tab changes', (WidgetTester tester) async {
    int initialTabIndex = 0;
    int availableConfigsCount = 5;
    int lastChangedTab = -1;

    Widget buildApp() {
      return MaterialApp(
        home: Scaffold(
          body: SizedBox(
            width: 1400,
            height: 900,
            child: AdvancedWorkbenchSection(
              initialTabIndex: initialTabIndex,
              availableConfigsCount: availableConfigsCount,
              checkedConfigsCount: 12,
              logCount: 2,
              docsCount: 3,
              processInfo: 'PID 123',
              onTabChanged: (int index) {
                lastChangedTab = index;
              },
              statisticsChild: const Center(child: Text('stats child')),
              configsChild: const Center(child: Text('configs child')),
              logsChild: const Center(child: Text('logs child')),
              docsChild: const Center(child: Text('docs child')),
              runtimeChild: const Center(child: Text('runtime child')),
            ),
          ),
        ),
      );
    }

    await tester.pumpWidget(buildApp());
    expect(find.text('stats child'), findsOneWidget);

    await tester.tap(find.text('Logs'));
    await tester.pumpAndSettle();
    expect(find.text('logs child'), findsOneWidget);
    expect(lastChangedTab, 2);

    availableConfigsCount = 9;
    await tester.pumpWidget(buildApp());
    await tester.pumpAndSettle();
    expect(find.text('logs child'), findsOneWidget);

    initialTabIndex = 3;
    await tester.pumpWidget(buildApp());
    await tester.pumpAndSettle();
    expect(find.text('docs child'), findsOneWidget);
    expect(lastChangedTab, 3);
  });
}
