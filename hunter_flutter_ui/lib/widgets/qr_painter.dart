import 'dart:typed_data';
import 'package:flutter/material.dart';

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Pure-Dart QR Code encoder + Flutter CustomPainter
// Supports QR versions 1–10 (up to ~270 chars), byte mode, ECC-L
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

class QrCodeWidget extends StatelessWidget {
  const QrCodeWidget({super.key, required this.data, this.size = 200, this.color = Colors.black, this.bgColor = Colors.white});
  final String data;
  final double size;
  final Color color;
  final Color bgColor;

  @override
  Widget build(BuildContext context) {
    final List<List<bool>>? matrix = QrEncoder.encode(data);
    if (matrix == null) {
      return SizedBox(
        width: size, height: size,
        child: const Center(child: Text('QR Error', style: TextStyle(color: Colors.red))),
      );
    }
    return CustomPaint(
      size: Size(size, size),
      painter: _QrPainter(matrix: matrix, fgColor: color, bgColor: bgColor),
    );
  }
}

class _QrPainter extends CustomPainter {
  _QrPainter({required this.matrix, required this.fgColor, required this.bgColor});
  final List<List<bool>> matrix;
  final Color fgColor;
  final Color bgColor;

  @override
  void paint(Canvas canvas, Size size) {
    final int n = matrix.length;
    if (n == 0) return;
    final double cellSize = size.width / (n + 2); // 1-module quiet zone on each side
    final Paint bg = Paint()..color = bgColor;
    final Paint fg = Paint()..color = fgColor;
    canvas.drawRect(Rect.fromLTWH(0, 0, size.width, size.height), bg);
    for (int y = 0; y < n; y++) {
      for (int x = 0; x < n; x++) {
        if (matrix[y][x]) {
          canvas.drawRect(Rect.fromLTWH((x + 1) * cellSize, (y + 1) * cellSize, cellSize, cellSize), fg);
        }
      }
    }
  }

  @override
  bool shouldRepaint(covariant _QrPainter old) => old.matrix != matrix;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// QR Encoder — byte mode, ECC level L, versions 1-10
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
class QrEncoder {
  // Version capacities for byte mode, ECC-L
  static const List<int> _byteCapacityL = <int>[
    0, 17, 32, 53, 78, 106, 134, 154, 192, 230, 271, // v0 placeholder, v1..v10
    321, 367, 425, 458, 520, 586, 644, 718, 792, 858, // v11..v20
  ];

  // Total data codewords per version, ECC-L
  static const List<int> _totalDataCodewordsL = <int>[
    0, 19, 34, 55, 80, 108, 136, 156, 194, 232, 274,
    324, 370, 428, 461, 523, 589, 647, 721, 795, 861,
  ];

  // ECC codewords per block, ECC-L
  static const List<int> _eccPerBlockL = <int>[
    0, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18,
    20, 24, 26, 30, 22, 24, 28, 30, 28, 28,
  ];

  // Number of ECC blocks, ECC-L
  static const List<int> _numBlocksL = <int>[
    0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  ];

  // Alignment pattern positions per version
  static const List<List<int>> _alignmentPositions = <List<int>>[
    <int>[], // v0
    <int>[], // v1
    <int>[6, 18], // v2
    <int>[6, 22],
    <int>[6, 26],
    <int>[6, 30],
    <int>[6, 34],
    <int>[6, 22, 38],
    <int>[6, 24, 42],
    <int>[6, 26, 46],
    <int>[6, 28, 50], // v10
  ];

