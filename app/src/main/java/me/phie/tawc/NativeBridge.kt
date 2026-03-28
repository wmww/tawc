package me.phie.tawc

import android.view.Surface

object NativeBridge {
    init {
        System.loadLibrary("smithay_android")
    }

    /** Start the compositor render loop. Called once with the initial surface. */
    external fun nativeOnSurfaceCreated(surface: Surface)

    /** Notify the compositor that the surface dimensions changed. */
    external fun nativeOnSurfaceChanged(width: Int, height: Int)

    /** Notify the compositor that the surface is being destroyed. */
    external fun nativeOnSurfaceDestroyed()
}
