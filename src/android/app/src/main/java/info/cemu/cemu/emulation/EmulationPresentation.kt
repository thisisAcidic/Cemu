package info.cemu.cemu.emulation

import android.annotation.SuppressLint
import android.app.Presentation
import android.content.Context
import android.graphics.Color
import android.os.Bundle
import android.view.Display
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager

class EmulationPresentation(
    context: Context,
    display: Display,
    private val isTV: Boolean,
    private val holderCallback: SurfaceHolder.Callback?,
    private val onSurfaceChanged: () -> Unit,
) : Presentation(context, display) {
    @SuppressLint("ClickableViewAccessibility")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // controller input has to keep going to the activity
        window?.addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE)

        if (holderCallback == null) {
            setContentView(View(context).apply { setBackgroundColor(Color.BLACK) })
            return
        }
        setContentView(SurfaceView(context).apply {
            setOnTouchListener(CanvasOnTouchListener(isTV))
            holder.addCallback(holderCallback)
            holder.addCallback(object : SurfaceHolder.Callback {
                override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                    onSurfaceChanged()
                }

                override fun surfaceCreated(holder: SurfaceHolder) {}

                override fun surfaceDestroyed(holder: SurfaceHolder) {}
            })
        })
    }
}
