package com.smartbrew.echo5.scale

import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.util.Log
import java.util.UUID

class GenericDriver : ScaleDriver {
    override val name: String = "Decent/Felicita/Cocinare"
    override val linkType: String = "generic"

    companion object {
        private const val TAG = "GenericDriver"
        val SERVICE_FFF0: UUID = UUID.fromString("0000fff0-0000-1000-8000-00805f9b34fb")
        val NOTIFY_FFF4: UUID  = UUID.fromString("0000fff4-0000-1000-8000-00805f9b34fb")
        val SERVICE_FFE0: UUID = UUID.fromString("0000ffe0-0000-1000-8000-00805f9b34fb")
        val NOTIFY_FFE1: UUID  = UUID.fromString("0000ffe1-0000-1000-8000-00805f9b34fb")

        val TARE_DECENT  = byteArrayOf(0x03, 0x0F, 0x01, 0x00, 0x00)
        val TARE_BOOKOO  = byteArrayOf(0x03, 0x0A, 0x01, 0x00, 0x00, 0x08)
        val TARE_ASCII_T = byteArrayOf('T'.code.toByte())
    }

    private var notifyChar: BluetoothGattCharacteristic? = null
    private var writeChar: BluetoothGattCharacteristic? = null
    private var tareTry = 0

    override fun matches(serviceUuids: List<UUID>, deviceName: String?): Boolean {
        if (serviceUuids.any { it == SERVICE_FFF0 || it == SERVICE_FFE0 }) return true
        val n = deviceName?.lowercase() ?: ""
        return n.contains("decent") || n.contains("felicita") || n.contains("cocinare") || n.contains("arc")
    }

    override fun onConnected(gatt: BluetoothGatt): Boolean {
        val sFFF0 = gatt.getService(SERVICE_FFF0)
        if (sFFF0 != null) {
            notifyChar = sFFF0.getCharacteristic(NOTIFY_FFF4)
            writeChar = notifyChar
        }
        if (notifyChar == null) {
            val sFFE0 = gatt.getService(SERVICE_FFE0)
            if (sFFE0 != null) {
                notifyChar = sFFE0.getCharacteristic(NOTIFY_FFE1)
                writeChar = notifyChar
            }
        }
        if (notifyChar == null) {
            for (s in gatt.services) {
                val c4 = s.getCharacteristic(NOTIFY_FFF4) ?: s.getCharacteristic(NOTIFY_FFE1)
                if (c4 != null) {
                    notifyChar = c4
                    writeChar = c4
                    break
                }
            }
        }
        return notifyChar != null
    }

    private fun xorCheckValid(data: ByteArray): Boolean {
        if (data.size < 4) return false
        var x = 0.toByte()
        for (i in 0 until data.size - 1) {
            x = (x.toInt() xor data[i].toInt()).toByte()
        }
        return x == data[data.size - 1]
    }

    override fun onCharacteristicChanged(
        gatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic,
        data: ByteArray
    ): ScaleReading? {
        if (data.size < 5) return null
        if (!xorCheckValid(data)) return null

        val b0 = data[0].toInt() and 0xFF
        val b1 = data[1].toInt() and 0xFF

        // Shape A: 0xAA or 0xBB
        if ((b0 == 0xAA || b0 == 0xBB) && data.size >= 7) {
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

        // Shape B: 0xA5, 0x01
        if (b0 == 0xA5 && b1 == 0x01 && data.size >= 8) {
            val sign = data[2].toInt() and 0xFF
            val mag = ((data[3].toLong() and 0xFF) shl 24) or
                      ((data[4].toLong() and 0xFF) shl 16) or
                      ((data[5].toLong() and 0xFF) shl 8) or
                      (data[6].toLong() and 0xFF)
            val g = mag.toFloat() / 100.0f
            val grams = if (sign == 0x2D) -g else g
            var battery = -1
            if (data.size >= 9) {
                val b = data[8].toInt() and 0xFF
                if (b in 0..100) battery = b
            }
            return ScaleReading(grams = grams, batteryPct = battery, stable = true)
        }

        return null
    }

    override fun sendTare(gatt: BluetoothGatt): Boolean {
        val wc = writeChar ?: notifyChar ?: return false
        val cmd = when (tareTry % 3) {
            0 -> TARE_DECENT
            1 -> TARE_BOOKOO
            else -> TARE_ASCII_T
        }
        tareTry++
        return try {
            wc.value = cmd
            wc.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            val ok = gatt.writeCharacteristic(wc)
            Log.i(TAG, "Generic tare sent (variant $tareTry): $ok")
            ok
        } catch (e: Exception) {
            Log.e(TAG, "Generic tare failed", e)
            false
        }
    }

    override fun getNotifyUuid(): UUID? = notifyChar?.uuid
    override fun getWriteUuid(): UUID? = writeChar?.uuid
}
