package info.cemu.cemu.common.android.display

import android.content.Context
import android.hardware.display.DisplayManager
import android.os.Handler
import android.os.Looper
import android.view.Display
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.flow.distinctUntilChanged

// Display.getType() is hidden, built-in panels report TYPE_INTERNAL (1)
private const val DISPLAY_TYPE_INTERNAL = 1

private fun Display.isBuiltIn(): Boolean = try {
    Display::class.java.getMethod("getType").invoke(this) == DISPLAY_TYPE_INTERNAL
} catch (_: ReflectiveOperationException) {
    false
}

data class SecondaryDisplays(val builtInDisplayId: Int? = null, val externalDisplayId: Int? = null)

fun DisplayManager.getSecondaryDisplays(excludedDisplayId: Int): SecondaryDisplays {
    val displays = getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)
        .filter { it.displayId != excludedDisplayId }
    return SecondaryDisplays(
        builtInDisplayId = displays.firstOrNull { it.isBuiltIn() }?.displayId,
        externalDisplayId = displays.firstOrNull { !it.isBuiltIn() }?.displayId,
    )
}

fun Context.secondaryDisplaysFlow(excludedDisplayId: Int): Flow<SecondaryDisplays> = callbackFlow {
    val displayManager = getSystemService(DisplayManager::class.java)
    val listener = object : DisplayManager.DisplayListener {
        override fun onDisplayAdded(displayId: Int) {
            trySend(displayManager.getSecondaryDisplays(excludedDisplayId))
        }

        override fun onDisplayRemoved(displayId: Int) {
            trySend(displayManager.getSecondaryDisplays(excludedDisplayId))
        }

        override fun onDisplayChanged(displayId: Int) {
            trySend(displayManager.getSecondaryDisplays(excludedDisplayId))
        }
    }
    displayManager.registerDisplayListener(listener, Handler(Looper.getMainLooper()))
    trySend(displayManager.getSecondaryDisplays(excludedDisplayId))
    awaitClose { displayManager.unregisterDisplayListener(listener) }
}.distinctUntilChanged()
