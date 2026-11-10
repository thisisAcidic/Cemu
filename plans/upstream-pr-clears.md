# Entwurf: PR an cemu-project/Cemu „Vulkan: Clear render targets inside render passes“

> **Wichtig (2026-11-10):** Cemus `CONTRIBUTING.md` verlangt für neue Beitragende, dass der Code von einem Menschen
> geschrieben ist (KI nur für Planung, Design, Review und Fragen, kein umformulierter KI-Code). Diese Commits sind
> KI-geschrieben und dürfen deshalb nicht als PR eingereicht werden. Stattdessen die Befunde (Messungen, Ursachen,
> Ansatz) als Issue oder Diskussion bei Cemu teilen, oder der Nutzer schreibt die Änderungen selbst.

Branch `upstream/vk-clears-in-renderpass` (lokal, auf `cemu/main`), 7 Commits, konfliktfrei übernommen. Nicht gepusht, kein PR eröffnet.

Vor dem PR noch nötig:
- Desktop-Build (Windows oder Linux, AMD/NVIDIA) und mindestens drei Spiele mit vielen Render-Passes ansehen (z. B. BotW, MK8, Xenoblade X), Vulkan-Validation-Layer an.
- macOS/MoltenVK: dort sollte der Gewinn ähnlich groß sein wie auf Adreno (TBDR), lohnt eine Messung.
- Die Commits ggf. zu drei PRs aufteilen: Render-Pass-Wiederverwendung, Clears im Render-Pass, Farbmaske bei deaktivierter Farbausgabe (ist ein eigenständiger Fix).

## PR-Text (Entwurf)

Title: Vulkan: Clear render targets inside the next render pass

Clears through vkCmdClearColorImage / vkCmdClearDepthStencilImage happen outside of render passes, so they write the
whole image to memory and the next render pass loads it again. On tile based GPUs (mobile GPUs, Apple Silicon through
MoltenVK) that is expensive: in LEGO City Undercover on an Adreno 740, clears and the following loads were a quarter
of the GPU time.

This PR remembers a clear and does it at the start of the next render pass that renders to the texture, as the
attachment's load operation (a render pass variant with LOAD_OP_CLEAR, compatible with the existing pipelines and
framebuffers). If the texture is already an attachment of the running render pass, vkCmdClearAttachments is used.
Every other use of the texture (sampling, copies, readback, uploads, presenting) applies the clear before that. The
first clear of a new texture still happens right away, because it also moves the image out of the UNDEFINED layout.

It also keeps the running render pass when the next framebuffer only uses a subset of its attachments, which is
common when a game switches between draws with one and with several outputs into the same targets. A draw only keeps
the pass if it can't write or test against the extra attachments. For that, color writes are now masked in the
pipeline while CB_COLOR_CONTROL disables color output, which so far only worked because such render passes had no
color attachments.

Measured on an AYN Thor (Snapdragon 8 Gen 2), LEGO City Undercover harbor, 60 FPS pack: GPU work −29 %, whole
device power 7.65 W → 6.65 W together with other changes, 99th percentile frame time 22.4 → 18.1 ms.

## Weitere Pakete für Cemu (lokale Branches, auf `cemu/main`, konfliktfrei)

| Branch | Commits | Inhalt |
|---|---|---|
| `upstream/vk-clears-in-renderpass` | 7 | Clears im Render-Pass, Render-Pass-Wiederverwendung, Farbmaske bei deaktivierter Farbausgabe |
| `upstream/idle-sleep` | 3 | Hauptkern und GPU-Thread schlafen im Leerlauf, Draw-Zeitstempel einmal pro Bild |
| `upstream/swapchain-wait` | 1 | Warten auf das vorherige Bild pro Swapchain (TV- und GamePad-Fenster) |
| `upstream/aarch64-codegen` | 7 | AArch64-JIT: kürzere Befehle, Flag-Sprünge, Zyklen im Register, blr/ret, Code-Arena, Xbyak-Ausnahme |
| `upstream/index-cache` | 2 | Indexdaten über Bilder hinweg cachen |
| `upstream/cmd-pools` | 2 | Command-Pool pro Command-Buffer, Reset auf eigenem Thread |

Nicht ohne Anpassung übertragbar: Warten auf das Acquire-Semaphor im nächsten Submit (`c589f3fb`), hängt an Fork-Änderungen. Android-spezifisches (Display-Activity, GamePad-Bildrate, Skalierung durch die Display-Hardware, Surface-Neuaufbau) gehört zum Android-Port von SSimco, nicht zu Cemu.
