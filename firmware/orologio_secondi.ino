#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <WiFiManager.h>
#include <WiFi.h>
#include <time.h>
#include <Preferences.h>
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "soc/rtc.h"
// esp_clk_slowclk_cal_get() sta in un header privato di ESP-IDF: se la versione del
// core non lo espone la diagnostica degrada senza rompere la compilazione.
#if __has_include("esp_private/esp_clk.h")
  #include "esp_private/esp_clk.h"
  #define HA_CALIBRAZIONE_ESPIDF 1
#else
  #define HA_CALIBRAZIONE_ESPIDF 0
#endif
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#define CS_PIN (SS)
#define BUSY_PIN (21)
#define RES_PIN (20)
#define DC_PIN (10)
#define BUTTON_PIN GPIO_NUM_5

#define PIN_BATTERIA 3
#define FW_VERSION "1.0"
#define PIN_USB 2
#define PIN_STDBY 9
#define PIN_DISPLAY_EN 8
#define TEMPO_ACCELERATO 40

// =======================================================
// CHIMICA DELLA BATTERIA
// Scommentare UNA sola riga, quella corrispondente al pacco montato.
// L'unico parametro che ne dipende e' SOGLIA_BATTERIA_SCARICA, cioe' quando
// compare l'icona di batteria scarica. Nel resto del file non c'e' altro da toccare.
// Ricordarsi che la scelta va coordinata con i jumper JP_DIODE / JP_LDO / JP_CS.
// =======================================================
#define BATTERIA_LIFEPO4
//#define BATTERIA_LIPO
//#define BATTERIA_3AA

#if (defined(BATTERIA_LIFEPO4) + defined(BATTERIA_LIPO) + defined(BATTERIA_3AA)) != 1
  #error "Selezionare esattamente una chimica: BATTERIA_LIFEPO4, BATTERIA_LIPO o BATTERIA_3AA."
#endif

#if defined(BATTERIA_LIFEPO4)
  // LiFePO4 1S: nominale 3.2V, fine carica 3.6V, ginocchio di scarica ~3.0V.
  // Curva molto piatta: sta sopra i 3.3V per la gran parte della capacita',
  // quindi 3.2V corrisponde grosso modo all'ultimo 20% di autonomia.
  // Jumper: JP_DIODE chiuso, JP_LDO aperto, JP_CS aperto (cutoff 3.6V).
  #define SOGLIA_BATTERIA_SCARICA 3.2
#elif defined(BATTERIA_LIPO)
  // Li-Po / Li-ion 1S: nominale 3.7V, fine carica 4.2V, cutoff 3.0V.
  // Jumper: JP_DIODE aperto, JP_LDO chiuso, JP_CS chiuso (cutoff 4.2V).
  #define SOGLIA_BATTERIA_SCARICA 3.5
#elif defined(BATTERIA_3AA)
  // 3 stilo alcaline in serie: nominale 3 x 1.5 = 4.5V, fresche fino a 4.8V.
  // 3.3V sono 1.1V a cella, cioe' praticamente esaurite.
  // Non ricaricabili: niente TP5000, niente USB, cicloRicarica() non parte mai.
  // Jumper: JP_DIODE aperto, JP_LDO chiuso. USB-C non montata.
  #define SOGLIA_BATTERIA_SCARICA 3.3
#endif

// Partitore di lettura sull'ADC: R10 e R11 uguali, quindi fattore 0.5 e
// moltiplicatore 2. Il valore assoluto non conta finche' le due sono uguali,
// conta il rapporto. Dimensionato per il caso peggiore (4.8V -> 2.4V all'ADC,
// dentro il fondo scala dell'ESP32-C3 con attenuazione 11 dB).
#define R_PARTITORE_ALTA  2000000.0   // R10, verso VBAT
#define R_PARTITORE_BASSA 2000000.0   // R11, verso GND
#define FATTORE_PARTITORE ((R_PARTITORE_ALTA + R_PARTITORE_BASSA) / R_PARTITORE_BASSA)

// Soglie di comportamento, prima sparse come numeri nudi dentro setup().
#define DURATA_PRESSIONE_LUNGA 1500  // ms di pressione continua per aprire il portale WiFi
#define ORA_MANUTENZIONE 3           // ora in cui si legge la batteria e si invia la telemetria
// La sincronizzazione NTP e' giornaliera, nello stesso slot della manutenzione: con un
// quarzo nudo la deriva e' di qualche secondo al giorno e una cadenza settimanale
// accumulerebbe mezzo minuto di errore.

// Secondi di anticipo del risveglio rispetto allo scoccare del minuto. Il refresh del
// pannello dura circa 0.8 s (boot, accensione di Q3, aggiornamento dell'inchiostro):
// svegliandosi in anticipo le cifre cambiano ESATTAMENTE sul minuto invece che dopo.
// Da ritarare se si misura una durata di refresh diversa.
#define ANTICIPO_RISVEGLIO 1

// Passate bianco/nero della pulizia morbida dello schermo. Una sola passata e' gia'
// sufficiente nell'uso normale; alzare a 2 se si nota ghosting residuo dopo il
// portale WiFi. Ogni passata costa circa un secondo.
#define CICLI_PULIZIA_SCHERMO 1

// Cicli del clock lento contati per calibrarlo. 256 cicli a 32768 Hz costano circa
// 8 ms di veglia ad ogni risveglio e danno una risoluzione di pochi ppm: alzarlo
// migliora poco la precisione e peggiora il consumo medio.
#define CICLI_CALIBRAZIONE_RTC 256

// Potenza di trasmissione WiFi. In access point i beacon 802.11b a 1 Mbps a piena
// potenza chiedono 350 mA di picco: troppo per il percorso a diodo. Abbassare qui
// se il rail continua a cedere, alzare se il telefono fatica a vedere la rete.
#define POTENZA_TX_PORTALE WIFI_POWER_11dBm
#define POTENZA_TX_NORMALE WIFI_POWER_19_5dBm

