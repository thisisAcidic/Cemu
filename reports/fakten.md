# Fakten: LCU EU v0 auf dem Thor Pro

Stand 2026-11-09, Test-App `info.cemu.cemu.profile` auf `dev` @ aca800b8 (vor dem Merge von `cemu/main`).

## Titel
- Titel-ID `00050000-10101b00`, Version v0, Region EU, kein Update, kein DLC
- RPX-Hash `7bbad1ee`
- Modul `legocity`, Prüfsumme für `moduleMatches`: **`0x01d048b1`**

## Treiber und Grafik
- Turnip über libadrenotools: `PurpleVK 26.3.0-devel (git-62ac221a33)`, Adreno 740
- `Vulkan: Can't find immediate present mode`: Der Treiber bietet keine Ausgabe ohne Vsync
- `VK_EXT_pipeline_robustness` fehlt (Fallback robustBufferAccess), `VK_FORMAT_R4G4_UNORM_PACK8` fehlt, `present_wait` fehlt
- Gerätespeicher: ein Heap mit 8448 MB

## Einstellungen beim Basislauf
- CPU-Modus: Multi-Core-Recompiler (Standard, Spielprofil leer)
- Async Compile an, Accurate Barriers an, Full sync at GX2DrawDone an, VSync 0
- Audio: `AXFXMultiChReverbInit` ist gestubbt (Hall-Effekt fehlt ohne `snd_user.rpl`)

## Beobachtungen
- Sprachauswahl beim ersten Start: FPS-Overlay 59,85
- Das FPS-Overlay zählt Swaps, nicht neue Bilder. LCU swappt zweimal pro Bild, also bedeuten ~60 im Overlay ~30 echte Bilder pro Sekunde.

## Gefundene und behobene Fehler
1. **Flackern (jedes zweite Bild schwarz).** LCU ruft `GX2SwapScanBuffers` zweimal pro Kopie in den Scan-Buffer auf. Cemu hat beim zweiten Swap ein auf Schwarz gelöschtes Swapchain-Bild ausgegeben. Am Gerät gezählt: 300 von 600 Ausgaben schwarz, nach dem Fix 0 von 600. Fix: `Latte: Only present backbuffers updated since the last swap` (4fed5d10).
2. **Zerrissene Geometrie nach dem Shader-Kompilieren.** Tritt mit Turnip und mit dem Qualcomm-Treiber gleich auf, also kein Treiberfehler. Behoben durch den Merge von `cemu/main` (30c9798b). Der Nutzer hat beide Läufe live gesehen: vorher Artefakte, nachher sauber. Nicht eingegrenzt, wahrscheinlich eine der PPCRec-Korrekturen seit Juni (`stfs` auf ARM64, `subfic`, `divw`, `fcmpo`).

## Offen
- Ton laggt zeitweise (Eindruck des Nutzers mit dem Stand vor dem Merge)

## Testablauf
`tools/lcu_repro.py` startet das Spiel, lädt Slot 1, überspringt das Intro mit + und X, wartet auf die Shader und bewegt die Kamera mit dem rechten Stick. Die Eingaben kommen über `sendevent` auf `/dev/input/event9` („Odin Controller“): + = `BTN_START` 315, X = `BTN_NORTH` 307, A = 304, rechter Stick `ABS_Z`/`ABS_RZ`. Für Flackern ist das Urteil des Nutzers am Gerät verlässlicher als einzelne Screenshots.

## 60-FPS-Patch für v0
Patch-Gruppe `[LCU60FPSEUv0]` (moduleMatches `0x01d048b1`) im Pack `Lego City Undercover/60FPS`, Fork `thisisAcidic/cemu_graphic_packs`, Zweig `lcu-60fps-v0`:
- `0x025d65bc = nop`: Die Present-Funktion ruft `GX2SwapScanBuffers` zweimal hintereinander auf (`0x025d65b8`, `0x025d65bc`), das ist die 30-FPS-Begrenzung. Das Swap-Intervall ist überall 1.
- `0x02413b70 = lfs f0, 0x28c4(r8)`: Die Hauptschleife (`0x02416250`) nimmt als Zeitschritt `+0x1c40` des Frame-Objekts, wenn es nicht 0 ist, sonst die gemessene Bildzeit (gekappt auf 0,1 s). Beim Start wird `+0x1c40` auf 1/30 gesetzt (`0x10082b54`), der Patch lädt stattdessen 0,0 (`0x100828c4`). Damit läuft die Engine mit variablem Zeitschritt und ohne Zeitlupe.

Ergebnis auf dem Thor (Turnip T30): rund 45 FPS im Spiel (Logger: Ø 45,5, p99 26 ms), Bild ohne sichtbare Fehler. Tempo-Vergleich am nicht übersprungenen Intro (`tools/intro_timing.py`): Mit Patch erscheinen die Bilder zu denselben Zeitpunkten wie ohne Patch, Verhältnis 1,00 bei +5 bis +50 s.
