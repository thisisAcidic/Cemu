# Entwurf: PR an cemu-project/Cemu „Vulkan: Clear render targets inside render passes“

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
