import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'src/capture_exporter.dart';
import 'src/gps_service.dart';
import 'src/rtl433_plugin.dart';
import 'src/sdr_intent_launcher.dart'; // now UsbHelper

void main() {
  runApp(const And433App());
}

class And433App extends StatelessWidget {
  const And433App({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'And433',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: Colors.indigo),
        useMaterial3: true,
      ),
      home: const DecoderPage(),
    );
  }
}

class DecoderPage extends StatefulWidget {
  const DecoderPage({super.key});

  @override
  State<DecoderPage> createState() => _DecoderPageState();
}

class _DecoderPageState extends State<DecoderPage> {
  static const _freqHz = 433920000;
  static const _sampleRate = 250000;

  bool _isRunning = false;
  String _status = 'Stopped';
  final List<Map<String, dynamic>> _packets = [];
  StreamSubscription<Map<String, dynamic>>? _sub;

  // Search & filter state
  String _searchQuery = '';
  final Set<String> _selectedModels = {};

  // SDR settings
  double _gainDb = 40.0; // 0.0 = AGC
  bool _biasT = false;

  List<Map<String, dynamic>> get _filteredPackets {
    final query = _searchQuery.toLowerCase();
    return _packets.where((p) {
      final matchesModel = _selectedModels.isEmpty ||
          _selectedModels.contains(p['model'] as String? ?? 'Unknown');
      final matchesSearch = query.isEmpty ||
          jsonEncode(p).toLowerCase().contains(query);
      return matchesModel && matchesSearch;
    }).toList();
  }

  List<String> get _uniqueModels {
    final seen = <String>{};
    return _packets
        .map((p) => p['model'] as String? ?? 'Unknown')
        .where(seen.add)
        .toList();
  }

  Future<void> _start() async {
    setState(() {
      _status = 'Opening USB device…';
      _packets.clear();
    });

    try {
      await GpsService.instance.start(); // best-effort; no GPS = no tags
      final devQuery = await UsbHelper.instance.openDevice();

      setState(() => _status = 'Starting decoder…');

      final stream = Rtl433Plugin.instance.startDecoding(
        devQuery: devQuery,
        freqHz: _freqHz,
        sampleRate: _sampleRate,
        gainStr: _gainDb == 0.0 ? null : _gainDb.toStringAsFixed(1),
        biasT: _biasT,
      );

      _sub = stream.listen(
        (packet) {
          GpsService.instance.inject(packet);
          setState(() {
            _packets.insert(0, packet);
            if (_packets.length > 200) _packets.removeLast();
          });
        },
        onError: (Object e) {
          setState(() {
            _status = 'Error: $e';
            _isRunning = false;
          });
        },
        onDone: () {
          setState(() {
            _status = 'Stopped';
            _isRunning = false;
          });
        },
      );

      setState(() {
        _isRunning = true;
        _status = 'Decoding…';
      });
    } on PlatformException catch (e) {
      setState(() => _status = 'USB error: ${e.message}');
    } catch (e) {
      setState(() => _status = 'Failed: $e');
    }
  }

  Future<void> _stop() async {
    setState(() => _status = 'Stopping…');
    await _sub?.cancel();
    _sub = null;
    await Rtl433Plugin.instance.stopDecoding();
    await UsbHelper.instance.closeDevice();
    GpsService.instance.stop();
    setState(() {
      _isRunning = false;
      _status = 'Stopped';
    });
  }

  @override
  void dispose() {
    _sub?.cancel();
    Rtl433Plugin.instance.stopDecoding();
    super.dispose();
  }

