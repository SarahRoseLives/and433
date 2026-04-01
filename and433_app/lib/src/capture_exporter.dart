import 'dart:convert';
import 'dart:io';

import 'package:path_provider/path_provider.dart';
import 'package:share_plus/share_plus.dart';

/// Exports a packet list in .a433 (JSONL) or .kml format and shares via
/// the system share sheet.
class CaptureExporter {
  CaptureExporter._();

  // ── .a433 JSONL ──────────────────────────────────────────────────────────

  /// Build the .a433 file content.
  /// Line 1: session header.
  /// Lines 2+: one JSON object per packet (already has _lat/_lon etc injected).
  static String buildA433(List<Map<String, dynamic>> packets, int freqHz) {
    final header = jsonEncode({
      '_a433': '1.0',
      '_created': DateTime.now().toUtc().toIso8601String(),
      '_freq_hz': freqHz,
    });
    final lines = [
      header,
      ...packets.map(jsonEncode),
    ];
    return lines.join('\n');
  }

  // ── KML ──────────────────────────────────────────────────────────────────

  static String buildKml(List<Map<String, dynamic>> packets) {
    final buf = StringBuffer();
    buf.writeln('<?xml version="1.0" encoding="UTF-8"?>');
    buf.writeln('<kml xmlns="http://www.opengis.net/kml/2.2">');
    buf.writeln('  <Document>');
    buf.writeln('    <name>And433 Capture</name>');

    for (final p in packets) {
      final lat = p['_lat'];
      final lon = p['_lon'];
      if (lat == null || lon == null) continue; // skip untagged

      final model = _esc(p['model'] as String? ?? 'Unknown');
      final time = _esc(p['time'] as String? ?? '');
      final desc = _buildDescription(p);

      buf.writeln('    <Placemark>');
      buf.writeln('      <name>$model — $time</name>');
      buf.writeln('      <description><![CDATA[$desc]]></description>');
      buf.writeln('      <Point>');
      buf.writeln('        <coordinates>$lon,$lat,0</coordinates>');
      buf.writeln('      </Point>');
      buf.writeln('    </Placemark>');
    }

    buf.writeln('  </Document>');
    buf.writeln('</kml>');
    return buf.toString();
  }

  static String _buildDescription(Map<String, dynamic> p) {
    final buf = StringBuffer('<table>');
    for (final e in p.entries) {
      if (e.key.startsWith('_')) continue; // skip internal GPS fields
      buf.write('<tr><td><b>${_esc(e.key)}</b></td>'
          '<td>${_esc(e.value.toString())}</td></tr>');
    }
    // GPS fields at the bottom
    for (final key in ['_lat', '_lon', '_gps_accuracy', '_gps_time']) {
      if (p.containsKey(key)) {
        buf.write('<tr><td><b>${_esc(key)}</b></td>'
            '<td>${_esc(p[key].toString())}</td></tr>');
      }
    }
    buf.write('</table>');
    return buf.toString();
  }

  static String _esc(String s) => s
      .replaceAll('&', '&amp;')
      .replaceAll('<', '&lt;')
      .replaceAll('>', '&gt;')
      .replaceAll('"', '&quot;');

  // ── Share helpers ─────────────────────────────────────────────────────────

  static Future<void> exportA433(
      List<Map<String, dynamic>> packets, int freqHz) async {
    final content = buildA433(packets, freqHz);
    await _shareText(content, 'capture.a433', 'application/json');
  }

  static Future<void> exportKml(List<Map<String, dynamic>> packets) async {
    final content = buildKml(packets);
    final geoTagged = packets.where((p) => p['_lat'] != null).length;
    if (geoTagged == 0) {
      throw Exception('No GPS-tagged packets to export as KML.');
    }
    await _shareText(content, 'capture.kml', 'application/vnd.google-earth.kml+xml');
  }

  static Future<void> _shareText(
      String content, String fileName, String mimeType) async {
    final dir = await getTemporaryDirectory();
    final file = File('${dir.path}/$fileName');
    await file.writeAsString(content, encoding: utf8);
    await SharePlus.instance.share(
      ShareParams(
        files: [XFile(file.path, mimeType: mimeType)],
        subject: fileName,
      ),
    );
  }
}
