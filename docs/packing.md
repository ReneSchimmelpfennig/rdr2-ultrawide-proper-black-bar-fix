# RDR2.exe ist gepackt — was das für AOB-Scanning heißt

Festgestellt am 2026-08-09 an `RDR2.exe` 1.0.1491.50 (85,41 MB, D:\Programme\Rockstar Games\Red Dead Redemption 2).

## Befund

Beide aus `RDR2NoBlackBars.asi` extrahierten Signaturen finden sich in der Datei
auf der Platte **null Mal**:

| Signatur | Treffer in der Datei |
|---|---|
| `C6 05 ? ? ? ? FF 0F 28 74 24 60` | 0 |
| `48 8B C4 48 89 58 08 56 57 41 56 48 81 EC C0 00 00 00 0F 29 70 D8` | 0 |

Das ist kein Scanner-Bug. Kontrolltest mit `48 89 5C 24 08` (`mov [rsp+8], rbx`,
einer der häufigsten x64-Prologe überhaupt): **15 Treffer** in 85 MB. Ein
normales MSVC-Binary dieser Größe hat davon Zehntausende.

Dazu die Sektionstabelle — sie hat elf Einträge, darunter **zwei** namens `.text`
und zwei komplett namenlose:

| # | Name | RVA | Virtual Size |
|---|---|---|---|
| 1 | `.text` | 0x1000 | 0x32D7000 (~51 MB) |
| 2 | `.rdata` | 0x32D8000 | 0x63E400 |
| 3 | `.data` | 0x3917000 | 0x239A6F8 |
| 4 | `.pdata` | 0x5CB2000 | 0x231E00 |
| 5 | *(namenlos)* | 0x5EE4000 | 0x200 |
| 6 | `_RDATA` | 0x5EE5000 | 0xA000 |
| 7 | *(namenlos)* | 0x5EEF000 | 0x200 |
| 8 | `.rsrc` | 0x5EF0000 | 0x61800 |
| 9 | `.reloc` | 0x5F52000 | 0xDBE00 |
| 10 | `.idata` | 0x602E000 | 0x4C00 |
| 11 | `.text` | 0x6033000 | 0x1364600 (~20 MB) |

Das ist die typische Arxan-Signatur: der eigentliche Code liegt verschlüsselt
in der Datei und wird erst zur Laufzeit entpackt.

## Konsequenzen für das Plugin

1. **Nur im Speicher scannen, nie in der Datei.** Genau deshalb benutzt das
   bestehende `RDR2NoBlackBars.asi` auch `K32GetModuleInformation` und nicht
   `CreateFileMapping`.

2. **Alle ausführbaren Sektionen scannen, nicht `.text`.** Ein
   `GetSection(".text")` erwischt nur die erste der beiden. `mem::executable_sections()`
   geht über `IMAGE_SCN_MEM_EXECUTE` und erfasst damit auch die namenlosen.

3. **Der Scan muss wiederholt werden.** Zum Zeitpunkt von `DllMain` ist der Code
   noch nicht entschlüsselt. `scan_until_found()` versucht es alle 2 s, bis zu
   60 mal (2 Minuten). Das Log zeigt, beim wievielten Versuch die Signatur
   auftaucht — dieser Wert ist die eigentlich interessante Messung beim ersten
   Lauf im Spiel.

4. **Unlesbare Seiten sind normal.** Arxan hinterlässt `PAGE_NOACCESS`- und
   Guard-Pages mitten in den ausführbaren Sektionen. `mem::find_all()` fragt
   deshalb per `VirtualQuery` ab und überspringt sie, statt eine Access
   Violation zu produzieren. Benachbarte lesbare Bereiche werden zusammengefasst,
   damit eine Signatur über eine Schutzgrenze hinweg trotzdem gefunden wird.
   Der Selbsttest deckt genau diesen Fall ab (`test_unreadable_pages`).

## Konsequenz für die statische Analyse: der Dumper

Ghidra an der Datei auf der Platte anzusetzen ist sinnlos — an `+0x320545`
steht dort Müll. Deshalb schreibt das Plugin das entpackte Image selbst heraus
(`src/dump.*`), aus dem Prozess, in dem es ohnehin läuft. Kein Debugger, damit
auch kein Risiko, Arxans Anti-Debug zu wecken.

**Wann:** einmal, nachdem eine vollständige Cutscene beobachtet wurde. Der
Zeitpunkt ist Absicht — so ist der Cutscene-Code garantiert entschlüsselt und
resident. Existiert die Datei schon, wird sie nicht neu geschrieben; zum
Erzwingen einfach löschen.

**Wohin:** `%LOCALAPPDATA%\RDR2UltrawideCutsceneFix\RDR2.dump.exe`, rund 115 MB.
Gehört nicht ins Repo — `dumps/` und `*.exe` sind in `.gitignore`.

**Was der Dump kann:** Er ist *realigned*. Jeder Sektionsheader bekommt
`PointerToRawData = VirtualAddress`, und `FileAlignment` wird auf
`SectionAlignment` angehoben. Dateioffsets sind damit identisch mit RVAs: was
das Log als `module +0x320545` meldet, liegt im Dump an Offset `0x320545`. Der
Selbsttest prüft genau diese Eigenschaft, indem er eine eigene Funktion über
beide Wege vergleicht.

**Was der Dump nicht kann:** Die Importtabelle wird *nicht* rekonstruiert. Die
IAT enthält aufgelöste Adressen aus dem dumpenden Prozess, importierte Aufrufe
erscheinen in Ghidra also als nackte Zeiger statt als benannte Funktionen. Für
unseren Zweck — Code lesen und Datenreferenzen verfolgen — reicht das. Wer
benannte Imports braucht, müsste ImpRec-artig nacharbeiten.

Unlesbare Seiten (die Arxan-Löcher) werden als Nullen geschrieben und im Log
gezählt. Eine hohe Zahl dort ist das Warnsignal, dass der Dump lückenhaft ist.

## Offene Frage

Ob Arxan zusätzlich Integritätsprüfungen über den entpackten Code laufen lässt.
Falls ja, kann ein MinHook-Trampolin im geprüften Bereich einen Crash oder ein
stilles Zurückschreiben auslösen. Dass `RDR2NoBlackBars.asi` mit direktem
Byte-Patching funktioniert, spricht dagegen — aber ein Detour ist ein größerer
Eingriff als ein einzelnes gepatchtes Byte. Beim ersten echten Hook im Auge behalten.
