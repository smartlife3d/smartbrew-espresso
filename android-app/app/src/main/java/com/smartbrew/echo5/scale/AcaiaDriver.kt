package com.smartbrew.echo5.scale

import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.util.Log
import java.util.UUID

class AcaiaDriver : ScaleDriver {
    override val name: String = "Acaia Lunar/Pearl"
    override val linkType: String = "acaia"

    companion object {
        private const val TAG = "AcaiaDriver"
        val SERVICE_UUID: UUID = UUID.fromString("00001820-0000-1000-8000-00805f9b34fb")
        val VENDOR_SERVICE_UUID: UUID = UUID.fromString("49535343-fe7d-4ae5-8fa9-9fafd205e455")
        val NOTIFY_UUID: UUID  = UUID.fromString("00002a80-0000-1000-8000-00805f9b34fb")

        val MSG_WEIGHT: Byte  = 0x05
        val MSG_BATTERY: Byte = 0x06

        val HANDSHAKE = byteArrayOf(0xEF.toByte(), 0xDD.toByte(), 0x00, 0x00, 0x00, 0x00, 0x00, 0x01)
        val HEARTBEAT = byteArrayOf(0xEF.toByte(), 0xDD.toByte(), 0x01)
        val TARE_CMD  = byteArrayOf(0xEF.toByte(), 0xDD.toByte(), 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00)
    }

    private var notifyChar: BluetoothGattCharacteristic? = null
    private var writeChar: BluetoothGattCharacteristic? = null
    private var lastGrams: Float = 0.0f
    private var lastHeartbeatTime: Long = 0
    private var handshakeSent: Boolean = false

    override fun matches(serviceUuids: List<UUID>, deviceName: String?): Boolean {
        if (serviceUuids.any { it == SERVICE_UUID || it == VENDOR_SERVICE_UUID }) return true
        val n = deviceName?.lowercase() ?: ""
        return n.contains("acaia") || n.contains("lunar") || n.contains("pearl") || n.contains("pyxis")
    }

    override fun onConnected(gatt: BluetoothGatt): Boolean {
        var service = gatt.getService(SERVICE_UUID) ?: gatt.getService(VENDOR_SERVICE_UUID)
        if (service != null) {
            notifyChar = service.getCharacteristic(NOTIFY_UUID)
            writeChar = notifyChar
        }
        if (notifyChar == null) {
            for (s in gatt.services) {
                val nc = s.getCharacteristic(NOTIFY_UUID)
                if (nc != null) {
                    notifyChar = nc
                    writeChar = nc
                    break
                }
            }
        }
        if (notifyChar != null && writeChar != null) {
            handshakeSent = false
            lastHeartbeatTime = 0
            return true
        }
        return false
    }

    override fun onCharacteristicChanged(
        gatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic,
        data: ByteArray
    ): ScaleReading? {
        if (data.size < 3) return null
        if (data[0] != 0xEF.toByte() || data[1] != 0xDD.toByte()) return null

        val msgType = data[2]
        if (msgType == MSG_WEIGHT && data.size >= 10) {
            // Little-endian 32-bit unsigned mg
            val mg = (data[3].toLong() and 0xFF) or
                     ((data[4].toLong() and 0xFF) shl 8) or
                     ((data[5].toLong() and 0xFF) shl 16) or
                     ((data[6].toLong() and 0xFF) shl 24)
            val grams = mg.toFloat() / 1000.0f
            lastGrams = grams
            val stable = (data[7].toInt() and 0x01) != 0
            var battery = -1
            if (data.size >= 9) {
                val b = data[8].toInt() and 0xFF
                if (b in 0..100) battery = b
            }
            return ScaleReading(grams = grams, batteryPct = battery, stable = stable)
        }

        if (msgType == MSG_BATTERY && data.size >= 4) {
            val b = data[3].toInt() and 0xFF
            if (b in 0..100) {
                return ScaleReading(grams = lastGrams, batteryPct = b, stable = true)
            }
        }

        return null
    }

    override fun service(gatt: BluetoothGatt): Boolean {
        val now = System.currentTimeMillis()
        val wc = writeChar ?: notifyChar ?: return false
        if (!handshakeSent) {
            lastHeartbeatTime = now
            return try {
                wc.value = HANDSHAKE
                wc.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                val ok = gatt.writeCharacteristic(wc)
                if (ok) {
                    handshakeSent = true
                }
                ok
            } catch (e: Exception) {
                false
            }
        }
        if (now - lastHeartbeatTime >= 2500) {
            lastHeartbeatTime = now
            return try {
                wc.value = HEARTBEAT
                wc.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                gatt.writeCharacteristic(wc)
            } catch (e: Exception) {
                false
            }
        }
        return false
    }

    override fun sendTare(gatt: BluetoothGatt): Boolean {
        val wc = writeChar ?: notifyChar ?: return false
        return try {
            wc.value = TARE_CMD
            wc.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            val ok = gatt.writeCharacteristic(wc)
            Log.i(TAG, "Acaia tare sent: $ok")
            ok
        } catch (e: Exception) {
            Log.e(TAG, "Acaia tare failed", e)
            false
        }
    }

    override fun getNotifyUuid(): UUID = NOTIFY_UUID
    override fun getWriteUuid(): UUID = NOTIFY_UUID
}
