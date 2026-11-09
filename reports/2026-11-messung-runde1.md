# Messrunde 1: LEGO City Undercover auf dem Thor Pro

Stand 2026-11-09. Test-App `info.cemu.cemu.profile` (Stand `dev` mit Flacker-Fix, `cemu/main` vom 2026-11-09, Bildzeit-Logger, korrigiertem FPS-Zähler), LCU EU v0 mit 60-FPS-Patch `[LCU60FPSEUv0]`, sofern nicht anders angegeben.

## Bedingungen
- Thor Pro, Android 13, Leistungsmodus „Standard“.
- **Small Underclock** (vom Nutzer gesetzt): schnellster Kern max. 2,48 GHz statt 3,19 GHz, mittlere Kerne 2,32 statt 2,80 GHz, GPU max. 680 MHz. Ein Referenzlauf lief noch mit Large Underclock (schnellster Kern 1,84 GHz).
- Gerät liegt auf dem Tisch, später kopfüber für bessere Luftzufuhr. Vor jedem Lauf Abkühlen auf unter 50 °C.
- Ton am Gerät aus.

## Szenen
- **A – Hafen:** Spielstart nach dem Intro, 5 Runden à 12 s Kamerabewegung mit dem rechten Stick. Ausgewertet werden die letzten 70 s.
- **B – Intro:** nicht übersprungene Engine-Zwischensequenz, ohne Eingabe, letzte 60 s von 150 s.

Weitere Szenen (Stadtfahrt, belebte Plätze) erreicht der Spielstand noch nicht. Sie sind nicht gemessen.

## Ergebnisse

| Lauf | Treiber | CPU-Modus | Ø FPS | p99 | ≤ 16,7 ms | > 50 ms | GPU Ø | GPU-Last | CPU max |
|---|---|---|---|---|---|---|---|---|---|
| Hafen, Large UC | Turnip T30 | Multi | 49,5 | 30,2 ms | 27,6 % | 1 | 401 MHz | 39 % | 77 °C |
| Hafen | Turnip T30 | Multi | 44,3 | 29,3 ms | 2,4 % | 0 | 401 MHz | 51 % | 75 °C |
| Hafen (Wdh.) | Turnip T30 | Multi | 49,3 | 23,9 ms | 7,0 % | 0 | 401 MHz | 50 % | 78 °C |
| Hafen | Qualcomm | Multi | **56,7** | 31,5 ms | **48,6 %** | 0 | 525 MHz | 52 % | 84 °C |
| Hafen (Wdh.) | Qualcomm | Multi | **57,3** | 30,6 ms | **48,8 %** | 1 | 507 MHz | 52 % | 82 °C |
| Hafen | Qualcomm | Single | 53,9 | 27,0 ms | 22,0 % | 0 | 497 MHz | 53 % | 80 °C |
| Hafen, **Stock-Takt** | Qualcomm | Multi | **59,9** | **18,9 ms** | 53,1 % | 0 | 522 MHz | 58 % | **93 °C** |
| Intro | Turnip T30 | Multi | 59,9 | 18,1 ms | 57,7 % | 0 | 401 MHz | 27 % | 82 °C |
| Intro | Qualcomm | Multi | 59,9 | 18,3 ms | 58,6 % | 0 | 418 MHz | 32 % | 79 °C |

Ohne 60-FPS-Patch läuft der Hafen stabil mit den nativen 30 FPS (Logger: Ø 30,05, p99 36 ms).

## Aussagen
1. **Der Qualcomm-Treiber ist am Hafen klar schneller als Turnip T30**, reproduzierbar mit 57 gegen 44 bis 49 FPS. Turnip hält die GPU die ganze Zeit auf 401 MHz, der Qualcomm-Treiber taktet sie auf über 500 MHz hoch. Grafikfehler gab es mit beiden Treibern keine.
2. **Multi-Core-Recompiler schlägt Single-Core** (57 gegen 54 FPS).
3. **Zwischensequenzen laufen mit vollen 60 FPS.** Das Gameplay am Hafen verfehlt sie knapp: Ø 57 FPS, aber nur die Hälfte der Bilder liegt unter 16,7 ms.
4. **Begrenzt wird von der CPU, nicht von der GPU.** Die GPU ist zu etwa 50 % ausgelastet.
5. **Mit Stock-Takt hält der Hafen die 60 FPS** (Ø 59,9, p99 18,9 ms statt 31 ms). Der schnellste Kern läuft dabei im Mittel mit 3,15 GHz. Der Preis: CPU bis 93 °C statt 84 °C. Ziel der Effizienzarbeit ist, dieselben 60 FPS mit dem Takt des Small Underclock (2,48 GHz) zu schaffen, also rund 25 % weniger Arbeit pro Bild auf dem Flaschenhals-Thread.

