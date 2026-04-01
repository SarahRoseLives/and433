// High-level Dart API for rtl_433 decoding.
//
// Exposes [startDecoding] which returns a [Stream] of decoded RF packets as
// [Map<String, dynamic>] objects (parsed from the JSON produced by rtl_433).
//
// Usage:
//   final stream = Rtl433Plugin.instance.startDecoding(devQuery: 'fd:5:/dev/bus/usb/001/002');
//   stream.listen((packet) => print(packet));
//   // later:
//   await Rtl433Plugin.instance.stopDecoding();

import 'dart:async';
import 'dart:convert';
import 'dart:ffi';

import 'package:ffi/ffi.dart';

import 'rtl433_ffi_bindings.dart';

class Rtl433Plugin {
  Rtl433Plugin._();
  static final Rtl433Plugin instance = Rtl433Plugin._();

  StreamController<Map<String, dynamic>>? _controller;
  StreamController<String>? _logController;
  NativeCallable<Rtl433DataCbNative>? _nativeCallback;
  NativeCallable<Rtl433LogCbNative>? _nativeLogCallback;
  bool _running = false;

  /// Stream of decoded RF packets.
  Stream<Map<String, dynamic>> startDecoding({
    required String devQuery,
    int freqHz = 433920000,
    int sampleRate = 250000,
    String? gainStr,   // null = AGC; "40" = 40 dB etc.
    bool biasT = false,
  }) {
    if (_running) throw StateError('Already decoding. Call stopDecoding() first.');

    _controller = StreamController<Map<String, dynamic>>.broadcast(
      onCancel: () => stopDecoding(),
    );
    _logController = StreamController<String>.broadcast();

    // NativeCallable.listener is safe to call from any native thread.
    _nativeCallback = NativeCallable<Rtl433DataCbNative>.listener(
      (Pointer<Utf8> json, Pointer<Void> ctx) {
        if (_controller == null || _controller!.isClosed) return;
        try {
          final decoded = jsonDecode(json.toDartString());
          if (decoded is Map<String, dynamic>) {
            _controller!.add(decoded);
          }
        } catch (_) {}
      },
    );

    _nativeLogCallback = NativeCallable<Rtl433LogCbNative>.listener(
      (int level, Pointer<Utf8> src, Pointer<Utf8> msg, Pointer<Void> ctx) {
        if (_logController == null || _logController!.isClosed) return;
        final label = switch (level) {
          1 => 'FATAL', 2 => 'CRIT', 3 => 'ERROR',
          4 => 'WARN',  5 => 'INFO', 6 => 'INFO', _ => 'DBG',
        };
        _logController!.add('[$label][${src.toDartString()}] ${msg.toDartString()}');
      },
    );

    Rtl433FfiBindings.instance.setLogCb(
      _nativeLogCallback!.nativeFunction,
      nullptr,
    );

    final devQueryPtr = devQuery.toNativeUtf8(allocator: malloc);
    final gainStrPtr = (gainStr != null && gainStr.isNotEmpty)
        ? gainStr.toNativeUtf8(allocator: malloc)
        : ''.toNativeUtf8(allocator: malloc);
    try {
      final rc = Rtl433FfiBindings.instance.start(
        devQueryPtr,
        freqHz,
        sampleRate,
        gainStrPtr,
        biasT ? 1 : 0,
        _nativeCallback!.nativeFunction,
        nullptr,
      );
      if (rc != 0) {
        _cleanup();
        throw Exception('rtl433_ffi_start failed with code $rc');
      }
    } finally {
      malloc.free(devQueryPtr);
      malloc.free(gainStrPtr);
    }

    _running = true;
    return _controller!.stream;
  }

  /// Stream of native log messages (for the in-app diagnostics panel).
  Stream<String> get logStream =>
      _logController?.stream ?? const Stream.empty();

  /// Stop decoding and release native resources.
  Future<void> stopDecoding() async {
    if (!_running) return;
    _running = false;

    Rtl433FfiBindings.instance.stop();
    _cleanup();
  }

  /// Current native status: 0=stopped, 1=running, -1=error.
  int get nativeStatus => Rtl433FfiBindings.instance.status();

  void _cleanup() {
    Rtl433FfiBindings.instance.setLogCb(nullptr, nullptr);
    _nativeLogCallback?.close();
    _nativeLogCallback = null;
    _nativeCallback?.close();
    _nativeCallback = null;
    _logController?.close();
    _logController = null;
    _controller?.close();
    _controller = null;
  }
}
