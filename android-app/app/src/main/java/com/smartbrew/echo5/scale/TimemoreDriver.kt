package com.smartbrew.echo5.scale

import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.util.Log
import java.util.UUID

class TimemoreDriver : ScaleDriver {
    override val name: String = "Timemore Black Mirror"
    override val linkType: String = "timemore"

    companion object {
        private const val TAG = "TimemoreDriver"
        val SERVICE_UUID: UUID = UUID.fromString("0000181d-0000-1000-8000-00805f9b34fb")
        val NOTIFY_UUID: UUID  = UUID.fromString("00002a9d-0000-1000-8000-00805f9b34fb")
        val WRITE_UUID: UUID   = UUID.fromString("00002a9e-0000-1000-8000-00805f9b34fb")

        val TARE_SEQ_NANO = byteArrayOf(0x03, 0x0A, 0x01, 0x00, 0x00, 0x08)
        val TARE_SEQ_DOT  = byteArrayOf(0x00)
    }

    private var notifyChar: BluetoothGattCharacteristic? = null
    private var writeChar: BluetoothGattCharacteristic? = null

    override fun matches(serviceUuids: List<UUID>, deviceName: String?): Boolean {
        if (serviceUuids.any { it == SERVICE_UUID }) return true
        val n = deviceName?.lowercase() ?: ""
        return n.contains("timemore") || n.contains("black mirror") || n.contains("nano")
    }

    override fun onConnected(gatt: BluetoothGatt): Boolean {
        val service = gatt.getService(SERVICE_UUID)
        if (service != null) {
            notifyChar = service.getCharacteristic(NOTIFY_UUID)
            writeChar = service.getCharacteristic(WRITE_UUID) ?: notifyChar
            return notifyChar != null
        }
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
        if (data.size < 4) return null
        if (data[0] == 0x03.toByte()) return null

        // Shape A: 0xA1 0xA2 <sign> <BE32 0.01g>
        val b0 = data[0].toInt() and 0xFF
        val b1 = data[1].toInt() and 0xFF
        if (b0 == 0xA1 && b1 == 0xA2 && data.size >= 7) {
            val sign = data[2].toInt() and 0xFF
            val mag = ((data[3].toLong() and 0xFF) shl 24) or
                      ((data[4].toLong() and 0xFF) shl 16) or
                      ((data[5].toLong() and 0xFF) shl 8) or
                      (data[6].toLong() and 0xFF)
            val g = mag.toFloat() / 100.0f
            val grams = if (sign == 0x2D) -g else g
            var battery = -1
            if (data.size >= 8) {
                val b = data[7].toInt() and 0xFF
                if (b in 0..100) battery = b
            }
            return ScaleReading(grams = grams, batteryPct = battery, stable = true)
        }

        // Shape B: <len> <cmd> <sign> <BE24 0.01g>
        if (data.size >= 7 && (data[1].toInt() == 0x2B || data[1].toInt() == 0x2D)) {
            val sign = data[1].toInt() and 0xFF
            val mag = ((data[2].toInt() and 0xFF) shl 16) or
                      ((data[3].toInt() and 0xFF) shl 8) or
                      (data[4].toInt() and 0xFF)
            val g = mag.toFloat() / 100.0f
            val grams = if (sign == 0x2D) -g else g
            return ScaleReading(grams = grams, stable = true)
        }

        // Standard BLE Weight Measurement (0x2A9D): [0] flags, [1] unit, [2..3] 16-bit BE
        val unit = data[1].toInt() and 0x03
        val raw = ((data[2].toInt() and 0xFF) shl 8) or (data[3].toInt() and 0xFF)
        val kg = when (unit) {
            0x00 -> raw * 0.005f
            0x01 -> raw * 0.1f * 0.45359237f
            0x02 -> raw * 0.005f
            0x03 -> raw.toFloat()
            else -> raw * 0.005f
        }
        return ScaleReading(grams = kg * 1000.0f, stable = true)
    }

    fun sendTareWakeup(gatt: BluetoothGatt): Boolean {
        val targetChar = writeChar ?: notifyChar ?: return false
        return try {
            targetChar.value = TARE_SEQ_DOT
            targetChar.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            gatt.writeCharacteristic(targetChar)
        } catch (e: Exception) {
            Log.e(TAG, "Timemore tare wake-up failed", e)
            false
        }
    }

    override fun sendTare(gatt: BluetoothGatt): Boolean {
        val targetChar = writeChar ?: notifyChar ?: return false
        return try {
            targetChar.value = TARE_SEQ_NANO
            targetChar.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            val ok = gatt.writeCharacteristic(targetChar)
            Log.i(TAG, "Timemore tare sent: $ok")
            ok
        } catch (e: Exception) {
            Log.e(TAG, "Timemore tare failed", e)
            false
        }
    }

    override fun getNotifyUuid(): UUID = NOTIFY_UUID
    override fun getWriteUuid(): UUID = WRITE_UUID
}
