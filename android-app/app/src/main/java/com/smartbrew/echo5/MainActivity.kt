package com.smartbrew.echo5

import android.Manifest
import android.annotation.SuppressLint
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.Color
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.webkit.*
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.webkit.WebViewAssetLoader
import com.smartbrew.echo5.network.McuBridge
import com.smartbrew.echo5.scale.BleScaleManager
import com.smartbrew.echo5.scale.DiscoveredScale
import com.smartbrew.echo5.scale.ScaleEventListener
import com.smartbrew.echo5.scale.ScaleReading
import com.smartbrew.echo5.screensaver.ScreenSaverController
import com.smartbrew.echo5.screensaver.ScreensaverView
import com.smartbrew.echo5.utils.AppPreferences
import org.json.JSONObject

class MainActivity : AppCompatActivity(), ScaleEventListener, McuBridge.McuDiscoveryListener {

    companion object {
        private const val TAG = "SmartBrewMainActivity"
    }

    private lateinit var webView: WebView
    private lateinit var loadingLayout: LinearLayout
    private lateinit var tvLoading: TextView
    private lateinit var screensaverView: ScreensaverView
    private lateinit var screenSaver: ScreenSaverController

    lateinit var scaleManager: BleScaleManager
        private set
    lateinit var mcuBridge: McuBridge
        private set
    lateinit var preferences: AppPreferences
        private set

    private var pendingPermissionCallback: (() -> Unit)? = null
    private val mainHandler = Handler(Looper.getMainLooper())

    // True while the gesture that woke the display up is still in progress.
    private var saverGestureSwallowed = false

    // UI-30: Remote connection fallback timer (7s timeout to local bundled UI)
    private val remoteLoadTimeoutHandler = Handler(Looper.getMainLooper())
    private val remoteLoadTimeoutRunnable = Runnable {
        if (!preferences.useLocalUi) {
            Log.w(TAG, "Remote Controller load timed out after 7s; falling back to bundled local UI")
            preferences.useLocalUi = true
            loadCockpitUi()
        }
    }

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { permissions ->
        val allGranted = permissions.entries.all { it.value }
        if (allGranted) {
            Log.i(TAG, "All BLE/location permissions granted")
            pendingPermissionCallback?.invoke()
        } else {
            Log.w(TAG, "Some permissions were denied: $permissions")
        }
        pendingPermissionCallback = null
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Lock landscape for Amazon Echo Show 5 (960x480)
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE

        // Remove all window limits to use true full hardware screen height (480px)
        window.setFlags(
            WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
            WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
        )
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            window.attributes.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        }

        setContentView(R.layout.activity_main)

        WindowCompat.setDecorFitsSystemWindows(window, false)
        window.decorView.setOnApplyWindowInsetsListener { _, _ -> WindowInsets.CONSUMED }

        hideSystemUI()

        preferences = AppPreferences(this)
        scaleManager = BleScaleManager(this).apply { listener = this@MainActivity }
        mcuBridge = McuBridge(this).apply { listener = this@MainActivity }

        webView = findViewById(R.id.webView)
        loadingLayout = findViewById(R.id.loadingLayout)
        tvLoading = findViewById(R.id.tvLoading)
        screensaverView = findViewById(R.id.screensaverView)
        screenSaver = ScreenSaverController(screensaverView)

        setupWebView()
        loadCockpitUi()

        // Automatically discover and link to the ESP32 Controller on local Wi-Fi
        mcuBridge.startAutoDiscovery(preferences.mcuHost)