  Future<void> _showExportDialog() async {
    final choice = await showDialog<String>(
      context: context,
      builder: (ctx) => SimpleDialog(
        title: const Text('Export captures as…'),
        children: [
          SimpleDialogOption(
            onPressed: () => Navigator.pop(ctx, 'a433'),
            child: const ListTile(
              leading: Icon(Icons.data_object),
              title: Text('.a433  (JSONL — full data)'),
            ),
          ),
          SimpleDialogOption(
            onPressed: () => Navigator.pop(ctx, 'kml'),
            child: const ListTile(
              leading: Icon(Icons.map),
              title: Text('.kml  (Google Earth / Maps)'),
            ),
          ),
        ],
      ),
    );
    if (choice == null) return;
    try {
      if (choice == 'a433') {
        await CaptureExporter.exportA433(_packets, _freqHz);
      } else {
        await CaptureExporter.exportKml(_packets);
      }
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Export failed: $e')),
        );
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('And433 – RF Decoder'),
        backgroundColor: Theme.of(context).colorScheme.inversePrimary,
        actions: [
          if (_packets.isNotEmpty)
            IconButton(
              icon: const Icon(Icons.upload_file),
              tooltip: 'Export',
              onPressed: _showExportDialog,
            ),
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 16),
            child: Center(
              child: Text(
                _status,
                style: Theme.of(context).textTheme.bodySmall,
              ),
            ),
          ),
        ],
      ),
      body: Column(
        children: [
          // ── SDR settings (shown when stopped) ─────────────────────────
          if (!_isRunning)
            Card(
              margin: const EdgeInsets.fromLTRB(12, 8, 12, 0),
              child: Padding(
                padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Row(
                      children: [
                        Expanded(
                          child: Text(
                            _gainDb == 0.0
                                ? 'Gain: AGC (automatic)'
                                : 'Gain: ${_gainDb.toStringAsFixed(1)} dB',
                            style: Theme.of(context).textTheme.bodyMedium,
                          ),
                        ),
                      ],
                    ),
                    Slider(
                      value: _gainDb,
                      min: 0.0,
                      max: 49.6,
                      divisions: 496,
                      label: _gainDb == 0.0
                          ? 'AGC'
                          : '${_gainDb.toStringAsFixed(1)} dB',
                      onChanged: (v) => setState(() => _gainDb = v),
                    ),
                    SwitchListTile(
                      contentPadding: EdgeInsets.zero,
                      title: const Text('Bias-T (5V antenna supply)'),
                      subtitle: const Text('RTL-SDR Blog V3/V4 only'),
                      value: _biasT,
                      onChanged: (v) => setState(() => _biasT = v),
                    ),
                  ],
                ),
              ),
            ),
          // ── Search bar ─────────────────────────────────────────────────
          Padding(
            padding: const EdgeInsets.fromLTRB(12, 8, 12, 4),
            child: TextField(
              decoration: InputDecoration(
                hintText: 'Search packets…',
                prefixIcon: const Icon(Icons.search),
                suffixIcon: _searchQuery.isNotEmpty
                    ? IconButton(
                        icon: const Icon(Icons.clear),
                        onPressed: () => setState(() => _searchQuery = ''),
                      )
                    : null,
                isDense: true,
                border: OutlineInputBorder(
                  borderRadius: BorderRadius.circular(24),
                ),
              ),
              onChanged: (v) => setState(() => _searchQuery = v),
            ),
          ),
          // ── Device filter chips ────────────────────────────────────────
          if (_uniqueModels.isNotEmpty)
            SizedBox(
              height: 40,
              child: ListView(
                padding: const EdgeInsets.symmetric(horizontal: 12),
                scrollDirection: Axis.horizontal,
                children: _uniqueModels.map((model) {
                  final selected = _selectedModels.contains(model);
                  return Padding(
                    padding: const EdgeInsets.only(right: 6),
                    child: FilterChip(
                      label: Text(model),
                      selected: selected,
                      onSelected: (on) => setState(() {
                        if (on) {
                          _selectedModels.add(model);
                        } else {
                          _selectedModels.remove(model);
                        }
                      }),
                    ),
                  );
                }).toList(),
              ),
            ),
          // ── Decoded packets ────────────────────────────────────────────
          Expanded(
            child: _filteredPackets.isEmpty
                ? Center(
                    child: Text(_packets.isEmpty
                        ? 'No packets decoded yet.'
                        : 'No packets match filters.'),
                  )
                : ListView.builder(
                    itemCount: _filteredPackets.length,
                    itemBuilder: (context, index) {
                      final p = _filteredPackets[index];
                      final model = p['model'] as String? ?? 'Unknown';
                      final time = p['time'] as String? ?? '';
                      return ListTile(
                        leading: const Icon(Icons.radio),
                        title: Text(model),
                        subtitle: Text(
                          p.entries
                              .where((e) => e.key != 'model' && e.key != 'time')
                              .map((e) => '${e.key}: ${e.value}')
                              .join('  '),
                          maxLines: 2,
                          overflow: TextOverflow.ellipsis,
                        ),
                        trailing: Text(
                          time.length > 8 ? time.substring(time.length - 8) : time,
                          style: Theme.of(context).textTheme.bodySmall,
                        ),
                      );
                    },
                  ),
          ),
        ],
      ),
      floatingActionButton: FloatingActionButton.extended(
        onPressed: _isRunning ? _stop : _start,
        icon: Icon(_isRunning ? Icons.stop : Icons.play_arrow),
        label: Text(_isRunning ? 'Stop' : 'Start'),
      ),
    );
  }
}
