# Cidemu

Cidemu is a Wii U emulator for Android handhelds. It is based on [Cemu](https://github.com/cemu-project/Cemu) and on the [Cemu Android port by SSimco](https://github.com/SSimco/Cemu), and focuses on running games at a steady frame rate with less power and heat on battery-powered devices.

Cidemu is an independent project. It is not affiliated with the Cemu project or with Nintendo. Please don't report Cidemu issues to the Cemu developers.

## Status

Cidemu is early software. Most of the work so far was measured and tested on an AYN Thor (Snapdragon 8 Gen 2, Adreno 740) with LEGO City Undercover. Other games and devices should work as well as they do in the Cemu Android port, but they have seen far less testing. If a game behaves differently in Cidemu than in the Cemu Android port, please open an issue.

## What's different

Changes on top of the Cemu Android port, mostly in the Vulkan renderer and the AArch64 recompiler:

- Render target clears happen inside the next render pass instead of as separate full-image clears. On tile-based mobile GPUs this cut the GPU work in the test scene by about a quarter.
- Consecutive draws to compatible framebuffers stay in one render pass.
- The image is scaled up to the screen by the display hardware instead of a full-resolution copy on the GPU.
- Decoded index data is kept across frames, and command buffers are reset on a separate thread.
- Idle emulated cores and the GPU thread sleep instead of spinning, so the CPU can clock down.
- With the GamePad shown on a second screen, both screens no longer wait on each other every frame.
- The GamePad image can be updated at a lower rate (automatic, full, low), and the TV and GamePad images can be placed on secondary displays. There they cover the launcher of that screen, so it stops drawing in the background.
- Several AArch64 code generation improvements in the recompiler.

In the LEGO City Undercover harbor scene with the 60 FPS graphic pack on the AYN Thor, these changes keep 60 FPS while drawing about 1 W less than the previous development build (7.65 W → 6.65 W for the whole device, 99th percentile frame time 22 ms → 18 ms).

## Requirements

- Android 11 or newer, 64-bit ARM (arm64-v8a)
- A GPU with Vulkan 1.1
- Your own dumped games and console files (keys, `otp.bin`, `seeprom.bin` for online features). Cidemu does not include any games or Nintendo files.

## Download

APKs are on the [Releases](https://github.com/thisisAcidic/Cemu/releases) page.

Cidemu installs as its own app (`com.cidix.cidemu`) next to the Cemu Android port, so both can be used side by side. To take your data over, copy the contents of the Cemu data folder into the Cidemu data folder. Both apps show their folder in the system file manager, and "Open Cidemu folder" in the game list menu opens it.

## Building

1. `git clone --recursive https://github.com/thisisAcidic/Cemu`
2. Open `src/android` in Android Studio, or run `./gradlew assembleRelease` in `src/android`.

See [BUILD.md](/BUILD.md) for the other platforms Cemu supports. Release APKs are built by the [Cidemu release workflow](/.github/workflows/android_release.yml) when an `android-vX.Y` tag is pushed.

## Contributing

Bug reports and pull requests are welcome. Changes that are not specific to Android are best proposed to [Cemu](https://github.com/cemu-project/Cemu) directly, see the [contribution guidelines](/CONTRIBUTING.md) and [coding style](/CODING_STYLE.md).

## License

Cidemu, like Cemu, is licensed under the [Mozilla Public License 2.0](/LICENSE.txt). Exempt from this are all files in the dependencies directory for which the licenses of the original code apply as well as some individual files in the src folder, as specified in those file headers respectively.

Cemu was created by Exzap and Petergov and is developed by the Cemu contributors. The Android port is by SSimco. Wii and Wii U are trademarks of Nintendo.
