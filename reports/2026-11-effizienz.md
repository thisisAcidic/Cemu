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
| r5 pipe (Pipelining-Fix) | **59,4** | 23,6 ms | **51,1 %** | lud | 95 °C | 78 °C |
| r5 all (Pipelining + Leerlauf-Schlaf) | **59,7** | **19,5 ms** | 51,0 % | lud | 95 °C | 77 °C |

- Der Pipelining-Fix bringt mit Dual Screen 4,4 FPS.
- Mit Leerlauf-Schlaf taktet der schnellste Kern zwischen 1,84 und 3,19 GHz statt dauerhaft 3,19 GHz. p99 sinkt auf 19,5 ms.
- Während pipe und all hing der Thor am Ladegerät. Der Akkustrom war dann der Ladestrom, die Leistungswerte sind unbrauchbar und gestrichen. Temperaturen sind durch das Laden ebenfalls etwas erhöht. Leistungsmessungen brauchen Akkubetrieb.

## JIT-Profil (r5-all-jit, Kern 1)

- 77 % der Zyklen von Kern 1 liegen im übersetzten Spielcode. Das Profil ist flach: Die 40 heißesten Gastfunktionen machen zusammen nur 19 % aus. Es gibt keine einzelne Schleife, die man gezielt verbessern könnte; der Hebel ist die allgemeine Qualität des erzeugten Codes.
- Verteilung der Samples im JIT-Code:

| Anteil | Ursache |
|---|---|
| **28 %** | **Zykluszähler `remainingCycles`**: pro Block `ldr` → `sub` → `str` über den Speicher, die Schleifenprüfung lädt ihn sofort wieder |
| 13,5 % | Gast-Register aus dem/in den Kontext laden/speichern (Blockgrenzen) |
| 10,4 % | `fmov`/`fcvt` (Float-Umwandlungen, GPR↔FPR-Transfers) |
| 10,0 % | `rev` (Byte-Tausch, Big Endian) |
| 6,7 % | `cmp`/`cset` (ein PPC-`cmpw` wurde zu 3× `cmp` + 3× `cset`) |
| 5,1 % | indirekte Sprünge (`br`) |
| 3,1 % | echte Gast-Speicherzugriffe |

- Umgesetzt auf `perf/aarch64-codegen`:
  1. Kürzere Speicher- und Logikbefehle (Offset 0 ohne `add`, `rev16`, Floats direkt ins FP-Register, Masken als Immediate)
  2. Vergleich + Sprung über die Flags (`cmp` + `b.cond`, der vorhandene x64-Pass funktioniert auch für AArch64)
  3. Ein Befehlsplatz pro Sprung statt zwei (kein `nop` mehr hinter fast jedem Sprung)
  4. **Zykluszähler in `x24`** statt im Speicher, zurückgeschrieben nur vor HLE-Aufrufen und beim Verlassen; gleiche Vergleiche teilen sich ein `cmp`
- Stufe 1 + 2 laufen fehlerfrei durch den Hafen-Ablauf (r5-cg2-power). Stufe 3 + 4 sind gebaut, Messung r7-cg4 läuft.

## Treiber (Hafen, Build cg2, Akku)

| Treiber | Ø FPS | p99 | GPU-Thread-Last | Kern-1-Last | Leistung |
|---|---|---|---|---|---|
| Qualcomm System-Treiber V@0676.53 | 59,9 | 18,3 ms | 73 % | 82 % | 9,71 W |
| Qualcomm 819.2 (tatsächlich der System-Treiber, siehe unten) | 59,5 | 23,3 ms | 72 % | 82 % | 9,43 W |
| **Turnip 27.0.0-R2** | 59,3 | 21,4 ms | **51 %** | 82 % | 9,80 W |

- Turnip 27.0.0-R2 ist deutlich besser als Turnip T30 (44–49 FPS) und senkt die CPU-Last des GPU-Threads um rund 20 Prozentpunkte. p99 ist etwas schlechter (21,4 gegen 18,3 ms), zwei Bilder über 50 ms.
- **Fehler im Treiber-Laden:** Cemu kopierte nur die Hauptbibliothek eines Treiberpakets in den internen Speicher. Qualcomm-Pakete bringen eigene Versionen ihrer Abhängigkeiten mit (`notgsl.so`, `notadreno_utils.so`, `notllvm-glnext.so`), die dadurch fehlten. Das Log meldete trotzdem „Loaded custom driver“, im Einsatz war aber der System-Treiber V@0676.53 vom Dezember 2023. Fix auf `fix/custom-driver-package`, Nachmessung r7-qcom819/837 läuft.
- Turnip Gen8 V39 ist für A8xx gebaut und wurde nicht getestet. Qualcomm 837.6 verlangt laut Metadaten Android 15 (API 35), der Thor hat Android 13.

## Offen
- GamePad-Bildrate: Messung mit 10 FPS auf dem zweiten Bildschirm (r7-pad10fps) zeigt, was eine adaptive Rate sparen kann.
- Gast-Register an Blockgrenzen (13,5 %), Float-Umwandlungen (10 %).
