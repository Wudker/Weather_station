#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_SGP40.h>
#include <Rtc_Pcf8563.h>
#include <WiFi.h>
#include <esp_now.h>
#include "driver/rtc_io.h"

#include "PogodynkaTypes.h"

// ===================== USTAWIENIA =====================
#define uS_TO_S_FACTOR       1000000ULL
#define BACKUP_TIMER_MINUTES 60

const int RTC_INT_PIN = 27;        // INT z PCF8563 (aktywny LOW)

// MAC ODBIORNIKA (ESP32 z www)
uint8_t peerAddress[] = {0x28, 0x56, 0x2F, 0x71, 0x92, 0x34};

// ===== Domyślne wartości (zmienialne z odbiornika) =====
static const uint16_t DEFAULT_SLEEP_MIN   = 5;     // testowo 5 min (docelowo 20–30)
static const uint16_t DEFAULT_MEAS_SEC    = 120;   // 2 min okno pomiarowe
static const uint16_t DEFAULT_LISTEN_MS   = 1800;  // nasłuch na config po wysyłce

// ===== Konfiguracja przesyłana z odbiornika =====
static const uint8_t CFG_MAGIC   = 0xC3;
static const uint8_t CFG_VERSION = 1;

typedef struct __attribute__((packed)) {
  uint8_t  magic;         // CFG_MAGIC
  uint8_t  version;       // CFG_VERSION
  uint16_t sleep_minutes; // 1..180
  uint16_t meas_seconds;  // 30..300
  uint16_t listen_ms;     // 200..5000
} CmdConfig;

// Persist przez deep sleep (RTC RAM) – dzięki temu ustawienia nie giną po sleep
RTC_DATA_ATTR uint16_t cfgSleepMinutes = DEFAULT_SLEEP_MIN;
RTC_DATA_ATTR uint16_t cfgMeasSeconds  = DEFAULT_MEAS_SEC;
RTC_DATA_ATTR uint16_t cfgListenMs     = DEFAULT_LISTEN_MS;

// ===================== RTC – Twoje ustawienia czasu =====================
#define RTC_FORCE_SET_ON_BOOT 0

const uint8_t SET_DAY   = 17;
const uint8_t SET_WDAY  = 4;
const uint8_t SET_MONTH = 2;
const uint8_t SET_CENT  = 0;
const uint8_t SET_YEAR  = 26;
const uint8_t SET_HOUR  = 1;
const uint8_t SET_MIN   = 15;
const uint8_t SET_SEC   = 45;

const int8_t RTC_SEC_OFFSET = 0; // np. -7

// ===================== Sprzęt / piny =====================
Adafruit_BME280 bme;
Adafruit_SGP40  sgp;
Rtc_Pcf8563     rtc;

bool bme_ok = false;
bool sgp_ok = false;

const float V_ref = 3.3f;
const int resolution = 4095;

const int Hall_pin          = 17;
const int WaterSensor       = 19;
const int WaterSensorAnalog = 34;
const int Battery_level     = 32;
const int Fotopanel_level   = 35;

const float Battery_divider    = 1.0f/2.0f;
const float Fotopanel_divider  = 1.0f/2.0f;
const float kompensacja_bateri = 1.0f;
const float kompensacja_panelu = 1.14f;

const int baudrate = 9600;
const float ramie = 0.3f; // 30 cm

volatile long Hall_count = 0;

DaneESP dane;

// ===================== MeasureAgg (MUSI BYĆ WCZEŚNIEJ!) =====================
struct MeasureAgg {
  float  t_sum=0, h_sum=0, p_sum=0; uint16_t thp_n=0;
  float  bat_sum=0, sun_sum=0;      uint16_t bs_n=0;
  float  rain_sum=0;                uint16_t rain_n=0;
  uint32_t sraw_sum=0;              uint16_t sraw_n=0;
  uint32_t voc_sum=0;               uint16_t voc_n=0;
  bool   raining_any=false;
  float  wind_speed=0;
};

// prototyp (ważne dla Arduino)
MeasureAgg measureWindow(uint16_t seconds);

// ===================== ISR =====================
void IRAM_ATTR HallISR() { Hall_count++; }

// ===================== INIT czujników =====================
bool BME_init() {
  if (!bme.begin(0x76)) {
    Serial.println("Nie znaleziono BME280");
    return false;
  }
  Serial.println("BME280 OK");
  return true;
}

bool SGP_init() {
  if (!sgp.begin(&Wire)) {
    Serial.println("Nie znaleziono SGP40");
    return false;
  }
  Serial.println("SGP40 OK");
  return true;
}

// ===================== RTC helpers =====================
RTC_Data RTC_dane() {
  RTC_Data t;
  t.godzina = rtc.getHour();
  t.minuta  = rtc.getMinute();
  t.sekunda = rtc.getSecond();
  t.dzien   = rtc.getDay();
  t.miesiac = rtc.getMonth();
  t.rok     = rtc.getYear();
  return t;
}