## CPU-Profil (Hafen, Qualcomm, 20 s, simpleperf)
- Threads: emulierter Wii-U-Kern 1 (`OSSched[core=1]`) 44 %, GPU-Thread (`LatteThread`) 36 %, Kern 2 18 % aller Takte.
- **Kern 1 läuft mit etwa 90 % eines 2,48-GHz-Kerns, er ist der Flaschenhals.** Davon entfallen 79 % auf den übersetzten Spielcode (JIT), 6,6 % auf `clock_gettime`, nur 6 % auf Cemus C++-Code.
- GPU-Thread (etwa 75 % eines mittleren Kerns): Cemu 42 %, Qualcomm-Treiber 29 %, Kernel 13 %, libc 11 %. Die größten Posten sind Index-Konvertierung (`LatteIndices_fastConvertU16_NEON`, 5,7 %), Befehlsverarbeitung (rund 9 %), Texturprüfung (rund 6 %), `clock_gettime` (3,2 %) und ausgelagerte Atomics (`__aarch64_cas4_acq`, 2 %).

## Dauerlauf (Hafen, Qualcomm, stehend, nach 20 min abgebrochen)
- Ab etwa Minute 7 ist alles eingependelt und bleibt bis zum Abbruch gleich.
- Stabil rund 53 FPS, p99 33 bis 35 ms.
- CPU 82 bis 84 °C, GPU 70 bis 72 °C.
- **Keine thermische Drosselung:** Der schnellste Kern hält durchgehend seine Underclock-Grenze von 2,48 GHz.
- In dieser stehenden Ansicht läuft die GPU am Anschlag (680 MHz, 81 % Last), anders als bei der Kamerabewegung (etwa 510 MHz, 52 %).
- Künftige Dauerläufe dauern höchstens 10 Minuten, denn das Gerät erreicht sein Maximum nach 3 bis 10 Minuten.

## Maßnahmen für Phase 3, nach Wirkung sortiert
1. **CPU-Takt (Entscheidung des Nutzers, kein Code):** Der Small Underclock begrenzt genau den Kern, der der Flaschenhals ist, auf 2,48 statt 3,19 GHz. Mit Stock-Takt sind am Hafen vermutlich die 60 FPS drin. Der Preis sind mehr Wärme und Akkuverbrauch. Ein Messlauf mit Stock-Takt beantwortet das in etwa 5 Minuten.
2. **Zeitabfragen (`clock_gettime`, wenig Aufwand):** 6,6 % des Flaschenhals-Threads und 3,2 % des GPU-Threads. `__rdtsc` liest auf ARM schon `cntvct_el0`, die Aufrufe kommen also von Timern auf Basis von `std::chrono`. Aufrufer finden und auf den Zähler umstellen sollte rund 5 % bringen.
3. **Code des AArch64-Recompilers (größter Hebel, viel Aufwand):** 79 % des Flaschenhals-Threads sind übersetzter Spielcode. Nötig sind Messungen auf Blockebene (Perf-Map für JIT-Code), danach gezielte Verbesserungen häufiger Muster wie Speicherzugriffe samt Byte-Tausch, Gleitkomma und Paired Singles.
4. **GPU-Thread (mittlerer Aufwand):** Er läuft mit rund 75 % eines Kerns nahe am Limit und begrenzt als Nächstes. Ansatzpunkte sind die Index-Konvertierung (5,7 %) mit Zwischenspeicher für unveränderte Indexpuffer, das Inlining von Atomics über `-march=armv8.2-a` nur für die Fork-Builds (2 %) und der Textur-Hash (2,7 %).
5. **Turnip (niedrige Priorität):** Turnip hält die GPU auf 401 MHz. Relevant ist das nur, falls Turnip für Grafik-Korrekturen gebraucht wird. Empfehlung bis dahin: Qualcomm-Treiber.

## Offen
- Audio-Lag (Ton am Gerät aus, nicht untersucht).
- Szenen mit mehr Last (Fahren, Stadt).
- Effekt des Large Underclock auf den Qualcomm-Treiber (nur Turnip gemessen).
