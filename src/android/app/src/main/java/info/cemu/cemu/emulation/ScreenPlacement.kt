package info.cemu.cemu.emulation

import info.cemu.cemu.common.settings.ExternalDisplayPadScreen

sealed interface ScreenTarget {
    data object Activity : ScreenTarget
    data class Display(val displayId: Int) : ScreenTarget
}

data class ScreenPlacement(val tv: ScreenTarget, val pad: ScreenTarget)

fun placeScreens(
    secondaryDisplayId: Int?,
    externalDisplayId: Int?,
    swapScreens: Boolean,
    externalDisplayPadScreen: ExternalDisplayPadScreen,
): ScreenPlacement {
    if (externalDisplayId != null) {
        val pad = if (secondaryDisplayId != null && externalDisplayPadScreen == ExternalDisplayPadScreen.SECONDARY) {
            ScreenTarget.Display(secondaryDisplayId)
        } else {
            ScreenTarget.Activity
        }
        return ScreenPlacement(tv = ScreenTarget.Display(externalDisplayId), pad = pad)
    }

    if (secondaryDisplayId != null) {
        return if (swapScreens) {
            ScreenPlacement(tv = ScreenTarget.Display(secondaryDisplayId), pad = ScreenTarget.Activity)
        } else {
            ScreenPlacement(tv = ScreenTarget.Activity, pad = ScreenTarget.Display(secondaryDisplayId))
        }
    }

    return ScreenPlacement(tv = ScreenTarget.Activity, pad = ScreenTarget.Activity)
}
