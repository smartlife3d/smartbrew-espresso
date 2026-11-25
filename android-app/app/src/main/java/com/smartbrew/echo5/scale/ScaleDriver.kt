package com.smartbrew.echo5.scale

import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import java.util.UUID

interface ScaleDriver {
    val name: String
    val linkType: String

    /** Check if advertising services or device name match this scale family */
    fun matches(serviceUuids: List<UUID>, deviceName: String?): Boolean

    /** Characteristic discovery and subscription */
    fun onConnected(gatt: BluetoothGatt): Boolean

    /** Process incoming BLE notification frame */
    fun onCharacteristicChanged(
        gatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic,
        data: ByteArray
    ): ScaleReading?

    /** Send tare command to hardware scale */
    fun sendTare(gatt: BluetoothGatt): Boolean

    /** Periodic service call (e.g. Acaia 3s heartbeat). Returns true if a GATT op was started. */
    fun service(gatt: BluetoothGatt): Boolean = false

    fun getNotifyUuid(): UUID?
    fun getWriteUuid(): UUID?
}
