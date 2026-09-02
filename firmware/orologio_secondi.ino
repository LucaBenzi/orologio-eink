#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <WiFiManager.h>
#include <WiFi.h>
#include <time.h>
#include <Preferences.h>
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "soc/rtc.h"
// esp_clk_slowclk_cal_get() lives in a private ESP-IDF header: if the core
// version doesn't expose it, diagnostics degrade without breaking compilation.
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
// BATTERY CHEMISTRY
// Uncomment ONE line only, the one matching the installed battery pack.
// The only parameter that depends on it is SOGLIA_BATTERIA_SCARICA, i.e. when
// the low-battery icon appears. Nothing else in the file needs to change.
// Remember to match the choice with jumpers JP_DIODE / JP_LDO / JP_CS.
// =======================================================
#define BATTERIA_LIFEPO4
//#define BATTERIA_LIPO
//#define BATTERIA_3AA

#if (defined(BATTERIA_LIFEPO4) + defined(BATTERIA_LIPO) + defined(BATTERIA_3AA)) != 1
  #error "Selezionare esattamente una chimica: BATTERIA_LIFEPO4, BATTERIA_LIPO o BATTERIA_3AA."
#endif

#if defined(BATTERIA_LIFEPO4)
  // LiFePO4 1S: nominal 3.2V, end-of-charge 3.6V, discharge knee ~3.0V.
  // Very flat curve: stays above 3.3V for most of the capacity,
  // so 3.2V corresponds roughly to the last 20% of runtime.
  // Jumpers: JP_DIODE closed, JP_LDO open, JP_CS open (cutoff 3.6V).
  #define SOGLIA_BATTERIA_SCARICA 3.2
#elif defined(BATTERIA_LIPO)
  // Li-Po / Li-ion 1S: nominal 3.7V, end-of-charge 4.2V, cutoff 3.0V.
  // Jumpers: JP_DIODE open, JP_LDO closed, JP_CS closed (cutoff 4.2V).
  #define SOGLIA_BATTERIA_SCARICA 3.5
#elif defined(BATTERIA_3AA)
  // 3 AA alkaline cells in series: nominal 3 x 1.5 = 4.5V, fresh up to 4.8V.
  // 3.3V means 1.1V per cell, i.e. practically depleted.
  // Not rechargeable: no TP5000, no USB, cicloRicarica() never runs.
  // Jumpers: JP_DIODE open, JP_LDO closed. USB-C not populated.
  #define SOGLIA_BATTERIA_SCARICA 3.3
#endif

// ADC voltage divider: R10 and R11 equal, so factor 0.5 and multiplier 2.
// The absolute value doesn't matter as long as the two are equal — only the
// ratio matters. Sized for worst case (4.8V -> 2.4V at ADC, within full scale
// of ESP32-C3 with 11 dB attenuation).
#define R_PARTITORE_ALTA  2000000.0   // R10, towards VBAT
#define R_PARTITORE_BASSA 2000000.0   // R11, towards GND
#define FATTORE_PARTITORE ((R_PARTITORE_ALTA + R_PARTITORE_BASSA) / R_PARTITORE_BASSA)

// Behavioral thresholds, previously scattered as magic numbers inside setup().
#define DURATA_PRESSIONE_LUNGA 1500  // ms of continuous press to open the WiFi portal
#define ORA_MANUTENZIONE 3           // hour when battery is read and telemetry is sent
// NTP sync is daily, in the same maintenance slot: with a bare crystal the drift
// is a few seconds per day and a weekly cadence would accumulate half a minute of error.

// Seconds of early wake-up before the minute rolls over. The panel refresh takes
// about 0.8 s (boot, Q3 turn-on, ink update): waking up early makes the digits
// change EXACTLY on the minute instead of after. Retune if a different refresh
// duration is measured.
#define ANTICIPO_RISVEGLIO 1

