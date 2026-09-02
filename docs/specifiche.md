# Nome da Definire — Specifiche Tecniche
**Versione:** 1.0 — Prototipo  
**Data:** Maggio 2026  
**Autore:** Luca

---

## 1. Descrizione Generale

Nome da Definire è un orologio da tavolo digitale con display e-ink, alimentato da doppia batteria LiFePO4 ricaricabile via USB-C, con ESP32-C3 come CPU. Il dispositivo si connette al WiFi domestico per sincronizzare automaticamente l'ora tramite NTP e gestisce il cambio ora legale/solare in modo autonomo. Una ricarica di 2 ore via USB-C garantisce circa 2 anni di funzionamento.

**Filosofia di prodotto:** "Lo appoggi e funziona. Lo ricarichi ogni 2 anni."

### 1.1 Descrizione Tecnica del funzionamento

Il cliente una volta ricevuto l'orologio si trova sullo schermo le istruzioni da seguire:
1. tieni premuto il pulsante sul fondo per tre secondi
2. connetti il tuo telefono alla rete che sto generando: "orologio".
3. seleziona la tua wifi di casa
4. inserisci la password del tuo wifi di casa
FINE

A questo punto l'orologio si connette a un server NTP, riceve e mostra l'orario sulla base dell'ora solare e posizione geografica.

Al primo avvio il display mostra "00:00" immediatamente (~2s), poi si aggiorna con l'ora corretta dopo la sync NTP.

Il display si refresha una volta al minuto aggiornando il minuto. L'ESP è in deep sleep e si sveglia soltanto per aggiornare il display. Un quarzo esterno 32.768 kHz garantisce un drift di ~1.7s/giorno durante il deep sleep.

Di notte alle 3:00 si connette all'NTP e regola l'ora per spaccare il minuto.

Il pulsante sottostante funziona tramite interrupt (deep sleep wakeup). Se l'utente lo tiene premuto per più di 1.5 secondi genera di nuovo la wifi per un po' di tempo e l'utente si può connettere e cambiare rete.


---

## 2. Hardware

### 2.1 Microcontrollore
| Parametro | Valore |
|---|---|
| Modello | ESP32-C3-MINI-1 (produzione) / ESP32-C3 SuperMini (prototipo) |
| Architettura | RISC-V 32-bit |
| Clock | 160 MHz |
| Flash | 4 MB |
| RAM | 400 KB SRAM |
| Connettività | WiFi 802.11 b/g/n 2.4 GHz |
| Tensione operativa | 3.0 – 3.6 V |
| Deep sleep consumption | ~5 µA |

**Schema di alimentazione a batteria (bypass ME6217):** La scheda Super Mini monta un LDO ME6217 con dropout ~280 mV a pieno carico (molto meno ai carichi µA–mA): per erogare 3.3V stabili anche durante i picchi WiFi servirebbe un ingresso ≳3.5V, e con la LiFePO4 a nominale (3.2V) andrebbe in dropout. La soluzione adottata è collegare la batteria **direttamente al pin 3.3V dell'header** tramite un diodo Schottky BAT60J (Vf ≈ 0.12V @ 100 mA), bypassando completamente il ME6217.

| Stato batteria | Tensione batteria | Tensione ESP32 (dopo BAT60J) |
|---|---|---|
| Carica completa (TP5000 cutoff) | 3.6V | **3.48V** — ben sotto abs max 3.6V |
| Nominale | 3.2V | **3.08V** — nella finestra operativa |
| Scarica | < 3.2V | < 3.08V — sotto il plateau LiFePO4 |

Il firmware rileva la tensione bassa via ADC (partitore collegato direttamente alla batteria, prima del diodo) e mostra l'icona low-battery a **3.2V sulla batteria** (fine del plateau piatto LiFePO4). L'ESP32-C3 crasha per brownout prima che il display smetta di funzionare.

**Funzionamento con USB collegata:** il TP5000 è collegato ai pin VBUS e GND del SuperMini — un solo cavo USB gestisce sia la ricarica che l'alimentazione. Con USB inserita: il ME6217 alimenta il rail a 3.3V tramite la via normale (VBUS → BAT60J onboard → VSYS → LDO), mentre il TP5000 carica la batteria. Il BAT60J esterno è polarizzato inversamente finché la batteria è sotto ~3.5V, quindi nessun conflitto. A fine carica (~3.6V), la batteria attraverso il BAT60J (~3.48V) diventa la sorgente dominante e il ME6217 si disattiva naturalmente. Con USB scollegata: il ME6217 non ha ingresso e la batteria tramite BAT60J alimenta tutto in autonomia.

### 2.2 Display
| Parametro | Valore |
|---|---|
| Modello | WeAct Studio 3.7" e-Paper (E037A75) |
| Driver IC | UC8253 |
| Libreria | GxEPD2 (GxEPD2_370_GDEY037T03) |
| Risoluzione | 240 × 416 pixel |
| Colori | Bianco e nero |
| Interfaccia | SPI 4-wire |
| Refresh completo | ~2-3 secondi |
| Refresh parziale | ~0.3 secondi |
| Consumo statico | ~0 µA (mantiene immagine senza alimentazione) |
| VCC range operativo | 2.4V – 3.6V (Typ 3.0V) |
| Consumo in aggiornamento | max 8 mA (200 mAs per refresh) |
| Stand-by (dopo POF) | ~26 µA (DSLP non usato: causa artefatti al risveglio) |
| Temperatura operativa | 0 – 40 °C (datasheet §8) |

