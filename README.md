# RemotiFy Transmitter

**Ein konfigurierbarer RC-Sender auf ESP32-Basis mit Touch-Bedienoberfläche, ESP-NOW-Funkverbindung und Verwaltung mehrerer Modelle.**

> **GitHub-Kurzbeschreibung:** ESP32-Touchscreen-RC-Sender mit konfigurierbaren Kanälen, ESP-NOW-Kopplung, Telemetrie und Multi-Modell-Verwaltung.

RemotiFy Transmitter ist die Sender-Firmware des RemotiFy-Systems. Sie liest angeschlossene Bedienelemente ein, verarbeitet ihre Werte zu RC-Kanalsignalen und überträgt Steuerdaten an einen gekoppelten Empfänger. Modell- und Kanaleinstellungen lassen sich direkt am Gerät anpassen und im Dateisystem ablegen. Ein Dashboard, Akku- und Telemetrieanzeigen sowie serielle Diagnosefunktionen ergänzen die Bedienung.

## Funktionen

- **Touch-Oberfläche:** LVGL-9-Benutzeroberfläche für ein 480 × 320 Pixel großes Display mit den Reitern Dashboard, Akku, Modelle, Configs (Kanalliste) und System.
- **Flexible Kanäle:** Bis zu 24 Kanäle pro Modell. Konfigurierbar sind Name, Typ, Hardware-Eingang, Verhalten, Invertierung, Trimmung, Limits, Totbereich, Dämpfung, Kurve und Failsafe-Vorgabe.
- **Eingangsverarbeitung:** Analoge Eingaben werden kalibriert und zu PWM-Werten im Bereich von 1000 bis 2000 µs verarbeitet. Die Kanalverarbeitung unterstützt unter anderem Totbereich, Kurven und unterschiedliche Dämpfungswerte für Hin- und Rückbewegung.
- **Modellverwaltung:** Bis zu 20 Empfänger können gekoppelt, benannt, sortiert, ausgewählt und aus der Liste entfernt werden. Die zugehörigen Konfigurationen bleiben gespeichert.
- **ESP-NOW-Kommunikation:** Überträgt laufende Steuerdaten und Modellkonfigurationen. Empfänger-Telemetrie wie Akkustand, Lagewerte, Gang, RSSI und Status wird für die Anzeige verarbeitet.
- **Akku- und Systemanzeigen:** Zeigt Sender- und – sofern verfügbar – Empfänger-Akkudaten sowie Systeminformationen an.
- **Systemeinstellungen:** Analoge Geber kalibrieren, Sender benennen, Displayhelligkeit und Audio einstellen; je nach Hardware können auch automatische Dimmung und Audiofilter konfiguriert werden.
- **Persistenz:** LittleFS speichert gekoppelte Modelle, deren Konfigurationen, Kalibrierung und Diagnoseprofile.
- **Serielle Diagnose:** Diagnoseausgaben können für einzelne Softwarebereiche ein- und ausgeschaltet werden.

## Projektstatus und Sicherheit

Die Firmware hängt von der konkreten Display-, Touch- und I/O-Hardware ab. Die angegebenen Board- und Pinwerte sind die im Projekt hinterlegten Voreinstellungen und nicht automatisch für andere ESP32-Boards oder Displayvarianten geeignet.

Vor einem Einsatz mit einem Fahrzeug oder anderen beweglichen Geräten müssen Sender, Empfänger, Kanalzuordnung und das Verhalten bei Funkverlust **am realen Aufbau** geprüft werden. Die Failsafe-Einstellung im Sender ersetzt keine Prüfung des Empfänger- und Aktuatorverhaltens. RemotiFy ist kein zertifiziertes oder sicherheitskritisches Steuerungssystem.

## Hardware und Anschlüsse

Die PlatformIO-Umgebung ist auf ein ESP32-WROOM-Board (`upesy_wroom`) ausgelegt. Der Sketch verwendet ein 480 × 320 Pixel großes TFT mit Touch-Eingabe und setzt die Touch-Kalibrierwerte im Haupt-Sketch fest.

Die Firmware erwartet derzeit folgende I²C-Konfiguration:

| Baustein/Funktion | Einstellung im Projekt |
| --- | --- |
| I²C-SDA / I²C-SCL | GPIO 32 / GPIO 25 |
| I²C-Takt | 400 kHz |
| ADS1115 für vier analoge Achsen | Adresse `0x48` |
| MCP23017-I/O-Erweiterung | Adresse `0x20` |
| MAX17048-Akku-Monitor | I²C |
| Display-Hintergrundbeleuchtung | GPIO 27, PWM |

Die im Firmware-Menü benannten analogen Eingänge sind Joystick L X/Y und Joystick R X/Y. Weitere Einträge sind Schalter A, Schalter B, Taster C und Encoder D. Welche davon tatsächlich angeschlossen und unterstützt sind, hängt von der bestückten Hardware und der Verdrahtung ab. I²C-Adressen, Pins und Touch-Kalibrierung müssen bei abweichender Hardware in der Firmware angepasst werden.

