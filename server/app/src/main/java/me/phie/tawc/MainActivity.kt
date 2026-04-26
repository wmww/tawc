package me.phie.tawc

import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Bundle
import android.util.Log
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import java.io.File

class MainActivity : Activity(), SurfaceHolder.Callback {
    private lateinit var surfaceView: SurfaceView

    /**
     * BroadcastReceiver for injecting text input from tests.
     * Usage: adb shell am broadcast -a me.phie.tawc.TEXT_INPUT --es text "hello"
     * Usage: adb shell am broadcast -a me.phie.tawc.KEY_EVENT --ei keycode 67
     *
     * Events go through TawcInputConnection (via InputMethodManager) so they
     * exercise the same code path as real Gboard input, including the
     * BaseInputConnection Editable updates and all TawcInputConnection logic.
     */
    private val testInputReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val ic = surfaceView.onCreateInputConnection(EditorInfo()) ?: return
            when (intent.action) {
                "me.phie.tawc.TEXT_INPUT" -> {
                    val text = intent.getStringExtra("text") ?: return
                    Log.d("tawc", "TestInput: commitText \"$text\"")
                    ic.commitText(text, 1)
                }
                "me.phie.tawc.KEY_EVENT" -> {
                    val keycode = intent.getIntExtra("keycode", -1)
                    if (keycode >= 0) {
                        Log.d("tawc", "TestInput: sendKeyEvent $keycode")
                        ic.sendKeyEvent(android.view.KeyEvent(android.view.KeyEvent.ACTION_DOWN, keycode))
                    }
                }
                "me.phie.tawc.QUERY_STATE" -> {
                    NativeBridge.nativeQueryState()
                }
            }
        }
    }

    @Suppress("ClickableViewAccessibility")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        extractXkbData()
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility = (
            View.SYSTEM_UI_FLAG_FULLSCREEN or
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
            View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
            View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        )
        window.addFlags(WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS)

        surfaceView = TawcSurfaceView(this)
        setContentView(surfaceView)
        surfaceView.holder.addCallback(this)
        surfaceView.isFocusable = true
        surfaceView.isFocusableInTouchMode = true
        surfaceView.setOnTouchListener { _, event -> dispatchTouchToCompositor(event) }
        NativeBridge.inputView = surfaceView

        // Register test input receiver
        val filter = IntentFilter().apply {
            addAction("me.phie.tawc.TEXT_INPUT")
            addAction("me.phie.tawc.KEY_EVENT")
            addAction("me.phie.tawc.QUERY_STATE")
        }
        @Suppress("UnspecifiedRegisterReceiverFlag")
        registerReceiver(testInputReceiver, filter, RECEIVER_EXPORTED)
    }

    override fun onDestroy() {
        super.onDestroy()
        unregisterReceiver(testInputReceiver)
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        NativeBridge.nativeOnSurfaceCreated(holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        NativeBridge.nativeOnSurfaceChanged(width, height)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        NativeBridge.nativeOnSurfaceDestroyed()
    }

    /**
     * Custom SurfaceView that provides our InputConnection to the IME.
     * This makes the view act as a text input target for Gboard.
     */
    private class TawcSurfaceView(context: Context) : SurfaceView(context) {
        override fun onCheckIsTextEditor(): Boolean = true

        override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
            outAttrs.inputType = EditorInfo.TYPE_CLASS_TEXT
            outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN or EditorInfo.IME_ACTION_NONE
            return TawcInputConnection(this)
        }
    }

    private fun extractXkbData() {
        val destDir = File(filesDir, "xkb")
        val versionFile = File(destDir, ".version")
        val currentVersion = try {
            packageManager.getPackageInfo(packageName, 0).longVersionCode
        } catch (_: PackageManager.NameNotFoundException) { 0L }

        if (versionFile.exists() && versionFile.readText().trim() == currentVersion.toString()) return

        destDir.deleteRecursively()
        fun extractDir(assetPath: String, destPath: File) {
            val children = assets.list(assetPath) ?: return
            if (children.isEmpty()) {
                assets.open(assetPath).use { input ->
                    destPath.outputStream().use { output -> input.copyTo(output) }
                }
            } else {
                destPath.mkdirs()
                for (child in children) {
                    extractDir("$assetPath/$child", File(destPath, child))
                }
            }
        }
        extractDir("xkb", destDir)
        versionFile.writeText(currentVersion.toString())
        Log.d("tawc", "Extracted xkb data to $destDir")
    }

    private fun dispatchTouchToCompositor(event: MotionEvent): Boolean {
        val actionMasked = event.actionMasked
        when (actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val idx = event.actionIndex
                NativeBridge.nativeOnTouchEvent(
                    actionMasked, event.getPointerId(idx),
                    event.getX(idx), event.getY(idx), event.eventTime
                )
            }
            MotionEvent.ACTION_MOVE -> {
                for (i in 0 until event.pointerCount) {
                    NativeBridge.nativeOnTouchEvent(
                        actionMasked, event.getPointerId(i),
                        event.getX(i), event.getY(i), event.eventTime
                    )
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                val idx = event.actionIndex
                NativeBridge.nativeOnTouchEvent(
                    actionMasked, event.getPointerId(idx),
                    event.getX(idx), event.getY(idx), event.eventTime
                )
            }
            MotionEvent.ACTION_CANCEL -> {
                for (i in 0 until event.pointerCount) {
                    NativeBridge.nativeOnTouchEvent(
                        MotionEvent.ACTION_UP, event.getPointerId(i),
                        event.getX(i), event.getY(i), event.eventTime
                    )
                }
            }
        }
        return true
    }
}
