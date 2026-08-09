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

## Lauf 2 — 2026-08-09, zweimal Cutscene rein und raus

293 protokollierte Frames. Ergebnis: **das Byte, auf das unsere Signatur zeigt,
ist konstant `0xFF`** — in Gameplay wie in Cutscene, über alle 293 Frames. Als
Trigger ist es wertlos. Wertvoll ist seine *Adresse*: es sitzt bei `+0x08` einer
32 Byte großen Struktur, in der der komplette Letterbox-Zustand liegt.

### Struktur, relativ zum Anker-Byte

| Offset | Typ | Inhalt |
|---|---|---|
| `-0x08` | float | **Gewicht**, 0.0 in Gameplay → 1.0 voll letterboxed |
| `-0x04` | float | Duplikat des Gewichts, in allen 293 Frames byte-identisch |
| `+0x00` | byte | konstant `0xFF` — das Anker-Byte |
| `+0x04` | float | Gewicht × **0.121749** |
| `+0x08` | float | Gewicht × **0.127907** |
| `+0x20` | — | die ganze Struktur wiederholt sich, ein Frame Versatz (Doppelpufferung) |

### Die beiden Konstanten

Die Verhältnisse `+0x04 / Gewicht` und `+0x08 / Gewicht` sind über alle Frames
**exakt konstant** (Minimum = Maximum auf sechs Nachkommastellen):

```
0.121749 = (1 - (16/9) / 2.35   ) / 2      Balkenhöhe für 2.35:1
0.127907 = (1 - (16/9) / 2.3889 ) / 2      Balkenhöhe für 3440x1440
```

Rückgerechnet ergibt die erste Konstante einen Zielaspect von 2.3500, die zweite
2.3889 — Letzteres ist auf vier Stellen unser Displayaspect. Anders gesagt:

```
0.127907 = (1 - k) / 2     mit k = 0.744186
```

**Das Spiel berechnet unser `k` bereits selbst, pro Frame, aus der echten
Auflösung.** `framing::correction_factor_from_bars()` dreht das um und gewinnt
`k` daraus zurück. Das ist der Auflösung aus `GetSystemMetrics` überlegen, weil
es im Fenstermodus, bei nicht-nativer Auflösung und bei Renderskalierung ohne
Zutun stimmt.

### Zeitverlauf

Beide Cutscenes verhalten sich identisch:

| Phase | Dauer | Frames |
|---|---|---|
| Einblenden | ~1,29 s | 71 bzw. 70 |
| Halten bei 1.0 | variabel | — |
| Ausblenden | ~1,00 s | 55 bzw. 55 |

Die Rampe ist **kein** linearer Verlauf: das Delta pro Frame fällt von 0,0222 auf
0,0217 — ein leichtes Ease-out. Genau deshalb wird der Wert gelesen und nicht
nachgebaut; eine selbstgebaute Kurve würde gegen die Balkenanimation laufen.

## Erledigt: Trigger und Interpolation kommen aus derselben Quelle

Die ursprüngliche Sorge war, dass `mov byte ptr [rip+disp32], 0FFh` nur ein
Boolean schreibt und damit die weiche Überblendung von `k` nicht tragen kann,
die CLAUDE.md voraussetzt. Das hat sich anders aufgelöst als erwartet: das Byte
ist tatsächlich nutzlos (immer `0xFF`), aber acht Bytes davor liegt genau der
0..1-Float, den das Design braucht.

**Trigger und Interpolationsgewicht sind derselbe Wert** — `Gewicht > 0` heißt
Cutscene, und der Betrag ist gleichzeitig das Blendgewicht. Die Designnotiz war
richtig, nur die vermutete Quelle war es nicht.
