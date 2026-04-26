package me.phie.tawc

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.view.Gravity
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import me.phie.tawc.compositor.CompositorActivity

/**
 * Home screen for the tawc app. Shows a simple Android UI from which the
 * user can launch the Wayland compositor (and, eventually, manage the
 * chroot and other features). The compositor lives in a separate
 * [CompositorActivity] so its full-screen Wayland surface is isolated
 * from the rest of the app's UI.
 */
class MainActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val padding = (24 * resources.displayMetrics.density).toInt()

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setPadding(padding, padding, padding, padding)
            layoutParams = LinearLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT)
        }

        val title = TextView(this).apply {
            text = "tawc"
            textSize = 32f
            gravity = Gravity.CENTER
        }

        val openCompositorBtn = Button(this).apply {
            text = "Open compositor"
            setOnClickListener {
                startActivity(Intent(this@MainActivity, CompositorActivity::class.java))
            }
        }

        root.addView(title, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).apply {
            bottomMargin = padding
        })
        root.addView(openCompositorBtn, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT))

        setContentView(root)
    }
}