void RTC_timePatch() {
#if RTC_FORCE_SET_ON_BOOT
  rtc.setDate(SET_DAY, SET_WDAY, SET_MONTH, SET_CENT, SET_YEAR);
  rtc.setTime(SET_HOUR, SET_MIN, SET_SEC);
  Serial.println("RTC: wymuszone ustawienie daty/czasu. Po wgraniu ustaw RTC_FORCE_SET_ON_BOOT=0!");
#endif

  if (RTC_SEC_OFFSET != 0) {
    int h = rtc.getHour(), m = rtc.getMinute(), s = rtc.getSecond();
    int total = h*3600 + m*60 + s + (int)RTC_SEC_OFFSET;
    while (total < 0) total += 86400;
    total %= 86400;
    rtc.setTime((uint8_t)(total/3600), (uint8_t)((total%3600)/60), (uint8_t)(total%60));
    Serial.printf("RTC: zastosowano korektę sekund: %+d\n", RTC_SEC_OFFSET);
  }
}

// alarm: “za deltaMin minut”
void RTC_setAlarmAfterMinutes(uint16_t deltaMin) {
  if (deltaMin < 1) deltaMin = 1;
  if (deltaMin > 12*60) deltaMin = 12*60;

  uint16_t now = (uint16_t)rtc.getHour()*60 + rtc.getMinute();
  uint16_t next = (now + deltaMin) % (24*60);

  uint8_t nextH = next / 60;
  uint8_t nextM = next % 60;

  rtc.clearAlarm();
  rtc.setAlarm(nextM, nextH, 99, 99);
  rtc.enableAlarm();
}

// ===================== ADC helpers =====================
float Rain_level() {
  float value=0.0f;
  for(int i=0;i<10;i++) value += analogRead(WaterSensorAnalog) * (V_ref / resolution);
  return value/10.0f;
}

float Battery_state() {
  float value=0.0f;
  for(int i=0;i<10;i++) value += analogRead(Battery_level) * (V_ref / resolution) / Battery_divider;
  return value/10.0f;
}

float Sun_state() {
  float value=0.0f;
  for(int i=0;i<10;i++) value += analogRead(Fotopanel_level) * (V_ref / resolution) / Fotopanel_divider;
  return value/10.0f;
}

// ===================== OKNO POMIAROWE (2 min) =====================
MeasureAgg measureWindow(uint16_t seconds) {
  MeasureAgg a;
  Hall_count = 0;

  float lastT = 25.0f;
  float lastH = 50.0f;

  // rozgrzewka SGP po wybudzeniu
  if (sgp_ok) {
    (void)sgp.measureRaw(lastT, lastH);
    (void)sgp.measureVocIndex(lastT, lastH);
  }

  const uint16_t DISCARD_SEC = 30; // pierwsze 30 s VOC ignorujemy

  for (uint16_t s=0; s<seconds; s++) {

    // BME co 10 s -> uśrednianie
    if (bme_ok && (s % 10 == 0)) {
      float t = bme.readTemperature();
      float p = bme.readPressure()/100.0f;
      float h = bme.readHumidity();
      if (!isnan(t) && !isnan(h) && !isnan(p)) {
        lastT = t; lastH = h;
        a.t_sum += t; a.h_sum += h; a.p_sum += p; a.thp_n++;
      }
    }

    // SGP co 1 s
    if (sgp_ok) {
      uint16_t raw = sgp.measureRaw(lastT, lastH);
      a.sraw_sum += raw; a.sraw_n++;

      int32_t voc = sgp.measureVocIndex(lastT, lastH);
      if (s >= DISCARD_SEC && voc >= 0) {
        if (voc > 500) voc = 500;
        a.voc_sum += (uint32_t)voc; a.voc_n++;
      }
    }

    // bat/panel co 5 s
    if (s % 5 == 0) {
      a.bat_sum += Battery_state();
      a.sun_sum += Sun_state();
      a.bs_n++;
    }

    // deszcz
    if (digitalRead(WaterSensor) == LOW) {
      a.raining_any = true;
      if (s % 5 == 0) {
        a.rain_sum += Rain_level();
        a.rain_n++;
      }
    }

    delay(1000);
  }

  // wiatr ze zliczeń impulsów w całym oknie
  float czas = (float)seconds;
  float omega = (Hall_count * 2.0f * PI) / (czas > 0.1f ? czas : 0.1f);
  a.wind_speed = omega * ramie;

  return a;
}

// ===================== Odbiór configu z odbiornika =====================
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
void OnCmdRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
#else
void OnCmdRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
#endif
  if (len != (int)sizeof(CmdConfig)) return;

  CmdConfig c;
  memcpy(&c, incomingData, sizeof(c));
  if (c.magic != CFG_MAGIC || c.version != CFG_VERSION) return;

  uint16_t sm = c.sleep_minutes;
  uint16_t ms = c.meas_seconds;
  uint16_t lm = c.listen_ms;

  if (sm < 1) sm = 1;
  if (sm > 180) sm = 180;
  if (ms < 30) ms = 30;
  if (ms > 300) ms = 300;
  if (lm < 200) lm = 200;
  if (lm > 5000) lm = 5000;

  cfgSleepMinutes = sm;
  cfgMeasSeconds  = ms;
  cfgListenMs     = lm;

  Serial.printf("CFG odebrane: sleep=%umin meas=%us listen=%ums\n",
                cfgSleepMinutes, cfgMeasSeconds, cfgListenMs);
}