// Gate di Q3 (AO3401A, P-ch): LOW = display alimentato, HIGH o alta impedenza = spento.
// R13 (100k verso +3.3V) garantisce l'interdizione quando l'ESP32 rilascia il pad.
#define ATTESA_RAIL_DISPLAY 50   // ms di assestamento di VCC_DISPLAY dopo l'accensione
#define DURATA_RESET_DISPLAY 20  // ms di impulso su RES: il UC8253 fa un avvio a freddo
#define GOOGLE_SHEETS_URL "https://script.google.com/macros/s/AKfycbz1fNkQNdFf_IsT9enqTU-47g7L5uAMJDx-hzJf2sJf3KGpJOuUKP3DO-dTtvNoLfhJ/exec"

RTC_DATA_ATTR float tensioneBatteria = 0.0;
RTC_DATA_ATTR int giorniDaBoot = 0;
RTC_DATA_ATTR bool usbCollegato = false;
RTC_DATA_ATTR int fallimentiNTP = 0;
RTC_DATA_ATTR bool batteriaScaricaMostrata = false;
RTC_DATA_ATTR bool devMode = false;
RTC_DATA_ATTR int contatoreRisvegli = 0;
// Ultimo orario effettivamente sul pannello: sopravvive al deep sleep e serve a
// ricostruire il buffer "precedente" del controller dopo il taglio di VCC.
RTC_DATA_ATTR int oraMostrata = -1;
RTC_DATA_ATTR int minutoMostrato = -1;
RTC_DATA_ATTR float voltMostrati = 0.0;
RTC_DATA_ATTR int iconaMostrata = -1;   // -1 nessuna, 0 batteria vuota, 1..3 tacche ricarica
RTC_DATA_ATTR bool avvisoWifiMostrato = false;
RTC_DATA_ATTR bool invertitoMostrato = false;
uint32_t freqClockLento = 0;              // misurata ad ogni risveglio
char diagBuf[48] = "";                    // riga diagnostica mostrata in devMode
RTC_DATA_ATTR char diagMostrata[48] = ""; // la stessa riga, com'e' sul pannello
float temperaturaInterna = 0.0;
bool inRicarica = false;
bool caricaCompleta = false;
int frameRicarica = 0;

// Stato dell'alimentazione del pannello. Volutamente NON RTC_DATA_ATTR: ad ogni
// risveglio il chip riparte da reset, il pad torna in alta impedenza e R13 spegne
// il display, quindi lo stato corretto all'avvio e' sempre "spento".
bool displayAlimentato = false;
bool displayInizializzato = false;
// Vero finche' la RAM "precedente" del controller non e' stata ricostruita in questo ciclo.
bool ramPannelloDaRicostruire = false;

// Frequenza reale del clock lento RTC, quello che scandisce il deep sleep e quindi
// la precisione dell'orologio. rtc_clk_cal() conta i cicli del clock lento contro il
// quarzo da 40 MHz del modulo e restituisce il periodo in microsecondi, in virgola
// fissa Q13.19. Se il quarzo esterno funziona ci si aspetta ~32768 Hz; se il firmware
// sta girando sull'oscillatore RC interno si legge un valore molto piu' alto e
// instabile. NON e' la stessa cosa di rtc_clk_slow_src_get(), che si limita a
// rileggere il registro appena scritto e risponde sempre di si'.
uint32_t frequenzaClockLento() {
  uint32_t periodo = rtc_clk_cal(RTC_CAL_RTC_MUX, CICLI_CALIBRAZIONE_RTC);
  if (periodo == 0) return 0;
  return (uint32_t)((1000000ULL << 19) / periodo);
}

// Misura la frequenza reale del clock lento e la confronta con la costante di
// calibrazione che ESP-IDF usa per convertire tick in microsecondi. Per un quarzo
// da 32768 Hz il periodo e' 30.5176 us, che nel formato Q13.19 vale esattamente
// 16.000.000: qualunque scostamento da quel valore e' l'errore sistematico
// dell'orologio, espresso direttamente in ppm.
void allineaClockLento() {
  // Misura il periodo reale del clock lento, una volta sola, e lo usa per due cose.
  uint32_t periodo = rtc_clk_cal(RTC_CAL_RTC_MUX, CICLI_CALIBRAZIONE_RTC);
  if (periodo == 0) {
    snprintf(diagBuf, sizeof(diagBuf), "RTC non calibrabile");
    return;
  }
  freqClockLento = (uint32_t)((1000000ULL << 19) / periodo);

#if HA_CALIBRAZIONE_ESPIDF
  // QUESTA e' la correzione. rtc_clk_slow_src_set() commuta l'hardware ma ESP-IDF
  // continua a convertire tick in microsecondi con la costante calcolata all'avvio,
  // quando la sorgente era ancora l'oscillatore RC interno a ~131 kHz. Il fattore
  // sbagliato e' circa 4x, e si applica al tempo passato in light sleep dentro
  // displayBusyCallback(): ad ogni attesa di BUSY l'orologio si vede accreditare
  // un quarto del tempo davvero trascorso, e resta indietro.
  esp_clk_slowclk_cal_set(periodo);

  long ppm = (long)(((double)periodo - 16000000.0) / 16.0);
  snprintf(diagBuf, sizeof(diagBuf), "%luHz cal %lu %+ldppm",
           (unsigned long)freqClockLento, (unsigned long)periodo, ppm);
#else
  snprintf(diagBuf, sizeof(diagBuf), "%luHz cal n/d", (unsigned long)freqClockLento);
#endif
}

void leggiBatteria() {
  pinMode(PIN_BATTERIA, INPUT);
  gpio_set_pull_mode((gpio_num_t)PIN_BATTERIA, GPIO_FLOATING);
  delay(10);
  long somma = 0;
  for (int i = 0; i < 16; i++) {
    somma += analogReadMilliVolts(PIN_BATTERIA);
    delay(5);
  }
  int adcMv = somma / 16;
  tensioneBatteria = adcMv * FATTORE_PARTITORE / 1000.0;
}

GxEPD2_BW<GxEPD2_370_GDEY037T03, GxEPD2_370_GDEY037T03::HEIGHT> display(GxEPD2_370_GDEY037T03(CS_PIN, DC_PIN, RES_PIN, BUSY_PIN));
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

// -------------------------------------------------------
// DISPLAY
// -------------------------------------------------------

void displayBusyCallback(const void*) {
  // Il light sleep fa commutare i pad digitali sulla loro configurazione di sleep,
  // che di default e' alta impedenza. Su GPIO8 significherebbe lasciare il gate di Q3
  // a R13, togliendo VCC_DISPLAY nel mezzo del refresh. Il hold congela il livello.
  gpio_hold_en((gpio_num_t)PIN_DISPLAY_EN);
  esp_light_sleep_start();
  gpio_hold_dis((gpio_num_t)PIN_DISPLAY_EN);
}