**Compatibilità tensione confermata (datasheet E037A75, sez. 9):** il VCC del display accetta 2.4V–3.6V. Il nostro rail 3.0V–3.4V è esattamente al centro della finestra operativa. Il modulo non monta LDO onboard: VCC va direttamente al driver IC e al boost converter interno che genera le tensioni di pilotaggio del pannello.

### 2.3 Batteria
| Parametro | Valore |
|---|---|
| Tipo | LiFePO4 3.2V |
| Configurazione | 2 celle in parallelo |
| Capacità totale | 3600 mAh (11520 mWh) |
| Formato | 18650 |
| Tensione nominale | 3.2 V |
| Tensione di carica max | 3.6 V |
| Autoscarica stimata | ~1-2%/mese |

### 2.4 Circuito di Ricarica USB-C
| Parametro | Valore |
|---|---|
| Connettore | USB Type-C |
| IC caricatore | TP5000 |
| Corrente di carica | 900 mA |
| Tensione di taglio | 3.6 V (LiFePO4) |
| Tempo di ricarica completa | ~2.5 ore |
| Topologia | CC/CV con cutoff automatico |

Il TP5000 gestisce autonomamente il profilo di carica CC/CV per LiFePO4, taglia a 3.6V, e accetta ingressi da USB 5V. Costo aggiuntivo: ~0.50€ (connettore + IC).

### 2.5 Partitore Resistivo Lettura Batteria

| Parametro | Valore |
|---|---|
| R1 (batteria → ADC) | 1 MΩ |
| R2 (ADC → GND) | 2.2 MΩ |
| Condensatore ADC | 100 nF (tra pin ADC e GND) |
| Corrente di scarica | ~1 µA (costante) |
| Impatto su autonomia | ~0.9% (~6 giorni su 700) |
| Collegamento | Sempre attivo, prima del diodo BAT60J (misura tensione batteria diretta) |

Il partitore scala la tensione batteria (3.2–3.6V) nel range ADC dell'ESP32-C3. La soglia software di low-battery è **3.2V sulla batteria** (fine del plateau piatto LiFePO4). L'ADC usa un riferimento interno bandgap indipendente da Vdd, quindi le letture restano accurate anche quando la tensione di alimentazione dell'ESP32 cala. Resistenze alte (MΩ) minimizzano la corrente di scarica. Il condensatore da 100 nF compensa l'alta impedenza della sorgente (R_th = 687 kΩ) fornendo carica al condensatore di campionamento interno dell'ADC (~7 pF). Tempo di assestamento: ~350 ms.

**Nota:** GPIO0 e GPIO1 sono riservati al quarzo esterno 32.768 kHz (XTAL_32K_P/N). GPIO2 è usato per il rilevamento VBUS (partitore 10kΩ+15kΩ). L'ADC batteria è su GPIO3 (ADC1_CH3) e i segnali display D/C, RES, BUSY sono su GPIO10, GPIO20, GPIO21.

### 2.6 Connessioni Pin

| GPIO | Funzione | Note |
|---|---|---|
| GPIO0 | XTAL_32K_P | Quarzo esterno 32.768 kHz |
| GPIO1 | XTAL_32K_N | Quarzo esterno 32.768 kHz |
| GPIO2 | Rilevamento USB (partitore 10kΩ+15kΩ da VBUS) | Deep sleep wakeup HIGH. **SuperMini: rimuovere UR15** (pull-up 3.3V→GPIO2, causa ~250 µA in deep sleep). PCB produzione: non montare pull-up su GPIO2. |
| GPIO3 | ADC batteria (partitore 1MΩ+2.2MΩ+100nF) | ADC1_CH3 |
| GPIO4 | SCK (SPI display) | |
| GPIO5 | Pulsante reset WiFi (attivo HIGH, 10kΩ pull-down esterno) | Deep sleep wakeup HIGH, MTDI/JTAG |
| GPIO6 | MOSI (SPI display) | |
| GPIO7 | CS (SPI display) | SS |
| GPIO8 | Libero (era VBUS, spostato su GPIO2) | LED onboard sulla SuperMini |
| GPIO9 | STDBY TP5000 (carica completata = LOW) | digitalRead, pull-up interno |
| GPIO10 | D/C display | |
| GPIO20 | RES display | **PCB: pull-up 100kΩ a VCC** (GPIO non-RTC, flotta in deep sleep; il pull-up interno UC8253 da 200kΩ è debole) |
| GPIO21 | BUSY display | |
| 3.3V | VCC display | |
| GND | GND display | |
| — | USB-C → TP5000 → LiFePO4 → BAT60J → ESP 3.3V pin | |

### 2.7 PCB di Produzione

Il prototipo utilizza la dev board ESP32-C3 SuperMini come modulo, con breakout board TP5000 separata e componenti collegati su breadboard/millefori. Per la produzione si passa a un **PCB unico** con tutti i componenti integrati.

#### Differenze prototipo → produzione

| Componente | Prototipo | Produzione |
|---|---|---|
| Microcontrollore | ESP32-C3 SuperMini (dev board) | **ESP32-C3-MINI-1** (modulo con antenna integrata) |
| Caricatore batteria | Breakout board TP5000 | **TP5000 integrato su PCB** (SOT-23-6 + ~5 componenti passivi) |
| LDO ME6217 | Presente sulla SuperMini (bypassato) | **Assente** — alimentazione diretta batteria → BAT60J → VDD |
| LED di stato | Da rimuovere manualmente dalla SuperMini | **Non montati** |
| Connettore USB-C | Sulla SuperMini | **Integrato sul PCB** |

#### BOM componenti principali