// ===================== SETUP / LOOP =====================
void setup() {
  Serial.begin(baudrate);
  btStop();
  Wire.begin();

  pinMode(RTC_INT_PIN, INPUT_PULLUP);
  rtc.clearAlarm();
  RTC_timePatch();

  bme_ok = BME_init();
  sgp_ok = SGP_init();

  pinMode(Hall_pin, INPUT);
  attachInterrupt(digitalPinToInterrupt(Hall_pin), HallISR, RISING);

  pinMode(WaterSensor, INPUT);
  pinMode(WaterSensorAnalog, INPUT);

  analogReadResolution(12);

  // Attenuation per pin (jeśli Twoja wersja core tego nie ma, skasuj te 3 linie)
  analogSetPinAttenuation(Battery_level, ADC_11db);
  analogSetPinAttenuation(Fotopanel_level, ADC_11db);
  analogSetPinAttenuation(WaterSensorAnalog, ADC_11db);

  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init FAILED");
    delay(500);
    esp_deep_sleep_start();
  }

  esp_now_register_recv_cb(OnCmdRecv);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, peerAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("ESP-NOW add_peer FAILED");
  }

  esp_sleep_enable_ext0_wakeup((gpio_num_t)RTC_INT_PIN, 0); // INT aktywne LOW
  esp_sleep_enable_timer_wakeup((uint64_t)BACKUP_TIMER_MINUTES * 60ULL * uS_TO_S_FACTOR);

  Serial.printf("Start: sleep=%umin meas=%us listen=%ums\n",
                cfgSleepMinutes, cfgMeasSeconds, cfgListenMs);
}

void loop() {
  // ===== OKNO POMIAROWE =====
  MeasureAgg agg = measureWindow(cfgMeasSeconds);
  RTC_Data rtcData = RTC_dane();

  float t_avg = (agg.thp_n ? agg.t_sum/agg.thp_n : NAN);
  float h_avg = (agg.thp_n ? agg.h_sum/agg.thp_n : NAN);
  float p_avg = (agg.thp_n ? agg.p_sum/agg.thp_n : NAN);

  float bat_avg  = (agg.bs_n ? agg.bat_sum/agg.bs_n : 0.0f);
  float sun_avg  = (agg.bs_n ? agg.sun_sum/agg.bs_n : 0.0f);
  float rain_avg = (agg.rain_n ? agg.rain_sum/agg.rain_n : 0.0f);

  uint16_t sraw_avg = (agg.sraw_n ? (uint16_t)(agg.sraw_sum/agg.sraw_n) : 0);
  uint16_t voc_avg  = (agg.voc_n  ? (uint16_t)(agg.voc_sum/agg.voc_n)   : 0);

  Serial.printf("AVG: T=%.2f H=%.1f P=%.0f | wind=%.2f | bat=%.2fV sun=%.2fV | sraw=%u voc=%u\n",
                t_avg, h_avg, p_avg, agg.wind_speed, bat_avg, sun_avg, sraw_avg, voc_avg);

  // ===== PAKOWANIE =====
  dane.temperatura = (int16_t)(t_avg * 10.0f);
  dane.cisnienie   = (uint16_t)(p_avg);
  dane.wilgotnosc  = (uint8_t)(h_avg);

  dane.opady    = (uint16_t)((agg.raining_any ? rain_avg : 0.0f) * 10.0f);
  dane.predkosc = (uint16_t)(agg.wind_speed * 10.0f);

  dane.poziom_slonca  = (uint16_t)(sun_avg * 1000.0f * kompensacja_panelu);
  dane.poziom_baterii = (uint16_t)(bat_avg * 1000.0f * kompensacja_bateri);

  dane.voc_raw   = sraw_avg;
  dane.voc_index = voc_avg;

  dane.godzina = rtcData.godzina;
  dane.minuta  = rtcData.minuta;
  dane.sekunda = rtcData.sekunda;
  dane.dzien   = rtcData.dzien;
  dane.miesiac = rtcData.miesiac;
  dane.rok     = rtcData.rok;

  dane.czy_pada = agg.raining_any ? 1 : 0;

  // ===== WYSYŁKA =====
  esp_now_send(peerAddress, (uint8_t*)&dane, sizeof(dane));

  // ===== OKNO NA ODEBRANIE NOWEJ KONFIGURACJI =====
  uint32_t tListen = millis();
  while (millis() - tListen < cfgListenMs) {
    delay(10); // callback OnCmdRecv może tu wpaść
  }

  // ===== ALARM + SLEEP =====
  RTC_setAlarmAfterMinutes(cfgSleepMinutes);

  if (sgp_ok) sgp.heaterOff();

  Serial.printf("Idę spać na %u min\n", cfgSleepMinutes);
  Serial.flush();
  esp_deep_sleep_start();
}