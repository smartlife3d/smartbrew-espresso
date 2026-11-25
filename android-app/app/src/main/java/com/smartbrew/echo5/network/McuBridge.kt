package com.smartbrew.echo5.network

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import android.util.Log
import kotlinx.coroutines.*
import kotlinx.coroutines.selects.select
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import okhttp3.*
import okhttp3.MediaType.Companion.toMediaTypeOrNull
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONArray
import org.json.JSONObject
import java.io.IOException
import java.net.Inet4Address
import java.net.NetworkInterface
import java.util.Collections
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.TimeUnit

data class McuDevice(
    val ip: String,
    val port: Int = 80,
    val hostname: String = "smartbrew",
    val name: String = "SmartBrew",
    val fw: String = ""
)

class McuBridge(private val context: Context) {

    companion object {
        private const val TAG = "McuBridge"
        private const val SERVICE_TYPE = "_http._tcp."
    }

    val client = OkHttpClient.Builder()
        .connectTimeout(5000, TimeUnit.MILLISECONDS)
        .readTimeout(5000, TimeUnit.MILLISECONDS)
        .writeTimeout(5000, TimeUnit.MILLISECONDS)
        .build()

    val proxyClient = OkHttpClient.Builder()
        .connectTimeout(600, TimeUnit.MILLISECONDS)
        .readTimeout(1200, TimeUnit.MILLISECONDS)
        .writeTimeout(1200, TimeUnit.MILLISECONDS)
        .build()

