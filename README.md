# Orologio E-Ink

E-ink desk clock with a 3.7" display, ESP32-C3 and LiFePO4 battery. Connects to WiFi, syncs time via NTP and runs for about 2 years on a single ~2-hour USB-C charge.

**Philosophy:** "Place it and it works. Recharge it every 2 years."

## Features

- 3.7" e-ink display (240x416 px) — sunlight readable, zero power draw at rest
- ESP32-C3 in deep sleep (~5 µA), wakes up every minute to update the display
- External 32.768 kHz crystal for accurate deep-sleep timekeeping (~1.7 s/day drift)
- Automatic nightly NTP sync to correct drift
- 2x LiFePO4 18650 cells in parallel (3600 mAh) — USB-C charging via TP5000
- Multiple battery chemistry support: LiFePO4, Li-Po, 3x AA alkaline (configurable via firmware and jumpers)
- WiFi setup via captive portal (WiFiManager) — no serial cable needed
- Automatic DST handling with IP geolocation
- Optional telemetry to Google Sheets (battery voltage, USB status, location)
- Custom PCB designed in KiCad and manufactured on JLCPCB

## Hardware

| Component | Model |
|---|---|
| MCU | ESP32-C3-MINI-1 |
| Display | WeAct Studio 3.7" e-Paper (GDEY037T03, driver UC8253) |
| Battery | 2x LiFePO4 18650 1800 mAh in parallel |
| Charger IC | TP5000 (CC/CV, cutoff 3.6 V) |
| LDO | MCP1700-330 |
| Connector | USB Type-C |
| Crystal | 32.768 kHz SMD 3215 |

Full pinout and circuit details are in [docs/specifiche.md](docs/specifiche.md).

## Repository structure

```
firmware/
  orologio_secondi.ino      # Arduino firmware (ESP32-C3)
  setup.html                 # WiFi configuration page (captive portal)
  telemetria_apps_script.js  # Google Apps Script for telemetry
pcb/
  progetto.kicad_pro         # KiCad 8 project
  progetto.kicad_sch         # schematic
  progetto.kicad_pcb         # PCB layout
  libs/                      # custom libraries and LCSC components
  loghi/                     # SVG logos on the board (CE, RoHS)
  production/                # Gerber, BOM, pick & place for JLCPCB
docs/
  specifiche.md              # full technical specifications
```

## Firmware setup

### Dependencies (Arduino IDE / PlatformIO)

- Board: `esp32` by Espressif (ESP32-C3)
- [GxEPD2](https://github.com/ZinggJM/GxEPD2)
- [U8g2_for_Adafruit_GFX](https://github.com/olikraus/U8g2_for_Adafruit_GFX)
- [WiFiManager](https://github.com/tzapu/WiFiManager)

### Battery configuration

In `orologio_secondi.ino`, uncomment the line matching the installed battery pack:

```cpp
#define BATTERIA_LIFEPO4    // LiFePO4 (default)
//#define BATTERIA_LIPO     // Li-Po / Li-ion
//#define BATTERIA_3AA      // 3x AA alkaline
```

This must match the jumper configuration on the PCB (JP_DIODE, JP_LDO, JP_CS).

### First boot

1. Power on the clock
2. Hold the button for 3 seconds
3. Connect your phone to the "orologio" WiFi network
4. Select your home WiFi and enter the password
5. The clock syncs via NTP and displays the time

## PCB

The KiCad project is in `pcb/`. The `production/` folder contains files ready for ordering on JLCPCB:

- `progetto.zip` — Gerber files
- `bom.csv` — Bill of Materials with LCSC part numbers
- `positions.csv` — pick & place file

## License

This project is open source. See the [LICENSE](LICENSE) file for details.
