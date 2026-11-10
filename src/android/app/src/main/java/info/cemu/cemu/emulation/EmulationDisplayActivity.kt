package info.cemu.cemu.emulation

import android.annotation.SuppressLint
import android.app.Activity
import android.app.ActivityOptions
import android.content.Intent
import android.os.Bundle
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowInsets
import java.lang.ref.WeakReference

/**
 * Shows the TV or GamePad image on another display.
 *
 * A presentation would only cover the activity below it, which then keeps running and drawing (for example a
 * launcher on the second screen of a dual screen handheld). An activity on that display stops it instead. Devices
 * that don't allow activities on other displays still get an EmulationPresentation.
 */
class EmulationDisplayActivity : Activity() {
    // the activity can be moved to another display when its display goes away
    private var displayId = 0

    @SuppressLint("ClickableViewAccessibility")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        displayId = display.displayId
        val content = EmulationDisplays.attach(displayId, this)
        if (content == null) {
            finish()
            return
        }

        window.setWindowAnimations(0)

        setContentView(SurfaceView(this).apply {
            setOnTouchListener(CanvasOnTouchListener(content.isTV))
            holder.addCallback(content.holderCallback)
            holder.addCallback(object : SurfaceHolder.Callback {
                override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                    content.onSurfaceChanged()
                }

                override fun surfaceCreated(holder: SurfaceHolder) {}

                override fun surfaceDestroyed(holder: SurfaceHolder) {}
            })
        })
        window.insetsController?.hide(WindowInsets.Type.systemBars())
    }

    override fun onDestroy() {
        super.onDestroy()
        EmulationDisplays.detach(displayId, this)
    }

    // Starting this activity or touching its display moves the input focus here. A window that can't take the focus
    // doesn't help, Android then holds back controller input until it times out. So hand it to the emulation instead.
    override fun dispatchKeyEvent(event: KeyEvent): Boolean =
        EmulationDisplays.inputActivity?.dispatchKeyEvent(event) ?: super.dispatchKeyEvent(event)

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean =
        EmulationDisplays.inputActivity?.dispatchGenericMotionEvent(event) ?: super.dispatchGenericMotionEvent(event)
}

/**
 * What the display activities show. The emulation screen registers the content of a display and starts the activity
 * there, both run in the emulation process. Only used from the main thread.
 */
object EmulationDisplays {
    class Content(
        val isTV: Boolean,
        val holderCallback: SurfaceHolder.Callback,
        val onSurfaceChanged: () -> Unit,
    )

    private val contents = mutableMapOf<Int, Content>()
    private val activities = mutableMapOf<Int, EmulationDisplayActivity>()
    private var inputActivityRef: WeakReference<Activity>? = null

    // the activity that handles input while one of the display activities has the focus
    val inputActivity: Activity?
        get() = inputActivityRef?.get()

    fun show(activity: Activity, displayId: Int, content: Content) {
        inputActivityRef = WeakReference(activity)
        contents[displayId] = content
        val intent = Intent(activity, EmulationDisplayActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_MULTIPLE_TASK or Intent.FLAG_ACTIVITY_NO_ANIMATION)
        val options = ActivityOptions.makeBasic().setLaunchDisplayId(displayId)
        try {
            activity.startActivity(intent, options.toBundle())
        } catch (_: SecurityException) {
            // the display went away or doesn't allow activities from other apps
            contents.remove(displayId)
        }
    }

    fun hide(displayId: Int) {
        contents.remove(displayId)
        activities.remove(displayId)?.finish()
    }

    fun attach(displayId: Int, activity: EmulationDisplayActivity): Content? {
        val content = contents[displayId] ?: return null
        // the display was shown again before the previous activity got there
        activities.put(displayId, activity)?.takeIf { it !== activity }?.finish()
        return content
    }

    fun detach(displayId: Int, activity: EmulationDisplayActivity) {
        if (activities[displayId] === activity) {
            activities.remove(displayId)
        }
    }
}
