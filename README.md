# And433

An Android app for decoding 433 MHz RF signals natively using an RTL-SDR dongle connected via USB OTG.

And433 uses [rtl_433](https://github.com/merbanan/rtl_433) compiled directly into the app via Android NDK and FFI — no external server or network connection required to decode signals.

---

## Features

- **Native RTL-SDR support** — connects directly to an RTL-SDR dongle over USB OTG using a patched librtlsdr ([rtl_tcp_andro](https://github.com/martinmarinov/rtl_tcp_andro))
- **Full rtl_433 decoder** — all 200+ device protocols supported out of the box
- **GPS tagging** — each decoded packet is tagged with the device's GPS coordinates at the time of capture
- **Search & filter** — live search across all packet fields; filter by device model using chips
- **Adjustable gain** — slider from 0 dB (hardware AGC) to 49.6 dB; defaults to 40 dB
- **Bias-T support** — optional 5V supply on the antenna port (RTL-SDR Blog V3/V4)
- **Export captures** in two formats:
  - `.a433` — JSONL format with session header and GPS-tagged packets (see below)
  - `.kml` — Google Earth / Maps compatible, one placemark per GPS-tagged packet
- **Share data with [433map.com](#433mapcom)** — automatic upload of new packets while decoding

---

## Requirements

- Android 6.0+ (API 23), arm64
- RTL-SDR dongle (RTL2832U-based)
- USB OTG cable or adapter

---

## Building

```bash
# Clone with submodule dependencies
git clone https://github.com/SarahRoseLives/and433.git
cd and433

# Build the Flutter app (NDK 27 required)
cd and433_app
flutter run
```

The NDK build compiles libusb, librtlsdr, and rtl_433 into a single `librtl433_ffi.so` via the CMakeLists in `rtl_433/android/`.

---

## Capture Format (.a433)

And433 uses a simple JSONL (JSON Lines) format with the `.a433` extension.

**Line 1** — session header:
```json
{"_a433":"1.0","_created":"2026-04-01T00:00:00.000Z","_freq_hz":433920000}
```

**Lines 2+** — one decoded packet per line, all rtl_433 fields plus injected GPS fields:
```json
{"time":"2026-04-01 00:01:23","model":"Acurite-Tower","id":12345,"temperature_C":21.4,"humidity":58,"_lat":51.5074,"_lon":-0.1278,"_gps_accuracy":4.2,"_gps_time":"2026-04-01T00:01:23.000Z"}
```

GPS fields (`_lat`, `_lon`, `_gps_accuracy`, `_gps_time`) are present when the device had a GPS fix at the time of capture. Packets without a fix omit these fields.

---

## 433map.com

And433 can automatically share your captures with **[433map.com](https://433map.com)**, a community map that plots GPS-tagged RF sensor data contributed by users worldwide.

### How it works

1. Log in to your 433map.com account via **Export → Upload to 433map.com** in the app
2. Once authenticated, new packets are uploaded automatically every 30 seconds while decoding
3. A cloud icon in the toolbar shows the current upload state
4. Your token is stored securely on-device — you only need to log in once

### API

The app communicates with two endpoints:

| Method | Endpoint | Description |
|--------|----------|-------------|
| `POST` | `/api/auth/login` | Authenticate with `{"username":"…","password":"…"}`, returns `{"token":"…"}` |
| `POST` | `/api/ingest` | Upload `.a433` JSONL body with `Authorization: Bearer <token>` header |

---

## Architecture

```
Flutter UI (Dart)
    │
    │  dart:ffi
    ▼
librtl433_ffi.so  (Android NDK / C)
    ├── rtl_433         — decodes IQ samples → JSON packets
    ├── librtlsdr       — patched for Android fd-based open (rtlsdr_open2)
    └── libusb          — patched for Android (no enumeration, fd-only)
    │
    │  UsbManager (Java/Kotlin)
    ▼
RTL-SDR dongle  (USB OTG)
```

---

## License

GNU General Public License v2.0 — the same license as [rtl_433](https://github.com/merbanan/rtl_433).  
See [COPYING](rtl_433/COPYING) for the full license text.