## Voraussetzungen

- Visual Studio Code mit der PlatformIO IDE **oder** eine installierte PlatformIO-Core-CLI
- PlatformIO-Plattform `espressif32` und Arduino-Framework (werden beim Build über PlatformIO verwaltet)
- Passendes ESP32-Board und USB-Datenkabel
- Kompatible Display-, Touch- und I²C-Hardware

Die Projektkonfiguration und die Partitionsaufteilung liegen in [`platformio.ini`](platformio.ini) und [`partitions.csv`](partitions.csv). Die verwendete TFT_eSPI-Bibliothek ist im Repository unter `lib/` eingebunden.

## Build und Flashen

Im Projektverzeichnis ausführen:

```sh
pio run -e RemotiFyTransmitter
```

Firmware auf das angeschlossene Board laden:

```sh
pio run -e RemotiFyTransmitter -t upload
```

Seriellen Monitor mit der konfigurierten Baudrate öffnen:

```sh
pio device monitor -b 115200
```

In [`platformio.ini`](platformio.ini) sind `COM5` als Upload- und Monitor-Port eingetragen. Den Port bei Bedarf an den eigenen Rechner anpassen. Ein erfolgreicher Build bestätigt die Kompilierung, ersetzt aber keine Prüfung der Hardware, Funkverbindung oder Failsafe-Funktion.

## Erste Inbetriebnahme

1. **Hardware prüfen:** Display, Touch und I²C-Bausteine passend zur Pin- und Adresskonfiguration anschließen. Versorgung und gemeinsame Masse kontrollieren.
2. **Firmware bauen und flashen:** PlatformIO-Umgebung `RemotiFyTransmitter` verwenden.
3. **Serielle Ausgabe öffnen:** Mit 115200 Baud starten. Beim Start werden Dateisystem, Hardware, ESP-NOW und Oberfläche initialisiert. Meldungen zu nicht gefundenen I²C-Bausteinen weisen auf Hardware-, Verdrahtungs- oder Adressprobleme hin.
4. **Geber kalibrieren:** Im Reiter **System → Kalibrierung** eine Achse auswählen. Geber an die jeweiligen Endpunkte und in die Mittelstellung bewegen und die passenden **Set Min**, **Set Mitte** und **Set Max**-Aktionen auslösen. Kalibrierung speichern.
5. **Empfänger koppeln:** Empfänger einschalten und im Sender unter **Modelle** das 30-Sekunden-Pairing starten. Die angezeigte Anfrage prüfen und am Sender bestätigen. Der Empfänger erscheint anschließend in der Modellliste.
6. **Modell auswählen und Kanäle konfigurieren:** Gewünschtes Modell aktivieren, im Reiter **Configs** Kanäle anlegen oder bearbeiten und jedem Kanal die passende Quelle sowie das Verhalten zuordnen. Änderungen über **Sichern** übernehmen.
7. **Funktion ohne Last prüfen:** Kanalrichtung, Neutralstellung, Endpunkte, Telemetrie und Reaktion bei ausgeschaltetem Empfänger testen, bevor Aktuatoren oder ein Fahrzeug betrieben werden.

Die gekoppelten Empfänger und zugehörigen Konfigurationen werden im LittleFS-Dateisystem abgelegt. Das Löschen oder Neuformatieren dieses Dateisystems entfernt diese gespeicherten Daten.

## Bedienoberfläche

### Dashboard

Das Dashboard stellt Bedienelemente und Zustände anhand des aktiven Modells dar. Lichtkanäle mit einem Namen im Format `LIGHT_…` werden als Beleuchtungssteuerungen angeboten. Bei einem `LIGHT_`-Kanal ohne Hardware-Eingang kann die Beleuchtung über die Oberfläche bedient werden; bei zugeordnetem Hardware-Eingang zeigt die Oberfläche den Zustand an. Falls das Modell einen Kanal namens `WINCH` enthält, zeigt das Dashboard zusätzlich Windensteuerung und Sperre an. Bei aktivierter Sperre wird der WINCH-Ausgang auf 1500 µs gehalten.

Empfangene Modellwerte wie Nick- und Rollwinkel sowie Gang- und Verbindungsinformationen werden angezeigt, sofern der Empfänger die entsprechenden Telemetriedaten sendet.

### Akku

Der Akku-Reiter zeigt Messwerte des Senders und – bei empfangener Telemetrie – des Modells. Die Verlaufsgrafik hält Messpunkte beim Wechsel zwischen Reitern im Arbeitsspeicher. Verfügbarkeit und Aussagekraft einzelner Messwerte hängen von den verbauten Sensoren und den gesendeten Empfängerdaten ab.

### Modelle

Über **Pairing (30s)** wird ein zeitlich begrenztes Kopplungsfenster geöffnet. Eine erkannte Anfrage muss am Sender bestätigt werden. In der Modellliste lassen sich Einträge aktivieren, priorisieren und löschen. Geänderte Modellkonfigurationen können über die jeweilige Speichern-Aktion gesichert werden.

### Kanäle und Kanalparameter

