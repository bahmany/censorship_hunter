import 'package:flutter/material.dart';

/// Racing / Neon / Trust — Color Psychology Palette
class C {
  C._();
  static const Color bg        = Color(0xFF0A0E17);
  static const Color surface   = Color(0xFF111827);
  static const Color card      = Color(0xFF1A2235);
  static const Color cardHi    = Color(0xFF1F2A3F);
  static const Color border    = Color(0xFF2A3650);
  static const Color neonCyan  = Color(0xFF22D3EE);
  static const Color neonGreen = Color(0xFF34D399);
  static const Color neonAmber = Color(0xFFFBBF24);
  static const Color neonRed   = Color(0xFFEF4444);
  static const Color neonPurple= Color(0xFFA78BFA);
  static const Color txt1      = Color(0xFFF1F5F9);
  static const Color txt2      = Color(0xFF94A3B8);
  static const Color txt3      = Color(0xFF64748B);
}

ThemeData hunterTheme() {
  return ThemeData(
    useMaterial3: true,
    brightness: Brightness.dark,
    scaffoldBackgroundColor: C.bg,
    colorScheme: ColorScheme.dark(
      primary: C.neonCyan,
      secondary: C.neonGreen,
      tertiary: C.neonAmber,
      error: C.neonRed,
      surface: C.surface,
      onSurface: C.txt1,
      onSurfaceVariant: C.txt2,
      outline: C.border,
      outlineVariant: C.border,
    ),
    cardTheme: CardThemeData(
      color: C.card,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(14)),
      elevation: 0,
    ),
    navigationRailTheme: NavigationRailThemeData(
      backgroundColor: C.surface,
      selectedIconTheme: const IconThemeData(color: C.neonCyan),
      unselectedIconTheme: const IconThemeData(color: C.txt3),
      indicatorColor: C.neonCyan.withValues(alpha: 0.12),
    ),
    appBarTheme: const AppBarTheme(
      backgroundColor: C.surface,
      surfaceTintColor: Colors.transparent,
      elevation: 0,
    ),
    fontFamily: 'Segoe UI',
  );
}