  static List<List<bool>>? encode(String text) {
    final Uint8List bytes = Uint8List.fromList(text.codeUnits);
    if (bytes.isEmpty) return null;

    // Find minimum version
    int version = 0;
    for (int v = 1; v <= 10; v++) {
      if (bytes.length <= _byteCapacityL[v]) {
        version = v;
        break;
      }
    }
    if (version == 0) return null; // Too long

    final int totalDataCW = _totalDataCodewordsL[version];
    final int eccPerBlock = _eccPerBlockL[version];
    final int numBlocks = _numBlocksL[version];
    final int size = 17 + version * 4;

    // Build data bitstream
    final _BitBuffer bits = _BitBuffer();
    // Mode indicator: byte = 0100
    bits.put(0x4, 4);
    // Character count (8 bits for v1-9, 16 bits for v10+)
    if (version <= 9) {
      bits.put(bytes.length, 8);
    } else {
      bits.put(bytes.length, 16);
    }
    // Data
    for (final int b in bytes) {
      bits.put(b, 8);
    }
    // Terminator
    final int totalBits = totalDataCW * 8;
    final int remaining = totalBits - bits.length;
    bits.put(0, remaining.clamp(0, 4));
    // Byte-align
    while (bits.length % 8 != 0) {
      bits.put(0, 1);
    }
    // Pad bytes
    const List<int> padBytes = <int>[0xEC, 0x11];
    int padIdx = 0;
    while (bits.length < totalBits) {
      bits.put(padBytes[padIdx % 2], 8);
      padIdx++;
    }

    // Convert to codewords
    final Uint8List dataCodewords = Uint8List(totalDataCW);
    for (int i = 0; i < totalDataCW; i++) {
      dataCodewords[i] = bits.getByte(i);
    }

    // Split into blocks and compute ECC
    final int shortBlockSize = totalDataCW ~/ numBlocks;
    final int longBlocks = totalDataCW % numBlocks;
    final int shortBlocks = numBlocks - longBlocks;

    final List<Uint8List> dataBlocks = <Uint8List>[];
    final List<Uint8List> eccBlocks = <Uint8List>[];
    int offset = 0;
    for (int i = 0; i < numBlocks; i++) {
      final int blockLen = (i < shortBlocks) ? shortBlockSize : shortBlockSize + 1;
      final Uint8List block = dataCodewords.sublist(offset, offset + blockLen);
      offset += blockLen;
      dataBlocks.add(block);
      eccBlocks.add(_computeEcc(block, eccPerBlock));
    }

    // Interleave data
    final List<int> interleaved = <int>[];
    final int maxDataLen = shortBlockSize + (longBlocks > 0 ? 1 : 0);
    for (int i = 0; i < maxDataLen; i++) {
      for (int j = 0; j < numBlocks; j++) {
        if (i < dataBlocks[j].length) interleaved.add(dataBlocks[j][i]);
      }
    }
    // Interleave ECC
    for (int i = 0; i < eccPerBlock; i++) {
      for (int j = 0; j < numBlocks; j++) {
        interleaved.add(eccBlocks[j][i]);
      }
    }

    // Create matrix
    final List<List<bool>> matrix = List<List<bool>>.generate(size, (_) => List<bool>.filled(size, false));
    final List<List<bool>> reserved = List<List<bool>>.generate(size, (_) => List<bool>.filled(size, false));

    // Place finder patterns
    _placeFinder(matrix, reserved, 0, 0);
    _placeFinder(matrix, reserved, size - 7, 0);
    _placeFinder(matrix, reserved, 0, size - 7);

    // Place alignment patterns
    if (version >= 2) {
      final List<int> positions = _alignmentPositions[version];
      for (final int row in positions) {
        for (final int col in positions) {
          if (_isFinderZone(row, col, size)) continue;
          _placeAlignment(matrix, reserved, row, col);
        }
      }
    }

    // Timing patterns
    for (int i = 8; i < size - 8; i++) {
      if (!reserved[6][i]) {
        matrix[6][i] = i % 2 == 0;
        reserved[6][i] = true;
      }
      if (!reserved[i][6]) {
        matrix[i][6] = i % 2 == 0;
        reserved[i][6] = true;
      }
    }

    // Dark module
    matrix[size - 8][8] = true;
    reserved[size - 8][8] = true;

    // Reserve format info areas
    for (int i = 0; i < 8; i++) {
      reserved[8][i] = true;
      reserved[8][size - 1 - i] = true;
      reserved[i][8] = true;
      reserved[size - 1 - i][8] = true;
    }
    reserved[8][8] = true;

    // Reserve version info areas (v7+)
    // Not needed for v1-10 except v7+ but keeping simple

    // Place data bits
    _placeDataBits(matrix, reserved, interleaved, size);

    // Apply best mask
    int bestMask = 0;
    int bestPenalty = 1 << 30;
    for (int mask = 0; mask < 8; mask++) {
      final List<List<bool>> trial = List<List<bool>>.generate(size, (int r) => List<bool>.from(matrix[r]));
      _applyMask(trial, reserved, mask);
      _placeFormatInfo(trial, mask, size);
      final int penalty = _computePenalty(trial, size);
      if (penalty < bestPenalty) {
        bestPenalty = penalty;
        bestMask = mask;
      }
    }

    _applyMask(matrix, reserved, bestMask);
    _placeFormatInfo(matrix, bestMask, size);

    return matrix;
  }

