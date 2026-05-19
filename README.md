# Asset Tracker — rideyourstyle

Dieses Repository ist ein Fork des [Nordic Asset Tracker Template](https://github.com/nrfconnect/Asset-Tracker-Template).
Der nRF Cloud CoAP Cloud-Stack wurde durch ein eigenes HTTP-REST-Modul ersetzt, das Positionsdaten direkt an `tracking.rideyourstyle.ch` sendet.

## Was wurde geändert

| Datei | Änderung |
|---|---|
| `project/app/src/modules/cloud/cloud.c` | Komplett ersetzt: HTTP POST statt nRF Cloud CoAP |
| `project/app/src/modules/cloud/Kconfig.cloud` | Vereinfacht: CoAP-Optionen entfernt, REST-Konfiguration hinzugefügt |
| `project/app/src/modules/cloud/CMakeLists.txt` | Nur noch `cloud.c` (keine CoAP-Untermodule) |
| `project/app/overlay-rest.conf` | Build-Overlay: aktiviert REST-Modul, deaktiviert nRF Cloud |

Das Cloud-Modul:
- Abonniert `location_chan` → GNSS-Position wird als JSON per HTTP POST gesendet
- Cached optional Umweltdaten (Temp/Druck) und Akkustand
- Stellt Stub für FOTA-Kanal bereit (FOTA ohne nRF Cloud nicht unterstützt)
- Beantwortet Shadow-Requests mit leeren Responses, damit `main.c` nicht blockiert

### API-Endpunkt

```
PUT http://dev.tracking.rideyourstyle.ch/v1/trackers/{tracker_id}
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
- J-Link (USB direkt am Thingy:91 X)

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
- `CONFIG_NRF_PROVISIONING=n` / `CONFIG_MODEM_ATTEST_TOKEN=n` — deaktiviert nRF-Provisioning
- `CONFIG_APP_CLOUD=y` — aktiviert das REST-Cloud-Modul
- `CONFIG_NET_TCP=y` / `CONFIG_HTTP_CLIENT=y` — TCP + HTTP für den REST-Call
- `CONFIG_HW_ID_LIBRARY=y` / `CONFIG_HW_ID_LIBRARY_SOURCE_IMEI=y` — IMEI als Tracker-ID

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

## Debuggen / Logs

Das Build ist mit `CONFIG_UART_CONSOLE=y` konfiguriert — Logs gehen über USB-Serial, **nicht über RTT**. RTT benötigt einen J-Link-Probe; der ist ohne zusätzliche Hardware nicht verfügbar (das Board ist im UART/MCUboot-Modus).

### Serielle Konsole (115200 Baud)

Das Board meldet sich als `/dev/ttyACM0` (Logs + Shell) und `/dev/ttyACM1` (Modem-Trace):

```bash
screen /dev/ttyACM0 115200
```

Beenden mit `Ctrl-A` dann `K`.

### Nützliche Shell-Befehle (im seriellen Terminal)

```
# AT-Befehle direkt senden
at AT+CGSN          # IMEI lesen (= Tracker-ID in der API)
at AT+CEREG?        # LTE-Registrierungsstatus

# Sofort Location-Fix + HTTP POST triggern (langen Button-Druck simulieren)
att_button long

# App-internen Zustand anzeigen
att_inspect

# Netzwerk
att_network connect
att_network disconnect

# Storage
att_storage stats
att_storage flush
```

## Konfiguration

Alle REST-Parameter können per Kconfig angepasst werden (in `overlay-rest.conf` oder `prj.conf`):

| Kconfig-Option | Standard | Beschreibung |
|---|---|---|
| `CONFIG_APP_CLOUD_REST_SERVER_HOST` | `tracking.rideyourstyle.ch` | API-Hostname |
| `CONFIG_APP_CLOUD_REST_SERVER_PORT` | `80` | TCP-Port |
| `CONFIG_APP_CLOUD_REST_API_PATH` | `/v1/tracks/positions` | Endpunkt-Pfad |
| `CONFIG_APP_CLOUD_REST_HTTP_TIMEOUT_SECONDS` | `30` | HTTP-Timeout |
| `CONFIG_APP_CLOUD_REST_JSON_BUFFER_SIZE` | `512` | JSON-Puffergrösse |
| `CONFIG_APP_CLOUD_REST_TRACKER_ID_FALLBACK` | `nrf-tracker-unknown` | Fallback-ID wenn IMEI nicht lesbar |

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
│       ├── overlay-rest.conf    ← Build-Overlay für REST
│       ├── boards/              ← Board-spezifische Kconfigs (inkl. thingy91x)
│       └── sysbuild/            ← MCUboot-Konfiguration
├── nrf/                         ← NCS SDK (importiert via project/west.yml)
└── zephyr/                      ← Zephyr RTOS
```

Der Build läuft aus diesem Workspace selbst — Zephyr 4.3.99 und das lokale `nrf/` (mit den Thingy91x Board-Definitionen) sind bereits enthalten. Der NCS v3.1.1 Toolchain liefert nur den Compiler und west.
