import 'dart:convert';

import 'package:http/http.dart' as http;
import 'package:shared_preferences/shared_preferences.dart';

/// Client for the 433map.com API.
///
/// Endpoints used:
///   POST /api/auth/login  { "username": "...", "password": "..." }
///                         → { "token": "..." }
///   POST /api/ingest       Authorization: Bearer `token`
///                          Content-Type: application/json
///                          body: .a433 JSONL string
class Api433MapService {
  Api433MapService._();
  static final instance = Api433MapService._();

  static const _base = 'https://433map.com';
  static const _tokenKey = '433map_token';
  static const _usernameKey = '433map_username';

  // ── Auth ───────────────────────────────────────────────────────────────

  Future<String?> savedToken() async {
    final prefs = await SharedPreferences.getInstance();
    return prefs.getString(_tokenKey);
  }

  Future<String?> savedUsername() async {
    final prefs = await SharedPreferences.getInstance();
    return prefs.getString(_usernameKey);
  }

  /// Log in with [username] and [password]. Stores token on success.
  /// Returns null on success, or an error message string on failure.
  Future<String?> login(String username, String password) async {
    try {
      final resp = await http
          .post(
            Uri.parse('$_base/api/auth/login'),
            headers: {'Content-Type': 'application/json'},
            body: jsonEncode({'username': username, 'password': password}),
          )
          .timeout(const Duration(seconds: 15));

      if (resp.statusCode == 200) {
        final body = jsonDecode(resp.body);
        final token = body['token'] as String?;
        if (token == null || token.isEmpty) {
          return 'Server returned no token';
        }
        final prefs = await SharedPreferences.getInstance();
        await prefs.setString(_tokenKey, token);
        await prefs.setString(_usernameKey, username);
        return null; // success
      }

      // Try to surface server error message
      try {
        final body = jsonDecode(resp.body);
        return body['message'] as String? ??
            body['error'] as String? ??
            'Login failed (${resp.statusCode})';
      } catch (_) {
        return 'Login failed (${resp.statusCode})';
      }
    } catch (e) {
      return 'Network error: $e';
    }
  }

  Future<void> logout() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.remove(_tokenKey);
    await prefs.remove(_usernameKey);
  }

  // ── Ingest ─────────────────────────────────────────────────────────────

  /// Upload [a433Content] to /api/ingest.
  /// Returns null on success, or an error message on failure.
  Future<String?> ingest(String a433Content) async {
    final token = await savedToken();
    if (token == null) return 'Not logged in';

    try {
      final resp = await http
          .post(
            Uri.parse('$_base/api/ingest'),
            headers: {
              'Content-Type': 'application/json',
              'Authorization': 'Bearer $token',
            },
            body: a433Content,
          )
          .timeout(const Duration(seconds: 30));

      if (resp.statusCode == 200 || resp.statusCode == 201) {
        return null; // success
      }

      // Token expired → clear it so next attempt triggers re-login
      if (resp.statusCode == 401 || resp.statusCode == 403) {
        await logout();
        return 'Session expired — please log in again';
      }

      try {
        final body = jsonDecode(resp.body);
        return body['message'] as String? ??
            body['error'] as String? ??
            'Upload failed (${resp.statusCode})';
      } catch (_) {
        return 'Upload failed (${resp.statusCode})';
      }
    } catch (e) {
      return 'Network error: $e';
    }
  }
}
