import 'package:flutter/material.dart';

import '../theme.dart';

class AdvancedWorkbenchSection extends StatefulWidget {
  const AdvancedWorkbenchSection({
    super.key,
    required this.initialTabIndex,
    required this.availableConfigsCount,
    required this.checkedConfigsCount,
    required this.logCount,
    required this.docsCount,
    required this.processInfo,
    required this.statisticsChild,
    required this.configsChild,
    required this.logsChild,
    required this.docsChild,
    required this.runtimeChild,
    this.onTabChanged,
  });

  final int initialTabIndex;
  final int availableConfigsCount;
  final int checkedConfigsCount;
  final int logCount;
  final int docsCount;
  final String processInfo;
  final Widget statisticsChild;
  final Widget configsChild;
  final Widget logsChild;
  final Widget docsChild;
  final Widget runtimeChild;
  final ValueChanged<int>? onTabChanged;

  @override
  State<AdvancedWorkbenchSection> createState() => _AdvancedWorkbenchSectionState();
}

class _AdvancedWorkbenchSectionState extends State<AdvancedWorkbenchSection> with SingleTickerProviderStateMixin {
  late final TabController _tabController;

  int _safeIndex(int value) {
    if (value < 0) return 0;
    if (value > 4) return 4;
    return value;
  }

  @override
  void initState() {
    super.initState();
    _tabController = TabController(length: 5, vsync: this, initialIndex: _safeIndex(widget.initialTabIndex));
    _tabController.addListener(_handleTabChanged);
  }

  @override
  void didUpdateWidget(covariant AdvancedWorkbenchSection oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.initialTabIndex == widget.initialTabIndex) {
      return;
    }
    final int targetIndex = _safeIndex(widget.initialTabIndex);
    if (_tabController.index != targetIndex) {
      _tabController.animateTo(targetIndex);
    }
  }

  @override
  void dispose() {
    _tabController.removeListener(_handleTabChanged);
    _tabController.dispose();
    super.dispose();
  }

  void _handleTabChanged() {
    if (_tabController.indexIsChanging) return;
    widget.onTabChanged?.call(_tabController.index);
  }

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.all(20),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Container(
            width: double.infinity,
            padding: const EdgeInsets.all(18),
            decoration: BoxDecoration(
              color: C.card,
              borderRadius: BorderRadius.circular(20),
              border: Border.all(color: C.border),
            ),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                const Text(
                  'Advanced',
                  style: TextStyle(
                    color: C.txt1,
                    fontSize: 24,
                    fontWeight: FontWeight.w900,
                  ),
                ),
                const SizedBox(height: 8),
                const Text(
                  'All professional tools, logs, deep metrics, docs, and runtime controls are available here.',
                  style: TextStyle(
                    color: C.txt2,
                    fontSize: 13,
                    height: 1.6,
                  ),
                ),
                const SizedBox(height: 14),
                Wrap(
                  spacing: 10,
                  runSpacing: 10,
                  children: <Widget>[
                    _Chip(label: 'Available', value: '${widget.availableConfigsCount}', color: C.neonGreen),
                    _Chip(label: 'Checked', value: '${widget.checkedConfigsCount}', color: C.neonCyan),
                    _Chip(label: 'Logs', value: '${widget.logCount}', color: C.neonAmber),
                    _Chip(label: 'Docs', value: '${widget.docsCount}', color: C.neonPurple),
                    _Chip(label: 'Process', value: widget.processInfo, color: C.txt2),
                  ],
                ),
              ],
            ),
          ),
          const SizedBox(height: 16),
          Container(
            decoration: BoxDecoration(
              color: C.card,
              borderRadius: BorderRadius.circular(18),
              border: Border.all(color: C.border),
            ),
            child: TabBar(
              controller: _tabController,
              isScrollable: true,
              dividerColor: Colors.transparent,
              indicatorColor: C.neonCyan,
              labelColor: C.neonCyan,
              unselectedLabelColor: C.txt3,
              tabs: const <Widget>[
                Tab(text: 'Stats'),
                Tab(text: 'Configs'),
                Tab(text: 'Logs'),
                Tab(text: 'Docs'),
                Tab(text: 'Runtime'),
              ],
            ),
          ),
          const SizedBox(height: 16),
          Expanded(
            child: Container(
              decoration: BoxDecoration(
                color: C.surface,
                borderRadius: BorderRadius.circular(20),
                border: Border.all(color: C.border),
              ),
              clipBehavior: Clip.antiAlias,
              child: TabBarView(
                controller: _tabController,
                children: <Widget>[
                  widget.statisticsChild,
                  widget.configsChild,
                  widget.logsChild,
                  widget.docsChild,
                  widget.runtimeChild,
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class _Chip extends StatelessWidget {
  const _Chip({
    required this.label,
    required this.value,
    required this.color,
  });

  final String label;
  final String value;
  final Color color;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.1),
        borderRadius: BorderRadius.circular(999),
        border: Border.all(color: color.withValues(alpha: 0.24)),
      ),
      child: RichText(
        text: TextSpan(
          style: const TextStyle(fontSize: 11, fontFamily: 'Consolas'),
          children: <InlineSpan>[
            TextSpan(
              text: '$label: ',
              style: TextStyle(color: color, fontWeight: FontWeight.w700),
            ),
            TextSpan(
              text: value,
              style: const TextStyle(color: C.txt1, fontWeight: FontWeight.w600),
            ),
          ],
        ),
      ),
    );
  }
}
