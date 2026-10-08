package me.phie.tawc.compositor

import android.content.Context
import android.hardware.input.InputManager
import android.os.Handler
import android.os.Looper
import android.view.InputDevice

/**
 * Tracks whether Android has any mouse-class `InputDevice` attached and
 * pushes the aggregate into the compositor.
 *
 * That boolean is one of the two reasons the Wayland seat advertises
 * `wl_pointer` (the other is pointer emulation); the
 * compositor owns the capability itself. See notes/input.md.
 *
 * Touchpads that drive a cursor report `SOURCE_MOUSE` as well, which is what
 * we want — they produce pointer events, not touch.
 */
class MouseWatcher(context: Context) : InputManager.InputDeviceListener {
    private val inputManager = context.getSystemService(InputManager::class.java)
    private val handler = Handler(Looper.getMainLooper())
    private var lastReported: Boolean? = null

    fun start() {
        val im = inputManager ?: return
        im.registerInputDeviceListener(this, handler)
        publish()
    }

    fun stop() {
        inputManager?.unregisterInputDeviceListener(this)
        lastReported = null
    }

    override fun onInputDeviceAdded(deviceId: Int) = publish()

    override fun onInputDeviceRemoved(deviceId: Int) = publish()

    override fun onInputDeviceChanged(deviceId: Int) = publish()

    private fun publish() {
        val attached = anyMouseAttached()
        if (attached == lastReported) return
        lastReported = attached
        NativeBridge.nativeOnMouseAttachedChanged(attached)
    }

    private fun anyMouseAttached(): Boolean {
        val im = inputManager ?: return false
        return im.inputDeviceIds.any { id ->
            val device = im.getInputDevice(id) ?: return@any false
            !device.isVirtual && device.supportsSource(InputDevice.SOURCE_MOUSE)
        }
    }
}
