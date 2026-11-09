package info.cemu.cemu.tests

import info.cemu.cemu.common.settings.ExternalDisplayPadScreen
import info.cemu.cemu.emulation.ScreenPlacement
import info.cemu.cemu.emulation.ScreenTarget
import info.cemu.cemu.emulation.placeScreens
import org.junit.Assert.assertEquals
import org.junit.Test

class ScreenPlacementTest {
    private val secondary = 2
    private val external = 5

    @Test
    fun `without other displays both images stay in the activity`() {
        assertEquals(
            ScreenPlacement(tv = ScreenTarget.Activity, pad = ScreenTarget.Activity),
            placeScreens(null, null, swapScreens = false, ExternalDisplayPadScreen.SECONDARY),
        )
    }

    @Test
    fun `second screen shows the pad`() {
        assertEquals(
            ScreenPlacement(tv = ScreenTarget.Activity, pad = ScreenTarget.Display(secondary)),
            placeScreens(secondary, null, swapScreens = false, ExternalDisplayPadScreen.SECONDARY),
        )
    }

    @Test
    fun `swapped screens show the tv on the second screen`() {
        assertEquals(
            ScreenPlacement(tv = ScreenTarget.Display(secondary), pad = ScreenTarget.Activity),
            placeScreens(secondary, null, swapScreens = true, ExternalDisplayPadScreen.SECONDARY),
        )
    }

    @Test
    fun `external display shows the tv and the pad goes to the chosen screen`() {
        assertEquals(
            ScreenPlacement(tv = ScreenTarget.Display(external), pad = ScreenTarget.Display(secondary)),
            placeScreens(secondary, external, swapScreens = false, ExternalDisplayPadScreen.SECONDARY),
        )
        assertEquals(
            ScreenPlacement(tv = ScreenTarget.Display(external), pad = ScreenTarget.Activity),
            placeScreens(secondary, external, swapScreens = true, ExternalDisplayPadScreen.PRIMARY),
        )
    }

    @Test
    fun `external display without second screen keeps the pad in the activity`() {
        assertEquals(
            ScreenPlacement(tv = ScreenTarget.Display(external), pad = ScreenTarget.Activity),
            placeScreens(null, external, swapScreens = false, ExternalDisplayPadScreen.SECONDARY),
        )
    }
}
