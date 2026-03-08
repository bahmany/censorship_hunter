import 'dart:math' as math;
import 'package:flutter/material.dart';
import '../theme.dart';

class ArcGaugePainter extends CustomPainter {
  ArcGaugePainter({required this.value, required this.max, required this.color});
  final double value;
  final double max;
  final Color color;

  @override
  void paint(Canvas canvas, Size size) {
    final double r = math.min(size.width, size.height) / 2 - 6;
    final Offset center = Offset(size.width / 2, size.height / 2 + 4);
    const double startAngle = math.pi * 0.75;
    const double sweepTotal = math.pi * 1.5;
    final double ratio = max > 0 ? (value / max).clamp(0.0, 1.0) : 0.0;

    // Background arc
    final Paint bgPaint = Paint()
      ..color = C.border.withValues(alpha: 0.4)
      ..style = PaintingStyle.stroke
      ..strokeWidth = 6
      ..strokeCap = StrokeCap.round;
    canvas.drawArc(Rect.fromCircle(center: center, radius: r), startAngle, sweepTotal, false, bgPaint);

    // Value arc
    if (ratio > 0) {
      final Paint valPaint = Paint()
        ..color = color
        ..style = PaintingStyle.stroke
        ..strokeWidth = 6
        ..strokeCap = StrokeCap.round;
      canvas.drawArc(Rect.fromCircle(center: center, radius: r), startAngle, sweepTotal * ratio, false, valPaint);

      // Glow
      final Paint glowPaint = Paint()
        ..color = color.withValues(alpha: 0.25)
        ..style = PaintingStyle.stroke
        ..strokeWidth = 12
        ..strokeCap = StrokeCap.round
        ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 6);
      canvas.drawArc(Rect.fromCircle(center: center, radius: r), startAngle, sweepTotal * ratio, false, glowPaint);
    }
  }

  @override
  bool shouldRepaint(covariant ArcGaugePainter old) =>
      old.value != value || old.max != max || old.color != color;
}

class SparklinePainter extends CustomPainter {
  SparklinePainter({
    required this.history,
    required this.line1Key,
    required this.line2Key,
    required this.line1Color,
    required this.line2Color,
  });

  final List<Map<String, dynamic>> history;
  final String line1Key;
  final String line2Key;
  final Color line1Color;
  final Color line2Color;

  List<double> _extract(String key) {
    return history
        .map((Map<String, dynamic> r) => r[key])
        .whereType<num>()
        .map((num v) => v.toDouble())
        .toList();
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

    // Grid line
    canvas.drawLine(
      Offset(0, size.height - 1),
      Offset(size.width, size.height - 1),
      Paint()
        ..color = C.border.withValues(alpha: 0.3)
        ..strokeWidth = 1,
    );

    void drawSparkLine(List<double> vals, Color color) {
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

      // Glow under the line
      final Paint fillPaint = Paint()
        ..shader = LinearGradient(
          begin: Alignment.topCenter,
          end: Alignment.bottomCenter,
          colors: <Color>[color.withValues(alpha: 0.15), color.withValues(alpha: 0.0)],
        ).createShader(Rect.fromLTWH(0, 0, size.width, size.height));
      final Path fillPath = Path.from(path);
      fillPath.lineTo(size.width, size.height);
      fillPath.lineTo(0, size.height);
      fillPath.close();
      canvas.drawPath(fillPath, fillPaint);
    }

    drawSparkLine(a, line1Color);
    drawSparkLine(b, line2Color);
  }

  @override
  bool shouldRepaint(covariant SparklinePainter old) =>
      old.history != history || old.line1Key != line1Key || old.line2Key != line2Key;
}