// Alimenta il pannello e lo inizializza. Idempotente.
// Il parametro "reset" e' il flag "initial" di GxEPD2: con true la libreria converte
// il primo refresh parziale in uno completo. Va tenuto false sui risvegli da timer,
// altrimenti ogni minuto diventa un full refresh.
void initDisplay(bool reset) {
  if (displayInizializzato) return;

  pinMode(PIN_DISPLAY_EN, OUTPUT);
  digitalWrite(PIN_DISPLAY_EN, LOW);
  // Il pad deve conservare questa configurazione anche in light sleep.
  gpio_sleep_sel_dis((gpio_num_t)PIN_DISPLAY_EN);
  displayAlimentato = true;
  delay(ATTESA_RAIL_DISPLAY);

  // R12 tira RES su VCC_DISPLAY, non su +3.3V: RES sale insieme al rail e il
  // power-on reset del controller deve darlo il firmware, non l'hardware.
  display.init(115200, reset, DURATA_RESET_DISPLAY, false);
  displayInizializzato = true;
  // Il pannello e' appena stato riacceso: la RAM del controller e' da ricostruire.
  ramPannelloDaRicostruire = true;

  display.epd2.selectSPI(SPI, SPISettings(10000000, MSBFIRST, SPI_MODE0));
  display.setRotation(3);
  u8g2Fonts.begin(display);
  u8g2Fonts.setFontMode(1);
  u8g2Fonts.setFontDirection(0);
  u8g2Fonts.setForegroundColor(GxEPD_BLACK);
  u8g2Fonts.setBackgroundColor(GxEPD_WHITE);

  gpio_wakeup_enable((gpio_num_t)BUSY_PIN, GPIO_INTR_HIGH_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  display.epd2.setBusyCallback(displayBusyCallback);
}

// Spegne il pannello. Va chiamata su OGNI percorso prima del deep sleep, anche
// quando il display non e' mai stato acceso in questo ciclo.
void spegniDisplay() {
  if (!displayAlimentato) {
    // Mai acceso: lascio il pad in alta impedenza, R13 tiene Q3 interdetto.
    pinMode(PIN_DISPLAY_EN, INPUT);
    return;
  }

  // 1) POF: il controller chiude il proprio DC/DC. Togliere VCC con le tensioni
  //    alte ancora attive stressa il pannello. Serve la SPI ancora viva.
  if (displayInizializzato) display.powerOff();

  // 2) A display spento BUSY resta flottante: disarmo il risveglio da light sleep.
  gpio_wakeup_disable((gpio_num_t)BUSY_PIN);

  // 3) Scollego le linee verso il pannello PRIMA di togliere VCC, altrimenti l'ESP32
  //    lo retro-alimenta attraverso i diodi di protezione degli ingressi.
  SPI.end();
  pinMode(RES_PIN, INPUT);
  pinMode(DC_PIN, INPUT);
  pinMode(CS_PIN, INPUT);
  pinMode(BUSY_PIN, INPUT);

  // 4) Solo adesso interdico Q3, pilotando il gate per una commutazione netta.
  gpio_hold_dis((gpio_num_t)PIN_DISPLAY_EN);
  digitalWrite(PIN_DISPLAY_EN, HIGH);
  delay(5);

  // 5) Rilascio il pad. GPIO8 non e' un RTC GPIO sull'ESP32-C3, quindi in deep sleep
  //    va comunque in alta impedenza: il livello alto lo garantisce R13.
  pinMode(PIN_DISPLAY_EN, INPUT);

  displayAlimentato = false;
  displayInizializzato = false;
}

// I colori sono parametrici perche' alle ore tonde la schermata e' invertita:
// con i valori fissi l'icona verrebbe disegnata nera su nero.
void disegnaIconaBatteria(int16_t bx, int16_t by, int tacche,
                          uint16_t colTesto = GxEPD_BLACK, uint16_t colSfondo = GxEPD_WHITE) {
  display.drawRect(bx, by, 23, 12, colTesto);
  display.drawRect(bx + 23, by + 3, 3, 6, colTesto);
  display.drawRect(bx + 22, by + 4, 2, 4, colSfondo);
  for (int i = 0; i < tacche && i < 3; i++) {
    display.fillRect(bx + 2 + i * 7, by + 2, 5, 8, colTesto);
  }
}

void animaBatteria() {
  int16_t bx = display.width() - 30;
  int16_t by = display.height() - 18;
  int tacche = caricaCompleta ? 3 : frameRicarica;
  display.setPartialWindow(bx - 1, by - 1, 29, 16);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    disegnaIconaBatteria(bx, by, tacche);
  } while (display.nextPage());
  // Anche questa funzione modifica l'angolo, quindi deve tenere aggiornata la
  // fotografia usata per ricostruire il buffer precedente.
  iconaMostrata = tacche;
}

// Riempie tutto lo schermo di un colore usando la finestra parziale, quindi con la
// LUT veloce e non con quella nativa del full refresh.
void riempiSchermo(uint16_t colore) {
  display.setPartialWindow(0, 0, display.width(), display.height());
  display.firstPage();
  do {
    display.fillScreen(colore);
  } while (display.nextPage());
}

// Pulizia morbida: nero, bianco, e lo schermo resta bianco pronto per il disegno.
// Alternativa al full refresh nativo, che per scuotere l'inchiostro inverte il
// pannello una cinquantina di volte in due secondi — corretto tecnicamente, ma
// visivamente allarmante per chi non sa cosa sta guardando.
// Pulisce meno a fondo, quindi NON sostituisce il full refresh notturno.
void pulisciSchermo() {
  // Dopo il taglio di VCC la RAM "precedente" del controller e' indefinita, quindi
  // la prima passata a nero piloterebbe solo i pixel che per caso risultano diversi.
  // La inizializzo a bianco: cosi' ogni pixel risulta cambiato e viene pilotato davvero.
  if (ramPannelloDaRicostruire) {
    display.setPartialWindow(0, 0, display.width(), display.height());
    display.firstPage();
    do {
      display.fillScreen(GxEPD_WHITE);
    } while (display.nextPageToPrevious());
  }

  for (int i = 0; i < CICLI_PULIZIA_SCHERMO; i++) {
    riempiSchermo(GxEPD_BLACK);
    riempiSchermo(GxEPD_WHITE);
  }

  // Il pannello adesso e' bianco e la RAM "precedente" del controller pure, perche'
  // l'ultima passata l'ha riallineata. Azzerando la fotografia, il disegno successivo
  // parte da bianco con un differenziale corretto e senza tentare ricostruzioni.
  oraMostrata = -1;
  minutoMostrato = -1;
  iconaMostrata = -1;
  avvisoWifiMostrato = false;
  invertitoMostrato = false;
  ramPannelloDaRicostruire = false;
}