| Componente | Package | Funzione | Note |
|---|---|---|---|
| ESP32-C3-MINI-1 | Modulo 13×16.6 mm | MCU + WiFi + antenna + flash + quarzo interno | Antenna PCB integrata, keep-out da rispettare (vedi datasheet) |
| TP5000 | SOT-23-6 | Caricatore LiFePO4 CC/CV | Pin CS floating per modalità 3.6V |
| RPROG (TP5000) | 0402/0603 | Imposta corrente di carica 900 mA | Valore da datasheet TP5000 |
| CIN (TP5000) | 0805 | Condensatore ingresso TP5000 | ~10 µF ceramico |
| COUT (TP5000) | 0805 | Condensatore uscita TP5000 | ~10 µF ceramico |
| BAT60J | SOD-323 | Diodo Schottky bypass alimentazione | Vf ~0.12V @ 100 mA |
| Quarzo 32.768 kHz | SMD 3215/2012 | Clock RTC per deep sleep | Condensatori di carico da ricalcolare per il quarzo scelto |
| Cx2 (quarzo) | 0402 | Condensatori di carico quarzo | Tipicamente 6.8–12.5 pF (dipende dal quarzo) |
| R partitore batteria | 0402 | 1 MΩ + 2.2 MΩ | Lettura ADC tensione batteria |
| C partitore batteria | 0402 | 100 nF | Compensazione impedenza sorgente ADC |
| R partitore VBUS | 0402 | 10 kΩ + 15 kΩ | Rilevamento USB collegato (GPIO2) |
| R pull-down pulsante | 0402 | 10 kΩ | Pull-down GPIO5 (vince pull-up JTAG interno) |
| R pull-up RST display | 0402 | 100 kΩ a VCC | **Critico:** GPIO20 è non-RTC, flotta in deep sleep. Previene glitch reset UC8253 |
| Connettore USB-C | SMD | Alimentazione + ricarica | Solo power, no data (2 pin + GND + shield) |
| Connettore batteria | JST PH 2.0 / pad | Collegamento celle LiFePO4 | |
| Connettore display | FPC/header | SPI 7 pin + VCC + GND | |
| Pulsante | SMD tact switch | Reset WiFi (pressione lunga >1.5s) | Montato sul fondo dell'orologio |

#### Note di layout PCB

- **Keep-out antenna:** l'ESP32-C3-MINI-1 richiede un'area libera da rame (GND e segnali) e componenti davanti all'antenna. Le quote esatte sono nel datasheet del modulo. Posizionare il modulo sul bordo del PCB con l'antenna che sporge oltre il piano di massa.
- **Piano di massa:** ground plane continuo sotto il modulo (esclusa zona antenna). Via di stitching attorno al modulo per ridurre impedenza GND.
- **TP5000:** posizionare vicino al connettore USB-C, tracce larghe per la corrente di carica (900 mA).
- **Partitore batteria:** posizionare vicino a GPIO3 per minimizzare rumore sull'ADC.
- **Condensatori di disaccoppiamento:** 100 nF + 10 µF vicini ai pin di alimentazione dell'ESP32-C3-MINI-1.

---

## 3. Bilancio Energetico

### 3.1 Consumo giornaliero

Stime basate su datasheet ESP32-C3, datasheet display E037A75/UC8253 (max 8 mA, 200 mAs per refresh) e firmware produzione con tutte le ottimizzazioni implementate (OPT 1-4, dettagli in ottimizzazioni.md):

- **OPT-1:** Light sleep durante BUSY display (ESP32 a 130 µA invece di 22 mA durante i 450 ms di refresh)
- **OPT-2:** Reset duration ridotta da 50 ms a 2 ms (init da 110 ms a 22 ms)
- **OPT-3:** Finestra parziale full-screen (anti-ghosting: waveform di mantenimento su tutto il pannello)
- **OPT-4:** SPI clock da 4 MHz a 10 MHz

| Voce | Dettaglio | Frequenza | Energia/giorno |
|---|---|---|---|
| Deep sleep (ESP32 + UC8253 stand-by + partitore) | ~32 µA @ 3.2V, ~59.5 s/min | continuo | 2.5 mWh |
| Wake: attivo (boot + SPI) | ~22 mA @ 3.2V, ~50 ms | 1439×/giorno | 1.6 mWh |
| Wake: light sleep (BUSY) | ~5.7 mA @ 3.2V, ~450 ms | 1439×/giorno | 3.2 mWh |
| Sync NTP (WiFi + full refresh) | ~100 mA @ 3.2V, ~7 s | 1×/settimana | 0.09 mWh |
| **Totale** | | | **~7.4 mWh/giorno** |

Consumo medio: **~2.3 mAh/giorno @ 3.2V**

**Nota deep sleep:** ESP32-C3 ~5 µA + UC8253 stand-by ~26 µA (dopo POF, non in DSLP) + partitore batteria ~1 µA. Il DSLP del UC8253 (~3 µA) non è utilizzabile perché richiede full refresh al risveglio (flash schermo ogni minuto).

### 3.2 Autonomia

Batteria: 2× LiFePO4 1800 mAh in parallelo = **3600 mAh (11520 mWh)**.

Considerando l'autoscarica delle celle (~1-2%/mese), il consumo effettivo è ~11-14 mWh/giorno.

| Parametro | Valore |
|---|---|
| Consumo circuito | 7.4 mWh/giorno |
| Autoscarica stimata | ~2-3.5 mWh/giorno |
| Consumo effettivo totale | ~10.2 mWh/giorno |
| **Autonomia stimata** | **~1130 giorni (~3.1 anni)** |

### 3.3 Ricarica USB-C

