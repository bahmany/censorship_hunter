import 'package:flutter/material.dart';

import '../services.dart';
import '../theme.dart';

class DocsSection extends StatefulWidget {
  const DocsSection({
    super.key,
    required this.docs,
    required this.selectedDocPath,
    required this.selectedDocContent,
    required this.loading,
    required this.error,
    required this.onRefresh,
    required this.onOpenDoc,
    required this.onCopyText,
  });

  final List<ProjectDocFile> docs;
  final String? selectedDocPath;
  final String selectedDocContent;
  final bool loading;
  final String? error;
  final Future<void> Function() onRefresh;
  final Future<void> Function(ProjectDocFile doc) onOpenDoc;
  final Future<void> Function(String text, {String? label}) onCopyText;

  @override
  State<DocsSection> createState() => _DocsSectionState();
}

class _DocsSectionState extends State<DocsSection> {
  final TextEditingController _searchCtl = TextEditingController();
  final ScrollController _contentScroll = ScrollController();

  @override
  void dispose() {
    _searchCtl.dispose();
    _contentScroll.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final String q = _searchCtl.text.trim().toLowerCase();
    final List<ProjectDocFile> visible = q.isEmpty
        ? widget.docs
        : widget.docs.where((ProjectDocFile doc) => doc.name.toLowerCase().contains(q)).toList();
    ProjectDocFile? selected;
    if (widget.selectedDocPath == null) {
      selected = visible.isNotEmpty ? visible.first : null;
    } else {
      for (final ProjectDocFile doc in widget.docs) {
        if (doc.absPath == widget.selectedDocPath) {
          selected = doc;
          break;
        }
      }
    }

    return LayoutBuilder(
      builder: (BuildContext context, BoxConstraints constraints) {
        final bool compact = constraints.maxWidth < 980;
        final bool extraCompact = constraints.maxWidth < 620;
        final Widget sidebar = _buildSidebar(context, visible, selected, extraCompact);
        final Widget content = _buildContentPane(context, selected, extraCompact);

        return Padding(
          padding: EdgeInsets.all(extraCompact ? 12 : 20),
          child: Column(
            children: <Widget>[
              _buildHeader(context, visible.length, extraCompact),
              SizedBox(height: extraCompact ? 12 : 16),
              Expanded(
                child: compact
                    ? Column(
                        children: <Widget>[
                          SizedBox(height: mathMin(320, constraints.maxHeight * 0.34), child: sidebar),
                          SizedBox(height: extraCompact ? 12 : 16),
                          Expanded(child: content),
                        ],
                      )
                    : Row(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: <Widget>[
                          SizedBox(width: 320, child: sidebar),
                          const SizedBox(width: 16),
                          Expanded(child: content),
                        ],
                      ),
              ),
            ],
          ),
        );
      },
    );
  }