// Black/white passes of the soft screen clean. A single pass is already enough
// in normal use; increase to 2 if residual ghosting is noticed after the WiFi
// portal. Each pass costs about one second.
#define CICLI_PULIZIA_SCHERMO 1

// Slow clock cycles counted for calibration. 256 cycles at 32768 Hz cost about
// 8 ms of awake time per wake-up and give a resolution of a few ppm: increasing
// it barely improves accuracy while worsening average power consumption.
#define CICLI_CALIBRAZIONE_RTC 256

// WiFi transmit power. In access-point mode the 802.11b beacons at 1 Mbps at
// full power draw 350 mA peak: too much for the diode path. Lower here if the
// rail still sags, raise if the phone struggles to see the network.
#define POTENZA_TX_PORTALE WIFI_POWER_11dBm
#define POTENZA_TX_NORMALE WIFI_POWER_19_5dBm

// Q3 gate (AO3401A, P-ch): LOW = display powered, HIGH or hi-Z = off.
// R13 (100k to +3.3V) ensures cut-off when the ESP32 releases the pad.
#define ATTESA_RAIL_DISPLAY 50   // ms settling time for VCC_DISPLAY after turn-on
#define DURATA_RESET_DISPLAY 20  // ms pulse on RES: the UC8253 does a cold start
#define GOOGLE_SHEETS_URL "https://script.google.com/macros/s/AKfycbz1fNkQNdFf_IsT9enqTU-47g7L5uAMJDx-hzJf2sJf3KGpJOuUKP3DO-dTtvNoLfhJ/exec"

RTC_DATA_ATTR float tensioneBatteria = 0.0;
RTC_DATA_ATTR int giorniDaBoot = 0;
RTC_DATA_ATTR bool usbCollegato = false;
RTC_DATA_ATTR int fallimentiNTP = 0;
RTC_DATA_ATTR bool batteriaScaricaMostrata = false;
RTC_DATA_ATTR bool devMode = false;
RTC_DATA_ATTR int contatoreRisvegli = 0;
// Last time actually shown on the panel: survives deep sleep and is used to
// reconstruct the controller's "previous" buffer after VCC is cut.
RTC_DATA_ATTR int oraMostrata = -1;
RTC_DATA_ATTR int minutoMostrato = -1;
RTC_DATA_ATTR float voltMostrati = 0.0;
RTC_DATA_ATTR int iconaMostrata = -1;   // -1 none, 0 empty battery, 1..3 charging bars
RTC_DATA_ATTR bool avvisoWifiMostrato = false;
RTC_DATA_ATTR bool invertitoMostrato = false;
uint32_t freqClockLento = 0;              // measured at every wake-up
char diagBuf[48] = "";                    // diagnostic line shown in devMode
RTC_DATA_ATTR char diagMostrata[48] = ""; // same line, as it is on the panel
float temperaturaInterna = 0.0;
bool inRicarica = false;
bool caricaCompleta = false;
int frameRicarica = 0;

// Panel power state. Deliberately NOT RTC_DATA_ATTR: at every wake-up the chip
// restarts from reset, the pad goes to hi-Z and R13 turns off the display,
// so the correct state at boot is always "off".
bool displayAlimentato = false;
bool displayInizializzato = false;
// True until the controller's "previous" RAM has been reconstructed in this cycle.
bool ramPannelloDaRicostruire = false;

// Actual frequency of the RTC slow clock, the one that times deep sleep and
// therefore the clock's accuracy. rtc_clk_cal() counts slow-clock cycles against
// the module's 40 MHz crystal and returns the period in microseconds, in Q13.19
// fixed point. If the external crystal is working, expect ~32768 Hz; if the
// firmware is running on the internal RC oscillator, the value will be much
// higher and unstable. This is NOT the same as rtc_clk_slow_src_get(), which
// simply re-reads the register just written and always says yes.
uint32_t frequenzaClockLento() {
  uint32_t periodo = rtc_clk_cal(RTC_CAL_RTC_MUX, CICLI_CALIBRAZIONE_RTC);
  if (periodo == 0) return 0;
  return (uint32_t)((1000000ULL << 19) / periodo);
}