| Parametro | Valore |
|---|---|
| Corrente di carica (TP5000) | 900 mA |
| Capacità da ricaricare | 3600 mAh |
| Tempo di ricarica completa | ~4 ore |

**Claim: "2 ore di ricarica USB-C ogni 2 anni."**

**Nota:** con le celle da 1800 mAh l'autonomia reale è ~2.9 anni con carica completa. Il claim "2 ore, 2 anni" è conservativo e resta valido anche con una singola cella o carica parziale. Le misure reali di corrente (INA226) determineranno la configurazione finale (1 o 2 celle).

---

## 4. Firmware

### 4.1 Stack Software
| Libreria | Versione | Scopo |
|---|---|---|
| GxEPD2 | latest | Driver display e-ink |
| U8g2_for_Adafruit_GFX | latest | Font vettoriali |
| WiFiManager (tzapu) | latest | Configurazione WiFi |
| Arduino ESP32 Core | latest | Framework base |

### 4.2 Architettura Software

Il firmware è basato su un ciclo **deep sleep / wakeup** — il loop() non viene mai eseguito.

---

## 5. Roadmap

### 5.0 Configurazione modulo TP5000 per LiFePO4 (URGENTE — prima di collegare la batteria)

Il TP5000 supporta sia Li-ion (4.2V) che LiFePO4 (3.6V). La selezione avviene tramite il **pin CS (pin 13)**:

| CS (pin 13) | Modalità | Tensione di cutoff |
|---|---|---|
| Collegato a VIN (HIGH) | Li-ion | 4.2V |
| **Floating** (non collegato) | **LiFePO4** | **3.6V** |
| Collegato a GND (LOW) | Shutdown | Off |

I moduli breakout cinesi arrivano tipicamente con CS collegato a VIN (modalità Li-ion 4.2V). **Caricare una LiFePO4 a 4.2V la danneggia e rischia il rigonfiamento.**

**Procedura di verifica:**
1. Individuare sulla board il jumper/ponte di saldatura vicino al pad CS o alla serigrafia "4.2V/3.6V"
2. Se CS è collegato a VIN → tagliare la pista o rimuovere il ponte di saldatura
3. Se presente un jumper a 3 posizioni → spostarlo su 3.6V
4. Verificare con multimetro: CS non deve avere continuità né con VIN né con GND
5. **Test senza batteria:** collegare USB, misurare tensione su BAT+ — deve stabilizzarsi a ~3.6V, non ~4.2V

### 5.1 Quarzo esterno 32.768 kHz (IMPLEMENTATO — 19/06/2026)

**Hardware:** quarzo cilindrico 32.768 kHz su GPIO0 (XTAL_32K_P) e GPIO1 (XTAL_32K_N) con condensatori di carico 2× 18 pF. Per il PCB finale i condensatori andranno ricalcolati in base al quarzo scelto (tipicamente 6.8–12.5 pF per quarzi SMD).

**Firmware:** al power cycle il quarzo viene abilitato e selezionato come clock RTC:
```
rtc_clk_32k_enable(true);
rtc_clk_32k_bootstrap(512);
delay(500);  // stabilizzazione quarzo
rtc_clk_slow_src_set(SOC_RTC_SLOW_CLK_SRC_XTAL32K);
```
Ai risvegli da deep sleep non serve riconfigurare — il quarzo resta alimentato dal dominio RTC.

**Benefici rispetto all'oscillatore RC interno:**
| | RC interno (~136 kHz) | Quarzo 32.768 kHz |
|---|---|---|
| Drift tipico | ±5% (~1-3 min/giorno) | ±20 ppm (~1.7 s/giorno) |
| Sensibilità a temperatura | Alta | Bassa |
| Funzionamento offline | Giorni | Mesi |

**Verifica:** il campo telemetria `xtal` conferma quale clock è attivo (1 = quarzo, 0 = RC). Il campo `drift` mostra il drift reale tra due sync NTP.

### 5.2 Ridurre frequenza sync NTP — IMPLEMENTATA

Con il quarzo installato, la sync NTP passa da giornaliera a **settimanale** (ogni 7 giorni). Drift atteso: ~12s/settimana — invisibile per un orologio che mostra solo ore:minuti. Riduce il numero di connessioni WiFi e il consumo energetico.

**Implementazione firmware:**
- **User mode:** telemetria inviata ogni giorno alle 3:00 (`drift=0`), NTP sync solo quando `giorniDaBoot % 7 == 0` (con drift misurato)
- **Dev mode:** telemetria ogni 60 risvegli (~90s), NTP sync ogni 7 "giorni simulati" (10080 risvegli = ~4.2 ore reali con `TEMPO_ACCELERATO=40`)
- Il campo `drift` nella telemetria è diverso da zero solo al momento della sync NTP, permettendo di misurare la deriva accumulata del quarzo

### 5.3 Telemetria su Google Sheets

L'invio della telemetria avviene subito dopo ogni chiamata a `leggiBatteria()`: in **produzione** una volta al giorno (alle 3:00, con WiFi già acceso per NTP), in **dev mode** ogni 60 risvegli (~1 minuto con `TEMPO_ACCELERATO=40`), permettendo il monitoraggio in tempo reale durante lo sviluppo.

**Infrastruttura:** Google Sheets + Google Apps Script. Uno script deployato come Web App riceve i dati via HTTP GET dall'ESP32 e appende una riga al foglio con timestamp automatico. L'URL dello script è l'unico parametro da salvare nel firmware. Limiti free: 20.000 esecuzioni/giorno (ampiamente sufficiente anche in dev mode a ~1440 invii/giorno). Il foglio permette filtraggio per dispositivo (MAC), grafici e analisi senza strumenti esterni.

