package com.smartbrew.echo5.scale

import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.util.Log
import java.util.UUID

class BookooDriver : ScaleDriver {
    override val name: String = "Bookoo Themis"
    override val linkType: String = "bookoo"

    companion object {
        private const val TAG = "BookooDriver"
        val SERVICE_UUID: UUID = UUID.fromString("00000ffe-0000-1000-8000-00805f9b34fb")
        val NOTIFY_UUID: UUID  = UUID.fromString("0000ff11-0000-1000-8000-00805f9b34fb")
        val WRITE_UUID: UUID   = UUID.fromString("0000ff12-0000-1000-8000-00805f9b34fb")

        val TARE_CMD = byteArrayOf(0x03, 0x0A, 0x01, 0x00, 0x00, 0x08)
    }

    private var notifyChar: BluetoothGattCharacteristic? = null
    private var writeChar: BluetoothGattCharacteristic? = null

    override fun matches(serviceUuids: List<UUID>, deviceName: String?): Boolean {
        if (serviceUuids.any { it == SERVICE_UUID }) return true
        val n = deviceName?.lowercase() ?: ""
        return n.contains("bookoo") || n.contains("themis")
    }

    override fun onConnected(gatt: BluetoothGatt): Boolean {
        val service = gatt.getService(SERVICE_UUID)
        if (service != null) {
            notifyChar = service.getCharacteristic(NOTIFY_UUID)
            writeChar = service.getCharacteristic(WRITE_UUID) ?: notifyChar
            return notifyChar != null
        }
        // Fallback: search all services
        for (s in gatt.services) {
            val nc = s.getCharacteristic(NOTIFY_UUID)
            if (nc != null) {
                notifyChar = nc
                writeChar = s.getCharacteristic(WRITE_UUID) ?: nc
                return true
            }
        }
        return false
    }

    override fun onCharacteristicChanged(
        gatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic,
        data: ByteArray
    ): ScaleReading? {
        if (data.size < 10) return null
        if (data[0] == 0x03.toByte()) return null // Ack frame

        val sign = data[6].toInt() and 0xFF
        if (sign != 0x2B && sign != 0x2D) return null

        val b7 = (data[7].toInt() and 0xFF) shl 16
        val b8 = (data[8].toInt() and 0xFF) shl 8
        val b9 = (data[9].toInt() and 0xFF)
        val mag = b7 or b8 or b9
        val grams = if (sign == 0x2D) -(mag / 100.0f) else (mag / 100.0f)

        var battery = -1
        if (data.size >= 20) {
            val b = data[2].toInt() and 0xFF
            if (b in 0..100) battery = b
        }

        return ScaleReading(
            grams = grams,
            batteryPct = battery,
            stable = true
        )
    }

    override fun sendTare(gatt: BluetoothGatt): Boolean {
        val targetChar = writeChar ?: notifyChar ?: return false
        return try {
            targetChar.value = TARE_CMD
            targetChar.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            val ok = gatt.writeCharacteristic(targetChar)
            Log.i(TAG, "Bookoo tare command sent: $ok")
            ok
        } catch (e: Exception) {
            Log.e(TAG, "Bookoo tare error", e)
            false
        }
    }

    override fun getNotifyUuid(): UUID = NOTIFY_UUID
    override fun getWriteUuid(): UUID = WRITE_UUID
}
