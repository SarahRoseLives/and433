import 'dart:async';

import 'package:geolocator/geolocator.dart';

/// Maintains a continuously-updated last known GPS position.
/// Call [start] when decoding begins, [stop] when done.
/// Use [inject] to add GPS fields to a decoded packet map.
class GpsService {
  GpsService._();
  static final instance = GpsService._();

  Position? _lastPosition;
  StreamSubscription<Position>? _sub;

  Future<bool> start() async {
    // Check / request permission
    var perm = await Geolocator.checkPermission();
    if (perm == LocationPermission.denied) {
      perm = await Geolocator.requestPermission();
    }
    if (perm == LocationPermission.denied ||
        perm == LocationPermission.deniedForever) {
      return false;
    }

    // Get an immediate fix, then keep updating
    try {
      _lastPosition = await Geolocator.getCurrentPosition(
        locationSettings: const LocationSettings(
          accuracy: LocationAccuracy.high,
          timeLimit: Duration(seconds: 10),
        ),
      );
    } catch (_) {
      // Non-fatal — we'll get position from the stream
    }

    _sub = Geolocator.getPositionStream(
      locationSettings: const LocationSettings(
        accuracy: LocationAccuracy.high,
        distanceFilter: 0,
      ),
    ).listen((pos) => _lastPosition = pos);

    return true;
  }

  void stop() {
    _sub?.cancel();
    _sub = null;
    _lastPosition = null;
  }

  /// Injects _lat/_lon/_gps_accuracy/_gps_time into [packet] in-place.
  /// Returns the same map (with or without GPS fields).
  Map<String, dynamic> inject(Map<String, dynamic> packet) {
    final pos = _lastPosition;
    if (pos != null) {
      packet['_lat'] = pos.latitude;
      packet['_lon'] = pos.longitude;
      packet['_gps_accuracy'] = pos.accuracy;
      packet['_gps_time'] = pos.timestamp.toUtc().toIso8601String();
    }
    return packet;
  }

  bool get hasPosition => _lastPosition != null;
}