**Geolocalizzazione via IP:** prima dell'invio telemetria, l'ESP32 chiama `ip-api.com/json` (gratuito, no API key) che restituisce latitudine, longitudine e città dall'IP pubblico della rete WiFi. Precisione: livello città (~5–50 km). Costo: ~1–2 s extra con WiFi già acceso.

**Endpoint ESP32:**
```
https://script.google.com/macros/s/.../exec?mac=AA:BB:CC&v=3.18&drift=2&rssi=-65&temp=28&fw=1.0&days=42&lat=45.46&lon=9.19&city=Milan
```

| Campo | Parametro | Sorgente | Note |
|---|---|---|---|
| MAC address | `mac` | `WiFi.macAddress()` | Identificativo univoco del dispositivo |
| Tensione rail | `v` | ADC GPIO3 (partitore 1MΩ/2.2MΩ) | Già implementata (`leggiBatteria()`) |
| Drift pre-sync | `drift` | Differenza RTC vs NTP | Con quarzo 32K il drift atteso è ~1-2s/giorno |
| RSSI WiFi | `rssi` | `WiFi.RSSI()` | Qualità segnale, diagnostica connessioni fallite |
| Temperatura | `temp` | `temperatureRead()` | Sensore interno ESP32-C3, leggere subito al wakeup prima che il chip si scaldi |
| Versione firmware | `fw` | Costante `#define` | Per tracciare aggiornamenti |
| Giorni da boot | `days` | `RTC_DATA_ATTR` contatore | Rileva riavvii inattesi (brownout, crash) |
| Latitudine | `lat` | `ip-api.com` JSON response | Geolocalizzazione IP (~5–50 km) |
| Longitudine | `lon` | `ip-api.com` JSON response | Geolocalizzazione IP (~5–50 km) |
| Città | `city` | `ip-api.com` JSON response | Leggibilità nel foglio |
| Quarzo 32K | `xtal` | `rtc_clk_slow_src_get()` | `1` = quarzo esterno attivo, `0` = RC interno |

### 5.4 Notifica batteria scarica sul display (IMPLEMENTATA)

Quando la tensione batteria scende sotto **3.2V** (fine del plateau piatto LiFePO4, costante `SOGLIA_BATTERIA_SCARICA`), viene mostrata un'icona di batteria vuota in basso a destra sul display. L'icona è composta da due rettangoli disegnati con le primitive GxEPD2: un corpo 22×12 px (`drawRect`) e un polo positivo 3×6 px (`fillRect`). L'interno è vuoto per comunicare immediatamente la necessità di ricarica.

**Visibilità:** l'icona viene disegnata durante il full refresh (boot + 3:00 AM) e resta visibile tra i partial refresh perché la finestra parziale copre solo la zona delle cifre. In DEV_MODE l'icona compare a sinistra della tensione numerica.

**Rilevamento USB e ciclo ricarica:** il pin **GPIO2** è collegato a VBUS tramite un partitore resistivo 10kΩ + 15kΩ (5V → ~3.0V, leggibile come HIGH). Quando il cavo USB viene collegato, il deep sleep GPIO wakeup (HIGH level) sveglia l'ESP32 istantaneamente. Il firmware entra in `cicloRicarica()`: resta sveglio, legge la batteria ogni 5s, controlla STDBY del TP5000 (GPIO9, LOW = carica completa), e aggiorna il display ogni minuto con un'animazione delle tacche batteria (1→2→3→1). Quando il cavo viene staccato, il firmware esce dal loop, legge la batteria, e se la tensione è sopra soglia resetta `batteriaScaricaMostrata`.

**Deep sleep wakeup:** pulsante (GPIO5) e USB (GPIO2) usano la stessa maschera con `esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_HIGH)`. Entrambi attivi HIGH — l'ESP32-C3 richiede lo stesso livello di trigger per tutti i pin di deep sleep wakeup.

| Componente | Dettaglio |
|---|---|
| Pin rilevamento USB | GPIO2 (era GPIO8, spostato il 19/06/2026) |
| Partitore VBUS | 10kΩ (verso VBUS) + 15kΩ (verso GND) → ~3.0V con USB |
| Corrente partitore | 0.2 mA solo con USB collegata |
| Pin carica completa | GPIO9 (STDBY TP5000, LOW = carica finita, pull-up interno) |
| Soglia low-battery | 3.2V sulla batteria |
| Pulsante WiFi | GPIO5 attivo HIGH, 10kΩ pull-down esterno (JTAG/MTDI ha pull-up interno) |

### 5.5 Telemetria stato batteria e ricarica (IMPLEMENTATA)

Due campi aggiunti all'invio telemetria su Google Sheets:

| Campo | Parametro | Valore | Sorgente |
|---|---|---|---|
| Icona batteria | `lowbat` | `1` / `0` | `tensioneBatteria < SOGLIA_BATTERIA_SCARICA` |
| Cavo USB | `usb` | `1` / `0` | `digitalRead(PIN_USB)` — GPIO2 HIGH = cavo collegato |
| Quarzo esterno | `xtal` | `1` / `0` | `rtc_clk_slow_src_get() == XTAL32K` — verifica clock RTC |

Questi dati permettono di tracciare il comportamento di ricarica degli utenti e verificare il funzionamento del quarzo esterno. Lo script Apps Script riceve i valori come `parseInt()` nelle ultime 3 colonne del foglio.

