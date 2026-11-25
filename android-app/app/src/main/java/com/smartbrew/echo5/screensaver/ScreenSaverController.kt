package com.smartbrew.echo5.screensaver

import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.View

/**
 * Drives the [ScreensaverView].
 *
 * The rules are the same as on the Waveshare display:
 *
 *  * after [idleMs] without any interaction the screen saver takes over;
 *  * a single touch brings the cockpit back exactly as it was;
 *  * while the machine is brewing the cockpit has to stay readable, so the
 *    screen saver is switched off and the idle timer is held back.
 */
class ScreenSaverController(
    private val saverView: ScreensaverView,
    private val idleMs: Long = DEFAULT_IDLE_MS
) {

    companion object {
        /** Ten minutes - long enough that nobody trips over it by accident. */
        const val DEFAULT_IDLE_MS = 10L * 60L * 1000L
    }

    private val handler = Handler(Looper.getMainLooper())
    private var lastInteractionMs = SystemClock.uptimeMillis()
    private var machineBusy = false
    private var active = false

    /**
     * True while a finger is on the screen.
     *
     * The screen saver must not take over in the middle of a gesture,
     * otherwise the WebView underneath would never receive the release of a
     * touch that started before the screen saver appeared.
     */
    var touchInProgress: Boolean = false

    private val startSaver = object : Runnable {
        override fun run() {
            if (machineBusy) return
            if (touchInProgress) {
                // Wait for the finger to lift, then try again.
                handler.postDelayed(this, 1000)
                return
            }
            if (SystemClock.uptimeMillis() - lastInteractionMs >= idleMs) {
                show()
            }
        }
    }

    init {
        // Safety net for the case where a touch reaches the overlay itself.
        saverView.onResumeRequested = { notifyUserInteraction() }
        saverView.visibility = View.GONE
        // Start the clock, so the screen saver also appears when nobody ever
        // touches the display.
        notifyUserInteraction()
    }

    /** True while the screen saver is on screen. */
    fun isActive(): Boolean = active

    /**
     * The user touched the screen.  Restart the idle timer and, if the screen
     * saver is up, take it down again.
     */
    fun notifyUserInteraction() {
        lastInteractionMs = SystemClock.uptimeMillis()
        handler.removeCallbacks(startSaver)
        if (machineBusy) return
        if (active) {
            dismiss()
        }
        // Always re-arm: after the screen saver was dismissed the user has to
        // be able to walk away again and have it come back.
        handler.postDelayed(startSaver, idleMs)
    }

    /**
     * Tell the controller whether the machine is currently working.  A brew
     * always wins over the screen saver.
     */
    fun setMachineBusy(busy: Boolean) {
        if (machineBusy == busy) return
        machineBusy = busy
        if (busy) {
            handler.removeCallbacks(startSaver)
            if (active) dismiss()
        } else {
            // The machine went back to idle - give the user the full idle
            // window again before the screen saver returns.
            notifyUserInteraction()
        }
    }

    /** Release the pending callbacks. */
    fun release() {
        handler.removeCallbacks(startSaver)
        saverView.onResumeRequested = null
    }

    private fun show() {
        if (active) return
        active = true
        saverView.visibility = View.VISIBLE
    }

    private fun dismiss() {
        if (!active) return
        active = false
        saverView.visibility = View.GONE
    }
}
