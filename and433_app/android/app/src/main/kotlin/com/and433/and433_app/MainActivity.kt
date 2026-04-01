package com.and433.and433_app

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

class MainActivity : FlutterActivity() {

    private val channel = "com.and433.and433_app/sdr_usb"

    companion object {
        private const val USB_PERMISSION_ACTION = "com.and433.and433_app.USB_PERMISSION"
        // Realtek RTL2832U USB vendor ID (covers all common RTL-SDR dongles)
        private const val RTL2832_VENDOR_ID = 0x0BDA
    }

    private var pendingResult: MethodChannel.Result? = null
    private var usbConnection: UsbDeviceConnection? = null
    private var pendingDevice: UsbDevice? = null

    private val usbPermissionReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action != USB_PERMISSION_ACTION) return
            val device = intent.getParcelableExtra<UsbDevice>(UsbManager.EXTRA_DEVICE)
            val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
            if (!granted || device == null) {
                pendingResult?.error("PERMISSION_DENIED", "USB permission denied by user", null)
                pendingResult = null
                return
            }
            openAndReturn(device)
        }
    }

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)

        registerReceiver(
            usbPermissionReceiver,
            IntentFilter(USB_PERMISSION_ACTION),
            RECEIVER_NOT_EXPORTED,
        )

        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, channel)
            .setMethodCallHandler { call, result ->
                when (call.method) {
                    "openDevice"  -> handleOpen(result)
                    "closeDevice" -> handleClose(result)
                    else          -> result.notImplemented()
                }
            }
    }

    private fun handleOpen(result: MethodChannel.Result) {
        val usbManager = getSystemService(USB_SERVICE) as UsbManager
        val device = usbManager.deviceList.values.firstOrNull {
            it.vendorId == RTL2832_VENDOR_ID
        }
        if (device == null) {
            result.error("NO_DEVICE", "No RTL-SDR (RTL2832U) device found — plug in dongle via OTG", null)
            return
        }
        pendingResult = result
        pendingDevice = device
        if (usbManager.hasPermission(device)) {
            openAndReturn(device)
        } else {
            val flags = PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
            val permIntent = PendingIntent.getBroadcast(
                this, 0, Intent(USB_PERMISSION_ACTION), flags
            )
            usbManager.requestPermission(device, permIntent)
        }
    }

    private fun openAndReturn(device: UsbDevice) {
        val usbManager = getSystemService(USB_SERVICE) as UsbManager
        val conn = usbManager.openDevice(device)
        if (conn == null) {
            pendingResult?.error("OPEN_FAILED", "Failed to open USB device connection", null)
            pendingResult = null
            return
        }
        usbConnection?.close()
        usbConnection = conn
        pendingResult?.success(
            mapOf(
                "fd"         to conn.fileDescriptor,
                "devicePath" to device.deviceName,
            )
        )
        pendingResult = null
    }

    private fun handleClose(result: MethodChannel.Result) {
        usbConnection?.close()
        usbConnection = null
        result.success(null)
    }

    override fun onDestroy() {
        try { unregisterReceiver(usbPermissionReceiver) } catch (_: Exception) { }
        usbConnection?.close()
        usbConnection = null
        super.onDestroy()
    }
}

