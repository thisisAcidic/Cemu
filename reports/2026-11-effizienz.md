# Effizienz: Wo Cemu auf dem Thor Leistung verschenkt

Stand 2026-11-09. Grundlage: simpleperf-Profil vom Hafen (r3, Small Underclock) und neue Messläufe mit Akkuleistung (Stock-Takt, Qualcomm-Treiber, 60-FPS-Patch, Dual Screen aktiv).

## Befunde

1. **Leerlauf wird nicht geschlafen, sondern durchgerechnet.**
   - Emulierter Kern 1: Hat er keinen lauffähigen PPC-Thread, dreht seine Idle-Schleife ununterbrochen. Sie fragt Audio, Alarme und die Run-Queue ab. Rund 14 % seiner Samples entfallen darauf (`clock_gettime` 6,6 %, TSC-Spinlock 2,1 %, Locks).
   - GPU-Thread: Ohne neue Befehle pollt er Ringpuffer, Vsync-Timer und `sched_yield`. Das sind rund 15 % seiner Samples.
   - Beide Kerne laufen deshalb immer mit vollem Takt, auch wenn sie nur warten. Der Governor kann nicht heruntertakten.
2. **Zeitabfrage pro Draw-Call.** Der GPU-Thread liest für jeden Draw die Uhr, nur damit die Textur-Speicherbereinigung (Auflösung 100 ms) einen Zeitstempel hat.
3. **Dual Screen zerstört das Pipelining.** Vor jedem Present wartet der Renderer auf den Befehlspuffer des vorherigen Presents. Mit GamePad-Fenster ist das der TV-Present desselben Bildes. Jedes Bild wartet also, bis die GPU es fertig gerendert hat, und der GPU-Thread kann nicht vorarbeiten.
   - Messung: Hafen mit Dual Screen 55,0 FPS (p99 23,8 ms, nur 18 % der Bilder ≤ 16,7 ms) gegen 59,9 FPS ohne Dual Screen.

## Änderungen

| Branch | Änderung |
|---|---|
| `perf/idle-waits` | Kern 1 schläft nach 50 µs Leerlauf bis zum nächsten Alarm oder lauffähigen Thread (max. 0,5 ms wegen Audio) |
| `perf/idle-waits` | GPU-Thread schläft nach 50 µs ohne Befehle bis zu neuen Befehlen, Async-Arbeit oder dem nächsten Vsync (max. 1 ms) |
| `perf/idle-waits` | Draw-Zeitstempel einmal pro Bild statt pro Draw |
| `perf/idle-waits` | Optional `jitmap.enable`: Perf-Map der übersetzten Funktionen für Profile nach Gastfunktion |
| `fix/dual-screen-frame-wait` | Warten auf das vorherige Bild pro Swapchain statt global |

## Messungen

Hafen, Stock-Takt, Qualcomm-Treiber, Dual Screen, letzte 70 s. Leistung = Akkuleistung des ganzen Geräts.

| Lauf | Ø FPS | p99 | ≤ 16,7 ms | Leistung | CPU max | GPU max |
|---|---|---|---|---|---|---|
| r5 Basis (`dev`) | 55,0 | 23,8 ms | 18,4 % | 9,56 W | 95 °C | 71 °C |
| r5 idle1 (Kern 1 schläft) | abgebrochen: Thor ging offline | | | | | |

## Offen
- Messläufe: Pipelining-Fix allein, alles zusammen.
- JIT-Profil nach Gastfunktionen (`--perf 20 --jitmap`), danach gezielte Verbesserungen im AArch64-Backend.