### 5.6 QR Code, istruzioni e localizzazione stringhe in inglese
Alla pressione lunga del pulsante (>3s), oltre ad attivare il portale WiFiManager, mostrare sul display un QR code che punta alla pagina di setup: **https://548b3b43.eterno-orologio.pages.dev/setup** (Cloudflare Pages, multilingua: EN/IT/FR/ES/DE/PT). Da valutare la UX migliore — il captive portal di iOS non si apre automaticamente per connessioni avviate da QR code.

In questa fase, convertire tutte le stringhe del firmware in inglese (messaggi WiFiManager, notifica "No WiFi", ecc.). I testi sono pochi (~5 stringhe) e l'inglese è universale — il multilingua non giustifica la complessità aggiuntiva.

### 5.7 Notifica di internet non disponibile (IMPLEMENTATA)

Se la sync NTP fallisce per **3 volte consecutive** (WiFi non raggiungibile o connessione senza risposta NTP), viene mostrata un'icona sul display in basso a destra, a sinistra dell'eventuale icona batteria. L'icona è composta da un **bitmap WiFi 16×10 px** (3 archi + punto, memorizzato in PROGMEM) affiancato da un **"!"** disegnato con font `u8g2_font_helvR10_tf`.

Il contatore `RTC_DATA_ATTR int fallimentiNTP` viene incrementato ad ogni fallimento (WiFi non connesso o NTP senza risposta) e resettato a zero alla prima sync riuscita. L'orologio continua a funzionare normalmente con l'ora dell'ultimo sync — la notifica serve solo a informare l'utente che l'orario potrebbe non essere preciso o che la rete WiFi configurata non è più raggiungibile.

**Visibilità:** stessa logica dell'icona batteria — disegnata durante il full refresh (boot + 3:00 AM), persiste nei partial refresh. Sparisce al primo NTP riuscito + successivo full refresh.

### 5.8 Welcome page per primo utilizzo

Al primo avvio in fabbrica (o dopo flash firmware), l'orologio mostra una schermata di benvenuto con il messaggio "Push the button below for three seconds" (full refresh). L'orologio entra in deep sleep e attende la pressione lunga del pulsante per avviare il WiFiManager. Questo sostituisce la schermata "00:00" attuale, che non ha significato per l'utente finale. Da collegare con la 5.6 (QR code + stringhe inglesi) per una UX di primo avvio completa.

### 5.9 Refactor del firmware

Riorganizzare il codice di `disegnaOrario()` e della logica icone (batteria scarica, no-internet) per migliorare leggibilità e manutenibilità. Attualmente la logica di disegno icone è duplicata tra i rami `DEV_MODE` e produzione e mescolata con il disegno dell'orario.

### 5.10 Gestione icona della batteria con pin VBUS (IMPLEMENTATA)
L'icona batteria scarica compare quando la tensione scende sotto 3.2V (`batteriaScaricaMostrata = true`). L'icona sparisce **solo** dopo una ricarica USB: in `cicloRicarica()`, quando il cavo viene scollegato e la tensione è sopra soglia, `batteriaScaricaMostrata` viene resettato. Questo evita che l'icona compaia e scompaia ciclicamente vicino alla soglia.

### 5.11 Deep sleep wakeup — storia e soluzione

**Problema 1 — GPIO8 non è RTC GPIO:** il partitore VBUS era su GPIO8, che non appartiene al dominio LP (solo GPIO0-5). `gpio_deep_sleep_wakeup_enable(GPIO8, ...)` falliva silenziosamente. Inoltre GPIO8 sulla SuperMini è collegato ai LED onboard.

**Problema 2 — livello unico:** sull'ESP32-C3, `esp_deep_sleep_enable_gpio_wakeup` richiede lo **stesso livello di trigger per tutti i pin**. Non è possibile avere un pin LOW e uno HIGH contemporaneamente.

**Problema 3 — GPIO5 è MTDI (JTAG):** ha un pull-up hardware interno di fabbrica. `gpio_reset_pin()` lo riattivava. In deep sleep il pull-down software poteva non reggere contro il pull-up JTAG, causando falsi wakeup.

**Soluzione implementata (19/06/2026):**
- Partitore VBUS spostato da GPIO8 a **GPIO2** (RTC GPIO)
- Pulsante cambiato da attivo LOW ad **attivo HIGH** (filo a 3.3V invece che GND)
- Aggiunto **10kΩ pull-down esterno** su GPIO5 (vince il pull-up JTAG interno)
- Rimosso `gpio_reset_pin(BUTTON_PIN)` dal firmware
- Unica chiamata: `esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_HIGH)` con maschera GPIO2 + GPIO5
- Entrambi i segnali sono attivi HIGH → stesso livello, wakeup istantaneo per pulsante e USB

### 5.12 Test ciclo ricarica completo — impatto ghosting animazione batteria