// Measures the actual slow-clock frequency and compares it with the calibration
// constant that ESP-IDF uses to convert ticks to microseconds. For a 32768 Hz
// crystal the period is 30.5176 us, which in Q13.19 format equals exactly
// 16,000,000: any deviation from that value is the clock's systematic error,
// expressed directly in ppm.
void allineaClockLento() {
  // Measure the real slow-clock period, once, and use it for two things.
  uint32_t periodo = rtc_clk_cal(RTC_CAL_RTC_MUX, CICLI_CALIBRAZIONE_RTC);
  if (periodo == 0) {
    snprintf(diagBuf, sizeof(diagBuf), "RTC non calibrabile");
    return;
  }
  freqClockLento = (uint32_t)((1000000ULL << 19) / periodo);

#if HA_CALIBRAZIONE_ESPIDF
  // THIS is the correction. rtc_clk_slow_src_set() switches the hardware but
  // ESP-IDF keeps converting ticks to microseconds with the constant computed
  // at boot, when the source was still the internal RC oscillator at ~131 kHz.
  // The wrong factor is about 4x, and it applies to time spent in light sleep
  // inside displayBusyCallback(): at every BUSY wait the clock is credited
  // a quarter of the time actually elapsed, and falls behind.
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
  // Light sleep switches digital pads to their sleep configuration, which
  // defaults to hi-Z. On GPIO8 that would leave Q3's gate to R13, cutting
  // VCC_DISPLAY in the middle of a refresh. The hold freezes the level.
  gpio_hold_en((gpio_num_t)PIN_DISPLAY_EN);
  esp_light_sleep_start();
  gpio_hold_dis((gpio_num_t)PIN_DISPLAY_EN);
}

// Powers the panel and initializes it. Idempotent.
// The "reset" parameter is GxEPD2's "initial" flag: when true the library
// converts the first partial refresh into a full one. Must be false on timer
// wake-ups, otherwise every minute becomes a full refresh.
void initDisplay(bool reset) {
  if (displayInizializzato) return;

  pinMode(PIN_DISPLAY_EN, OUTPUT);
  digitalWrite(PIN_DISPLAY_EN, LOW);
  // The pad must keep this configuration even during light sleep.
  gpio_sleep_sel_dis((gpio_num_t)PIN_DISPLAY_EN);
  displayAlimentato = true;
  delay(ATTESA_RAIL_DISPLAY);

  // R12 pulls RES to VCC_DISPLAY, not to +3.3V: RES rises together with the
  // rail and the controller's power-on reset must come from firmware, not hardware.
  display.init(115200, reset, DURATA_RESET_DISPLAY, false);
  displayInizializzato = true;
  // The panel was just powered on: the controller's RAM needs to be reconstructed.
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

// Turns off the panel. Must be called on EVERY path before deep sleep, even
// when the display was never turned on in this cycle.
void spegniDisplay() {
  if (!displayAlimentato) {
    // Never turned on: leave the pad at hi-Z, R13 keeps Q3 in cut-off.
    pinMode(PIN_DISPLAY_EN, INPUT);
    return;
  }

  // 1) POF: the controller shuts down its own DC/DC. Cutting VCC with the
  //    high voltages still active stresses the panel. SPI must still be alive.
  if (displayInizializzato) display.powerOff();

  // 2) With the display off BUSY floats: disarm the light-sleep wake-up.
  gpio_wakeup_disable((gpio_num_t)BUSY_PIN);

  // 3) Disconnect lines to the panel BEFORE cutting VCC, otherwise the ESP32
  //    back-powers it through the input protection diodes.
  SPI.end();
  pinMode(RES_PIN, INPUT);
  pinMode(DC_PIN, INPUT);
  pinMode(CS_PIN, INPUT);
  pinMode(BUSY_PIN, INPUT);

  // 4) Only now cut off Q3, driving the gate for a clean switch-off.
  gpio_hold_dis((gpio_num_t)PIN_DISPLAY_EN);
  digitalWrite(PIN_DISPLAY_EN, HIGH);
  delay(5);

  // 5) Release the pad. GPIO8 is not an RTC GPIO on the ESP32-C3, so in deep
  //    sleep it goes to hi-Z anyway: R13 guarantees the high level.
  pinMode(PIN_DISPLAY_EN, INPUT);

  displayAlimentato = false;
  displayInizializzato = false;
}

// Colors are parametric because at the top of every hour the screen is inverted:
// with hardcoded values the icon would be drawn black on black.
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
  // This function also modifies the corner, so the snapshot used to
  // reconstruct the previous buffer must be updated too.
  iconaMostrata = tacche;
}

