import 'package:flutter_test/flutter_test.dart';

import 'package:hunter_dashboard/models.dart';
import 'package:hunter_dashboard/services.dart';

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
}