#define OFFSET_CORNICE 15

// Disegna una schermata nel buffer di pagina corrente. Scorporata da disegnaOrario()
// perche' ora va eseguita due volte: una per ricostruire l'immagine precedente nella
// RAM del controller, una per quella nuova.
// Tutti gli elementi variabili passano per parametro, nessuno letto dalle globali:
// la ricostruzione del buffer precedente deve poter ridisegnare lo stato di ALLORA,
// non quello di adesso. icona: -1 nessuna, 0 batteria vuota, 1..3 tacche di ricarica.
void renderOrario(int ore, int minuti, float volt, int icona, bool avvisoWifi, bool invertito, const char* diag) {
  uint16_t colSfondo = invertito ? GxEPD_BLACK : GxEPD_WHITE;
  uint16_t colTesto  = invertito ? GxEPD_WHITE : GxEPD_BLACK;

  char buf[6];
  sprintf(buf, "%02d:%02d", ore, minuti);

  display.fillScreen(colSfondo);
  u8g2Fonts.setForegroundColor(colTesto);
  u8g2Fonts.setBackgroundColor(colSfondo);

  u8g2Fonts.setFont(u8g2_font_logisoso92_tn);
  int16_t tw = u8g2Fonts.getUTF8Width(buf);
  int16_t tx = (display.width() - tw) / 2 + OFFSET_CORNICE;
  int16_t ty = (display.height() + 70) / 2;
  u8g2Fonts.setCursor(tx, ty);
  u8g2Fonts.print(buf);

  int16_t bx = display.width() - 30;
  int16_t by = display.height() - 18;
  if (devMode) {
    char vBuf[8];
    sprintf(vBuf, "%.4fV", volt);
    u8g2Fonts.setFont(u8g2_font_helvR10_tf);
    int16_t vw = u8g2Fonts.getUTF8Width(vBuf);
    u8g2Fonts.setCursor(bx - vw - 5, display.height() - 5);
    u8g2Fonts.print(vBuf);
  }
  if (icona >= 0) {
    disegnaIconaBatteria(bx, by, icona, colTesto, colSfondo);
  }
  if (avvisoWifi) {
    u8g2Fonts.setFont(u8g2_font_helvR10_tf);
    u8g2Fonts.setCursor(30, display.height() - 5);
    u8g2Fonts.print("No WiFi - Tieni premuto il pulsante 3 secondi");
  } else if (devMode && diag && diag[0]) {
    u8g2Fonts.setFont(u8g2_font_helvR10_tf);
    u8g2Fonts.setCursor(30, display.height() - 5);
    u8g2Fonts.print(diag);
  }
}

// invertito: alle ore tonde bianco e nero si scambiano. Non e' solo estetica —
// invertendo, OGNI pixel della finestra cambia stato e viene quindi pilotato dal
// refresh parziale. Un minuto dopo torna indietro. Sono due passate complete di
// tutta la matrice ogni ora, che rinfrescano il contrasto senza il lampeggio di
// un vero full refresh.
void disegnaOrario(bool fullRefresh, int ore, int minuti, bool invertito = false) {
  int icona = -1;
  if (inRicarica) {
    icona = caricaCompleta ? 3 : frameRicarica;
  } else if (batteriaScaricaMostrata) {
    icona = 0;
  }
  bool avvisoWifi = (fallimentiNTP >= 3);

  // Il taglio di VCC azzera la RAM del UC8253, compreso il buffer "precedente" da cui
  // parte il confronto differenziale. Senza ricostruirlo il refresh parziale produce
  // garbage: e' l'autore stesso di GxEPD2 ad avvertirlo nel commento di init().
  // nextPageToPrevious() scrive nella RAM vecchia senza fare alcun refresh.
  // Fondamentale passare lo stato MEMORIZZATO e non quello attuale: se ricostruissi
  // l'immagine precedente con l'icona di adesso, il controller la considererebbe gia'
  // presente sul pannello e non la disegnerebbe: verrebbe grigia invece che nera.
  if (!fullRefresh && ramPannelloDaRicostruire && oraMostrata >= 0) {
    display.setPartialWindow(0, 0, display.width(), display.height());
    display.firstPage();
    do {
      renderOrario(oraMostrata, minutoMostrato, voltMostrati, iconaMostrata,
                   avvisoWifiMostrato, invertitoMostrato, diagMostrata);
    } while (display.nextPageToPrevious());
  }

  if (fullRefresh) {
    display.setFullWindow();
  } else {
    display.setPartialWindow(0, 0, display.width(), display.height());
  }

  display.firstPage();
  do {
    renderOrario(ore, minuti, tensioneBatteria, icona, avvisoWifi, invertito, diagBuf);
  } while (display.nextPage());

  u8g2Fonts.setForegroundColor(GxEPD_BLACK);
  u8g2Fonts.setBackgroundColor(GxEPD_WHITE);

  // Da qui in poi le due RAM del controller sono allineate a cio' che e' sul pannello.
  ramPannelloDaRicostruire = false;
  oraMostrata = ore;
  minutoMostrato = minuti;
  voltMostrati = tensioneBatteria;
  iconaMostrata = icona;
  avvisoWifiMostrato = avvisoWifi;
  invertitoMostrato = invertito;
  strncpy(diagMostrata, diagBuf, sizeof(diagMostrata) - 1);
  diagMostrata[sizeof(diagMostrata) - 1] = '\0';
}