// Fills the entire screen with a color using the partial window, hence the
// fast LUT and not the native full-refresh one.
void riempiSchermo(uint16_t colore) {
  display.setPartialWindow(0, 0, display.width(), display.height());
  display.firstPage();
  do {
    display.fillScreen(colore);
  } while (display.nextPage());
}

// Soft clean: black, white, and the screen is left white ready for drawing.
// Alternative to the native full refresh, which shakes the ink by inverting the
// panel about fifty times in two seconds — technically correct, but visually
// alarming for someone who doesn't know what they're looking at.
// Cleans less thoroughly, so it does NOT replace the nightly full refresh.
void pulisciSchermo() {
  // After VCC cut the controller's "previous" RAM is undefined, so the first
  // black pass would only drive pixels that happen to differ by chance.
  // Initialize it to white: this way every pixel registers as changed and
  // actually gets driven.
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

  // The panel is now white and the controller's "previous" RAM is too, because
  // the last pass realigned it. By zeroing the snapshot, the next draw starts
  // from white with a correct differential and without attempting reconstructions.
  oraMostrata = -1;
  minutoMostrato = -1;
  iconaMostrata = -1;
  avvisoWifiMostrato = false;
  invertitoMostrato = false;
  ramPannelloDaRicostruire = false;
}

#define OFFSET_CORNICE 15

// Draws a screen into the current page buffer. Factored out of disegnaOrario()
// because it now needs to run twice: once to reconstruct the previous image in
// the controller's RAM, once for the new one.
// All variable elements are passed as parameters, none read from globals: the
// previous-buffer reconstruction must redraw the THEN state, not the current one.
// icona: -1 none, 0 empty battery, 1..3 charging bars.
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

// Inverted: at the top of every hour black and white swap. It's not just
// cosmetic — by inverting, EVERY pixel in the window changes state and is
// therefore driven by the partial refresh. One minute later it reverts. That's
// two full-matrix passes per hour, which refresh contrast without the flicker
// of a true full refresh.
void disegnaOrario(bool fullRefresh, int ore, int minuti, bool invertito = false) {
  int icona = -1;
  if (inRicarica) {
    icona = caricaCompleta ? 3 : frameRicarica;
  } else if (batteriaScaricaMostrata) {
    icona = 0;
  }
  bool avvisoWifi = (fallimentiNTP >= 3);

  // Cutting VCC zeroes the UC8253's RAM, including the "previous" buffer that
  // the differential comparison starts from. Without reconstructing it, partial
  // refresh produces garbage: GxEPD2's author himself warns about this in the
  // init() comment.
  // nextPageToPrevious() writes into the old RAM without performing any refresh.
  // It is critical to pass the STORED state and not the current one: if the
  // previous image were reconstructed with the current icon, the controller
  // would consider it already on the panel and not draw it — it would come out
  // grey instead of black.
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

  // From here on, both controller RAMs are aligned with what is on the panel.
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

  // Soft clean instead of native full refresh: this screen appears when the
  // user presses the button, the worst moment to scare them with fifty flashes.
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
// WIFI AND NTP
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

  // Reduced transmit power while in access-point mode. AP beacons go out in
  // 802.11b at 1 Mbps and at full power draw 350 mA peak according to the
  // module datasheet table 12 — the absolute worst case. At that current the
  // drop across D1 rises to ~0.3V and the rail falls below the module's 3.0V
  // minimum, triggering brownout. The user's phone is thirty centimeters away:
  // eleven dBm is plenty.
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
    // Restore full power: we're now talking to the home router, which may be
    // in another room. Current draw goes back up but in station mode it's less
    // critical because there are no 1 Mbps beacons.
    WiFi.setTxPower(POTENZA_TX_NORMALE);
    sincronizzaNTP();
    spegniWiFi();
  }

  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    // The return to the clock also goes through the soft clean: coming from a
    // screen dense with text and QR code, this transition would leave the most
    // ghosting.
    pulisciSchermo();
    disegnaOrario(false, timeinfo.tm_hour, timeinfo.tm_min);
  }
}

