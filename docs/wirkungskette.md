# Wie der Fix tatsächlich funktioniert

Der Weg dahin steht in [ghidra.md](ghidra.md) und [messungen.md](messungen.md).
Hier nur das Ergebnis, in der Reihenfolge, in der es zur Laufzeit passiert.

## Die Kette

| Schritt | Wo | Was |
|---|---|---|
| 1 | AOB `kLetterboxStructAnchor` | findet die Letterbox-Struktur |
| 2 | `Anker −0x08` | **Gewicht** 0..1 — Trigger *und* Blendfaktor |
| 3 | `Anker +0x08` | Balkenhöhe für den echten Displayaspect → `k` |
| 4 | AOB `kFovGetter` | liefert die Adresse des FOV-Masters, dient der Plausibilisierung |
| 5 | AOB `kCameraApply` | `ApplyCameraState(dst, src)` — hier wird gehookt |
| 6 | `dst +0x60` | die vertikale FOV in Grad |
| 7 | `bars::set_hidden` | patcht das Immediate der Ankerinstruktion von `FF` auf `00` |

`k` kommt aus Schritt 3, nicht aus `GetSystemMetrics`: Das Spiel rechnet es
selbst aus der tatsächlichen Auflösung, also stimmt es auch im Fenstermodus und
bei Renderskalierung.

## Die Korrektur

```
k       = 1 − 2 · (Balkenhöhe / Gewicht)
faktor  = 1 + (k − 1) · Gewicht          // ruckfreier Übergang
vFOV_neu = 2 · atan(faktor · tan(vFOV_alt / 2))
```

Bei 3440x1440 ist `k = 0,744186`, und das ist exakt `2560/3440` — die Breite des
Pillarbox-Fensters, das das Spiel für Cutscenes aufspannt. Die Korrektur bildet
also genau das ursprüngliche Framing auf den vollen Bildschirm ab.

## Die drei Regeln im Detour

Alle drei stammen aus Messungen, nicht aus Vorsicht. Ohne sie war das Ergebnis
nachweislich falsch.

**1. Nur die gerenderte Kamera.** `ApplyCameraState` läuft für über zwei Dutzend
Kamerazustände. Korrigiert man alle, korrigiert man auch die Quellen einer
Überblendung — und danach noch deren Mischergebnis. Welcher Zustand gerendert
wird, verrät die Shader-Konstante bei `+0x3AE24B8`: sie trägt dessen Wert, einen
Frame verzögert. Die Zuordnung ist eindeutig, gemessen 99 % gegen 0 %.

Die Erkennung ist **klebrig**: bei einem Kameraschnitt springt die
Shader-Konstante, und für einen Frame passt gar nichts. Ohne das Festhalten
fielen 4 von 489 Frames aus, jeder als doppelt großer Schritt sichtbar.

**2. Höchstens eine Korrektur pro Frame.** Während einer Rampe erreicht ein
korrigierter Wert über die Blendfeder einen anderen Zustand — durch Arithmetik,
die jede Markierung wegrundet. Ohne diese Regel wurden zwei bis drei Mal pro
Frame korrigiert, mit einem Ergebnis rund 25 % zu eng. Das Gewicht dient als
Frame-Marke, weil das Spiel es genau einmal pro Frame berechnet.

**3. Markierung gegen exakte Kopien.** Die unteren acht Mantissenbits tragen ein
festes Muster (Abweichung < 0,0003°). Das erkennt einen korrigierten Wert, der
unverändert weitergereicht wird — der Normalfall im eingeschwungenen Zustand.

## Was nicht funktioniert hat

Damit es niemand wiederholt:

- **Den Getter hooken.** Er wird 235-mal pro Sekunde gelesen und speist trotzdem
  nur Peripherie (LOD, Schärfentiefe). Kein Einfluss aufs Bild.
- **Den Master überschreiben.** Ein Lese-Watchpoint zeigt: im ganzen Spielcode
  liest ihn genau eine Instruktion, der Getter. Sackgasse.
- **Statische Analyse der Schreibstelle.** Der Store geht über ein Register
  (`movss [rbx+0x60], xmm0`), Ghidra sieht keine Referenz. Nur ein
  Hardware-Watchpoint hat sie gefunden.
- **Eine Toleranz gegen zuletzt geschriebene Werte.** Funktioniert im Prinzip,
  hängt aber an Ringgröße und Schwelle. Mit 512 Einträgen wurde daraus ein Sieb,
  das 94 % aller Korrekturen verwarf.

## Bekannte Grenzen

- Beim Kameraschnitt am Cutscene-Ende springt die FOV, weil das Spiel dort
  selbst schneidet (48,84° → 51,28°). Die Korrektur verkleinert den Sprung,
  erzeugt ihn aber nicht.
- Die Offsets in `patterns::candidates` sind rohe Adressen für 1.0.1491.50 und
  gelten für keinen anderen Build. Nur die Shader-Konstante daraus wird im
  Normalbetrieb benutzt; alles andere dient der Diagnose.