  Widget _buildHeader(BuildContext context, int visibleCount, bool extraCompact) {
    return Container(
      padding: EdgeInsets.symmetric(horizontal: extraCompact ? 12 : 16, vertical: 12),
      decoration: BoxDecoration(
        color: C.card,
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: C.border),
      ),
      child: Wrap(
        spacing: 12,
        runSpacing: 10,
        crossAxisAlignment: WrapCrossAlignment.center,
        children: <Widget>[
          const Text('PROJECT DOCS', style: TextStyle(color: C.txt2, fontSize: 11, fontWeight: FontWeight.w700, letterSpacing: 1.6)),
          _chip('${widget.docs.length} files', C.neonCyan),
          _chip('$visibleCount visible', C.neonGreen),
          SizedBox(
            width: extraCompact ? double.infinity : 260,
            child: TextField(
              controller: _searchCtl,
              onChanged: (_) => setState(() {}),
              style: const TextStyle(color: C.txt1, fontSize: 12),
              decoration: InputDecoration(
                isDense: true,
                hintText: 'Search docs...',
                hintStyle: const TextStyle(color: C.txt3, fontSize: 12),
                prefixIcon: const Icon(Icons.search, color: C.txt3, size: 18),
                filled: true,
                fillColor: C.surface,
                border: OutlineInputBorder(borderRadius: BorderRadius.circular(10), borderSide: BorderSide(color: C.border.withValues(alpha: 0.5))),
                enabledBorder: OutlineInputBorder(borderRadius: BorderRadius.circular(10), borderSide: BorderSide(color: C.border.withValues(alpha: 0.5))),
                focusedBorder: OutlineInputBorder(borderRadius: BorderRadius.circular(10), borderSide: const BorderSide(color: C.neonCyan)),
              ),
            ),
          ),
          TextButton.icon(
            onPressed: widget.loading ? null : widget.onRefresh,
            icon: widget.loading
                ? const SizedBox(width: 14, height: 14, child: CircularProgressIndicator(strokeWidth: 1.6))
                : const Icon(Icons.refresh, size: 16),
            label: const Text('Refresh'),
            style: TextButton.styleFrom(foregroundColor: C.neonCyan),
          ),
        ],
      ),
    );
  }

  Widget _buildSidebar(BuildContext context, List<ProjectDocFile> visible, ProjectDocFile? selected, bool extraCompact) {
    return Container(
      decoration: BoxDecoration(
        color: C.card,
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: C.border),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Padding(
            padding: EdgeInsets.fromLTRB(extraCompact ? 12 : 16, 14, extraCompact ? 12 : 16, 10),
            child: const Text('DOCUMENT INDEX', style: TextStyle(color: C.txt3, fontSize: 10, fontWeight: FontWeight.w700, letterSpacing: 1.4)),
          ),
          if (widget.error != null)
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16),
              child: Text(widget.error!, style: const TextStyle(color: C.neonRed, fontSize: 11)),
            ),
          Expanded(
            child: visible.isEmpty
                ? const Center(child: Text('No matching docs', style: TextStyle(color: C.txt3, fontSize: 12)))
                : ListView.builder(
                    padding: const EdgeInsets.fromLTRB(8, 4, 8, 8),
                    itemCount: visible.length,
                    itemBuilder: (BuildContext context, int index) {
                      final ProjectDocFile doc = visible[index];
                      final bool active = selected?.absPath == doc.absPath;
                      final String tag = _docTag(doc.name);
                      final Color accent = _docColor(tag);
                      return InkWell(
                        onTap: () => widget.onOpenDoc(doc),
                        borderRadius: BorderRadius.circular(12),
                        child: AnimatedContainer(
                          duration: const Duration(milliseconds: 180),
                          margin: const EdgeInsets.symmetric(vertical: 4),
                          padding: const EdgeInsets.all(12),
                          decoration: BoxDecoration(
                            color: active ? accent.withValues(alpha: 0.1) : C.surface.withValues(alpha: 0.55),
                            borderRadius: BorderRadius.circular(12),
                            border: Border.all(color: active ? accent.withValues(alpha: 0.45) : C.border.withValues(alpha: 0.65)),
                          ),
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: <Widget>[
                              Wrap(
                                spacing: 8,
                                runSpacing: 6,
                                crossAxisAlignment: WrapCrossAlignment.center,
                                children: <Widget>[
                                  _chip(tag, accent),
                                  Text(_docSize(doc.sizeBytes), style: const TextStyle(color: C.txt3, fontSize: 10, fontFamily: 'Consolas')),
                                ],
                              ),
                              const SizedBox(height: 8),
                              Text(
                                doc.name,
                                maxLines: 2,
                                overflow: TextOverflow.ellipsis,
                                style: TextStyle(color: active ? C.txt1 : C.txt2, fontSize: 12, fontWeight: FontWeight.w700),
                              ),
                              const SizedBox(height: 6),
                              Text(
                                _docSubtitle(doc.name),
                                maxLines: 2,
                                overflow: TextOverflow.ellipsis,
                                style: const TextStyle(color: C.txt3, fontSize: 10),
                              ),
                            ],
                          ),
                        ),
                      );
                    },
                  ),
          ),
        ],
      ),
    );
  }

  Widget _buildContentPane(BuildContext context, ProjectDocFile? selected, bool extraCompact) {
    return Container(
      decoration: BoxDecoration(
        color: C.card,
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: C.border),
      ),
      child: selected == null
          ? const Center(child: Text('Select a document to start reading', style: TextStyle(color: C.txt3, fontSize: 12)))
          : Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Padding(
                  padding: EdgeInsets.fromLTRB(extraCompact ? 12 : 18, 16, extraCompact ? 12 : 18, 12),
                  child: Wrap(
                    spacing: 10,
                    runSpacing: 8,
                    crossAxisAlignment: WrapCrossAlignment.center,
                    children: <Widget>[
                      Text(
                        selected.name,
                        style: const TextStyle(color: C.txt1, fontSize: 16, fontWeight: FontWeight.w800),
                      ),
                      _chip(_docTag(selected.name), _docColor(_docTag(selected.name))),
                      Text(_docSize(selected.sizeBytes), style: const TextStyle(color: C.txt3, fontSize: 10, fontFamily: 'Consolas')),
                      TextButton.icon(
                        onPressed: widget.selectedDocContent.trim().isEmpty
                            ? null
                            : () => widget.onCopyText(widget.selectedDocContent, label: 'Document copied'),
                        icon: const Icon(Icons.content_copy, size: 16),
                        label: const Text('Copy'),
                        style: TextButton.styleFrom(foregroundColor: C.neonGreen),
                      ),
                    ],
                  ),
                ),
                const Divider(height: 1, color: C.border),
                Expanded(
                  child: Scrollbar(
                    controller: _contentScroll,
                    thumbVisibility: true,
                    child: SingleChildScrollView(
                      controller: _contentScroll,
                      padding: EdgeInsets.all(extraCompact ? 12 : 18),
                      child: SelectableText(
                        widget.selectedDocContent.trim().isEmpty ? 'Document is empty' : widget.selectedDocContent,
                        style: TextStyle(
                          color: widget.selectedDocContent.trim().isEmpty ? C.txt3 : C.txt1,
                          fontSize: extraCompact ? 12 : 13,
                          height: 1.55,
                          fontFamily: 'Consolas',
                        ),
                      ),
                    ),
                  ),
                ),
              ],
            ),
    );
  }

  Widget _chip(String label, Color color) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.12),
        borderRadius: BorderRadius.circular(999),
        border: Border.all(color: color.withValues(alpha: 0.25)),
      ),
      child: Text(label, style: TextStyle(color: color, fontSize: 10, fontWeight: FontWeight.w700, letterSpacing: 0.4)),
    );
  }

  String _docTag(String name) {
    final String lower = name.toLowerCase();
    if (lower.contains('architecture')) return 'ARCH';
    if (lower.contains('guide') || lower.contains('setup')) return 'GUIDE';
    if (lower.contains('summary')) return 'SUMMARY';
    if (lower.contains('report')) return 'REPORT';
    if (lower.contains('fix')) return 'FIX';
    return 'DOC';
  }

  Color _docColor(String tag) {
    return switch (tag) {
      'ARCH' => C.neonPurple,
      'GUIDE' => C.neonCyan,
      'SUMMARY' => C.neonGreen,
      'REPORT' => C.neonAmber,
      'FIX' => C.neonRed,
      _ => C.txt2,
    };
  }

  String _docSubtitle(String name) {
    return name.replaceAll('_', ' ').replaceAll('.md', '').replaceAll('.txt', '');
  }

  String _docSize(int sizeBytes) {
    if (sizeBytes >= 1024 * 1024) return '${(sizeBytes / 1024 / 1024).toStringAsFixed(1)} MB';
    if (sizeBytes >= 1024) return '${(sizeBytes / 1024).toStringAsFixed(1)} KB';
    return '$sizeBytes B';
  }
}

double mathMin(double a, double b) => a < b ? a : b;
