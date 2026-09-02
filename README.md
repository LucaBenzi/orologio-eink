# Orologio E-Ink

Orologio da tavolo con display e-ink 3.7", ESP32-C3 e batteria LiFePO4. Si connette al WiFi, sincronizza l'ora via NTP e funziona per circa 2 anni con una singola ricarica USB-C di ~2 ore.

**Filosofia:** "Lo appoggi e funziona. Lo ricarichi ogni 2 anni."

## Caratteristiche

- Display e-ink 3.7" (240x416 px) — visibile in piena luce, consumo zero a riposo
- ESP32-C3 in deep sleep (~5 µA), wakeup ogni minuto per aggiornare il display
- Quarzo esterno 32.768 kHz per timekeeping preciso in deep sleep (~1.7 s/giorno di drift)
- Sync NTP automatica notturna per correggere il drift
- 2x batterie LiFePO4 18650 in parallelo (3600 mAh) — ricarica USB-C con TP5000
- Supporto per chimiche multiple: LiFePO4, Li-Po, 3x AA alcaline (configurabile via firmware e jumper)
- Configurazione WiFi via portale captive (WiFiManager) — nessun cavo seriale necessario
- Gestione automatica ora legale/solare con geolocalizzazione IP
- Telemetria opzionale su Google Sheets (tensione batteria, stato USB, posizione)
- PCB custom progettata in KiCad e prodotta su JLCPCB

## Hardware

| Componente | Modello |
|---|---|
| MCU | ESP32-C3-MINI-1 |
| Display | WeAct Studio 3.7" e-Paper (GDEY037T03, driver UC8253) |
| Batteria | 2x LiFePO4 18650 1800 mAh in parallelo |
| Caricatore | TP5000 (CC/CV, cutoff 3.6 V) |
| LDO | MCP1700-330 |
| Connettore | USB Type-C |
| Quarzo | 32.768 kHz SMD 3215 |

Lo schema completo dei pin e il circuito sono nel file [docs/specifiche.md](docs/specifiche.md).

## Struttura del repository

```
firmware/
  orologio_secondi.ino   # firmware Arduino (ESP32-C3)
  setup.html              # pagina di configurazione WiFi (portale captive)
  telemetria_apps_script.js  # Google Apps Script per telemetria
pcb/
  progetto.kicad_pro      # progetto KiCad 8
  progetto.kicad_sch      # schematico
  progetto.kicad_pcb      # layout PCB
  libs/                   # librerie custom e componenti LCSC
  loghi/                  # loghi SVG sulla board (CE, RoHS)
  production/             # Gerber, BOM, pick&place per JLCPCB
docs/
  specifiche.md           # specifiche tecniche complete
```

## Setup firmware

### Dipendenze (Arduino IDE / PlatformIO)

- Board: `esp32` by Espressif (ESP32-C3)
- [GxEPD2](https://github.com/ZinggJM/GxEPD2)
- [U8g2_for_Adafruit_GFX](https://github.com/olikraus/U8g2_for_Adafruit_GFX)
- [WiFiManager](https://github.com/tzapu/WiFiManager)

### Configurazione batteria

Nel file `orologio_secondi.ino`, decommentare la chimica corrispondente al pacco batteria montato:

```cpp
#define BATTERIA_LIFEPO4    // LiFePO4 (default)
//#define BATTERIA_LIPO     // Li-Po / Li-ion
//#define BATTERIA_3AA      // 3x stilo alcaline
```

La scelta va coordinata con i jumper sulla PCB (JP_DIODE, JP_LDO, JP_CS).

### Primo avvio

1. Alimentare l'orologio
2. Tenere premuto il pulsante per 3 secondi
3. Connettere il telefono alla rete WiFi "orologio"
4. Selezionare la propria rete WiFi e inserire la password
5. L'orologio si sincronizza via NTP e mostra l'ora

## PCB

Il progetto KiCad si trova in `pcb/`. La cartella `production/` contiene i file pronti per l'ordine su JLCPCB:

- `progetto.zip` — Gerber
- `bom.csv` — Bill of Materials con part number LCSC
- `positions.csv` — file pick & place

## Licenza

Questo progetto è open source. Vedi il file [LICENSE](LICENSE) per i dettagli.
