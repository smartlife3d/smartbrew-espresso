package com.smartbrew.echo5.scale

import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.util.*

data class DiscoveredScale(
    val name: String,
    val address: String,
    val rssi: Int,
    val linkType: String
)

interface ScaleEventListener {
    fun onReading(reading: ScaleReading)
    fun onStateChanged(state: String, deviceName: String?, mac: String?)
    fun onScaleDiscovered(scale: DiscoveredScale)
}

@SuppressLint("MissingPermission")
class BleScaleManager(private val context: Context) {

    companion object {
        private const val TAG = "BleScaleManager"
        val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    }

    private val bluetoothManager = context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
    private val bluetoothAdapter: BluetoothAdapter? get() = bluetoothManager?.adapter
    private val bleScanner: BluetoothLeScanner? get() = bluetoothAdapter?.bluetoothLeScanner

    private val drivers: List<ScaleDriver> = listOf(
        BookooDriver(),
        TimemoreDriver(),
        AcaiaDriver(),
        GenericDriver()
    )

    private var activeDriver: ScaleDriver? = null
    private var bluetoothGatt: BluetoothGatt? = null
    private var isScanning = false
    private val mainHandler = Handler(Looper.getMainLooper())

    var listener: ScaleEventListener? = null
    var currentState: String = "IDLE"
        private set
    var connectedDeviceName: String? = null
        private set
    var connectedDeviceMac: String? = null
        private set

    // Flow rate calculation
    private var lastGrams: Float = 0.0f
    private var lastTimeMs: Long = 0
    private var currentFlowGs: Float = 0.0f
    private val flowEmaAlpha: Float = 0.35f

    // UI-28: Suspend 1 Hz keepalive runnable when activity is paused
    private var isHeartbeatActive = false

    fun pauseHeartbeat() {
        isHeartbeatActive = false
        mainHandler.removeCallbacks(serviceRunnable)
        Log.d(TAG, "Scale service heartbeat paused")
    }

    fun resumeHeartbeat() {
        if (activeDriver != null && bluetoothGatt != null && !isHeartbeatActive) {
            isHeartbeatActive = true
            mainHandler.removeCallbacks(serviceRunnable)
            mainHandler.post(serviceRunnable)
            Log.d(TAG, "Scale service heartbeat resumed")
        }
    }

    // Keepalive runnable for drivers that need periodic heartbeat (e.g. Acaia)
    private val serviceRunnable = object : Runnable {
        override fun run() {
            if (!isHeartbeatActive) return
            val gatt = bluetoothGatt
            val driver = activeDriver
            if (gatt != null && driver != null) {
                enqueueGattOp {
                    driver.service(gatt)
                }
            }
            mainHandler.postDelayed(this, 1000)
        }
    }

    // UI-27: Serialized GATT operation queue to prevent dropped writes
    private val gattOpQueue = ArrayDeque<() -> Boolean>()
    private var isGattOpBusy = false
    private val gattTimeoutRunnable = Runnable {
        Log.w(TAG, "GATT op timed out, advancing queue")
        synchronized(this@BleScaleManager) {
            isGattOpBusy = false
            processNextGattOp()
        }
    }

    @Synchronized
    private fun enqueueGattOp(op: () -> Boolean): Boolean {
        gattOpQueue.add(op)
        if (!isGattOpBusy) {
            processNextGattOp()
        }
        return true
    }

    @Synchronized
    private fun processNextGattOp() {
        mainHandler.removeCallbacks(gattTimeoutRunnable)
        val nextOp = gattOpQueue.poll()
        if (nextOp == null) {
            isGattOpBusy = false
            return
        }
        isGattOpBusy = true
        mainHandler.postDelayed(gattTimeoutRunnable, 2000)
        val started = try {
            nextOp.invoke()
        } catch (e: Exception) {
            Log.e(TAG, "Exception executing GATT op", e)
            false
        }
        if (!started) {
            mainHandler.removeCallbacks(gattTimeoutRunnable)
            isGattOpBusy = false
            processNextGattOp()
        }
    }

