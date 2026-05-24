#pragma once
#include <Arduino.h>

// ===================== PAKIET Z ESP-NOW (DANE) =====================
struct __attribute__((packed)) DaneESP_V2 {
  int16_t  temperatura;     // *10
  uint16_t cisnienie;       // hPa
  uint8_t  wilgotnosc;      // %

  uint16_t opady;           // *10
  uint16_t predkosc;        // *10

  uint16_t poziom_slonca;   // mV
  uint16_t poziom_baterii;  // mV

  uint16_t voc_raw;         // SRAW avg
  uint16_t voc_index;       // 0..500

  uint8_t  godzina, minuta, sekunda;
  uint8_t  dzien, miesiac, rok;

  uint8_t  czy_pada;        // 0/1
};

// ===================== HISTORIA (punkt) =====================
struct __attribute__((packed)) HistPoint {
  int16_t  temp10;
  uint16_t pres;
  uint8_t  hum;

  uint16_t wind10;
  uint16_t rain10;

  uint16_t sunmV;
  uint16_t battmV;

  uint16_t voc;      // VOC index

  uint8_t  h, m;
  uint8_t  day, mon, yr;

  uint8_t  rain;     // 0/1
};

// ===================== CONFIG (do stacji) =====================
static const uint8_t CFG_MAGIC   = 0xC3;
static const uint8_t CFG_VERSION = 1;

struct __attribute__((packed)) CmdConfig {
  uint8_t  magic;
  uint8_t  version;
  uint16_t sleep_minutes; // 1..180
  uint16_t meas_seconds;  // 30..300
  uint16_t listen_ms;     // 200..5000
};