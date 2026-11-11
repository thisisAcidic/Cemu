# Changelog

## Cidemu 0.4

- Fixed Need for Speed: Most Wanted U crashing right after the start in about half of the launches. The emulated Wii U memory was only aligned to the 4 KB pages of Android, so memory that a game requested with a larger alignment could come back misaligned, depending on where Android placed the emulated memory. The game then built its memory pools with a null pointer. The emulated memory is now aligned to 4 GB. The Cemu Android port has the same problem.
- Vulkan: Pipelines are stored in the pipeline cache together with the attachment formats of their render pass. A pipeline used in a render pass that Cidemu kept open for more draws was precompiled for the wrong render pass at the next start. That only mattered when the driver's own cache was empty, for example after a driver update, but then those pipelines were compiled again during play.
- The crash log contains the CPU registers at the time of the crash, which makes crashes in or near recompiled code easier to locate.
- Games start like on the console: the screens stay black, then show the game's boot images on the TV and GamePad screens with its boot sound until the game turns on its own output. The loading dialog, the app icon, the loading texts and the account and controller notice are gone. A thin bar at the bottom only appears if loading takes longer than three seconds, for example after a driver update. The boot sound didn't play on Android before, it can be turned off in *Settings > Audio settings*. Existing settings get the new defaults for the boot sound and the notice once.
- Graphics: *Anti-aliasing (FXAA)* in *Settings > Graphics settings* smooths jagged edges of the TV image. It leaves textures sharp and adds about 5 % GPU work in LEGO City Undercover.
- Second screen: The Home key brings both screens to the home screen, also after returning to the game from the launcher. Before, the GamePad screen kept the input focus after it was shown, so Home only reached that screen while the game kept running on the other one.

## Cidemu 0.3

- The app id is now `dev.cidix.cidemu`, after the cidix.dev domain. Cidemu 0.3 installs as a new app next to 0.2, copy the data folder over once (see the README) and uninstall 0.2 afterwards.
- Cocoon: The release comes with `NintendoWiiU-Cidemu.json`, a platform file that Cocoon imports to start Wii U games in Cidemu. The README also explains ES-DE and other frontends.
- The About screen links to the renamed repository, github.com/thisisAcidic/Cidemu.

## Cidemu 0.2

The first release under the Cidemu name. It installs as a new app (`com.cidix.cidemu`) next to Cemu and next to the 0.1 test build, so data has to be copied over once (see the README).

- New name and icon. The About screen links to the source code, credits Cemu and the Cemu Android port and shows the licenses of the native libraries.
- The game list tells you where to add your game folder while none is set.
- Vulkan: Render target clears happen inside the next render pass, which saves a large part of the GPU work on mobile GPUs.
- Vulkan: Consecutive draws to compatible framebuffers stay in one render pass.
- Vulkan: The display hardware scales the image up to the screen.
- Vulkan: Command buffers are reset on a separate thread, and acquired swapchain images are waited on in the next submit.
- Vulkan: With the GamePad on a second screen, each screen only waits for its own previous frame. This brings dual screen play back to a full 60 FPS where it lost frames before.
- Latte: Decoded index data is kept across frames, and the draw call tick is updated once per frame.
- LEGO City Undercover: The default game profile turns off the accurate shader multiplication, which saves about 0.3 to 0.4 W on the AYN Thor without visible differences in the tested scenes. The first start afterwards compiles the shaders again.
- Idle emulated cores and the GPU thread sleep instead of spinning, so the CPU can clock down.
- Controllers are polled every 8 ms instead of every millisecond, and the process of the game list doesn't poll them at all while a game runs. Games read the controller state directly, so input latency stays the same.
- Recompiler: Shorter AArch64 code for memory and logical instructions, branches on condition flags, the remaining cycles kept in a register, native calls and returns for guest functions, and recompiled functions packed into shared executable memory.
- Second screens: The TV and GamePad images can be shown on secondary displays, with an option to swap them. They cover the second screen with their own activity, so the launcher behind them stops drawing instead of running along (on the AYN Thor, Cocoon Shell drew about 21 frames per second behind the GamePad image).
- GamePad: The GamePad image is updated at a lower rate when it doesn't need the full one (Settings > General settings > GamePad screen frame rate, default automatic).
- Fixed the scissor after drawing the GamePad image and after tracking GamePad image changes.
- Fixed the Android surface not being recreated when the main window changed.
- Custom drivers: the whole driver package is copied, not only the library.

## Cidemu 0.1

Test build (`info.cemu.cemu.fork`) based on the Cemu Android port and the November 2026 state of Cemu. Only backbuffers updated since the last swap are presented, and an optional frame time log can be turned on by creating an empty file `frametimes.enable` in the data folder.