// -------------------------------------------------------
// CHARGING LOOP — stays awake while the USB cable is connected
// -------------------------------------------------------

void cicloRicarica() {
  inRicarica = true;
  caricaCompleta = false;
  frameRicarica = 0;
  int minutoPrecedente = -1;
  unsigned long ultimaLettura = 0;

  // INPUT and NOT INPUT_PULLUP, even though the TP5000's STDBY is open-drain.
  // On the schematic R8 (1M) sits in series between STDBY and IO9, to decouple
  // the charger output from the GPIO9 strapping pin (SW3, the BOOT button, is
  // on the same node). The internal pull-up is ~45k: in series with the megaohm
  // it would form a divider holding the pin at ~3.16V, and the low level would
  // never be detectable.
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
        // Close the animation on the full icon: without this call the last
        // drawn frame would stay on screen until the minute changes, and it
        // could have been the zero-bar frame, i.e. empty battery.
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
  // The corner has been cleared: the snapshot of what is on the panel must be
  // updated, otherwise the previous-buffer reconstruction would redraw an icon
  // that is no longer there and the new one would not be driven.
  iconaMostrata = -1;
}

// -------------------------------------------------------
// WAKE-UP PATHS
// -------------------------------------------------------

// Long button press: opens the WiFi portal.
// Short press with USB present: enters the charging loop.
void gestisciRisveglioGPIO() {
  pinMode(BUTTON_PIN, INPUT_PULLDOWN);
  pinMode(PIN_USB, INPUT);
  delay(10);

  // With the cable connected the button is disabled and the charging loop
  // always wins. Opening the WiFi portal while the board is plugged into a PC
  // makes no sense, and adding the 350 mA peak of the access point on top of
  // the charging current makes the rail sag — that's what was smearing the
  // screen grey.
  if (digitalRead(PIN_USB)) {
    initDisplay(false);
    leggiBatteria();
    cicloRicarica();
    return;
  }

  if (digitalRead(BUTTON_PIN) == HIGH) {
    // Power the panel immediately, BEFORE counting the seconds of the press.
    // With VCC_DISPLAY active the UC8253 is in a defined state and can no
    // longer be back-powered half-way: the background stays white for the
    // entire wait.
    // initial = false: with true GxEPD2 would convert the first partial
    // refresh into a native one, i.e. the fifty flashes right after the press.
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
      // Short press: redraw the clock so the user sees a clean screen
      // instead of whatever was there at wake-up.
      struct tm timeinfo;
      if (getLocalTime(&timeinfo)) {
        disegnaOrario(false, timeinfo.tm_hour, timeinfo.tm_min);
      }
    }
  }
}