        // Scale connects directly to ESP32 Controller over BLE (not the Android client device)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            hideSystemUI()
        }
    }

    /**
     * Every touch counts as activity for the screen saver.
     *
     * While the screen saver is up the touch belongs to the screen saver: the
     * whole gesture is swallowed here, so it can never reach the cockpit
     * underneath.  That is what makes "touch to resume" restore the previous
     * screen exactly.
     */
    override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
        if (!::screenSaver.isInitialized) return super.dispatchTouchEvent(ev)

        val saverWasUp = screenSaver.isActive()

        when (ev.actionMasked) {
            MotionEvent.ACTION_DOWN -> screenSaver.touchInProgress = true
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL ->
                screenSaver.touchInProgress = false
        }
        // A touch is activity, but only the start and the end of a gesture
        // have to restart the idle timer.
        if (ev.actionMasked != MotionEvent.ACTION_MOVE) {
            screenSaver.notifyUserInteraction()
        }

        if (saverWasUp) {
            // This gesture woke the display up - swallow all of it.
            saverGestureSwallowed = true
            return true
        }
        if (saverGestureSwallowed) {
            if (ev.actionMasked == MotionEvent.ACTION_UP ||
                ev.actionMasked == MotionEvent.ACTION_CANCEL
            ) {
                saverGestureSwallowed = false
            }
            return true
        }
        return super.dispatchTouchEvent(ev)
    }

    fun hideSystemUI() {
        try {
            WindowCompat.setDecorFitsSystemWindows(window, false)
            val windowInsetsController = WindowCompat.getInsetsController(window, window.decorView)
            windowInsetsController.systemBarsBehavior =
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            windowInsetsController.hide(WindowInsetsCompat.Type.systemBars())
        } catch (e: Exception) {
            Log.w(TAG, "Failed to apply WindowInsetsControllerCompat, falling back to legacy flags", e)
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                or View.SYSTEM_UI_FLAG_FULLSCREEN
            )
        }
    }

    @SuppressLint("SetJavaScriptEnabled")
    private fun setupWebView() {
        val assetLoader = WebViewAssetLoader.Builder()
            .setHttpAllowed(true)
            .setDomain("appassets.androidplatform.net")
            .addPathHandler("/assets/", WebViewAssetLoader.AssetsPathHandler(this))
            .build()

        webView.setBackgroundColor(Color.parseColor("#07090E"))

        val settings = webView.settings
        settings.javaScriptEnabled = true
        settings.domStorageEnabled = true
        settings.databaseEnabled = true
        // UI-18: Drop file access; bundle is served via WebViewAssetLoader
        settings.allowFileAccess = false
        settings.allowContentAccess = true // Preserved for profile upload and OTA firmware flashing file chooser
        @Suppress("DEPRECATION")
        settings.allowFileAccessFromFileURLs = false
        @Suppress("DEPRECATION")
        settings.allowUniversalAccessFromFileURLs = false
        // UI-18: Permit mixed content so https://appassets.androidplatform.net can communicate with local LAN Controller over HTTP & WS
        settings.mixedContentMode = WebSettings.MIXED_CONTENT_ALWAYS_ALLOW
        settings.mediaPlaybackRequiresUserGesture = false
        settings.loadWithOverviewMode = true
        settings.useWideViewPort = false
        settings.builtInZoomControls = false
        settings.displayZoomControls = false

        // Bridge exposure
        val jsInterface = WebAppInterface(this, scaleManager, preferences)
        webView.addJavascriptInterface(jsInterface, "AndroidScale")
        webView.addJavascriptInterface(jsInterface, "EchoCompanion")

        webView.webViewClient = object : WebViewClient() {
            private fun isAllowedNavigationHost(host: String?): Boolean {
                if (host == null) return false
                val currentMcu = mcuBridge.currentMcuIp
                val savedMcu = preferences.mcuHost
                return host == "appassets.androidplatform.net" ||
                       host == "192.168.4.1" ||
                       host == "smartbrew.local" ||
                       host == "brew-mcu.local" ||
                       host == "elizabeth.local" ||
                       host == currentMcu ||
                       host == savedMcu
            }

            override fun shouldOverrideUrlLoading(
                view: WebView?,
                request: WebResourceRequest?
            ): Boolean {
                val uri = request?.url ?: return false
                val host = uri.host
                if (!isAllowedNavigationHost(host)) {
                    Log.w(TAG, "Blocked navigation to unverified host: $host ($uri)")
                    runOnUiThread {
                        Toast.makeText(this@MainActivity, "Navigation to $host blocked for security", Toast.LENGTH_SHORT).show()
                    }
                    return true
                }
                return false
            }

            override fun onPageStarted(view: WebView?, url: String?, favicon: Bitmap?) {
                super.onPageStarted(view, url, favicon)
                val uri = url?.let { Uri.parse(it) }
                val host = uri?.host
                if (host != null && !isAllowedNavigationHost(host)) {
                    Log.e(TAG, "onPageStarted: Blocked navigation to untrusted host $host ($url)")
                    view?.stopLoading()
                    runOnUiThread {
                        Toast.makeText(this@MainActivity, "Blocked untrusted host: $host", Toast.LENGTH_SHORT).show()
                        preferences.useLocalUi = true
                        loadCockpitUi()
                    }
                }
            }

            override fun shouldInterceptRequest(
                view: WebView?,
                request: WebResourceRequest?
            ): WebResourceResponse? {
                if (request != null) {
                    val uri = request.url
                    // 1. Intercept local assets if using https://appassets.androidplatform.net
                    if (uri.host == "appassets.androidplatform.net") {
                        return assetLoader.shouldInterceptRequest(uri)
                    }

                    // 2. Transparently proxy Controller GET calls through native OkHttp client to bypass CORS
                    val host = uri.host
                    val currentMcu = mcuBridge.currentMcuIp ?: preferences.mcuHost
                    if ((host == currentMcu || host == "smartbrew.local" || host == "brew-mcu.local" || host == "elizabeth.local" || host == "192.168.4.1")
                        && uri.path?.startsWith("/api/") == true
                        && request.method.equals("GET", ignoreCase = true)
                    ) {
                        val responseHeaders = mapOf(
                            "Access-Control-Allow-Origin" to "*",
                            "Access-Control-Allow-Methods" to "GET, POST, OPTIONS",
                            "Access-Control-Allow-Headers" to "*"
                        )
                        if (!mcuBridge.isMcuOnline && mcuBridge.currentMcuIp == null) {
                            val errBytes = """{"ok":false,"error":"offline"}""".toByteArray(Charsets.UTF_8)
                            return WebResourceResponse(
                                "application/json",
                                "utf-8",
                                503,
                                "Service Unavailable",
                                responseHeaders,
                                java.io.ByteArrayInputStream(errBytes)
                            )
                        }
                        try {
                            val targetUrl = "http://$currentMcu${uri.path}${if (uri.query != null) "?${uri.query}" else ""}"
                            val okReq = okhttp3.Request.Builder()
                                .url(targetUrl)
                                .header("User-Agent", "SmartBrew-Echo5/1.0")
                                .build()
                            val okResp = mcuBridge.proxyClient.newCall(okReq).execute()
                            val mimeType = okResp.header("Content-Type")?.split(";")?.get(0)?.trim() ?: "application/json"
                            return WebResourceResponse(
                                mimeType,
                                "utf-8",
                                okResp.code,
                                okResp.message.ifBlank { "OK" },
                                responseHeaders,
                                okResp.body?.byteStream()
                            )
                        } catch (e: Exception) {
                            Log.w(TAG, "Proxy GET error for ${uri.path}: ${e.message}")
                            val errBytes = """{"ok":false,"error":"offline"}""".toByteArray(Charsets.UTF_8)
                            return WebResourceResponse(
                                "application/json",
                                "utf-8",
                                503,
                                "Service Unavailable",
                                responseHeaders,
                                java.io.ByteArrayInputStream(errBytes)
                            )
                        }
                    }
                }
                return super.shouldInterceptRequest(view, request)
            }

            override fun onPageFinished(view: WebView?, url: String?) {
                super.onPageFinished(view, url)
                remoteLoadTimeoutHandler.removeCallbacks(remoteLoadTimeoutRunnable)
                loadingLayout.visibility = View.GONE

                // Dynamically inject Echo 5 full-screen style overrides and native bridge (idempotent)
                try {
                    val css = assets.open("web/echo5_style.css").bufferedReader().use { it.readText() }
                    val jsCss = "(function() { var s = document.getElementById('echo5-injected-style'); if (!s) { s = document.createElement('style'); s.id = 'echo5-injected-style'; document.head.appendChild(s); s.textContent = " + JSONObject.quote(css) + "; } })();"
                    webView.evaluateJavascript(jsCss, null)

                    val jsBridge = assets.open("web/echo5_bridge.js").bufferedReader().use { it.readText() }
                    webView.evaluateJavascript("if (!window.__echo5BridgeInstalled) { $jsBridge }", null)
                } catch (e: Exception) {
                    Log.w(TAG, "Error injecting Echo 5 styles/bridge", e)
                }

                // Immediately configure Controller host in JS bridge (UI-17: sanitize host parameter)
                val activeHost = mcuBridge.currentMcuIp ?: preferences.mcuHost
                webView.evaluateJavascript(
                    "if (window.setMcuTarget) { window.setMcuTarget(${JSONObject.quote(activeHost)}); }",
                    null
                )

                // Notify Web UI of the active Controller host and device mode
                sendJsEvent("onEchoCompanionReady", JSONObject().apply {
                    put("mcuHost", activeHost)
                    put("scaleState", scaleManager.currentState)
                    put("scaleName", scaleManager.connectedDeviceName ?: "")
                    put("scaleMac", scaleManager.connectedDeviceMac ?: "")
                }.toString())
            }

            override fun onReceivedError(
                view: WebView?,
                request: WebResourceRequest?,
                error: WebResourceError?
            ) {
                super.onReceivedError(view, request, error)
                if (request?.isForMainFrame == true) {
                    remoteLoadTimeoutHandler.removeCallbacks(remoteLoadTimeoutRunnable)
                    Log.w(TAG, "Failed loading: ${request.url}, error: ${error?.description}")
                    // If remote load failed, fall back to bundled local UI
                    if (!preferences.useLocalUi) {
                        runOnUiThread {
                            preferences.useLocalUi = true
                            loadCockpitUi()
                        }
                    }
                }
            }
        }

        webView.webChromeClient = object : WebChromeClient() {
            override fun onConsoleMessage(consoleMessage: ConsoleMessage?): Boolean {
                Log.d("WebUI", "${consoleMessage?.message()} -- line ${consoleMessage?.lineNumber()} of ${consoleMessage?.sourceId()}")
                return true
            }
        }
    }

    fun loadCockpitUi() {
        val targetHost = mcuBridge.currentMcuIp ?: preferences.mcuHost
        remoteLoadTimeoutHandler.removeCallbacks(remoteLoadTimeoutRunnable)
        if (preferences.useLocalUi) {
            // UI-18: Serve local asset via WebViewAssetLoader (Option 1b: HTTP avoids mixed content blocks to local Controller)
            loadingLayout.visibility = View.GONE
            webView.loadUrl("http://appassets.androidplatform.net/assets/web/index.html")
        } else {
            // Direct load from Controller HTTP server
            loadingLayout.visibility = View.VISIBLE
            tvLoading.text = getString(R.string.connecting_to_mcu)
            // UI-30: 7s timeout fallback to bundled local UI if Controller does not respond
            remoteLoadTimeoutHandler.postDelayed(remoteLoadTimeoutRunnable, 7000)
            webView.loadUrl("http://$targetHost/")
        }
    }

    fun onMcuHostUpdated(newHost: String) {
        mcuBridge.setManualTarget(newHost)
        sendJsEvent("onMcuHostChanged", JSONObject().apply {
            put("host", newHost)
        }.toString())
        webView.evaluateJavascript(
            "if (window.setMcuTarget) { window.setMcuTarget(${JSONObject.quote(newHost)}); }",
            null
        )
        if (!preferences.useLocalUi) {
            loadCockpitUi()
        }
    }

    fun sendJsEvent(eventName: String, jsonArg: String) {
        val script = "if (window.$eventName) { window.$eventName($jsonArg); } else if (window.dispatchEvent) { window.dispatchEvent(new CustomEvent('$eventName', { detail: $jsonArg })); }"
        webView.evaluateJavascript(script, null)
    }

    // --- ScaleEventListener Implementations ---

    override fun onReading(reading: ScaleReading) {
        val json = JSONObject().apply {
            put("grams", reading.grams)
            put("flowGs", reading.flowGs)
            put("batteryPct", reading.batteryPct)
            put("stable", reading.stable)
            put("timestamp", reading.timestampMs)
        }.toString()

        sendJsEvent("onAndroidScaleReading", json)

        // Relay to Controller if configured
        if (preferences.relayWeightToMcu) {
            mcuBridge.relayWeight(preferences.mcuHost, reading.grams)
        }
    }

    override fun onStateChanged(state: String, deviceName: String?, mac: String?) {
        val json = JSONObject().apply {
            put("state", state)
            put("deviceName", deviceName ?: "")
            put("mac", mac ?: "")
        }.toString()

        sendJsEvent("onAndroidScaleStateChanged", json)
    }

    override fun onScaleDiscovered(scale: DiscoveredScale) {
        val json = JSONObject().apply {
            put("name", scale.name)
            put("address", scale.address)
            put("rssi", scale.rssi)
            put("linkType", scale.linkType)
        }.toString()

        sendJsEvent("onAndroidScaleDiscovered", json)
    }

    // --- McuDiscoveryListener Implementation ---

    override fun onMcuFound(host: String, port: Int) {
        val previousHost = preferences.mcuHost
        Log.i(TAG, "Controller discovered at $host:$port (previous: $previousHost)")
        preferences.mcuHost = host
        runOnUiThread {
            if (!preferences.useLocalUi) {
                val currentUrl = webView.url
                // UI-31: Avoid visible page reloads if already pointed to this host
                if (currentUrl == null || !currentUrl.startsWith("http://$host")) {
                    Log.i(TAG, "Navigating WebView to newly discovered Controller: http://$host/")
                    loadCockpitUi()
                }
            } else {
                // UI-17: Sanitize host parameter
                webView.evaluateJavascript(
                    "if (window.setMcuTarget) { window.setMcuTarget(${JSONObject.quote(host)}); }",
                    null
                )
            }
        }
        sendJsEvent("onMcuDiscovered", JSONObject().apply {
            put("host", host)
            put("port", port)
        }.toString())
    }

    override fun onMcuStatusChecked(online: Boolean, detailsJson: String?) {
        sendJsEvent("onMcuStatus", JSONObject().apply {
            put("online", online)
            put("host", preferences.mcuHost)
        }.toString())

        // The screen saver must never hide a running shot.
        if (::screenSaver.isInitialized) {
            screenSaver.setMachineBusy(controllerIsBusy(detailsJson))
        }
    }

    /**
     * True while the Controller reports that it is working.
     *
     * The Cockpit has to stay readable during a brew, so the screen saver is
     * held back until the machine is idle again.
     */
    private fun controllerIsBusy(detailsJson: String?): Boolean {
        if (detailsJson.isNullOrBlank()) return false
        return try {
            val state = JSONObject(detailsJson).optString("state", "IDLE").uppercase()
            state == "STARTING" || state == "BREWING" ||
                state == "FLUSHING" || state == "STOPPING"
        } catch (e: Exception) {
            Log.w(TAG, "Could not parse Controller status: ${e.message}")
            false
        }
    }

    override fun onMcuListUpdated(mcuListJson: String) {
        sendJsEvent("onMcuListDiscovered", mcuListJson)
    }

    // --- Permissions ---

    fun requestBlePermissionsThen(action: () -> Unit) {
        val permissions = mutableListOf<String>()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED) {
                permissions.add(Manifest.permission.BLUETOOTH_SCAN)
            }
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
                permissions.add(Manifest.permission.BLUETOOTH_CONNECT)
            }
        } else {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED) {
                permissions.add(Manifest.permission.ACCESS_FINE_LOCATION)
            }
        }

        if (permissions.isEmpty()) {
            action()
        } else {
            pendingPermissionCallback = action
            permissionLauncher.launch(permissions.toTypedArray())
        }
    }

    // UI-28: Pause / resume 1 Hz BLE scale heartbeat on activity state transitions
    override fun onPause() {
        super.onPause()
        scaleManager.pauseHeartbeat()
    }

    override fun onResume() {
        super.onResume()
        scaleManager.resumeHeartbeat()
    }

    // UI-29: Release WebView and native interfaces on destroy
    override fun onDestroy() {
        super.onDestroy()
        scaleManager.disconnect()
        mcuBridge.destroy()
        screenSaver.release()
        remoteLoadTimeoutHandler.removeCallbacksAndMessages(null)
        try {
            webView.removeJavascriptInterface("EchoCompanion")
            webView.removeJavascriptInterface("AndroidScale")
            webView.stopLoading()
            (webView.parent as? ViewGroup)?.removeView(webView)
            webView.destroy()
        } catch (e: Exception) {
            Log.w(TAG, "Error cleaning up WebView in onDestroy", e)
        }
    }
}
