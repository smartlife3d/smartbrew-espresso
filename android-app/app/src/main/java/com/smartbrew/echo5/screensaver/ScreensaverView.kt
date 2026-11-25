package com.smartbrew.echo5.screensaver

import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.RadialGradient
import android.graphics.Shader
import android.os.SystemClock
import android.util.AttributeSet
import android.util.TypedValue
import android.view.MotionEvent
import android.view.View
import com.smartbrew.echo5.R
import kotlin.math.cos
import kotlin.math.min
import kotlin.math.sin

/**
 * Screen saver for the Android (Echo Show 5) cockpit.
 *
 * The view is an overlay on top of the WebView.  It draws a slowly floating
 * espresso cup on the OLED-black cockpit background and swallows every touch,
 * so the tap that wakes the display up never reaches the web UI underneath.
 *
 * The art is the very same drawing that the Waveshare display uses - see
 * `tools/gen_screensaver_art.py`, which emits `espresso_cup.png` and
 * `espresso_steam.png` for this view and the 8-bit alpha masks for LVGL.
 */
class ScreensaverView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {

    companion object {
        // House palette, mirrored from data/index.html.
        // Color.rgb() on purpose: a raw hex literal such as 0x07090E has an
        // alpha byte of 0x00 and would make the colour fully transparent.
        private val BG_TOP = Color.rgb(0x07, 0x09, 0x0E)
        private val BG_BOTTOM = Color.rgb(0x2E, 0x14, 0x09)
        private val CREMA = Color.rgb(0xF5, 0x9E, 0x0B)
        private val HINT = Color.rgb(0x8A, 0x5A, 0x2B)

        // The cup drifts on two sine paths of different length, so it traces a
        // slow Lissajous figure instead of a straight line.
        private const val DRIFT_X_MS = 17_000.0
        private const val DRIFT_Y_MS = 11_000.0

        // Steam curl.
        private const val STEAM_RISE_MS = 3_600.0
        private const val STEAM_WISPS = 2

        // Design space of the generated art is 96 px wide; the steam curl is
        // 40 x 56 px inside it.  The offsets place the curl just above the rim
        // of the cup - it only drifts a little further up while it fades out,
        // so the vapour always stays at the top of the cup.
        private const val DESIGN_W = 96f
        private const val STEAM_W_DESIGN = 40f
        private const val STEAM_H_DESIGN = 56f
        private const val STEAM_X_BASE = 23f
        private const val STEAM_X_SWAY = 27f
        private const val STEAM_Y_START = -16f
        private const val STEAM_Y_END = -36f
        private const val STEAM_ALPHA = 105f
    }

    /** Invoked when the user touches the display. */
    var onResumeRequested: (() -> Unit)? = null

    private val cupBitmap: Bitmap? =
        BitmapFactory.decodeResource(resources, R.drawable.espresso_cup)
    private val steamBitmap: Bitmap? =
        BitmapFactory.decodeResource(resources, R.drawable.espresso_steam)

