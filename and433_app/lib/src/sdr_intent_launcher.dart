// Requests access to an RTL-SDR USB dongle via Android's UsbManager API.
//
// Uses a MethodChannel to the Android side, which:
//   1. Locates a Realtek RTL2832U device in the USB device list
//   2. Requests USB permission (shows system dialog if needed)
//   3. Opens a UsbDeviceConnection and returns the file descriptor + device path
//
// The returned "fd:N:path" string is passed directly to rtl433_ffi_start().

import 'dart:async';
import 'package:flutter/services.dart';

class UsbHelper {
  UsbHelper._();
  static final UsbHelper instance = UsbHelper._();

  static const _channel = MethodChannel('com.and433.and433_app/sdr_usb');

  /// Open the RTL-SDR USB device and return a dev_query string of the form
  /// `"fd:N:/dev/bus/usb/XXX/YYY"` suitable for passing to rtl433_ffi_start().
  ///
  /// Throws a [PlatformException] if no device is found, permission is denied,
  /// or the device cannot be opened.
  Future<String> openDevice() async {
    final result = await _channel.invokeMapMethod<String, dynamic>('openDevice');
    if (result == null) throw PlatformException(code: 'NULL', message: 'No result');
    final fd = result['fd'] as int;
    final path = result['devicePath'] as String;
    return 'fd:$fd:$path';
  }

  /// Release the USB connection (close the UsbDeviceConnection on the Kotlin side).
  Future<void> closeDevice() async {
    try {
      await _channel.invokeMethod<void>('closeDevice');
    } on PlatformException {
      // ignore — already closed
    }
  }
}