L'animazione dell'icona batteria durante la ricarica (partial refresh continui nell'angolo in basso a destra) causa accumulo di grigio visibile dal lato destro del display. Da verificare con un ciclo di ricarica completo (da scarica a STDBY) per valutare l'entità del ghosting a fine carica.

**Soluzione alternativa se il ghosting è fastidioso:** sostituire l'animazione con icone statiche — batteria con fulmine (in carica), batteria piena con spunta (carica completata). Solo 3 stati fissi senza alternanza, eliminando i partial refresh ripetuti che causano il grigio.

### 5.13 Ottimizzazioni pre-sleep UC8253

**Rimuovere POF ridondante:** il comando POF (0x02) + delay(50) nella sequenza pre-sleep è superfluo — `_Update_Part()` chiama già `_PowerOff()` che manda lo stesso comando e attende BUSY. Il booster è già spento quando si arriva ai comandi manuali.

**Ridurre delay(100) prima del deep sleep:** il delay serve a far processare al UC8253 i comandi CDI e PSR prima che i GPIO vadano floating. Essendo semplici scritture registro (microsecondi), 10ms sarebbero sufficienti. Impatto energetico trascurabile (~0.2 mAh/giorno), priorità bassa.

### 5.14 Telemetria settimanale invece che giornaliera

Attualmente il firmware accende il WiFi ogni notte alle 3:00 per inviare telemetria, anche nei giorni senza sync NTP (`inviaTelemtria(0)` nel ramo `else`). Rendere la telemetria settimanale come la sync NTP: un'unica connessione WiFi ogni 7 giorni riduce il consumo e l'usura del ciclo WiFi. Impatto energetico: elimina ~6 connessioni WiFi/settimana (~0.5 mWh/giorno).

### 5.15 Screenshot step-by-step nella pagina setup

Aggiungere alla pagina web di setup (eterno-orologio.pages.dev/setup) gli screenshot del telefono per ogni step della configurazione, in modo che l'utente veda esattamente cosa aspettarsi su schermo. Da fare per tutte le lingue supportate.

### 5.16 Transistor di taglio VCC display (PRIORITÀ ALTA — hardware)

**Problema:** il UC8253 in stand-by (dopo POF) consuma ~26 µA costanti — l'81% del consumo deep sleep totale (~32 µA). Il comando DSLP ridurrebbe a ~3 µA ma causa artefatti al risveglio (richiede full refresh ogni minuto).

**Soluzione:** aggiungere un **P-MOSFET** (o PNP) pilotato da un GPIO che stacca completamente VCC del display durante il deep sleep. Il display e-ink mantiene l'immagine senza alimentazione — il taglio VCC è trasparente per l'utente.

| Parametro | Valore |
|---|---|
| GPIO di controllo | **GPIO8** (libero, era VBUS) |
| Tipo transistor | P-MOSFET (es. SI2301, SOT-23) o PNP (es. BC857) |
| Logica | GPIO8 LOW → MOSFET ON → display alimentato; GPIO8 HIGH (o floating in deep sleep) → MOSFET OFF → display spento |
| Corrente max display | ~8 mA (durante refresh) — qualsiasi SOT-23 P-FET regge |
| Resistenza gate | 100 kΩ pull-up a VBAT (garantisce OFF in deep sleep quando GPIO8 flotta) |

**Nota GPIO8 non-RTC:** GPIO8 non appartiene al dominio LP (solo GPIO0–5 sono RTC). In deep sleep il pin **flotta** — il pull-up esterno a VBAT sul gate lo porta HIGH, spegnendo il P-MOSFET. Al wakeup il firmware porta GPIO8 LOW per alimentare il display prima di iniziare il refresh SPI.

**Impatto energetico:**

| Voce | Prima | Dopo |
|---|---|---|
| Deep sleep totale | ~32 µA | **~6 µA** (ESP32 5 µA + partitore 1 µA) |
| Energia deep sleep/giorno | 2.5 mWh | **~0.5 mWh** |
| Autonomia stimata | ~3.1 anni | **~4.5+ anni** |

**Sequenza firmware:**
1. Wakeup → `digitalWrite(GPIO8, LOW)` → display alimentato
2. Delay ~5 ms (stabilizzazione VCC + boot UC8253)
3. `display.init()` → SPI refresh → `_PowerOff()` (POF)
4. `digitalWrite(GPIO8, HIGH)` → display spento
5. Deep sleep

**Azione PCB produzione:** aggiungere P-MOSFET + resistenza pull-up tra VBAT e VCC display, gate su GPIO8. Footprint: 1× SOT-23 + 1× 0402.

**Azione prototipo:** cablare su breadboard con SI2301 o equivalente. Testare che il display si inizializzi correttamente dopo il taglio VCC (il UC8253 perde lo stato dei registri — serve `display.init()` completa ad ogni wakeup).

### 5.17 Transistor di isolamento TP5000 dalla batteria (PRIORITÀ ALTA — hardware)

**Problema:** il TP5000 è collegato permanentemente alla batteria. Quando il cavo USB non è presente, la corrente può fluire dalla batteria verso il TP5000 (corrente quiescente inversa), consumando energia inutilmente. Il TP5000 dovrebbe essere completamente spento quando non c'è ricarica in corso.

**Soluzione:** aggiungere un **transistor** (P-MOSFET o PNP) tra l'uscita del TP5000 e la batteria, pilotato dalla presenza di VBUS. Quando il cavo USB è scollegato il transistor è OFF e il TP5000 è completamente isolato dalla batteria — corrente di fuga: 0 µA.

| Parametro | Valore |
|---|---|
| Posizione | Tra uscita BAT del TP5000 e il polo + della batteria |
| Tipo transistor | P-MOSFET (es. SI2301, SOT-23) |
| Segnale di controllo | **VBUS** (5V USB) — pilota il gate direttamente o tramite partitore |
| Logica | VBUS presente → MOSFET ON → TP5000 collegato alla batteria (ricarica attiva); VBUS assente → MOSFET OFF → TP5000 isolato (consumo zero) |
| Corrente max | 900 mA (corrente di carica TP5000) — scegliere MOSFET con Rds(on) bassa per non dissipare |

**Funzionamento:**
- **USB collegata:** VBUS (5V) alimenta il TP5000 e pilota il gate del P-MOSFET portandolo in conduzione → il TP5000 carica la batteria normalmente
- **USB scollegata:** senza VBUS il gate del P-MOSFET viene tirato alto dal pull-up → MOSFET OFF → nessun percorso dalla batteria verso il TP5000

**Azione PCB produzione:** aggiungere P-MOSFET tra uscita TP5000 e batteria, gate controllato da VBUS. Footprint: 1× SOT-23 + resistenze di polarizzazione gate.

**Azione prototipo:** verificare sul breadboard che la ricarica CC/CV del TP5000 non sia influenzata dalla Rds(on) del MOSFET (caduta di tensione aggiuntiva sul percorso di carica). Misurare con INA226 la corrente di fuga con USB scollegata per confermare l'isolamento.

### 5.18 Misure consumi deep sleep e UR15 (IDENTIFICATO — 25/06/2026)

**Setup di misura:** INA226 (16-bit ADC, I2C) con shunt R100 (0.1Ω) in serie sulla linea batteria. Arduino Pro Mini 5V come logger seriale. 1 LSB shunt = 25 µA.

**Risultati componenti SuperMini (sketch only_deep_sleep, display scollegato):**

| Componenti presenti | LSB | Corrente |
|---|---|---|
| LED rosso + LED blu + ME6217 | 19–26 | 475–650 µA |
| LED blu + ME6217 | 4–14 | 100–350 µA |
| Solo ME6217 | 4–14 | 100–350 µA |
| Niente (solo ESP32-C3) | -3 a 6 | rumore ADC (~5 µA, a spec) |

- **LED rosso** è il peggior colpevole (~375 µA): GPIO8 non-RTC flotta in deep sleep e drena attraverso il LED
- **LED blu** non contribuisce (forward voltage più alta o cablaggio diverso)
- **ME6217 LDO** contribuisce ~100–350 µA di corrente quiescente/perdita

**Risultati isolamento firmware (sketch only_deep_sleep con flag incrementali):**

| Test | LSB | Note |
|---|---|---|
| Nessun wakeup source | 2–9 | baseline chip |
| Solo timer 60s | 4–14 | OK |
| Solo INPUT_PULLDOWN GPIO5 | 4–14 | OK |
| GPIO wakeup GPIO5 + GPIO2 | 14–21 | anomalo |
| GPIO wakeup solo GPIO5 (button) | 4–14 | OK |
| GPIO wakeup solo GPIO2 (USB) | 14–19 | **colpevole** |

**Causa:** UR15 sullo schematico SuperMini è un pull-up da 3.3V a GPIO2. Quando `esp_deep_sleep_enable_gpio_wakeup()` monitora GPIO2, il circuito di wakeup interno crea un percorso verso GND e la corrente scorre attraverso UR15 (~250 µA).

**Azione prototipo:** dissaldare UR15 dalla SuperMini. GPIO2 non necessita di pull-up esterno: il partitore da VBUS (10kΩ+15kΩ) fornisce ~3V quando il cavo è collegato, e il pin flotta basso quando il cavo è scollegato.

**Azione PCB produzione:** non montare pull-up su GPIO2. Nessun componente equivalente a UR15.

---

## Appendice A — Studio pannello solare indoor (scartato)

È stato valutato un approccio ad autosufficienza energetica tramite pannello solare amorfo per uso indoor. Lo studio è conservato come riferimento per eventuali evoluzioni future del prodotto.

### Pannello valutato: Panasonic AM-1816CA (Amorton)
| Parametro | Valore |
|---|---|
| Tipo | Silicio amorfo, substrato vetro |
| Dimensioni | 96.7 × 56.7 × 1.1 mm |
| Voc (200 lux FL) | 4.9 V |
| Isc (200 lux FL) | ~100 µA |
| Vmp / Imp (200 lux FL) | 3.0 V / 84 µA |
| Pmax (200 lux FL) | 252 µW |

### Produzione stimata

Corrente scala linearmente con illuminamento. Efficienza circuito con diodo Schottky: ~94%.

| Condizione | Lux | I pannello | P pannello | P netta batteria |
|---|---|---|---|---|
| Luce artificiale scarsa | ~50 | ~21 µA | 63 µW | 59 µW |
| Stanza illuminata | ~200 | ~84 µA | 252 µW | 237 µW |
| Buona luce indiretta | ~300 | ~126 µA | 378 µW | 355 µW |
| Vicino finestra | ~500 | ~210 µA | 630 µW | 592 µW |
| Finestra luminosa | ~700 | ~294 µA | 882 µW | 829 µW |
| Finestra esposta | ~1000 | ~420 µA | 1.26 mW | 1.18 mW |
| Davanzale, cielo coperto | ~5000 | ~2.1 mA | 6.3 mW | 5.9 mW |

### Circuito di ricarica valutato

Carica diretta tramite diodo Schottky BAT54S (Vf ~0.24V @ 100 µA) con Zener BZX84C3V6 (3.6V) di protezione sulla batteria. Quiescent current: 0 nA. Efficienza ~94%. Il BQ25504 (boost converter con MPPT) era stato valutato inizialmente ma scartato: con Vmp del pannello (3.0V) vicina alla tensione LiFePO4 (3.2V), il diodo diretto era più efficiente (~94% vs ~60-65%).

### Motivo dello scarto

Misurazioni reali in ambiente indoor hanno mostrato livelli tipici di ~50 lux, che producono appena 0.59 mWh/giorno (10h) — l'8% del consumo dell'orologio (7.4 mWh/giorno). L'autosufficienza solare richiederebbe ~630+ lux costanti, raggiungibili solo vicino a finestre luminose. Il pannello non giustifica il costo e la complessità aggiuntiva rispetto alla soluzione USB-C, che offre un'esperienza utente più chiara e prevedibile.