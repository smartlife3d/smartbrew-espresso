package com.smartbrew.echo5

import android.webkit.JavascriptInterface
import android.widget.Toast
import com.smartbrew.echo5.scale.BleScaleManager
import com.smartbrew.echo5.utils.AppPreferences
import org.json.JSONObject

class WebAppInterface(
    private val activity: MainActivity,
    private val scaleManager: BleScaleManager,
    private val preferences: AppPreferences
) {

    @JavascriptInterface
    fun isNative(): Boolean = true

    @JavascriptInterface
    fun isEchoDevice(): Boolean = true

    @JavascriptInterface
    fun getMcuHost(): String = preferences.mcuHost

    @JavascriptInterface
    fun setMcuHost(host: String): Boolean {
        if (host.isBlank()) return false
        preferences.mcuHost = host.trim()
        activity.runOnUiThread {
            activity.onMcuHostUpdated(preferences.mcuHost)
        }
        return true
    }

    /**
     * UI-32: Asynchronously dispatches a Controller health check HTTP probe.
     * Contract: Returns an immediate acknowledgement payload: `{"status":"checking","host":"<host>"}`.
     * The verified health result is delivered asynchronously via the `onMcuHealthResult` JS event
     * with schema `{ online: Boolean, host: String, details?: Object }`.
     */
    @JavascriptInterface
    fun checkMcuHealth(): String {
        val host = preferences.mcuHost
        activity.mcuBridge.checkHealth(host) { ok, json ->
            activity.sendJsEvent("onMcuHealthResult", JSONObject().apply {
                put("online", ok)
                put("host", host)
                if (json != null) put("details", JSONObject(json))
            }.toString())
        }
        return JSONObject().apply {
            put("status", "checking")
            put("host", host)
        }.toString()
    }

    @JavascriptInterface
    fun discoverMcu() {
        activity.mcuBridge.startAutoDiscovery(preferences.mcuHost)
    }

    @JavascriptInterface
    fun getDiscoveredMcus(): String {
        val arr = org.json.JSONArray()
        activity.mcuBridge.discoveredMcus.values.forEach { mcu ->
            arr.put(JSONObject().apply {
                put("ip", mcu.ip)
                put("port", mcu.port)
                put("hostname", mcu.hostname)
                put("name", mcu.name)
                put("fw", mcu.fw)
            })
        }
        return arr.toString()
    }

    @JavascriptInterface
    fun setActiveProfile(profileId: String): Boolean {
        if (profileId.isBlank()) return false
        val host = preferences.mcuHost
        activity.mcuBridge.setProfile(host, profileId.trim())
        return true
    }

    @JavascriptInterface
    fun setSimulationMode(enabled: Boolean): Boolean {
        val host = preferences.mcuHost
        activity.mcuBridge.setSimulationMode(host, enabled)
        return true
    }

    // --- BLE Scale Control ---

    @JavascriptInterface
    fun startBleScan(timeoutMs: Long) {
        activity.requestBlePermissionsThen {
            scaleManager.startScan(if (timeoutMs > 0) timeoutMs else 12000)
        }
    }

    @JavascriptInterface
    fun stopBleScan() {
        scaleManager.stopScan()
    }

    @JavascriptInterface
    fun connectScale(macAddress: String) {
        preferences.pairedScaleMac = macAddress
        activity.requestBlePermissionsThen {
            scaleManager.connect(macAddress)
        }
    }

    @JavascriptInterface
    fun disconnectScale() {
        preferences.pairedScaleMac = null
        preferences.pairedScaleName = null
        scaleManager.disconnect()
    }

    @JavascriptInterface
    fun tareScale(): Boolean {
        return scaleManager.tare()
    }

    @JavascriptInterface
    fun getScaleStatus(): String {
        val obj = JSONObject().apply {
            put("state", scaleManager.currentState)
            put("name", scaleManager.connectedDeviceName ?: "")
            put("mac", scaleManager.connectedDeviceMac ?: "")
            put("autoConnect", preferences.autoConnectScale)
        }
        return obj.toString()
    }

    @JavascriptInterface
    fun setAutoConnectScale(enabled: Boolean) {
        preferences.autoConnectScale = enabled
    }

    @JavascriptInterface
    fun setRelayWeight(enabled: Boolean) {
        preferences.relayWeightToMcu = enabled
    }

    @JavascriptInterface
    fun switchUiSource(useLocal: Boolean) {
        preferences.useLocalUi = useLocal
        activity.runOnUiThread {
            activity.loadCockpitUi()
        }
    }

    @JavascriptInterface
    fun toggleFullscreen() {
        activity.runOnUiThread {
            activity.hideSystemUI()
        }
    }

    @JavascriptInterface
    fun showToast(message: String) {
        activity.runOnUiThread {
            Toast.makeText(activity, message, Toast.LENGTH_SHORT).show()
        }
    }
}
