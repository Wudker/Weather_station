#pragma once
#include <Arduino.h>

// --- BME280 ---
struct BME_Data {
  float temperatura;
  float cisnienie;
  float wilgotnosc;
};

// --- RTC ---
struct RTC_Data {
  uint8_t godzina, minuta, sekunda;
  uint8_t dzien, miesiac, rok;
};

// --- Wiatr + SGP ---
struct SpeedAir {
  float predkosc;
  uint16_t voc_raw_avg;
  uint16_t voc_index_last;
};

// --- Paczka do ESP-NOW ---
// Uwaga: używamy uint8_t zamiast bool, żeby uniknąć różnic rozmiaru/ABI między kompilatorami.
struct __attribute__((packed)) DaneESP {
  int16_t  temperatura;     // *10
  uint16_t cisnienie;       // hPa
  uint8_t  wilgotnosc;      // %

  uint16_t opady;           // *10
  uint16_t predkosc;        // *10

  uint16_t poziom_slonca;   // mV
  uint16_t poziom_baterii;  // mV

  uint16_t voc_raw;         // SRAW (średnia z próbek)
  uint16_t voc_index;       // 0..500

  uint8_t godzina, minuta, sekunda;
  uint8_t dzien, miesiac, rok;

  uint8_t czy_pada;         // 0/1
};