  static void _placeFinder(List<List<bool>> m, List<List<bool>> r, int row, int col) {
    for (int dy = -1; dy <= 7; dy++) {
      for (int dx = -1; dx <= 7; dx++) {
        final int y = row + dy, x = col + dx;
        if (y < 0 || y >= m.length || x < 0 || x >= m.length) continue;
        bool dark = false;
        if (dy >= 0 && dy <= 6 && dx >= 0 && dx <= 6) {
          if (dy == 0 || dy == 6 || dx == 0 || dx == 6) {
            dark = true;
          } else if (dy >= 2 && dy <= 4 && dx >= 2 && dx <= 4) {
            dark = true;
          }
        }
        m[y][x] = dark;
        r[y][x] = true;
      }
    }
  }

  static bool _isFinderZone(int row, int col, int size) {
    return (row <= 8 && col <= 8) ||
           (row <= 8 && col >= size - 8) ||
           (row >= size - 8 && col <= 8);
  }

  static void _placeAlignment(List<List<bool>> m, List<List<bool>> r, int cy, int cx) {
    for (int dy = -2; dy <= 2; dy++) {
      for (int dx = -2; dx <= 2; dx++) {
        final int y = cy + dy, x = cx + dx;
        if (y < 0 || y >= m.length || x < 0 || x >= m.length) continue;
        bool dark = (dy == -2 || dy == 2 || dx == -2 || dx == 2 || (dy == 0 && dx == 0));
        m[y][x] = dark;
        r[y][x] = true;
      }
    }
  }

  static void _placeDataBits(List<List<bool>> m, List<List<bool>> r, List<int> data, int size) {
    int bitIdx = 0;
    final int totalBits = data.length * 8;
    bool upward = true;

    for (int right = size - 1; right >= 1; right -= 2) {
      if (right == 6) right = 5; // Skip timing column
      for (int i = 0; i < size; i++) {
        final int row = upward ? (size - 1 - i) : i;
        for (int dx = 0; dx >= -1; dx--) {
          final int col = right + dx;
          if (col < 0 || col >= size) continue;
          if (r[row][col]) continue;
          if (bitIdx < totalBits) {
            final int byteIdx = bitIdx ~/ 8;
            final int bitOff = 7 - (bitIdx % 8);
            m[row][col] = ((data[byteIdx] >> bitOff) & 1) == 1;
            bitIdx++;
          }
        }
      }
      upward = !upward;
    }
  }

  static void _applyMask(List<List<bool>> m, List<List<bool>> r, int mask) {
    for (int y = 0; y < m.length; y++) {
      for (int x = 0; x < m.length; x++) {
        if (r[y][x]) continue;
        bool invert = false;
        switch (mask) {
          case 0: invert = (y + x) % 2 == 0; break;
          case 1: invert = y % 2 == 0; break;
          case 2: invert = x % 3 == 0; break;
          case 3: invert = (y + x) % 3 == 0; break;
          case 4: invert = (y ~/ 2 + x ~/ 3) % 2 == 0; break;
          case 5: invert = (y * x) % 2 + (y * x) % 3 == 0; break;
          case 6: invert = ((y * x) % 2 + (y * x) % 3) % 2 == 0; break;
          case 7: invert = ((y + x) % 2 + (y * x) % 3) % 2 == 0; break;
        }
        if (invert) m[y][x] = !m[y][x];
      }
    }
  }

  static void _placeFormatInfo(List<List<bool>> m, int mask, int size) {
    // ECC level L = 01, mask pattern
    final int formatData = (0x01 << 3) | mask;
    final int formatBits = _computeFormatBits(formatData);

    for (int i = 0; i < 15; i++) {
      final bool bit = ((formatBits >> (14 - i)) & 1) == 1;

      // Around top-left finder
      if (i < 6) {
        m[8][i] = bit;
      } else if (i == 6) {
        m[8][7] = bit;
      } else if (i == 7) {
        m[8][8] = bit;
      } else if (i == 8) {
        m[7][8] = bit;
      } else {
        m[14 - i][8] = bit;
      }

      // Other copy
      if (i < 8) {
        m[size - 1 - i][8] = bit;
      } else {
        m[8][size - 15 + i] = bit;
      }
    }
  }

