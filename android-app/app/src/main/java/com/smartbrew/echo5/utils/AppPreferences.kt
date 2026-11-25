package com.smartbrew.echo5.utils

import android.content.Context
import android.content.SharedPreferences

class AppPreferences(context: Context) {
    companion object {
        private const val PREFS_NAME = "smartbrew_echo5_prefs"
        private const val KEY_SCHEMA_VERSION = "pref_schema_version"
        private const val CURRENT_SCHEMA_VERSION = 2
        private const val KEY_MCU_HOST = "mcu_host"
        private const val KEY_SCALE_MAC = "scale_mac"
        private const val KEY_SCALE_NAME = "scale_name"
        private const val KEY_AUTO_CONNECT_SCALE = "auto_connect_scale"
        private const val KEY_RELAY_WEIGHT = "relay_weight_to_mcu"
        private const val KEY_USE_LOCAL_UI = "use_local_ui"
        private const val KEY_AUTO_STOP = "auto_stop_yield"
    }

    private val prefs: SharedPreferences =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    init {
        val ver = prefs.getInt(KEY_SCHEMA_VERSION, 1)
        if (ver < CURRENT_SCHEMA_VERSION) {
            val editor = prefs.edit()
            val storedHost = prefs.getString(KEY_MCU_HOST, null)
            if (storedHost != null && storedHost.startsWith("192.168.40.")) {
                editor.putString(KEY_MCU_HOST, "smartbrew.local")
            }
            editor.putBoolean(KEY_USE_LOCAL_UI, true)
            editor.putInt(KEY_SCHEMA_VERSION, CURRENT_SCHEMA_VERSION)
            editor.apply()
        }
    }

    var mcuHost: String
        get() = prefs.getString(KEY_MCU_HOST, "smartbrew.local") ?: "smartbrew.local"
        set(value) = prefs.edit().putString(KEY_MCU_HOST, value.trim()).apply()

    var pairedScaleMac: String?
        get() = prefs.getString(KEY_SCALE_MAC, null)
        set(value) = prefs.edit().putString(KEY_SCALE_MAC, value).apply()

    var pairedScaleName: String?
        get() = prefs.getString(KEY_SCALE_NAME, null)
        set(value) = prefs.edit().putString(KEY_SCALE_NAME, value).apply()

    var autoConnectScale: Boolean
        get() = prefs.getBoolean(KEY_AUTO_CONNECT_SCALE, false)
        set(value) = prefs.edit().putBoolean(KEY_AUTO_CONNECT_SCALE, value).apply()

    var relayWeightToMcu: Boolean
        get() = prefs.getBoolean(KEY_RELAY_WEIGHT, false)
        set(value) = prefs.edit().putBoolean(KEY_RELAY_WEIGHT, value).apply()

    var useLocalUi: Boolean
        get() = prefs.getBoolean(KEY_USE_LOCAL_UI, true)
        set(value) = prefs.edit().putBoolean(KEY_USE_LOCAL_UI, value).apply()

    var autoStopOnYield: Boolean
        get() = prefs.getBoolean(KEY_AUTO_STOP, true)
        set(value) = prefs.edit().putBoolean(KEY_AUTO_STOP, value).apply()
}