#define QR_SIZE 33
#define QR_SCALE 3
const uint8_t qrBitmap[165] PROGMEM = {
  0xFE, 0x4C, 0xCB, 0x3F, 0x80,
  0x82, 0x3B, 0xBF, 0xA0, 0x80,
  0xBA, 0xEE, 0xE9, 0x2E, 0x80,
  0xBA, 0x77, 0x73, 0x2E, 0x80,
  0xBA, 0x22, 0x21, 0x2E, 0x80,
  0x82, 0x44, 0x41, 0xA0, 0x80,
  0xFE, 0xAA, 0xAA, 0xBF, 0x80,
  0x00, 0xEE, 0xEF, 0x00, 0x00,
  0xEF, 0xB3, 0x36, 0xE2, 0x00,
  0x09, 0xB3, 0x30, 0xE4, 0x80,
  0xBE, 0xC4, 0x44, 0xDD, 0x80,
  0x14, 0x11, 0x15, 0x2D, 0x00,
  0x56, 0x88, 0x8C, 0x64, 0x80,
  0x4D, 0xDD, 0xDC, 0xB6, 0x80,
  0xD6, 0xDB, 0xB8, 0xB1, 0x80,
  0x81, 0x2E, 0xEC, 0xED, 0x00,
  0xA7, 0x93, 0x3D, 0xE1, 0x80,
  0xFD, 0xB3, 0x34, 0x62, 0x80,
  0xBF, 0x64, 0x48, 0x45, 0x80,
  0x4C, 0xB1, 0x16, 0xAD, 0x00,
  0xCA, 0x08, 0x85, 0xE1, 0x80,
  0x49, 0xDD, 0xCE, 0xE2, 0x80,
  0x8F, 0x3B, 0xA4, 0x45, 0x80,
  0x45, 0x8E, 0xFD, 0x51, 0x00,
  0x86, 0xB3, 0x27, 0xF8, 0x00,
  0x00, 0xF3, 0x29, 0x89, 0x80,
  0xFE, 0xA4, 0x4D, 0xAF, 0x80,
  0x82, 0x91, 0x1E, 0x88, 0x80,
  0xBA, 0xA8, 0x84, 0xFD, 0x80,
  0xBA, 0x7D, 0xCE, 0x89, 0x80,
  0xBA, 0xBB, 0xA7, 0x9A, 0x80,
  0x82, 0xCE, 0xFF, 0xA1, 0x00,
  0xFE, 0xF3, 0x2D, 0xD9, 0x80
};

void disegnaQR(int ox, int oy) {
  for (int y = 0; y < QR_SIZE; y++) {
    for (int x = 0; x < QR_SIZE; x++) {
      int byteIdx = y * 5 + (x / 8);
      int bitIdx = 7 - (x % 8);
      if (pgm_read_byte(&qrBitmap[byteIdx]) & (1 << bitIdx)) {
        display.fillRect(ox + x * QR_SCALE, oy + y * QR_SCALE, QR_SCALE, QR_SCALE, GxEPD_BLACK);
      }
    }
  }
}

void disegnaMessaggioWifi() {
  const int sepX = 249;
  int qrPx = QR_SIZE * QR_SCALE;
  int qrX = sepX + 1 + (416 - sepX - 1 - qrPx) / 2;
  int qrY = 6;

  // Pulizia morbida al posto del full refresh nativo: questa schermata compare
  // quando l'utente preme il pulsante, ed e' il momento peggiore per spaventarlo
  // con cinquanta lampeggi.
  pulisciSchermo();

  display.setPartialWindow(0, 0, display.width(), display.height());
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    u8g2Fonts.setForegroundColor(GxEPD_BLACK);

    u8g2Fonts.setFont(u8g2_font_helvB14_tf);
    u8g2Fonts.setCursor(38, 28);
    u8g2Fonts.print("Setup");

    u8g2Fonts.setFont(u8g2_font_helvR10_tf);
    u8g2Fonts.setCursor(38, 56);
    u8g2Fonts.print("1. Press and hold the");
    u8g2Fonts.setCursor(48, 72);
    u8g2Fonts.print("button below for 3 sec");
    u8g2Fonts.setCursor(38, 98);
    u8g2Fonts.print("2. On your phone, connect");
    u8g2Fonts.setCursor(48, 114);
    u8g2Fonts.print("to Wi-Fi \"orologio\"");
    u8g2Fonts.setCursor(38, 140);
    u8g2Fonts.print("3. Select your home");
    u8g2Fonts.setCursor(48, 156);
    u8g2Fonts.print("Wi-Fi network");
    u8g2Fonts.setCursor(38, 182);
    u8g2Fonts.print("4. Enter your Wi-Fi");
    u8g2Fonts.setCursor(48, 198);
    u8g2Fonts.print("password and save");

    u8g2Fonts.setFont(u8g2_font_helvB10_tf);
    u8g2Fonts.setCursor(38, 228);
    u8g2Fonts.print("ETERNO - Orologio");

    display.drawFastVLine(sepX, 0, 240, GxEPD_BLACK);

    disegnaQR(qrX, qrY);

    u8g2Fonts.setFont(u8g2_font_helvR10_tf);
    int tx = sepX + 8;
    int ty = qrY + qrPx + 14;
    u8g2Fonts.setCursor(tx, ty);
    u8g2Fonts.print("English");
    u8g2Fonts.setCursor(tx, ty + 16);
    u8g2Fonts.print("Italiano");
    u8g2Fonts.setCursor(tx, ty + 32);
    u8g2Fonts.print("Fran\xc3\xa7" "ais");
    u8g2Fonts.setCursor(tx, ty + 48);
    u8g2Fonts.print("Espa\xc3\xb1" "ol");
    u8g2Fonts.setCursor(tx, ty + 64);
    u8g2Fonts.print("Deutsch");
    u8g2Fonts.setCursor(tx, ty + 80);
    u8g2Fonts.print("Portugu\xc3\xaa" "s");
  } while (display.nextPage());
}

// -------------------------------------------------------
// WIFI E NTP
// -------------------------------------------------------