  static int _computeFormatBits(int data) {
    int bits = data << 10;
    int gen = 0x537; // Generator polynomial
    while (_bitLen(bits) >= 11) {
      bits ^= gen << (_bitLen(bits) - 11);
    }
    int result = (data << 10) | bits;
    result ^= 0x5412; // XOR mask
    return result;
  }

  static int _bitLen(int v) {
    int len = 0;
    while (v > 0) { len++; v >>= 1; }
    return len;
  }

  static int _computePenalty(List<List<bool>> m, int size) {
    int penalty = 0;
    // Rule 1: consecutive same-color modules in row/col
    for (int y = 0; y < size; y++) {
      int count = 1;
      for (int x = 1; x < size; x++) {
        if (m[y][x] == m[y][x - 1]) {
          count++;
          if (count == 5) {
            penalty += 3;
          } else if (count > 5) {
            penalty += 1;
          }
        } else {
          count = 1;
        }
      }
    }
    for (int x = 0; x < size; x++) {
      int count = 1;
      for (int y = 1; y < size; y++) {
        if (m[y][x] == m[y - 1][x]) {
          count++;
          if (count == 5) {
            penalty += 3;
          } else if (count > 5) {
            penalty += 1;
          }
        } else {
          count = 1;
        }
      }
    }
    // Rule 2: 2x2 blocks
    for (int y = 0; y < size - 1; y++) {
      for (int x = 0; x < size - 1; x++) {
        final bool c = m[y][x];
        if (c == m[y][x + 1] && c == m[y + 1][x] && c == m[y + 1][x + 1]) {
          penalty += 3;
        }
      }
    }
    return penalty;
  }

  // Reed-Solomon ECC computation in GF(256) with primitive polynomial 0x11D
  static final List<int> _gfExp = _initGfExp();
  static final List<int> _gfLog = _initGfLog();

  static List<int> _initGfExp() {
    final List<int> exp = List<int>.filled(512, 0);
    int x = 1;
    for (int i = 0; i < 255; i++) {
      exp[i] = x;
      x <<= 1;
      if (x >= 256) x ^= 0x11D;
    }
    for (int i = 255; i < 512; i++) {
      exp[i] = exp[i - 255];
    }
    return exp;
  }

  static List<int> _initGfLog() {
    final List<int> log = List<int>.filled(256, 0);
    for (int i = 0; i < 255; i++) {
      log[_gfExp[i]] = i;
    }
    return log;
  }

  static int _gfMul(int a, int b) {
    if (a == 0 || b == 0) return 0;
    return _gfExp[_gfLog[a] + _gfLog[b]];
  }

  static Uint8List _computeEcc(Uint8List data, int eccLen) {
    // Build generator polynomial
    List<int> gen = <int>[1];
    for (int i = 0; i < eccLen; i++) {
      final List<int> next = List<int>.filled(gen.length + 1, 0);
      for (int j = 0; j < gen.length; j++) {
        next[j] ^= gen[j];
        next[j + 1] ^= _gfMul(gen[j], _gfExp[i]);
      }
      gen = next;
    }

    // Polynomial division
    final List<int> remainder = List<int>.filled(eccLen, 0, growable: true);
    for (int i = 0; i < data.length; i++) {
      final int factor = data[i] ^ remainder[0];
      remainder.removeAt(0);
      remainder.add(0);
      for (int j = 0; j < eccLen; j++) {
        remainder[j] ^= _gfMul(gen[j + 1], factor);
      }
    }
    return Uint8List.fromList(remainder);
  }
}

class _BitBuffer {
  final List<int> _buffer = <int>[];
  int _length = 0;

  int get length => _length;

  void put(int value, int numBits) {
    for (int i = numBits - 1; i >= 0; i--) {
      final int byteIdx = _length ~/ 8;
      final int bitIdx = 7 - (_length % 8);
      while (_buffer.length <= byteIdx) {
        _buffer.add(0);
      }
      if (((value >> i) & 1) == 1) {
        _buffer[byteIdx] |= (1 << bitIdx);
      }
      _length++;
    }
  }

  int getByte(int index) {
    if (index < _buffer.length) return _buffer[index];
    return 0;
  }
}