    private val probeClient = OkHttpClient.Builder()
        .connectTimeout(2000, TimeUnit.MILLISECONDS)
        .readTimeout(2500, TimeUnit.MILLISECONDS)
        .writeTimeout(2000, TimeUnit.MILLISECONDS)
        .build()

    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())
    private val nsdManager = context.getSystemService(Context.NSD_SERVICE) as? NsdManager
    private val wifiManager = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager

    private var multicastLock: WifiManager.MulticastLock? = null
    private var lastRelayTimeMs: Long = 0
    private var isRelaying = false
    private var isScanningSubnet = false
    private var isDiscoveryStarted = false
    private var watchdogJob: Job? = null

    var currentMcuIp: String? = null
        private set

    /**
     * Body of the last successful `/api/v1/status` response.
     *
     * The watchdog used to report only "online/offline", but the client also
     * needs the Controller state to know whether the machine is brewing - the
     * cockpit has to stay on screen while a shot is running.
     */
    @Volatile
    var lastStatusJson: String? = null
        private set

    @Volatile
    var isMcuOnline: Boolean = false
        private set

    val discoveredMcus = ConcurrentHashMap<String, McuDevice>()

    interface McuDiscoveryListener {
        fun onMcuFound(host: String, port: Int)
        fun onMcuStatusChecked(online: Boolean, detailsJson: String?)
        fun onMcuListUpdated(mcuListJson: String)
    }

    var listener: McuDiscoveryListener? = null

    fun startAutoDiscovery(lastKnownHost: String? = null) {
        // 1. Acquire MulticastLock so Android doesn't drop mDNS packets
        try {
            if (multicastLock == null) {
                multicastLock = wifiManager?.createMulticastLock("SmartBrewMulticastLock")?.apply {
                    setReferenceCounted(true)
                    acquire()
                    Log.i(TAG, "Acquired Wi-Fi MulticastLock for mDNS discovery")
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "Failed to acquire MulticastLock", e)
        }

        // 2. Test last known host or cached IP first (instant connect)
        scope.launch {
            if (!lastKnownHost.isNullOrBlank()) {
                if (probeAndRecordMcu(lastKnownHost)) {
                    Log.i(TAG, "Verified last known Controller host: $lastKnownHost")
                    notifyMcuFound(lastKnownHost, 80)
                    startWatchdog()
                    return@launch
                }
            }

            // 3. Kick off mDNS and fast local subnet sweep simultaneously
            startMdnsDiscovery()
            scanLocalSubnet()
            startWatchdog()
        }
    }

    private suspend fun probeAndRecordMcu(ip: String, port: Int = 80): Boolean {
        return withContext(Dispatchers.IO) {
            try {
                val req = Request.Builder()
                    .url("http://$ip:$port/api/v1/status")
                    .header("User-Agent", "SmartBrew-Echo5/1.0")
                    .build()
                probeClient.newCall(req).execute().use { resp ->
                    val body = resp.body?.string()
                    if (resp.isSuccessful && body != null && body.contains("\"fw\":")) {
                        lastStatusJson = body
                        isMcuOnline = true
                        try {
                            val json = JSONObject(body)
                            val hostname = json.optString("hostname", json.optString("mcu_name", "smartbrew"))
                            val fw = json.optString("fw", "")
                            val displayName = if (hostname.isNotBlank() && hostname != "brew-mcu" && hostname != "smartbrew") {
                                "SmartBrew ($hostname)"
                            } else {
                                "SmartBrew Controller ($ip)"
                            }
                            val dev = McuDevice(ip = ip, port = port, hostname = hostname, name = displayName, fw = fw)
                            discoveredMcus[ip] = dev
                            notifyMcuListUpdated()
                        } catch (e: Exception) {
                            discoveredMcus[ip] = McuDevice(ip = ip, port = port, hostname = "smartbrew", name = "SmartBrew Controller ($ip)")
                            notifyMcuListUpdated()
                        }
                        true
                    } else false
                }
            } catch (e: Exception) {
                false
            }
        }
    }

    private suspend fun probeMcu(ip: String): Boolean {
        return probeAndRecordMcu(ip)
    }

    fun scanLocalSubnet() {
        if (isScanningSubnet) return
        isScanningSubnet = true

        scope.launch {
            // Always check standard SoftAP gateway and default mDNS hostname
            launch {
                if (probeAndRecordMcu("192.168.4.1")) {
                    Log.i(TAG, "Located Controller on SoftAP IP: 192.168.4.1")
                    if (currentMcuIp == null) notifyMcuFound("192.168.4.1", 80)
                } else if (probeAndRecordMcu("smartbrew.local")) {
                    Log.i(TAG, "Located Controller on mDNS: smartbrew.local")
                    if (currentMcuIp == null) notifyMcuFound("smartbrew.local", 80)
                }
            }

            val localIp = getDeviceIp()
            if (localIp == null) {
                Log.w(TAG, "Device Wi-Fi IP unavailable; skipping /24 subnet sweep")
                isScanningSubnet = false
                return@launch
            }
            Log.i(TAG, "Echo Show 5 local IP is $localIp. Scanning subnet...")

            val parts = localIp.split(".")
            if (parts.size != 4) {
                isScanningSubnet = false
                return@launch
            }
            val subnetPrefix = "${parts[0]}.${parts[1]}.${parts[2]}."

            // UI-21: Bound concurrency with Semaphore(24)
            val semaphore = Semaphore(24)
            val jobs = (1..254).map { i ->
                launch {
                    semaphore.withPermit {
                        val targetIp = "$subnetPrefix$i"
                        if (probeAndRecordMcu(targetIp)) {
                            Log.i(TAG, "Subnet sweep discovered SmartBrew Controller at: $targetIp")
                            if (currentMcuIp == null) {
                                notifyMcuFound(targetIp, 80)
                            }
                        }
                    }
                }
            }

            withTimeoutOrNull(4500) {
                jobs.joinAll()
            }
            isScanningSubnet = false
            Log.i(TAG, "Subnet scan completed. Found ${discoveredMcus.size} Controller(s)")
            notifyMcuListUpdated()
        }
    }

    private fun notifyMcuListUpdated() {
        val arr = JSONArray()
        discoveredMcus.values.forEach { mcu ->
            arr.put(JSONObject().apply {
                put("ip", mcu.ip)
                put("port", mcu.port)
                put("hostname", mcu.hostname)
                put("name", mcu.name)
                put("fw", mcu.fw)
            })
        }
        val jsonStr = arr.toString()
        // UI-23: Use existing scope with Dispatchers.Main
        scope.launch(Dispatchers.Main) {
            listener?.onMcuListUpdated(jsonStr)
        }
    }

    fun setManualTarget(host: String) {
        currentMcuIp = host
        scope.launch {
            probeAndRecordMcu(host)
        }
    }

    private fun notifyMcuFound(host: String, port: Int) {
        currentMcuIp = host
        // UI-23: Use existing scope with Dispatchers.Main
        scope.launch(Dispatchers.Main) {
            listener?.onMcuFound(host, port)
        }
    }

    // UI-22: Single-instance watchdog guard
    private fun startWatchdog() {
        watchdogJob?.cancel()
        watchdogJob = scope.launch {
            var consecutiveFails = 0
            while (isActive) {
                delay(5000)
                val target = currentMcuIp
                if (target != null) {
                    val ok = probeMcu(target)
                    isMcuOnline = ok
                    withContext(Dispatchers.Main) {
                        // Only report the Controller state while it is actually
                        // reachable - a stale "BREWING" would keep the screen
                        // saver switched off forever.
                        listener?.onMcuStatusChecked(ok, if (ok) lastStatusJson else null)
                    }
                    if (!ok) {
                        consecutiveFails++
                        if (consecutiveFails >= 2) {
                            Log.w(TAG, "Lost connection to Controller at $target, re-scanning subnet...")
                            scanLocalSubnet()
                            consecutiveFails = 0
                        }
                    } else {
                        consecutiveFails = 0
                    }
                } else {
                    scanLocalSubnet()
                }
            }
        }
    }

    private fun getDeviceIp(): String? {
        try {
            val interfaces = Collections.list(NetworkInterface.getNetworkInterfaces())
            for (intf in interfaces) {
                if (intf.name.contains("wlan") || intf.name.contains("eth")) {
                    val addrs = Collections.list(intf.inetAddresses)
                    for (addr in addrs) {
                        if (!addr.isLoopbackAddress && addr is Inet4Address) {
                            return addr.hostAddress
                        }
                    }
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "Error resolving device IP", e)
        }
        return null
    }

    fun checkHealth(host: String, callback: (Boolean, String?) -> Unit) {
        scope.launch {
            val url = "http://$host/api/v1/status"
            val request = Request.Builder()
                .url(url)
                .header("User-Agent", "SmartBrew-Echo5/1.0")
                .build()

            try {
                client.newCall(request).execute().use { response ->
                    val body = response.body?.string()
                    val ok = response.isSuccessful && body != null && body.contains("\"fw\":")
                    withContext(Dispatchers.Main) {
                        callback(ok, body)
                    }
                }
            } catch (e: Exception) {
                withContext(Dispatchers.Main) {
                    callback(false, null)
                }
            }
        }
    }

    // UI-26: Scale connects directly to ESP32 controller over BLE; relay to /sim/weight is disabled
    @Suppress("UNUSED_PARAMETER")
    fun relayWeight(host: String, grams: Float) {
        // No-op: Scale connects directly to ESP32 controller over BLE; readings arrive directly via WebSocket telemetry
    }

    fun sendCommand(host: String, cmdJson: String, callback: ((Boolean) -> Unit)? = null) {
        scope.launch {
            try {
                val json = JSONObject(cmdJson)
                val cmd = json.optString("cmd", "").uppercase()
                val url = when (cmd) {
                    "USE", "LOAD_PROFILE" -> {
                        val id = java.net.URLEncoder.encode(json.optString("id", ""), "UTF-8")
                        "http://$host/api/v1/profiles/active?id=$id"
                    }
                    "SIM" -> {
                        val on = if (json.optBoolean("on", false)) "1" else "0"
                        "http://$host/api/v1/bench/sim?on=$on"
                    }
                    else -> {
                        val encodedCmd = java.net.URLEncoder.encode(cmd, "UTF-8")
                        val secParam = if (json.has("sec")) "?sec=${json.optDouble("sec", 5.0)}" else ""
                        "http://$host/api/v1/cmd/$encodedCmd$secParam"
                    }
                }
                val emptyBody = "".toRequestBody("application/x-www-form-urlencoded".toMediaTypeOrNull())
                val request = Request.Builder()
                    .url(url)
                    .post(emptyBody)
                    .build()

                client.newCall(request).execute().use { response ->
                    withContext(Dispatchers.Main) {
                        callback?.invoke(response.isSuccessful)
                    }
                }
            } catch (e: Exception) {
                withContext(Dispatchers.Main) {
                    callback?.invoke(false)
                }
            }
        }
    }

    fun setProfile(host: String, profileId: String, callback: ((Boolean) -> Unit)? = null) {
        scope.launch {
            try {
                val encodedId = java.net.URLEncoder.encode(profileId, "UTF-8")
                val url = "http://$host/api/v1/profiles/active?id=$encodedId"
                val emptyBody = "".toRequestBody("application/x-www-form-urlencoded".toMediaTypeOrNull())
                val request = Request.Builder()
                    .url(url)
                    .post(emptyBody)
                    .build()

                client.newCall(request).execute().use { response ->
                    withContext(Dispatchers.Main) {
                        callback?.invoke(response.isSuccessful)
                    }
                }
            } catch (e: Exception) {
                withContext(Dispatchers.Main) {
                    callback?.invoke(false)
                }
            }
        }
    }

    fun setSimulationMode(host: String, enabled: Boolean, callback: ((Boolean) -> Unit)? = null) {
        scope.launch {
            try {
                val url = "http://$host/api/v1/bench/sim?on=" + if (enabled) "1" else "0"
                val emptyBody = "".toRequestBody("application/x-www-form-urlencoded".toMediaTypeOrNull())
                val request = Request.Builder()
                    .url(url)
                    .post(emptyBody)
                    .build()

                client.newCall(request).execute().use { response ->
                    withContext(Dispatchers.Main) {
                        callback?.invoke(response.isSuccessful)
                    }
                }
            } catch (e: Exception) {
                withContext(Dispatchers.Main) {
                    callback?.invoke(false)
                }
            }
        }
    }


    private fun startMdnsDiscovery() {
        if (isDiscoveryStarted) return
        val nsd = nsdManager ?: return
        try {
            nsd.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, discoveryListener)
            isDiscoveryStarted = true
            Log.i(TAG, "Started mDNS discovery for SmartBrew Controller...")
        } catch (e: Exception) {
            Log.w(TAG, "Failed to start mDNS discovery", e)
        }
    }

    private fun stopMdnsDiscovery() {
        if (!isDiscoveryStarted) return
        val nsd = nsdManager ?: return
        try {
            nsd.stopServiceDiscovery(discoveryListener)
            isDiscoveryStarted = false
        } catch (e: Exception) {
            // ignore
        }
    }

    private val discoveryListener = object : NsdManager.DiscoveryListener {
        override fun onStartDiscoveryFailed(serviceType: String?, errorCode: Int) {
            Log.e(TAG, "Discovery failed: $errorCode")
            isDiscoveryStarted = false
        }

        override fun onStopDiscoveryFailed(serviceType: String?, errorCode: Int) {
            Log.e(TAG, "Stop discovery failed: $errorCode")
        }

        override fun onDiscoveryStarted(serviceType: String?) {
            Log.d(TAG, "Service discovery started")
        }

        override fun onDiscoveryStopped(serviceType: String?) {
            Log.d(TAG, "Service discovery stopped")
            isDiscoveryStarted = false
        }

        override fun onServiceFound(serviceInfo: NsdServiceInfo?) {
            serviceInfo ?: return
            val name = serviceInfo.serviceName.lowercase()
            if (name.contains("smartbrew") || name.contains("brew") || name.contains("espresso")) {
                nsdManager?.resolveService(serviceInfo, object : NsdManager.ResolveListener {
                    override fun onResolveFailed(info: NsdServiceInfo?, errorCode: Int) {
                        Log.w(TAG, "Resolve failed: $errorCode")
                    }

                    override fun onServiceResolved(info: NsdServiceInfo?) {
                        info ?: return
                        val host = info.host?.hostAddress ?: return
                        val port = info.port
                        Log.i(TAG, "mDNS Resolved SmartBrew Controller at $host:$port (${info.serviceName})")
                        scope.launch {
                            if (probeAndRecordMcu(host, port)) {
                                if (currentMcuIp == null) {
                                    notifyMcuFound(host, port)
                                }
                            }
                        }
                    }
                })
            }
        }

        override fun onServiceLost(serviceInfo: NsdServiceInfo?) {}
    }

    fun destroy() {
        watchdogJob?.cancel()
        stopMdnsDiscovery()
        try {
            if (multicastLock?.isHeld == true) {
                multicastLock?.release()
            }
        } catch (e: Exception) {}
        scope.cancel()
    }
}