    private val bgPaint = Paint()
    private val glowPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val cupPaint = Paint(Paint.FILTER_BITMAP_FLAG or Paint.DITHER_FLAG)
    private val steamPaint = Paint(Paint.FILTER_BITMAP_FLAG or Paint.DITHER_FLAG)
    private val hintPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = HINT
        alpha = 190
        textAlign = Paint.Align.CENTER
        letterSpacing = 0.18f
    }

    private var startedAtMs = SystemClock.uptimeMillis()
    private var cupSize = 0f

    // Only promise a touch gesture on hardware that actually has a screen to
    // touch - an Echo Show 5 does not have one.
    private val hintText: String =
        if (context.packageManager.hasSystemFeature(PackageManager.FEATURE_TOUCHSCREEN)) {
            "TOUCH TO RESUME"
        } else {
            "SMARTBREW"
        }

    init {
        isClickable = true
        isFocusable = true
        hintPaint.textSize = TypedValue.applyDimension(
            TypedValue.COMPLEX_UNIT_SP, 12f, resources.displayMetrics
        )
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        if (w <= 0 || h <= 0) return

        bgPaint.shader = LinearGradient(
            0f, 0f, 0f, h.toFloat(),
            BG_TOP, BG_BOTTOM, Shader.TileMode.CLAMP
        )

        cupSize = min(w, h) * 0.42f
        // The halo is centred on the cup and drawn with a translated canvas so
        // the shader does not have to be rebuilt on every frame.
        glowPaint.shader = RadialGradient(
            0f, 0f, cupSize * 0.667f,
            intArrayOf(
                Color.argb(150, Color.red(CREMA), Color.green(CREMA), Color.blue(CREMA)),
                Color.argb(0, Color.red(CREMA), Color.green(CREMA), Color.blue(CREMA))
            ),
            floatArrayOf(0f, 1f),
            Shader.TileMode.CLAMP
        )
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        startedAtMs = SystemClock.uptimeMillis()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f || h <= 0f || cupSize <= 0f) return

        canvas.drawRect(0f, 0f, w, h, bgPaint)

        val elapsed = (SystemClock.uptimeMillis() - startedAtMs).toDouble()

        // --- Where is the cup right now? -----------------------------------------------------------
        val xMin = cupSize * 0.35f
        val xMax = w - cupSize * 1.35f
        val yMin = cupSize * 0.45f
        val yMax = h - cupSize * 1.45f
        val cupLeft = xMin + (xMax - xMin) * (sine01(elapsed / DRIFT_X_MS)).toFloat()
        val cupTop = yMin + (yMax - yMin) * (sine01(elapsed / DRIFT_Y_MS)).toFloat()

        // --- Warm halo behind the cup --------------------------------------------------------------
        val glowCx = cupLeft + cupSize * 0.5f
        val glowCy = cupTop + cupSize * 0.5f
        canvas.save()
        canvas.translate(glowCx, glowCy)
        canvas.drawCircle(0f, 0f, cupSize * 0.667f, glowPaint)
        canvas.restore()

        // --- Steam ----------------------------------------------------------------------------------
        val steam = steamBitmap
        if (steam != null) {
            val scale = cupSize / DESIGN_W
            // The bitmap is rendered at 4x the design size so it stays crisp,
            // but it is *drawn* at the design size - 40 x 56 units of the cup's
            // own box.  Scaling by the bitmap size instead would make the curl
            // four times too big and push it off to the side of the cup.
            val steamW = STEAM_W_DESIGN * scale
            val steamH = STEAM_H_DESIGN * scale
            for (i in 0 until STEAM_WISPS) {
                val phase = i * (STEAM_RISE_MS / STEAM_WISPS)
                val cycle = (((elapsed + phase) % STEAM_RISE_MS) / STEAM_RISE_MS).toFloat()
                val alpha = sin(Math.PI * cycle.toDouble()).toFloat().coerceAtLeast(0f)
                if (alpha <= 0.02f) continue

                val rise = (STEAM_Y_START + (STEAM_Y_END - STEAM_Y_START) * cycle) * scale
                val sway = (STEAM_X_BASE + (STEAM_X_SWAY - STEAM_X_BASE) *
                    sine01((elapsed + phase) / (STEAM_RISE_MS / 2.0)).toFloat()) * scale

                steamPaint.alpha = (alpha * STEAM_ALPHA).toInt().coerceIn(0, 255)
                canvas.drawBitmap(
                    steam,
                    null,
                    android.graphics.RectF(
                        cupLeft + sway,
                        cupTop + rise,
                        cupLeft + sway + steamW,
                        cupTop + rise + steamH
                    ),
                    steamPaint
                )
            }
        }

        // --- The cup --------------------------------------------------------------------------------
        cupBitmap?.let { cup ->
            canvas.drawBitmap(
                cup,
                null,
                android.graphics.RectF(
                    cupLeft, cupTop,
                    cupLeft + cupSize, cupTop + cupSize
                ),
                cupPaint
            )
        }

        // --- Hint -----------------------------------------------------------------------------------
        canvas.drawText(hintText, w / 2f, h - hintPaint.textSize * 1.6f, hintPaint)

        postInvalidateOnAnimation()
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (event.actionMasked == MotionEvent.ACTION_DOWN) {
            performClick()
            onResumeRequested?.invoke()
        }
        return true
    }

    override fun performClick(): Boolean {
        super.performClick()
        return true
    }

    /** 0 -> 1 -> 0 over one period; the same curve the LVGL side uses. */
    private fun sine01(phase: Double): Double = 0.5 - 0.5 * cos(2.0 * Math.PI * phase)
}
