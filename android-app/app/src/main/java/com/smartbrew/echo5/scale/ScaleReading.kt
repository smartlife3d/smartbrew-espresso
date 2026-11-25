package com.smartbrew.echo5.scale

data class ScaleReading(
    val grams: Float = 0.0f,
    val flowGs: Float = 0.0f,
    val batteryPct: Int = -1,
    val stable: Boolean = true,
    val timestampMs: Long = System.currentTimeMillis()
)
