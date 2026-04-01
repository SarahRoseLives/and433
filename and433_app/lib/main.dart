import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

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
  final List<String> _logs = [];
  StreamSubscription<Map<String, dynamic>>? _sub;
  StreamSubscription<String>? _logSub;

  Future<void> _start() async {
    setState(() {
      _status = 'Opening USB device…';
      _packets.clear();
    });

    try {
      // Get a native USB file descriptor from Android's UsbManager.
      // Returns "fd:N:/dev/bus/usb/XXX/YYY" for rtl433_ffi_start.
      final devQuery = await UsbHelper.instance.openDevice();

      setState(() => _status = 'Starting decoder…');

      final stream = Rtl433Plugin.instance.startDecoding(
        devQuery: devQuery,
        freqHz: _freqHz,
        sampleRate: _sampleRate,
      );

      _logSub = Rtl433Plugin.instance.logStream.listen((line) {
        setState(() {
          _logs.insert(0, line);
          if (_logs.length > 100) _logs.removeLast();
        });
      });

      _sub = stream.listen(
        (packet) {
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
    await _logSub?.cancel();
    _logSub = null;
    await _sub?.cancel();
    _sub = null;
    await Rtl433Plugin.instance.stopDecoding();
    await UsbHelper.instance.closeDevice();
    setState(() {
      _isRunning = false;
      _status = 'Stopped';
    });
  }

  @override
  void dispose() {
    _logSub?.cancel();
    _sub?.cancel();
    Rtl433Plugin.instance.stopDecoding();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('And433 – RF Decoder'),
        backgroundColor: Theme.of(context).colorScheme.inversePrimary,
        actions: [
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
          // ── Diagnostics log (collapsible) ──────────────────────────────
          if (_isRunning || _logs.isNotEmpty)
            ExpansionTile(
              title: Text('Diagnostics (${_logs.length})',
                  style: Theme.of(context).textTheme.bodySmall),
              initiallyExpanded: _packets.isEmpty,
              children: [
                SizedBox(
                  height: 160,
                  child: _logs.isEmpty
                      ? const Center(
                          child: Text('Waiting for log messages…',
                              style: TextStyle(fontSize: 12, color: Colors.grey)))
                      : ListView.builder(
                    reverse: true,
                    itemCount: _logs.length,
                    itemBuilder: (_, i) => Padding(
                      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 1),
                      child: Text(_logs[i],
                          style: TextStyle(
                            fontFamily: 'monospace',
                            fontSize: 11,
                            color: _logs[i].contains('ERROR') || _logs[i].contains('CRIT')
                                ? Colors.red
                                : _logs[i].contains('WARN')
                                    ? Colors.orange
                                    : null,
                          )),
                    ),
                  ),
                ),
              ],
            ),
          // ── Decoded packets ────────────────────────────────────────────
          Expanded(
            child: _packets.isEmpty
                ? const Center(child: Text('No packets decoded yet.'))
                : ListView.builder(
                    itemCount: _packets.length,
                    itemBuilder: (context, index) {
                      final p = _packets[index];
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