    @Synchronized
    private fun clearGattQueue() {
        mainHandler.removeCallbacks(gattTimeoutRunnable)
        gattOpQueue.clear()
        isGattOpBusy = false
    }

    fun isBluetoothEnabled(): Boolean = bluetoothAdapter?.isEnabled == true

    fun startScan(timeoutMs: Long = 12000) {
        val scanner = bleScanner ?: run {
            Log.w(TAG, "BluetoothLeScanner unavailable")
            return
        }
        if (isScanning) return

        isScanning = true
        updateState("SCANNING")
        Log.i(TAG, "Starting BLE Scale scan...")

        val scanSettings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        try {
            scanner.startScan(null, scanSettings, scanCallback)
            mainHandler.postDelayed({ stopScan() }, timeoutMs)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start BLE scan", e)
            isScanning = false
            updateState("IDLE")
        }
    }

    fun stopScan() {
        if (!isScanning) return
        isScanning = false
        try {
            bleScanner?.stopScan(scanCallback)
            Log.i(TAG, "BLE Scale scan stopped")
        } catch (e: Exception) {
            Log.w(TAG, "Error stopping scan", e)
        }
        if (currentState == "SCANNING") {
            updateState(if (bluetoothGatt != null) "CONNECTED" else "IDLE")
        }
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult?) {
            result ?: return
            val device = result.device ?: return
            val record = result.scanRecord
            val name = record?.deviceName ?: device.name ?: "Unknown Scale"
            val uuids = record?.serviceUuids?.map { it.uuid } ?: emptyList()

            // Check if matches any known driver
            for (driver in drivers) {
                if (driver.matches(uuids, name)) {
                    val disc = DiscoveredScale(
                        name = name,
                        address = device.address,
                        rssi = result.rssi,
                        linkType = driver.linkType
                    )
                    mainHandler.post { listener?.onScaleDiscovered(disc) }
                    break
                }
            }
        }