int sincronizzaNTP() {
  int drift = 0;

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin();
    int tentativi = 0;
    while (WiFi.status() != WL_CONNECTED && tentativi < 30) {
      delay(500);
      tentativi++;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    struct tm preInfo;
    bool avevaOra = getLocalTime(&preInfo);
    time_t pre = time(NULL);
    unsigned long msStart = millis();

    configTime(0, 0, "pool.ntp.org");
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    struct tm timeinfo;
    int tentativi = 0;
    while (!getLocalTime(&timeinfo) && tentativi < 10) {
      delay(500);
      tentativi++;
    }

    if (getLocalTime(&timeinfo)) {
      if (avevaOra) {
        time_t post = time(NULL);
        int elapsed = (millis() - msStart) / 1000;
        drift = (int)(post - pre) - elapsed;
      }
      fallimentiNTP = 0;
    } else {
      fallimentiNTP++;
    }
  } else {
    fallimentiNTP++;
  }

  return drift;
}

void spegniWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void inviaTelemtria(int drift) {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin();
    int tentativi = 0;
    while (WiFi.status() != WL_CONNECTED && tentativi < 30) {
      delay(500);
      tentativi++;
    }
    if (WiFi.status() != WL_CONNECTED) {
      spegniWiFi();
      return;
    }
  }

  String lat = "", lon = "", city = "";
  HTTPClient http;
  http.begin("http://ip-api.com/json/?fields=lat,lon,city");
  if (http.GET() == 200) {
    String body = http.getString();
    int latIdx = body.indexOf("\"lat\":");
    int lonIdx = body.indexOf("\"lon\":");
    int cityIdx = body.indexOf("\"city\":\"");
    if (latIdx >= 0 && lonIdx >= 0 && cityIdx >= 0) {
      int latEnd = body.indexOf(',', latIdx + 6);
      if (latEnd < 0) latEnd = body.indexOf('}', latIdx + 6);
      lat = body.substring(latIdx + 6, latEnd);

      int lonEnd = body.indexOf(',', lonIdx + 6);
      if (lonEnd < 0) lonEnd = body.indexOf('}', lonIdx + 6);
      lon = body.substring(lonIdx + 6, lonEnd);

      int cityStart = cityIdx + 8;
      int cityEnd = body.indexOf('"', cityStart);
      city = body.substring(cityStart, cityEnd);
      city.replace(" ", "%20");
    }
  }
  http.end();

  String url = String(GOOGLE_SHEETS_URL)
    + "?mac=" + WiFi.macAddress()
    + "&v=" + String(tensioneBatteria, 4)
    + "&drift=" + String(drift)
    + "&rssi=" + String(WiFi.RSSI())
    + "&temp=" + String(temperaturaInterna, 1)
    + "&fw=" + FW_VERSION
    + "&days=" + String(giorniDaBoot)
    + "&lat=" + lat
    + "&lon=" + lon
    + "&city=" + city
    + "&lowbat=" + String(tensioneBatteria > 0 && tensioneBatteria < SOGLIA_BATTERIA_SCARICA ? 1 : 0)
    + "&usb=" + String(digitalRead(PIN_USB))
    + "&xtal=" + String(freqClockLento);

  WiFiClientSecure client;
  client.setInsecure();
  http.begin(client, url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.GET();
  http.end();
}

void avviaWiFiManager() {
  disegnaMessaggioWifi();

  WiFiManager wm;

  wm.setShowStaticFields(false);
  wm.setShowDnsFields(false);
  wm.setShowInfoErase(false);
  wm.setShowInfoUpdate(false);
  wm.setMinimumSignalQuality(10);
  wm.setScanDispPerc(true);
  wm.setDarkMode(false);
  wm.setTitle("Orologio");

  const char* menuItems[] = {"wifi"};
  wm.setMenu(menuItems, 1);
  wm.setCustomHeadElement(
      "<script>if(location.pathname==='/')location.replace('/wifi');</script>"
      "<style>label[for='s'],input#s{display:none;}</style>"
  );

  wm.setConfigPortalBlocking(false);
  wm.startConfigPortal("orologio");

  // Potenza di trasmissione ridotta finche' siamo in access point. I beacon dell'AP
  // vanno in 802.11b a 1 Mbps e a piena potenza: 350 mA di picco secondo la tabella 12
  // del datasheet del modulo, che e' il caso peggiore in assoluto. Con quella corrente
  // la caduta su D1 sale a ~0.3V e il rail scende sotto i 3.0V minimi del modulo,
  // facendo scattare il brownout. Il telefono dell'utente e' a trenta centimetri:
  // undici dBm sono abbondanti.
  WiFi.setTxPower(POTENZA_TX_PORTALE);

  unsigned long inizio = millis();
  bool clienteConnesso = false;
  unsigned long timeoutMs = 60000;
  bool configurato = false;

  while (millis() - inizio < timeoutMs) {
    wm.process();

    if (!clienteConnesso && WiFi.softAPgetStationNum() > 0) {
      clienteConnesso = true;
      timeoutMs = 300000;
      inizio = millis();
    }

    if (WiFi.status() == WL_CONNECTED) {
      configurato = true;
      break;
    }

    delay(100);
  }

  if (configurato) {
    // Ripristina la potenza piena: qui si parla col router di casa, che puo' essere
    // in un'altra stanza. Il consumo torna alto ma in station mode e' meno critico,
    // perche' non ci sono i beacon a 1 Mbps.
    WiFi.setTxPower(POTENZA_TX_NORMALE);
    sincronizzaNTP();
    spegniWiFi();
  }

  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    // Anche il ritorno all'orologio passa dalla pulizia morbida: si viene da una
    // schermata densa di testo e QR, ed e' la transizione che lascerebbe piu' ghosting.
    pulisciSchermo();
    disegnaOrario(false, timeinfo.tm_hour, timeinfo.tm_min);
  }
}

// -------------------------------------------------------
// CICLO RICARICA — resta sveglio finché il cavo USB è collegato
// -------------------------------------------------------