// Power-on from off: configures the 32.768 kHz crystal, detects devMode from
// the button, shows 00:00 immediately and then the real time once NTP responds.
void gestisciPowerCycle() {
  rtc_clk_32k_enable(true);
  rtc_clk_32k_bootstrap(512);

  pinMode(BUTTON_PIN, INPUT_PULLDOWN);
  delay(500);
  devMode = (digitalRead(BUTTON_PIN) == HIGH);

  rtc_clk_slow_src_set(SOC_RTC_SLOW_CLK_SRC_XTAL32K);
  delay(200);                              // let the oscillator start
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

// System time rounded to the nearest minute.
// Needed because the firmware wakes up ANTICIPO_RISVEGLIO seconds BEFORE the
// minute rolls over, so that the panel refresh — which takes about 0.8 s between
// boot, Q3 turn-on and ink update — finishes exactly on the minute.
// Without rounding, the previous minute would still be read at wake-up and the
// display would show the wrong time.
// The rounding tolerates a wake-up error of up to 30 s in either direction, so
// it is immune to jitter and self-corrects even if a cycle runs long.
bool oraArrotondata(struct tm& out) {
  time_t adesso;
  time(&adesso);
  time_t minutoVicino = ((adesso + 30) / 60) * 60;
  localtime_r(&minutoVicino, &out);
  return (out.tm_year > (2016 - 1900));
}

// Per-minute update. Single path for normal mode and devMode: only WHEN
// maintenance and full refresh trigger changes, not what is done.
// In devMode time is simulated by contatoreRisvegli, where 60 wake-ups equal
// one hour and 1440 equal one day; in normal mode the real clock is used.
void tickMinuto(struct tm& timeinfo) {
  initDisplay(false);

  bool fullRefresh  = false;
  bool manutenzione = false;   // battery reading + telemetry send
  bool avanzaGiorno = false;   // day rollover: increments giorniDaBoot and syncs NTP

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
    // A single native full refresh per day, in the same maintenance slot:
    // one "noisy" moment at 3 AM instead of four between 2 and 5.
    // Keeping pixels clean during the day is handled by the inversion at the
    // top of every hour, which drives the entire matrix twice anyway.
    fullRefresh  = manutenzione;
  }

  if (manutenzione) {
    leggiBatteria();

    if (avanzaGiorno) {
      // DAILY NTP sync, and deliberately BEFORE the full refresh: with a bare
      // crystal the drift is a few seconds per day (~46 ppm measured), so a
      // weekly resync would accumulate half a minute of error. This is the
      // device's peak power moment and a conscious choice: WiFi on and full
      // refresh in the same nightly slot, when no one is watching.
      giorniDaBoot++;
      int drift = sincronizzaNTP();
      oraArrotondata(timeinfo);   // the time just changed under our feet
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

// Timer wake-up: if system time is missing, attempt a sync;
// otherwise run the normal per-minute update.
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
// SETUP — runs at every wake-up
// -------------------------------------------------------

// Aims to wake up ANTICIPO_RISVEGLIO seconds BEFORE the minute rolls over, so
// that the refresh finishes on the exact minute and the digits change in sync
// with a phone. Uses raw time, not the rounded one: real seconds are needed here.
// In devMode time is accelerated and the early wake-up makes no sense.
int calcolaSecondiSleep() {
  if (devMode) return max(1, 60 / TEMPO_ACCELERATO);

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return 60;

  int secondi = 60 - timeinfo.tm_sec - ANTICIPO_RISVEGLIO;
  while (secondi <= 0) secondi += 60;
  return secondi;
}

// Single exit point of the firmware: WiFi portal, charging loop and tick all
// converge here.
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
  // Explicit initial state: pad at hi-Z, R13 keeps Q3 in cut-off.
  // The display stays disconnected until a path calls initDisplay().
  pinMode(PIN_DISPLAY_EN, INPUT);
  displayAlimentato = false;
  displayInizializzato = false;

  // Immediate isolation of lines towards the panel. While VCC_DISPLAY is off,
  // any driven pin back-powers the UC8253 through its protection diodes: the
  // controller wakes up half-way and smears the screen grey. The ROM bootloader
  // drives GPIO21 as UART0 TX before our code even starts, so these lines must
  // be the first to run.
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