        override fun onScanFailed(errorCode: Int) {
            Log.e(TAG, "BLE scan failed with code: $errorCode")
            isScanning = false
            updateState("IDLE")
        }
    }

    fun connect(macAddress: String) {
        stopScan()
        disconnect()

        val adapter = bluetoothAdapter ?: return
        val device = try {
            adapter.getRemoteDevice(macAddress)
        } catch (e: Exception) {
            Log.e(TAG, "Invalid MAC address: $macAddress", e)
            return
        }

        updateState("CONNECTING", device.name, macAddress)
        Log.i(TAG, "Connecting to scale: ${device.name} ($macAddress)")

        bluetoothGatt = device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
    }

    fun disconnect() {
        pauseHeartbeat()
        clearGattQueue()
        try {
            bluetoothGatt?.disconnect()
            bluetoothGatt?.close()
        } catch (e: Exception) {
            Log.w(TAG, "Error disconnecting GATT", e)
        }
        bluetoothGatt = null
        activeDriver = null
        lastGrams = 0f
        lastTimeMs = 0
        currentFlowGs = 0f
        updateState("IDLE")
    }

    // UI-27: Enqueue tare command through serialized GATT op queue
    fun tare(): Boolean {
        val gatt = bluetoothGatt ?: return false
        val driver = activeDriver ?: return false
        if (driver is TimemoreDriver) {
            enqueueGattOp {
                driver.sendTareWakeup(gatt)
            }
        }
        return enqueueGattOp {
            driver.sendTare(gatt)
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            val dev = gatt.device
            if (newState == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {
                Log.i(TAG, "GATT connected to ${dev.name} (${dev.address}), discovering services...")
                connectedDeviceName = dev.name ?: "BLE Scale"
                connectedDeviceMac = dev.address
                updateState("CONNECTING", connectedDeviceName, connectedDeviceMac)
                gatt.discoverServices()
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                Log.i(TAG, "GATT disconnected from ${dev.address}")
                disconnect()
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                Log.w(TAG, "Service discovery failed: $status")
                disconnect()
                return
            }

            val dev = gatt.device
            val uuids = gatt.services.map { it.uuid }
            val devName = dev.name

            // Find matching driver
            var matched: ScaleDriver? = null
            for (driver in drivers) {
                if (driver.matches(uuids, devName) || driver.onConnected(gatt)) {
                    matched = driver
                    break
                }
            }

            if (matched == null) {
                // Try fallback discovery on each driver
                for (driver in drivers) {
                    if (driver.onConnected(gatt)) {
                        matched = driver
                        break
                    }
                }
            }

            if (matched != null) {
                activeDriver = matched
                Log.i(TAG, "Matched scale driver: ${matched.name} (${matched.linkType})")

                // Subscribe to notifications on matched characteristic
                val notifyUuid = matched.getNotifyUuid()
                if (notifyUuid != null) {
                    subscribeNotification(gatt, notifyUuid)
                }

                updateState("CONNECTED", connectedDeviceName ?: matched.name, dev.address)
                resumeHeartbeat()
            } else {
                Log.e(TAG, "No compatible scale driver matched for device ${dev.name}")
                disconnect()
            }
        }

        override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            val data = characteristic.value ?: return
            val driver = activeDriver ?: return

            val reading = driver.onCharacteristicChanged(gatt, characteristic, data)
            if (reading != null) {
                // Calculate flow rate
                val now = System.currentTimeMillis()
                if (lastTimeMs > 0 && now > lastTimeMs) {
                    val dt = (now - lastTimeMs).toFloat() / 1000.0f
                    if (dt in 0.04f..1.5f) {
                        val dGrams = reading.grams - lastGrams
                        val instFlow = dGrams / dt
                        // Simple EMA filter
                        currentFlowGs = (currentFlowGs * (1.0f - flowEmaAlpha)) + (instFlow * flowEmaAlpha)
                        if (currentFlowGs < 0.05f && currentFlowGs > -0.05f) currentFlowGs = 0.0f
                    }
                }
                lastGrams = reading.grams
                lastTimeMs = now

                val enrichedReading = reading.copy(flowGs = currentFlowGs)
                mainHandler.post { listener?.onReading(enrichedReading) }
            }
        }

        // UI-27: Notify serialized queue on descriptor completion
        override fun onDescriptorWrite(gatt: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            Log.d(TAG, "Descriptor write status=$status for ${descriptor.uuid}")
            synchronized(this@BleScaleManager) {
                isGattOpBusy = false
                processNextGattOp()
            }
        }

        // UI-27: Notify serialized queue on characteristic write completion
        override fun onCharacteristicWrite(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
            Log.d(TAG, "Characteristic write status=$status for ${characteristic.uuid}")
            synchronized(this@BleScaleManager) {
                isGattOpBusy = false
                processNextGattOp()
            }
        }
    }

    // UI-27: Serialize notification CCCD descriptor subscription
    private fun subscribeNotification(gatt: BluetoothGatt, charUuid: UUID) {
        for (service in gatt.services) {
            val ch = service.getCharacteristic(charUuid)
            if (ch != null) {
                gatt.setCharacteristicNotification(ch, true)
                val descriptor = ch.getDescriptor(CCCD_UUID)
                if (descriptor != null) {
                    enqueueGattOp {
                        descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                        val ok = gatt.writeDescriptor(descriptor)
                        Log.i(TAG, "Subscribed to CCCD notifications for $charUuid (write result: $ok)")
                        ok
                    }
                }
                return
            }
        }
    }

    private fun updateState(state: String, devName: String? = connectedDeviceName, mac: String? = connectedDeviceMac) {
        currentState = state
        if (state == "IDLE") {
            connectedDeviceName = null
            connectedDeviceMac = null
        }
        mainHandler.post {
            listener?.onStateChanged(state, devName, mac)
        }
    }
}