void cicloRicarica() {
  inRicarica = true;
  caricaCompleta = false;
  frameRicarica = 0;
  int minutoPrecedente = -1;
  unsigned long ultimaLettura = 0;

  // INPUT e NON INPUT_PULLUP, anche se STDBY del TP5000 e' un open-drain.
  // Sullo schematico R8 (1M) e' in serie tra STDBY e IO9, per disaccoppiare l'uscita
  // del caricatore dal pin di strapping GPIO9 (sullo stesso nodo c'e' SW3, il tasto
  // BOOT). Il pull-up interno vale ~45k: in serie col megaohm formerebbe un partitore
  // che tiene il pin a ~3.16V, e il livello basso non sarebbe mai rilevabile.
  pinMode(PIN_STDBY, INPUT);
  pinMode(PIN_USB, INPUT);

  leggiBatteria();
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    minutoPrecedente = timeinfo.tm_min;
    disegnaOrario(false, timeinfo.tm_hour, timeinfo.tm_min);
  }

  while (digitalRead(PIN_USB)) {
    if (millis() - ultimaLettura >= 5000) {
      leggiBatteria();
      ultimaLettura = millis();

      if (!caricaCompleta && digitalRead(PIN_STDBY) == LOW) {
        caricaCompleta = true;
        // Chiude l'animazione sull'icona piena: senza questa chiamata l'ultimo
        // fotogramma disegnato resterebbe a schermo fino al cambio di minuto,
        // e poteva essere anche quello a zero tacche, cioe' batteria vuota.
        animaBatteria();
      }
    }

    struct tm ti;
    if (getLocalTime(&ti)) {
      if (ti.tm_min != minutoPrecedente) {
        minutoPrecedente = ti.tm_min;
        disegnaOrario(false, ti.tm_hour, ti.tm_min);
      }
    }

    if (!caricaCompleta) {
      frameRicarica = (frameRicarica + 1) % 4;
      animaBatteria();
      delay(250);
    } else {
      delay(1000);
    }
  }

  inRicarica = false;
  caricaCompleta = false;
  leggiBatteria();
  if (tensioneBatteria >= SOGLIA_BATTERIA_SCARICA) {
    batteriaScaricaMostrata = false;
  }
  usbCollegato = false;

  int16_t bx = display.width() - 30;
  int16_t by = display.height() - 18;
  display.setPartialWindow(bx - 1, by - 1, 29, 16);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
  } while (display.nextPage());
  // L'angolo e' stato ripulito: la fotografia di cio' che e' sul pannello va
  // aggiornata, altrimenti la ricostruzione del buffer precedente ridisegnerebbe
  // un'icona che non c'e' piu' e quella nuova non verrebbe pilotata.
  iconaMostrata = -1;
}

// -------------------------------------------------------
// PERCORSI DI RISVEGLIO
// -------------------------------------------------------

// Pressione lunga del pulsante: apre il portale WiFi.
// Pressione breve con USB presente: entra nel ciclo di ricarica.
void gestisciRisveglioGPIO() {
  pinMode(BUTTON_PIN, INPUT_PULLDOWN);
  pinMode(PIN_USB, INPUT);
  delay(10);

  // Con il cavo collegato il pulsante e' disabilitato e vince sempre il ciclo di
  // ricarica. Aprire il portale WiFi mentre la scheda e' al PC non ha senso, e
  // sommare i 350 mA di picco dell'access point alla corrente di carica fa cedere
  // il rail: e' quello che sporcava lo schermo di grigio.
  if (digitalRead(PIN_USB)) {
    initDisplay(false);
    leggiBatteria();
    cicloRicarica();
    return;
  }

  if (digitalRead(BUTTON_PIN) == HIGH) {
    // Alimenta subito il pannello, PRIMA di contare i secondi di pressione.
    // Con VCC_DISPLAY attivo il UC8253 e' in uno stato definito e non puo' piu'
    // essere retro-alimentato a meta': lo sfondo resta bianco per tutta l'attesa.
    // initial = false: con true GxEPD2 convertirebbe il primo refresh parziale in
    // uno nativo, cioe' i cinquanta lampeggi proprio dopo la pressione.
    initDisplay(false);

    unsigned long inizio = millis();
    bool premutoLungo = false;

    while (millis() - inizio < DURATA_PRESSIONE_LUNGA) {
      if (digitalRead(BUTTON_PIN) == LOW) {
        premutoLungo = false;
        break;
      }
      premutoLungo = true;
      delay(10);
    }

    if (premutoLungo) {
      avviaWiFiManager();
    } else {
      // Pressione breve: ridisegna l'orologio, cosi' l'utente vede una schermata
      // pulita invece di quella che ha trovato al risveglio.
      struct tm timeinfo;
      if (getLocalTime(&timeinfo)) {
        disegnaOrario(false, timeinfo.tm_hour, timeinfo.tm_min);
      }
    }
  }
}

// Accensione da spento: configura il quarzo 32.768 kHz, rileva devMode dal
// pulsante, mostra subito 00:00 e poi l'ora vera appena NTP risponde.
void gestisciPowerCycle() {
  rtc_clk_32k_enable(true);
  rtc_clk_32k_bootstrap(512);

  pinMode(BUTTON_PIN, INPUT_PULLDOWN);
  delay(500);
  devMode = (digitalRead(BUTTON_PIN) == HIGH);

  rtc_clk_slow_src_set(SOC_RTC_SLOW_CLK_SRC_XTAL32K);
  delay(200);                              // lascia partire l'oscillatore
  allineaClockLento();

  initDisplay(true);
  disegnaOrario(true, 0, 0);

  leggiBatteria();
  int drift = sincronizzaNTP();
  inviaTelemtria(drift);
  spegniWiFi();

  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    disegnaOrario(false, timeinfo.tm_hour, timeinfo.tm_min);
  } else {
    fallimentiNTP = 3;
  }
}

// Ora di sistema arrotondata al minuto piu' vicino.
// Serve perche' il firmware si sveglia ANTICIPO_RISVEGLIO secondi PRIMA dello scoccare
// del minuto, in modo che il refresh del pannello — che dura circa 0.8 s tra boot,
// accensione di Q3 e aggiornamento dell'inchiostro — finisca esattamente sul minuto.
// Senza arrotondamento, al risveglio si leggerebbe ancora il minuto precedente e il
// display mostrerebbe l'orario sbagliato.
// L'arrotondamento tollera un errore di sveglia fino a 30 s in entrambi i versi, quindi
// e' immune al jitter e si autocorregge anche se un ciclo dovesse allungarsi.
bool oraArrotondata(struct tm& out) {
  time_t adesso;
  time(&adesso);
  time_t minutoVicino = ((adesso + 30) / 60) * 60;
  localtime_r(&minutoVicino, &out);
  return (out.tm_year > (2016 - 1900));
}

