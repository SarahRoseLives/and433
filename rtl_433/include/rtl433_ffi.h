/** @file
    Public C FFI interface for rtl_433, intended for use with Flutter dart:ffi on Android.

    Connects to an rtl_tcp server (e.g. rtl_tcp_andro) and streams decoded RF packet data
    as newline-terminated JSON strings via a callback.

    Copyright (C) 2026 and433 contributors

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#ifndef INCLUDE_RTL433_FFI_H_
#define INCLUDE_RTL433_FFI_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback invoked on the decode thread for each decoded RF packet.
 *
 * @param json      Null-terminated JSON string for the decoded packet. Valid only for
 *                  the duration of the callback — copy if you need to retain it.
 * @param ctx       User context pointer passed to rtl433_ffi_start().
 */
typedef void (*rtl433_data_cb)(const char *json, void *ctx);

/**
 * Callback invoked for each rtl_433 log message.
 *
 * @param level     Severity: 1=FATAL 2=CRITICAL 3=ERROR 4=WARNING 5=NOTICE 6=INFO 7=DEBUG
 * @param src       Null-terminated source/module name.
 * @param msg       Null-terminated log message.
 * @param ctx       User context pointer passed to rtl433_ffi_set_log_cb().
 */
typedef void (*rtl433_log_cb)(int level, const char *src, const char *msg, void *ctx);

/**
 * Set a callback to receive rtl_433 log messages.
 * Call before rtl433_ffi_start().  Pass NULL to remove.
 */
void rtl433_ffi_set_log_cb(rtl433_log_cb cb, void *ctx);

/**
 * Start decoding RF signals from an RTL-SDR device.
 *
 * This function is non-blocking: it starts background threads and returns immediately.
 * The supplied callback will be invoked from a background thread for each decoded packet.
 *
 * Only one concurrent instance is supported.  Call rtl433_ffi_stop() before calling
 * rtl433_ffi_start() again.
 *
 * @param dev_query     Device selector string.  Three forms are supported:
 *                        "fd:N:path"               – Android native USB (pre-opened fd N,
 *                                                    USB device node path e.g.
 *                                                    /dev/bus/usb/001/002)
 *                        "rtl_tcp://host:port"      – network rtl_tcp server
 *                        "0"  / ":serial"           – RTL-SDR device index or serial
 * @param freq_hz       Center frequency in Hz (e.g. 433920000 for 433.92 MHz).
 * @param sample_rate   Sample rate in Hz (e.g. 250000).
 * @param gain_str      Tuner gain string.  NULL or "" enables hardware AGC.
 *                      Otherwise a decimal dB value, e.g. "40" = 40 dB, "49.6" = max.
 * @param bias_t        Set non-zero to enable the bias-T 5V supply on the antenna port.
 *                      Requires a compatible dongle (e.g. RTL-SDR Blog V3/V4).
 * @param cb            Callback to invoke for each decoded JSON packet.  Must not be NULL.
 * @param ctx           Opaque pointer forwarded to every callback invocation.
 * @return              0 on success, negative errno on failure.
 */
int rtl433_ffi_start(const char *dev_query,
                     uint32_t freq_hz, uint32_t sample_rate,
                     const char *gain_str, int bias_t,
                     rtl433_data_cb cb, void *ctx);

/**
 * Stop decoding and release all resources.
 *
 * Blocks until the background threads have exited.  Safe to call even if
 * rtl433_ffi_start() was never called or already returned an error.
 */
void rtl433_ffi_stop(void);

/**
 * Query the current decoder state.
 *
 * @return  0 = stopped, 1 = starting/running, -1 = error (use rtl433_ffi_stop() to reset).
 */
int rtl433_ffi_status(void);

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_RTL433_FFI_H_ */
