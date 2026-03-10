import 'package:flutter/material.dart';
import '../theme.dart';
import 'qr_painter.dart';

/// Shows a dialog with a QR code for the given config URI.
/// The user can scan this with their mobile phone to import the config.
void showQrDialog(BuildContext context, String uri, {String? title}) {
  showDialog<void>(
    context: context,
    builder: (BuildContext ctx) {
      return Dialog(
        backgroundColor: C.card,
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(16)),
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: <Widget>[
              Row(
                children: <Widget>[
                  const Icon(Icons.qr_code, color: C.neonCyan, size: 20),
                  const SizedBox(width: 8),
                  Expanded(
                    child: Text(
                      title ?? 'Scan with mobile',
                      style: const TextStyle(color: C.txt1, fontSize: 14, fontWeight: FontWeight.w700),
                    ),
                  ),
                  IconButton(
                    icon: const Icon(Icons.close, color: C.txt3, size: 18),
                    onPressed: () => Navigator.of(ctx).pop(),
                    padding: EdgeInsets.zero,
                    constraints: const BoxConstraints(),
                  ),
                ],
              ),
              const SizedBox(height: 16),
              Container(
                padding: const EdgeInsets.all(12),
                decoration: BoxDecoration(
                  color: Colors.white,
                  borderRadius: BorderRadius.circular(8),
                ),
                child: QrCodeWidget(
                  data: uri,
                  size: 220,
                  color: Colors.black,
                  bgColor: Colors.white,
                ),
              ),
              const SizedBox(height: 12),
              Container(
                width: double.infinity,
                padding: const EdgeInsets.all(8),
                decoration: BoxDecoration(
                  color: C.bg.withValues(alpha: 0.6),
                  borderRadius: BorderRadius.circular(6),
                ),
                child: SelectableText(
                  uri,
                  style: const TextStyle(color: C.txt2, fontSize: 10, fontFamily: 'Consolas'),
                  maxLines: 3,
                ),
              ),
              const SizedBox(height: 12),
              Text(
                'Scan this QR code with your V2Ray/Clash app on mobile',
                style: TextStyle(color: C.txt3.withValues(alpha: 0.7), fontSize: 10),
                textAlign: TextAlign.center,
              ),
            ],
          ),
        ),
      );
    },
  );
}