// Aggiornamento al minuto. Ramo unico per modalita' normale e devMode: cambia
// soltanto QUANDO scattano manutenzione e full refresh, non cosa viene fatto.
// In devMode il tempo e' simulato da contatoreRisvegli, dove 60 risvegli valgono
// un'ora e 1440 un giorno; in modalita' normale si guarda l'orologio vero.
void tickMinuto(struct tm& timeinfo) {
  initDisplay(false);

  bool fullRefresh  = false;
  bool manutenzione = false;   // lettura batteria + invio telemetria
  bool avanzaGiorno = false;   // rollover del giorno: incrementa giorniDaBoot e sincronizza NTP

  if (devMode) {
    contatoreRisvegli++;
    manutenzione = (contatoreRisvegli % 60 == 0);
    if (contatoreRisvegli >= 1440) {
      contatoreRisvegli = 0;
      avanzaGiorno = true;
      fullRefresh  = true;
    }
  } else {
    manutenzione = (timeinfo.tm_hour == ORA_MANUTENZIONE && timeinfo.tm_min == 0);
    avanzaGiorno = manutenzione;
    // Un solo full refresh nativo al giorno, nello stesso slot della manutenzione:
    // un unico momento "rumoroso" alle 3 di notte invece di quattro fra le 2 e le 5.
    // A tenere puliti i pixel durante il giorno ci pensa l'inversione allo scoccare
    // di ogni ora, che pilota comunque l'intera matrice due volte.
    fullRefresh  = manutenzione;
  }

  if (manutenzione) {
    leggiBatteria();

    if (avanzaGiorno) {
      // Sincronizzazione NTP GIORNALIERA, e volutamente PRIMA del full refresh:
      // con un quarzo nudo la deriva e' di qualche secondo al giorno (~46 ppm
      // misurati), quindi una risincronizzazione settimanale accumulerebbe mezzo
      // minuto di errore. Questo e' il momento di massimo consumo del dispositivo
      // ed e' una scelta consapevole: WiFi acceso e full refresh nello stesso
      // slot notturno, quando nessuno guarda.
      giorniDaBoot++;
      int drift = sincronizzaNTP();
      oraArrotondata(timeinfo);   // l'ora e' appena cambiata sotto i piedi
      inviaTelemtria(drift);
    } else {
      inviaTelemtria(0);
    }
    spegniWiFi();
  }

  if (tensioneBatteria > 0 && tensioneBatteria < SOGLIA_BATTERIA_SCARICA) {
    batteriaScaricaMostrata = true;
  }

  disegnaOrario(fullRefresh, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_min == 0);
}

// Risveglio da timer: se l'ora di sistema manca si tenta una sincronizzazione,
// altrimenti si esegue il normale aggiornamento al minuto.
void gestisciRisveglioTimer() {
  struct tm timeinfo;

  if (!oraArrotondata(timeinfo)) {
    initDisplay(false);
    sincronizzaNTP();
    spegniWiFi();
    if (oraArrotondata(timeinfo)) {
      disegnaOrario(true, timeinfo.tm_hour, timeinfo.tm_min);
    }
    return;
  }

  tickMinuto(timeinfo);
}

// -------------------------------------------------------
// SETUP — eseguito ad ogni risveglio
// -------------------------------------------------------

// Mira a svegliarsi ANTICIPO_RISVEGLIO secondi PRIMA dello scoccare del minuto, cosi'
// che il refresh finisca sul minuto esatto e le cifre cambino in sincronia con un
// telefono. Usa l'ora grezza, non quella arrotondata: qui servono i secondi veri.
// In devMode il tempo e' accelerato e l'anticipo non ha senso.
int calcolaSecondiSleep() {
  if (devMode) return max(1, 60 / TEMPO_ACCELERATO);

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return 60;

  int secondi = 60 - timeinfo.tm_sec - ANTICIPO_RISVEGLIO;
  while (secondi <= 0) secondi += 60;
  return secondi;
}

// Unica uscita del firmware: qui convergono portale WiFi, ciclo ricarica e tick.
void vaiInDeepSleep(int secondiSleep) {
  spegniDisplay();

  pinMode(BUTTON_PIN, INPUT_PULLDOWN);
  pinMode(PIN_USB, INPUT);

  const uint64_t wakeupMask = (1ULL << BUTTON_PIN) | (1ULL << PIN_USB);
  esp_deep_sleep_enable_gpio_wakeup(wakeupMask, ESP_GPIO_WAKEUP_GPIO_HIGH);
  esp_sleep_enable_timer_wakeup((uint64_t)secondiSleep * 1000000ULL);

  esp_deep_sleep_start();
}

void setup() {
  // Stato di partenza esplicito: pad in alta impedenza, R13 tiene Q3 interdetto.
  // Il display resta scollegato finche' un percorso non chiama initDisplay().
  pinMode(PIN_DISPLAY_EN, INPUT);
  displayAlimentato = false;
  displayInizializzato = false;

  // Isolamento immediato delle linee verso il pannello. Finche' VCC_DISPLAY e' spento
  // ogni pin pilotato retro-alimenta il UC8253 attraverso i diodi di protezione: il
  // controller si sveglia a meta' e sporca lo schermo di grigio. Il ROM bootloader
  // pilota GPIO21 come TX di UART0 prima ancora che parta il nostro codice, quindi
  // queste righe devono essere le prime a girare.
  pinMode(RES_PIN, INPUT);
  pinMode(DC_PIN, INPUT);
  pinMode(CS_PIN, INPUT);
  pinMode(BUSY_PIN, INPUT);

  temperaturaInterna = temperatureRead();

  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();

  esp_sleep_wakeup_cause_t causa = esp_sleep_get_wakeup_cause();

  if (causa == ESP_SLEEP_WAKEUP_TIMER || causa == ESP_SLEEP_WAKEUP_GPIO) {
    rtc_clk_32k_enable(true);
    rtc_clk_slow_src_set(SOC_RTC_SLOW_CLK_SRC_XTAL32K);
    allineaClockLento();
  }

  if (causa == ESP_SLEEP_WAKEUP_GPIO) {
    gestisciRisveglioGPIO();
  } else if (causa == ESP_SLEEP_WAKEUP_TIMER) {
    gestisciRisveglioTimer();
  } else {
    gestisciPowerCycle();
  }

  vaiInDeepSleep(calcolaSecondiSleep());
}


void loop() {}
