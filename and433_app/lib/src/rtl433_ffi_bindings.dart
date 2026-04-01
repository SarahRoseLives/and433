// Dart FFI bindings for librtl433_ffi.so
//
// Maps directly to the C types and functions declared in rtl433_ffi.h.

import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';

// ---- C type aliases ----

/// C: typedef void (*rtl433_data_cb)(const char *json, void *ctx);
typedef Rtl433DataCbNative = Void Function(Pointer<Utf8> json, Pointer<Void> ctx);
typedef Rtl433DataCb = void Function(Pointer<Utf8> json, Pointer<Void> ctx);

/// C: typedef void (*rtl433_log_cb)(int level, const char *src, const char *msg, void *ctx);
typedef Rtl433LogCbNative = Void Function(Int32 level, Pointer<Utf8> src, Pointer<Utf8> msg, Pointer<Void> ctx);
typedef Rtl433LogCb = void Function(int level, Pointer<Utf8> src, Pointer<Utf8> msg, Pointer<Void> ctx);

// ---- Native function signatures ----

typedef Rtl433StartNative = Int32 Function(
  Pointer<Utf8> devQuery,
  Uint32 freqHz,
  Uint32 sampleRate,
  Pointer<Utf8> gainStr,
  Int32 biasT,
  Pointer<NativeFunction<Rtl433DataCbNative>> cb,
  Pointer<Void> ctx,
);
typedef Rtl433StartDart = int Function(
  Pointer<Utf8> devQuery,
  int freqHz,
  int sampleRate,
  Pointer<Utf8> gainStr,
  int biasT,
  Pointer<NativeFunction<Rtl433DataCbNative>> cb,
  Pointer<Void> ctx,
);

typedef Rtl433SetLogCbNative = Void Function(
  Pointer<NativeFunction<Rtl433LogCbNative>> cb,
  Pointer<Void> ctx,
);
typedef Rtl433SetLogCbDart = void Function(
  Pointer<NativeFunction<Rtl433LogCbNative>> cb,
  Pointer<Void> ctx,
);

typedef Rtl433StopNative = Void Function();
typedef Rtl433StopDart = void Function();

typedef Rtl433StatusNative = Int32 Function();
typedef Rtl433StatusDart = int Function();

// ---- Library loader ----

DynamicLibrary _loadLib() {
  if (Platform.isAndroid) {
    return DynamicLibrary.open('librtl433_ffi.so');
  }
  throw UnsupportedError('rtl433_ffi is only supported on Android');
}

// ---- Bindings class ----

class Rtl433FfiBindings {
  final DynamicLibrary _lib;

  late final Rtl433StartDart start;
  late final Rtl433SetLogCbDart setLogCb;
  late final Rtl433StopDart stop;
  late final Rtl433StatusDart status;

  Rtl433FfiBindings._({DynamicLibrary? lib}) : _lib = lib ?? _loadLib() {
    start = _lib
        .lookupFunction<Rtl433StartNative, Rtl433StartDart>('rtl433_ffi_start');
    setLogCb = _lib
        .lookupFunction<Rtl433SetLogCbNative, Rtl433SetLogCbDart>('rtl433_ffi_set_log_cb');
    stop = _lib
        .lookupFunction<Rtl433StopNative, Rtl433StopDart>('rtl433_ffi_stop');
    status = _lib
        .lookupFunction<Rtl433StatusNative, Rtl433StatusDart>('rtl433_ffi_status');
  }

  static Rtl433FfiBindings? _instance;

  static Rtl433FfiBindings get instance {
    _instance ??= Rtl433FfiBindings._();
    return _instance!;
  }

  /// Visible for testing — allows injecting a mock [DynamicLibrary].
  static Rtl433FfiBindings forLibrary(DynamicLibrary lib) =>
      Rtl433FfiBindings._(lib: lib);
}

