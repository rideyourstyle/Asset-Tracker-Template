# Asset Tracker — rideyourstyle

Dieses Repository ist ein Fork des [Nordic Asset Tracker Template](https://github.com/nrfconnect/Asset-Tracker-Template).
Der nRF Cloud CoAP Cloud-Stack wurde durch ein eigenes HTTPS-REST-Modul ersetzt, das Positionsdaten an `dev.tracking.rideyourstyle.ch` sendet.

## Was wurde geändert

| Datei | Änderung |
|---|---|
| `project/app/src/modules/cloud/cloud.c` | Komplett ersetzt: HTTPS REST, Peek-and-ACK, kompakter `tracker_record` |
| `project/app/src/modules/cloud/tracker_record.h` | Neues kompaktes Record-Struct (48 Bytes) mit allen API-Feldern |
| `project/app/src/modules/cloud/Kconfig.cloud` | CoAP-Optionen entfernt, REST/TLS/API-Key-Konfiguration hinzugefügt |
| `project/app/src/modules/cloud/CMakeLists.txt` | Nur noch `cloud.c` (keine CoAP-Untermodule) |
| `project/app/src/modules/storage/storage.h` | `STORAGE_BATCH_ACK` Message-Typ hinzugefügt |
| `project/app/src/modules/storage/storage.c` | Peek-and-ACK Protokoll: Records bleiben bis zur erfolgreichen Übertragung |
| `project/app/src/modules/storage/storage_data_types.*` | Nur noch ein Storage-Typ: `TRACKER` (statt BATTERY/ENVIRONMENTAL/LOCATION) |
| `project/app/src/main.c` | Sample-Timer auf Stunden-Boundaries ausgerichtet; Send-Trigger nach jedem Sample statt separatem Timer |
| `project/app/overlay-rest.conf` | Build-Overlay: REST, LittleFS-Storage, PSM, TLS |
| `project/app/boards/thingy91x_nrf9151_ns.overlay` | LittleFS-Partition auf 8 MB erweitert |

### Architektur: Aufzeichnen und Senden getrennt

Aufzeichnung und Übertragung laufen unabhängig:

- **Aufzeichnung**: GNSS-Fix alle `CONFIG_APP_SAMPLING_INTERVAL_SECONDS`, auf die volle Stunde synchronisiert (z.B. 30 s → :00, :30; 15 min → :00, :15, :30, :45)
- **Senden**: Nach jedem abgeschlossenen Sample wird geprüft, ob `CONFIG_APP_CLOUD_UPDATE_INTERVAL_SECONDS` seit dem letzten Sende-Vorgang vergangen sind. Kein separater Send-Timer — Senden findet immer an einem Sample-Boundary statt.
- **Erster Record**: Wird erstellt sobald die Uhrzeit nach dem Boot zum ersten Mal bekannt ist (LTE-/GNSS-Sync). Danach auf Stunden-Boundaries ausgerichtet.
- **Kein Netz**: Aufzeichnung läuft weiter — bis zu ~14 Tage Puffer im externen Flash
- **Nach Reconnect**: Alle gepufferten Fixes werden sofort übertragen (älteste zuerst)
- **Peek-and-ACK**: Ein Record wird erst aus dem Flash gelöscht, wenn der Server HTTP 2xx zurückgibt. Bei Fehler bleibt er für den nächsten Versuch erhalten.

`CONFIG_APP_SAMPLING_INTERVAL_SECONDS` muss ein ganzzahliger Teiler von 3600 sein — wird zur Compile-Zeit geprüft. Gültige Werte: 1, 2, 3, 4, 5, 6, 10, 12, 15, 20, 24, 30, 36, 40, 45, 60, 72, 90, 120, 150, 180, 300, 360, 600, 900, 1200, 1800, 3600.

### API-Endpunkt

```
PUT https://dev.tracking.rideyourstyle.ch/v1/trackers/{tracker_id}
Content-Type: application/json
X-Api-Key: <api-key>

{
  "sampleTimestamp": "2025-01-01T12:00:00.000Z",
  "latitude": 47.1234567,
  "longitude": 8.4564567,
  "pressure": 1013,
  "speed": 0,
  "temperature": 22,
  "gnssAcc": 10,
  "battery": 3700,
  "cellRssi": 0
}
```

Die Tracker-ID (`{tracker_id}`) wird aus der Modem-IMEI gelesen (`hw_id_get()`).
Mit `CONFIG_APP_CLOUD_REST_TRACKER_ID_OVERRIDE=y` und `CONFIG_APP_CLOUD_REST_TRACKER_ID_FALLBACK="90D0BE69"` kann sie fix gesetzt werden.

## Voraussetzungen

- nRF Util: `/home/peter/opt/nrfutil`
- NCS v3.1.1 Toolchain: `/home/peter/ncs/toolchains/v3.1.1` (liefert Compiler, CMake, west)

## Build

Der Build läuft aus dem **lokalen Workspace** (`asset-tracker-template`) — dort ist Zephyr 4.3.99 enthalten, das die aktuelle SMF-API unterstützt. Der NCS v3.1.1 Toolchain liefert nur den Compiler und west.

**1. In den Workspace wechseln:**
```bash
cd /home/peter/repos/tracky_two/asset-tracker-template
```

**2. Toolchain-Shell starten** (einmalig pro Terminal-Session):
```bash
/home/peter/opt/nrfutil toolchain-manager launch --ncs-version v3.1.1 --shell
```

**3. Bauen:**

Nur Code-Änderungen (schnell, ~15–30s — TF-M/MCUboot aus Cache):
```bash
west build -b thingy91x/nrf9151/ns -d project/app/build project/app -- -DEXTRA_CONF_FILE=overlay-rest.conf
```

Vollständiger Rebuild (nötig nach Kconfig-Änderungen oder Board-Wechsel, ~3–4 min):
```bash
west build --pristine -b thingy91x/nrf9151/ns -d project/app/build project/app -- -DEXTRA_CONF_FILE=overlay-rest.conf
```

### Was `overlay-rest.conf` macht

- `CONFIG_NRF_CLOUD=n` — deaktiviert nRF Cloud und alles was davon abhängt (CoAP, FOTA, AGNSS, Provisioning)
- `CONFIG_APP_CLOUD=y` — aktiviert das REST-Cloud-Modul
- `CONFIG_APP_CLOUD_REST_TLS=y` / Port 443 — HTTPS mit TLS-Offload im Modem
- `CONFIG_NET_TCP=y` / `CONFIG_HTTP_CLIENT=y` — TCP + HTTP für den REST-Call
- `CONFIG_HW_ID_LIBRARY=y` / `CONFIG_HW_ID_LIBRARY_SOURCE_IMEI=y` — IMEI als Tracker-ID
- `CONFIG_APP_STORAGE_BACKEND_LITTLEFS=y` — Positions-Puffer im externen SPI-Flash (8 MB, ~7 Tage)
- `CONFIG_LTE_PSM_REQ=y` — LTE Power Saving Mode (~2 µA im Schlaf zwischen Sendevorgängen)
- `CONFIG_NRF_MODEM_LIB_TRACE=n` — Modem-Traces deaktiviert (Produktion)

## Flashen

Das Thingy:91 X erscheint am USB als UART-Gerät (`mcuBoot`-Trait, kein J-Link). `west flash` funktioniert daher nicht. Stattdessen direkt mit `nrfutil`:

```bash
/home/peter/opt/nrfutil device program \
  --firmware project/app/build/app_image.hex \
  --traits mcuBoot \
  --options target=nRF91
```

Serial-Nummer des angeschlossenen Geräts anzeigen (falls mehrere Geräte angeschlossen):
```bash
/home/peter/opt/nrfutil device list
```

Dann mit expliziter Serial-Nummer flashen:
```bash
/home/peter/opt/nrfutil device program \
  --firmware project/app/build/app_image.hex \
  --serial-number <SERIAL> \
  --options target=nRF91
```

## LED-Anzeige

| Farbe | Bedeutung |
|---|---|
| **Blau** blinkt | GNSS-Suche läuft |
| **Grün** blinkt | HTTP-Batch wird gesendet |
| **Rot** blinkt | Kein Mobilfunknetz (wartet auf Reconnect) |
| **Lila** blinkt | FOTA-Download (nicht unterstützt in dieser Version) |
| Keine LED | Gerät schläft (wartet auf nächsten Sampling-Zyklus) |

Typischer Ablauf (30 s Intervall, 5 min Senden):
1. **Blau** — GNSS-Fix suchen (~2–10 s bei warmem Empfänger)
2. **Keine LED** — CPU und Modem schlafen bis zum nächsten Boundary
3. Nach 10 Samples kurz **Grün** — gespeicherte Fixes senden

## Stromverbrauch / Schlafverhalten

Das Gerät schläft während der Wartezeit zwischen Samples auf mehreren Ebenen:

| Komponente | Wartezustand | Stromverbrauch |
|---|---|---|
| ARM Cortex-M33 | Zephyr Idle → `WFI` (Wait For Interrupt) | < 1 mA |
| LTE-Modem | PSM (Power Saving Mode, T3324 = 0 s) | ~2–3 µA |
| UART-Konsole | **aktiv wenn USB angeschlossen** (VBUS vorhanden) | erhöht |
| UART-Konsole | **automatisch deaktiviert ohne USB** (`VBUS_REMOVED`) | gespart |

Die UART-Konsole (`ttyACM0`) ist nur sichtbar wenn ein USB-Kabel angeschlossen ist. Im Batteriebetrieb schaltet das Power-Modul beide UARTs automatisch ab (`CONFIG_APP_POWER_DISABLE_UART_ON_VBUS_REMOVED=y`). Der Modem schläft sofort nach dem Senden in PSM und wacht erst beim nächsten Sende-Zyklus auf.

## Debuggen / Logs

Logs gehen über USB-Serial (`CONFIG_UART_CONSOLE=y`), **nicht über RTT**.

### Serielle Konsole (115200 Baud)

Das Board meldet sich als `/dev/ttyACM0` (Logs + Shell), solange USB angeschlossen ist:

```bash
screen /dev/ttyACM0 115200
```

Beenden mit `Ctrl-A` dann `K`.

> Modem-Traces auf `ttyACM1` sind im Produktions-Build deaktiviert (`CONFIG_NRF_MODEM_LIB_TRACE=n`).

### Nützliche Shell-Befehle (im seriellen Terminal)

```
# AT-Befehle direkt senden
at AT+CGSN          # IMEI lesen (= Tracker-ID in der API)
at AT+CEREG?        # LTE-Registrierungsstatus
at AT+CPSMS?        # Aktuell gewährten PSM-Timer abfragen

# Sofort Location-Fix + Sende-Zyklus triggern (langen Button-Druck simulieren)
att_button long

# App-internen Zustand anzeigen
att_inspect

# Netzwerk
att_network connect
att_network disconnect

# Storage (LittleFS im externen Flash)
att_storage stats    # Anzahl gepufferter Records anzeigen
att_storage flush    # Alle Records ausgeben (Debug)
```

## Konfiguration

Alle Parameter können per Kconfig angepasst werden (in `overlay-rest.conf`):

| Kconfig-Option | Aktuell | Beschreibung |
|---|---|---|
| `CONFIG_APP_CLOUD_REST_SERVER_HOST` | `dev.tracking.rideyourstyle.ch` | API-Hostname |
| `CONFIG_APP_CLOUD_REST_SERVER_PORT` | `443` | TCP-Port |
| `CONFIG_APP_CLOUD_REST_TLS` | `y` | TLS/HTTPS (Modem-Offload) |
| `CONFIG_APP_CLOUD_REST_API_KEY` | *(gesetzt)* | X-Api-Key Header |
| `CONFIG_APP_CLOUD_REST_API_PATH` | `/v1/trackers` | Endpunkt-Pfad |
| `CONFIG_APP_CLOUD_REST_GNSS_MIN_ACCURACY_METERS` | `25` | Min. Genauigkeit in Metern (0 = immer senden) |
| `CONFIG_APP_CLOUD_REST_HTTP_TIMEOUT_SECONDS` | `30` | HTTP-Timeout |
| `CONFIG_APP_CLOUD_REST_TRACKER_ID_OVERRIDE` | `y` | Feste Tracker-ID verwenden |
| `CONFIG_APP_CLOUD_REST_TRACKER_ID_FALLBACK` | `90D0BE69` | Feste Tracker-ID |
| `CONFIG_APP_SAMPLING_INTERVAL_SECONDS` | `30` | GNSS-Aufzeichnungsintervall (muss Teiler von 3600 sein) |
| `CONFIG_APP_CLOUD_UPDATE_INTERVAL_SECONDS` | `300` | Mindestabstand zwischen zwei Sendevorgängen |
| `CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE` | `20000` | Max. gepufferte Records (~14 Tage) |
| `CONFIG_APP_STORAGE_INITIAL_THRESHOLD` | `0` | Storage-Threshold deaktiviert (Senden via Zeitintervall) |
| `CONFIG_PM_PARTITION_SIZE_LITTLEFS` | `0x800000` | Flash-Partition (8 MB) |
| `CONFIG_LTE_PSM_REQ_RPTAU` | `00100001` | PSM-Timer: 10 min Schlaf |

## Storage: Externer SPI-Flash

Das Thingy:91 X hat einen **GD25LE255E (32 MB)** SPI-NOR-Flash. Die Firmware verwendet davon 8 MB für LittleFS.

| | |
|---|---|
| Flash-Chip | GD25LE255E, 32 MB |
| Partition | 8 MB (`0x800000`) |
| Kapazität | ~20 000 Records |
| Pufferdauer | **~14 Tage** bei 30 s Intervall |
| Stromausfall-sicher | Ja — Daten bleiben bei Neustart erhalten |
| Sendegarantie | Peek-and-ACK: Record bleibt bis HTTP 2xx bestätigt |

## CI/CD mit Jenkins

Das `Jenkinsfile` liegt im Repository-Root (`project/Jenkinsfile`).

### Jenkins-Job einrichten

1. Neuen **Pipeline**-Job erstellen
2. Unter *Pipeline*: „Pipeline script from SCM" wählen
3. SCM: Git, Repository-URL eintragen
4. **Script Path**: `project/Jenkinsfile`

> `skipDefaultCheckout(true)` ist im Jenkinsfile gesetzt — Jenkins checkt **nicht** automatisch ins Workspace-Root aus. Das Jenkinsfile erledigt den Checkout selbst in das Unterverzeichnis `project/`, damit der west-Workspace korrekt aufgebaut wird.

### Workspace-Struktur (Jenkins)

```
WORKSPACE/              ← west Workspace-Root
├── .west/              ← west-Marker (von west init)
├── project/            ← git-Checkout (dieses Repo)
│   ├── app/
│   ├── west.yml
│   ├── Jenkinsfile
│   └── ...
├── nrf/                ← von west update (~5 GB)
├── zephyr/             ← von west update
└── modules/ ...        ← von west update
```

### Pipeline-Parameter

| Parameter | Standard | Beschreibung |
|---|---|---|
| `CLEAN_WORKSPACE` | `false` | Löscht den gesamten west-Workspace (ausser `project/`) und lädt alle SDK-Abhängigkeiten neu herunter (~10–20 min). Nötig bei SDK-Versionswechsel oder korruptem Workspace. |
| `PRISTINE` | `false` | Vollständiger Firmware-Rebuild (`--pristine`). Nötig nach Kconfig-Änderungen in `overlay-rest.conf` oder `prj.conf`. |
| `FLASH` | `false` | Flasht das angeschlossene Thingy:91 X nach erfolgreichem Build. Nur sinnvoll wenn der Jenkins-Agent direkt per USB am Gerät hängt. |

### Pipeline-Stages

| Stage | Beschreibung |
|---|---|
| **Checkout** | Checkt das Repository in `WORKSPACE/project/` aus |
| **West setup** | Initialisiert den west-Workspace (`west init -l project/`) und aktualisiert alle SDK-Abhängigkeiten (`west update`) |
| **Build** | Baut die Firmware; archiviert `app_image.hex` als Build-Artefakt |
| **Flash** | Flasht via `nrfutil device program` (nur wenn `FLASH=true`) |

## Projektstruktur

```
asset-tracker-template/          ← west Workspace-Root (dieses Repo)
├── project/                     ← west manifest + App-Code
│   └── app/
│       ├── src/modules/cloud/   ← REST Cloud-Modul (rideyourstyle)
│       ├── src/modules/storage/ ← Storage-Modul mit Peek-and-ACK
│       ├── overlay-rest.conf    ← Build-Overlay für REST
│       ├── boards/              ← Board-spezifische Kconfigs (inkl. thingy91x)
│       └── sysbuild/            ← MCUboot-Konfiguration
├── nrf/                         ← NCS SDK (importiert via project/west.yml)
└── zephyr/                      ← Zephyr RTOS
```

Der Build läuft aus diesem Workspace selbst — Zephyr 4.3.99 und das lokale `nrf/` (mit den Thingy91x Board-Definitionen) sind bereits enthalten. Der NCS v3.1.1 Toolchain liefert nur den Compiler und west.
