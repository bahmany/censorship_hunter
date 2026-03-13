import 'package:flutter/material.dart';

import '../theme.dart';

class TacticalFrame extends StatelessWidget {
  const TacticalFrame({
    super.key,
    required this.child,
    this.accent = C.neonCyan,
    this.padding = const EdgeInsets.all(16),
    this.glow = true,
    this.pulseValue = 0.0,
    this.sweep = false,
  });

  final Widget child;
  final Color accent;
  final EdgeInsetsGeometry padding;
  final bool glow;
  final double pulseValue;
  final bool sweep;

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(14),
        boxShadow: glow
            ? <BoxShadow>[
                BoxShadow(
                  color: accent.withValues(alpha: 0.05 + 0.08 * pulseValue),
                  blurRadius: 16 + 10 * pulseValue,
                  spreadRadius: 1 + pulseValue,
                ),
              ]
            : null,
      ),
      child: CustomPaint(
        painter: _TacticalFramePainter(
          accent: accent,
          pulseValue: pulseValue,
          sweep: sweep,
        ),
        child: Padding(
          padding: padding,
          child: child,
        ),
      ),
    );
  }
}

class _TacticalFramePainter extends CustomPainter {
  const _TacticalFramePainter({
    required this.accent,
    required this.pulseValue,
    required this.sweep,
  });

  final Color accent;
  final double pulseValue;
  final bool sweep;

  @override
  void paint(Canvas canvas, Size size) {
    final RRect shell = RRect.fromRectAndRadius(Offset.zero & size, const Radius.circular(14));
    final Paint fill = Paint()
      ..shader = LinearGradient(
        begin: Alignment.topLeft,
        end: Alignment.bottomRight,
        colors: <Color>[
          accent.withValues(alpha: 0.05 + 0.05 * pulseValue),
          C.card,
          C.surface,
        ],
      ).createShader(Offset.zero & size);
    final Paint border = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.1
      ..color = accent.withValues(alpha: 0.22 + 0.16 * pulseValue);
    final Paint inner = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 0.8
      ..color = C.border.withValues(alpha: 0.85);
    final Paint grid = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 0.5
      ..color = accent.withValues(alpha: 0.05 + 0.06 * pulseValue);

    canvas.drawRRect(shell, fill);
    canvas.drawRRect(shell, border);
    canvas.drawRRect(shell.deflate(6), inner);

    for (double y = 20; y < size.height; y += 26) {
      canvas.drawLine(Offset(18, y), Offset(size.width - 18, y), grid);
    }
    for (double x = 26; x < size.width; x += 36) {
      canvas.drawLine(Offset(x, 18), Offset(x, size.height - 18), grid);
    }

    final Paint bracket = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 2
      ..strokeCap = StrokeCap.square
      ..color = accent.withValues(alpha: 0.56 + 0.20 * pulseValue);
    const double corner = 18;
    const double inset = 10;

    void cornerBracket(Offset a, Offset b, Offset c) {
      canvas.drawLine(a, b, bracket);
      canvas.drawLine(b, c, bracket);
    }

    cornerBracket(Offset(inset, inset + corner), Offset(inset, inset), Offset(inset + corner, inset));
    cornerBracket(Offset(size.width - inset - corner, inset), Offset(size.width - inset, inset), Offset(size.width - inset, inset + corner));
    cornerBracket(Offset(inset, size.height - inset - corner), Offset(inset, size.height - inset), Offset(inset + corner, size.height - inset));
    cornerBracket(Offset(size.width - inset - corner, size.height - inset), Offset(size.width - inset, size.height - inset), Offset(size.width - inset, size.height - inset - corner));

    final Paint tick = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1
      ..color = accent.withValues(alpha: 0.12 + 0.12 * pulseValue);
    final double midX = size.width / 2;
    canvas.drawLine(Offset(midX - 18, 8), Offset(midX + 18, 8), tick);
    canvas.drawLine(Offset(midX - 18, size.height - 8), Offset(midX + 18, size.height - 8), tick);

    if (sweep) {
      final double sweepY = 16 + (size.height - 32) * pulseValue;
      final Rect sweepRect = Rect.fromLTWH(14, sweepY - 10, size.width - 28, 20);
      final Paint sweepPaint = Paint()
        ..shader = LinearGradient(
          begin: Alignment.topCenter,
          end: Alignment.bottomCenter,
          colors: <Color>[
            accent.withValues(alpha: 0.0),
            accent.withValues(alpha: 0.02 + 0.05 * pulseValue),
            accent.withValues(alpha: 0.12 + 0.10 * pulseValue),
            accent.withValues(alpha: 0.02 + 0.05 * pulseValue),
            accent.withValues(alpha: 0.0),
          ],
          stops: const <double>[0.0, 0.2, 0.5, 0.8, 1.0],
        ).createShader(sweepRect);
      canvas.save();
      canvas.clipRRect(shell.deflate(8));
      canvas.drawRect(sweepRect, sweepPaint);
      final Paint scanLine = Paint()
        ..color = accent.withValues(alpha: 0.18 + 0.18 * pulseValue)
        ..strokeWidth = 1.2;
      canvas.drawLine(Offset(18, sweepY), Offset(size.width - 18, sweepY), scanLine);
      canvas.restore();
    }
  }

  @override
  bool shouldRepaint(covariant _TacticalFramePainter oldDelegate) {
    return oldDelegate.accent != accent ||
        oldDelegate.pulseValue != pulseValue ||
        oldDelegate.sweep != sweep;
  }
}
