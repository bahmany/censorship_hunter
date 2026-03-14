import 'package:flutter/material.dart';
import '../models.dart';
import '../theme.dart';

class OneButtonConnect extends StatelessWidget {
  const OneButtonConnect({
    super.key,
    required this.runState,
    required this.onPressed,
  });

  final HunterRunState runState;
  final VoidCallback? onPressed;

  @override
  Widget build(BuildContext context) {
    final bool busy = runState == HunterRunState.starting || runState == HunterRunState.stopping;
    final bool connected = runState == HunterRunState.running;
    final Color color = switch (runState) {
      HunterRunState.running => C.neonRed,
      HunterRunState.starting || HunterRunState.stopping => C.neonAmber,
      HunterRunState.stopped => C.neonGreen,
    };
    final String label = switch (runState) {
      HunterRunState.stopped => 'Start Discovery',
      HunterRunState.starting => 'Starting...',
      HunterRunState.running => 'Stop Discovery',
      HunterRunState.stopping => 'Stopping...',
    };

    return AnimatedContainer(
      duration: const Duration(milliseconds: 240),
      width: 230,
      height: 230,
      decoration: BoxDecoration(
        shape: BoxShape.circle,
        gradient: RadialGradient(
          colors: <Color>[
            color.withValues(alpha: 0.28),
            color.withValues(alpha: 0.08),
            C.card,
          ],
          stops: const <double>[0.0, 0.55, 1.0],
        ),
        border: Border.all(color: color.withValues(alpha: 0.75), width: 3),
        boxShadow: <BoxShadow>[
          BoxShadow(color: color.withValues(alpha: 0.24), blurRadius: 28, spreadRadius: 4),
        ],
      ),
      child: Material(
        color: Colors.transparent,
        child: InkWell(
          onTap: onPressed,
          customBorder: const CircleBorder(),
          child: Center(
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: <Widget>[
                if (busy)
                  SizedBox(
                    width: 42,
                    height: 42,
                    child: CircularProgressIndicator(
                      strokeWidth: 3,
                      color: color,
                    ),
                  )
                else
                  Icon(
                    connected ? Icons.power_settings_new_rounded : Icons.shield_rounded,
                    size: 56,
                    color: color,
                  ),
                const SizedBox(height: 18),
                Text(
                  label,
                  textAlign: TextAlign.center,
                  style: TextStyle(
                    color: color,
                    fontSize: 24,
                    fontWeight: FontWeight.w800,
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