Im Reiter **Configs** bietet die Kanalliste und Detailansicht des aktiven Modells folgende Einstellungen:

| Einstellung | Bedeutung |
| --- | --- |
| Name, Typ und Eingang | Kanalbezeichnung, Analog/Digital/Multi-State und zugeordneter Hardware-Eingang |
| Verhalten | Restore Zero, Stick, Momentary oder Toggle |
| Invertierung | Kehrt die Richtung eines Kanals um |
| Trim und Limits | Verschiebt die Neutralstellung und begrenzt den Ausgangsbereich |
| Totbereich | Unterdrückt kleine Eingangsänderungen um die Neutralstellung |
| Dämpfung | Stellt getrennte Reaktionswerte für Vorwärts- und Rückbewegung ein |
| Kurve | Linear, progressiv oder degressiv mit einstellbarem Kurvenfaktor |
| Failsafe | Übermittelt die Vorgabe HOLD oder Wert samt konfiguriertem Wert an den Empfänger |
| Multi-State-Zustände | Kommagetrennte Bezeichnungen, zum Beispiel `R,N,1,2,3` |

Nach Änderungen **Sichern** wählen, um sie zu übernehmen. **Abbrechen** verwirft die Änderungen der Detailansicht. Die konkrete Failsafe-Reaktion bei Funkverlust ist zusätzlich vom Empfänger abhängig und muss dort geprüft werden.

### System

Unter **System** befinden sich Einstellungen für:

- **Kalibrierung:** vier analoge Achsen mit Min-, Mitte- und Maximalwerten
- **Audio:** Ausgabe, Lautstärke und Audiofilter, abhängig von der angeschlossenen Hardware
- **Helligkeit:** Displayhelligkeit und – sofern unterstützt – automatische Dimmung
- **Sendername:** Kennung, die bei der Kopplung verwendet wird
- **Systeminformationen:** Laufzeit- und Speicherinformationen

## Serieller Monitor und Diagnose

Der serielle Befehlseingang arbeitet mit 115200 Baud. Befehle in Großbuchstaben eingeben und mit Enter abschicken. `HELP` zeigt die aktuelle Befehlsübersicht.

| Befehl | Wirkung |
| --- | --- |
| `HELP` oder `?` | Befehlsübersicht anzeigen |
| `SYSINFO` | Systeminformationen ausgeben |
| `MODELINFO` | Aktives Modell und Kanäle anzeigen |
| `DEBUG STATUS` | Status der Diagnosemodule anzeigen |
| `DEBUG ON` / `DEBUG OFF` | Alle Diagnosemodule ein- oder ausschalten |
| `DEBUG <MODUL> ON` / `DEBUG <MODUL> OFF` | Einzelnes Diagnosemodul schalten |
| `TELEMETRY ON` / `TELEMETRY OFF` | Telemetrie-Diagnose schalten |
| `CHAN <1-24>` | Diagnose auf einen Kanal filtern |
| `CHAN <1-24> ON` / `CHAN <1-24> OFF` | Kanalfilter setzen oder für den gewählten Kanal deaktivieren |
| `CHAN OFF` | Kanalfilter deaktivieren |
| `SAVE <name>` / `LOAD <name>` | Diagnoseprofil speichern oder laden |
| `CONFIGS` | Gespeicherte Diagnoseprofile auflisten |

Verfügbare Modulnamen für `DEBUG` sind `SYSTEM`, `MEMORY`, `HARDWARE`, `CHANNELS`, `TELEMETRY`, `ESPNOW`, `PAIRING`, `UI`, `BATTERY`, `AUDIO`, `MODELS` und `CORE`. Kanalnummern in seriellen Befehlen sind **1-basiert**. Die Diagnoseprofile betreffen die seriellen Diagnoseeinstellungen und sind nicht dasselbe wie die Modell- und Kanalkonfigurationen.

## Projektstruktur

| Pfad | Inhalt |
| --- | --- |
| `src/RemotiFyTransmitter_016.ino` | Firmware-Einstiegspunkt, Hardware- und UI-Initialisierung |
| `src/` | UI-Seiten, Kanalverarbeitung, Kommunikation, Pairing, Audio, Diagnose und Systemdienste |
| `include/` | Modul-Header und gemeinsame Datenstrukturen |
| `lib/` | Lokal eingebundene Bibliotheken, darunter TFT_eSPI und LVGL |
| `platformio.ini` | Build-Umgebung und PlatformIO-Einstellungen |
| `partitions.csv` | ESP32-Partitionslayout einschließlich LittleFS |

## Zusammenarbeit mit dem Empfänger

Der Sender kommuniziert mit einem RemotiFy-kompatiblen Empfänger über ESP-NOW. Pakettypen, Datenstrukturen und Konfigurationslayout müssen auf beiden Seiten zueinander passen. Änderungen an gemeinsam verwendeten Strukturen oder am Übertragungsprotokoll daher immer auch im Empfängerprojekt prüfen und gemeinsam testen. Funk-, Telemetrie- und Failsafe-Verhalten hängen von der jeweiligen Empfänger-Firmware ab.
