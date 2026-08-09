# Messungen im laufenden Spiel

Alle Adressen als **Moduloffset**, nicht absolut — RDR2.exe ist ASLR-relokiert,
die Basis ist bei jedem Start eine andere.

## Lauf 1 — 2026-08-09, RDR2.exe 1.0.1491.50

Erster Lauf des Skeletons. Vollständiges Log:
`%LOCALAPPDATA%\RDR2UltrawideCutsceneFix\plugin.log`

| Ding | Wert |
|---|---|
| Modulbasis (dieser Lauf) | `0x00007FF6750A0000` |
| Image-Größe im Speicher | 121.206.272 Bytes (115,6 MB) |
| Ausführbare Sektionen | 2 |
| `.text` #1 | `+0x1000`, 50,8 MB |
| `.text` #2 | `+0x6033000`, 19,4 MB |
| Letterbox-Store gefunden bei | **`+0x320545`** (in `.text` #1) |
| Adresse des Letterbox-Flags | **`+0x39751B4`** (in `.data`) |
| Unknown prologue gefunden bei | **`+0x57A458`** (in `.text` #1) |
| Scandauer | 9,4 ms bzw. 68,9 ms über 70 MB |

### Was daraus folgt

**Beide Signaturen sind auf diesem Build eindeutig** — je genau ein Treffer. Sie
sind damit als Anker brauchbar, ohne dass sie weiter eingeengt werden müssten.

**Der Code ist beim Laden des Plugins bereits entpackt.** Treffer schon bei
Scanversuch 1, rund 80 ms nach `DLL_PROCESS_ATTACH`. Arxan entschlüsselt also
vor dem Entry Point, nicht verzögert. Die Retry-Schleife in `scan_until_found()`
bleibt trotzdem: sie kostet im Erfolgsfall nichts und fängt den Fall nach einem
Spielpatch ab.

**Das Flag liegt in `.data`,** `0x5E1B4` hinter dem Sektionsanfang (`.data` ab
RVA `0x3917000`). Ein globales Byte — passt zum Profil eines Enable-Flags.

## Offen: Trigger ja, Interpolation fraglich

Die Signatur ist `mov byte ptr [rip+disp32], 0FFh`. Geschrieben wird ein
**Byte**, und zwar `0xFF`. Als Trigger reicht das, aber die Designnotiz in
CLAUDE.md setzt voraus, dass derselbe Wert auch die weiche Überblendung von `k`
trägt. Ein Byte, das nur 0 und 0xFF kennt, kann das nicht.

Drei Möglichkeiten:

1. Ein benachbarter Wert (Float?) rampt während des Übergangs — dann taucht er
   im beobachteten Fenster auf.
2. Der Rampenwert liegt woanders und muss eigenständig gesucht werden.
3. Es gibt gar keinen — dann muss das Plugin selbst weich überblenden,
   getriggert durch die Flanke des Flags.

`monitor_letterbox_flag()` in `src/dllmain.cpp` beantwortet das rein lesend: es
pollt `[flag-0x10 .. flag+0x30)` einmal pro Frame und protokolliert jede
Änderung als Hexdump. Fall 1 ist dann direkt sichtbar, Fall 2 und 3 zeigen sich
als „nur das Flag kippt, sonst nichts".
