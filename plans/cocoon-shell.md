# Cidemu in Cocoon Shell

## So startet Cocoon Wii-U-Spiele
- Feste Definition `android/wiiu.yaml` im Repo KansoMirai/CocoonFE, Liste `players`.
- Einziger Eintrag: `app: info.cemu.cemu/.emulation.EmulationActivity`, `game: uri`, Flags `clear-task`, `clear-top`, keine Extras.
- Daraus wird `am start -n <app> -d <content-URI des Spiels> --activity-clear-task --activity-clear-top`.
- Cidemu nimmt den Start genauso an wie Cemu: `EmulationActivity` liest `intent.data`, wenn das Extra fehlt.

## Wege
1. **Eigener Player in Cocoon** (ohne fremdes Repo): Cocoon reserviert IDs mit `custom.` für selbst angelegte Player. Wo die Oberfläche dafür liegt, ist nicht dokumentiert; am Gerät nachsehen unter Einstellungen → Library & Data → Platforms → Wii U.
2. **Eintrag in CocoonFE** (PR, Nutzer entscheidet): zweiter Player in `wiiu.yaml`, danach für alle Cocoon-Nutzer auswählbar.
3. **Cidemu als `info.cemu.cemu`**: würde das offizielle Cemu ersetzen. Nicht empfohlen.

## Eintrag für Weg 2 (Paket-ID noch offen)
```yaml
  - name: Cidemu
    id: wiiu.<paket-id>
    app: <paket-id>/info.cemu.cemu.emulation.EmulationActivity
    action: none
    game: uri
    files: [wua, wux, rpx]
    flags: [clear-task, clear-top]
```
Die Kurzform `.emulation.EmulationActivity` geht nicht, weil der Klassenname im Namespace `info.cemu.cemu` bleibt und die Paket-ID davon abweicht.

## Vor dem PR
- Paket-ID festlegen (heute `info.cemu.cemu.fork`). Noch hat niemand Cidemu installiert, ein Wechsel kostet jetzt nichts.
- Start mit einer echten Cocoon-URI auf dem Thor testen.
