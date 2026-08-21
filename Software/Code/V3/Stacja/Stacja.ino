#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_SGP40.h>
#include <Rtc_Pcf8563.h>
#include <WiFi.h>
#include <esp_now.h>
#include "driver/rtc_io.h"
#include "PogodynkaTypes.h"
//#define RTC_FORCE_SET_ON_BOOT 0
Adafruit_BME280 bme;
Adafruit_SGP40  sgp;
Rtc_Pcf8563     rtc;
const int RTC_INT_PIN = 27;      
uint8_t peerAddress[] = {0x28, 0x56, 0x2F, 0x71, 0x92, 0x34};
esp_now_peer_info_t peerInfo;
const uint8_t SET_DAY   = 17;
const uint8_t SET_WDAY  = 4;
const uint8_t SET_MONTH = 2;
const uint8_t SET_CENT  = 0;
const uint8_t SET_YEAR  = 26;
const uint8_t SET_HOUR  = 1;
const uint8_t SET_MIN   = 15;
const uint8_t SET_SEC   = 45;
bool bme_ok = false;
const int Hall_pin          = 17;
const int WaterSensor       = 19;
const int WaterSensorAnalog = 34;
const int Battery_level     = 32;
const int Fotopanel_level   = 35;
const float Resistor_division =2.0f;
const int baudrate = 9600;
const float ramie = 0.096f; // ~10 cm
volatile uint32_t  Hall_count = 0;
const uint32_t pomiar_predkosci_czas =10; //sec
uint16_t Minuty_sleep=30;
uint8_t Measurments_mean = 5;
const int Power_peryf   = 25;
Dane_ESP Dane;


bool BME_init() {
  if (!bme.begin(0x76)) {
    return false;
  }
  return true;
}
double Temperature(){
  double value=0.0f;
  for(int i=0;i<Measurments_mean;i++){ 
    value += bme.readTemperature() ;}
    return value/Measurments_mean;
}
double Humility(){
  double value=0.0f;
  for(int i=0;i<Measurments_mean;i++){ 
    value += bme.readHumidity() ;}
    return value/Measurments_mean;
}
double Pressure(){
  double value=0.0f;
  for(int i=0;i<Measurments_mean;i++){ 
    value += bme.readPressure() ;}
    return value/Measurments_mean;
}
float Rain_level() {
  if(digitalRead(WaterSensor)==false){
    float value=0.0f;
    for(int i=0;i<Measurments_mean;i++){ 
      value += analogReadMilliVolts(WaterSensorAnalog);}
      return value/Measurments_mean;}
  return 0.0f;
}
float Battery_state() {
  float value=0.0f;
  for(int i=0;i<Measurments_mean;i++) {value += analogReadMilliVolts(Battery_level)*Resistor_division;}
  return value/Measurments_mean;
}
float Sun_state() {
  float value=0.0f;
  for(int i=0;i<Measurments_mean;i++) {value += analogReadMilliVolts(Fotopanel_level)*Resistor_division;}
  return value/Measurments_mean;
}
float Wind_speed(){
  unsigned long time_start, time_end;
  float omega, speed;
  uint32_t start_hall = Hall_count;
  time_start = millis();
  while(true){
    time_end = millis() - time_start;
    if (time_end>=pomiar_predkosci_czas*1000.0f){
        uint32_t last_hall = Hall_count;
        omega = ((last_hall-start_hall) * 2.0f * PI) / (time_end/1000.0f);
        speed = omega * ramie;
        return speed;
    }
    delay(50);
  }
}


void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("\r\nLast Packet Send Status:\t");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
}
        
void IRAM_ATTR HallISR() {Hall_count++;}

void setup() {
  Serial.begin(baudrate);
  Wire.begin();
  pinMode(RTC_INT_PIN, INPUT_PULLUP);
  rtc.clearAlarm();
  bme_ok = BME_init();
  pinMode(Hall_pin, INPUT);
  attachInterrupt(digitalPinToInterrupt(Hall_pin), HallISR, RISING);
  pinMode(WaterSensor, INPUT);
  pinMode(Power_peryf, OUTPUT);
  pinMode(WaterSensorAnalog, INPUT);
  pinMode(Fotopanel_level, INPUT);
  pinMode(Battery_level, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(WaterSensorAnalog, ADC_11db);
  analogSetPinAttenuation(Battery_level, ADC_11db);
  analogSetPinAttenuation(Fotopanel_level, ADC_11db);
  digitalWrite(Power_peryf, LOW);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)RTC_INT_PIN, 0);
}


void loop() {
digitalWrite(Power_peryf, HIGH);
delay(10);
Pomiar();
Send_dane();
RTC_set_alarm();
digitalWrite(Power_peryf, LOW);
esp_deep_sleep_start();
}
//~10mA sleep
//~90mA pomiary
//~200mA sending


void Pomiar(){
  Dane.Day=rtc.getDay();
  Dane.Month=rtc.getMonth();
  Dane.Year=rtc.getYear();
  Dane.Hour=rtc.getHour();
  Dane.Minute=rtc.getMinute();
    Dane.Rain=Rain_level();
    Dane.Battery_level=Battery_state();
    Dane.Sun_level=Sun_state();
  if(bme_ok){
    Dane.Temperature=Temperature();
    Dane.Pressure=Pressure();
    Dane.Humility=Humility();
  }
  Dane.Wind_speed=Wind_speed();
}
void RTC_set_alarm() {
  uint16_t Aktualna_godzina = rtc.getHour();
  uint16_t Aktualna_minuta  = rtc.getMinute();

  uint32_t total = Aktualna_godzina * 60u + Aktualna_minuta;

  total = ((total / 30u) + 1u) * 30u;
  total %= 24u * 60u;

  uint16_t Alarm_hour = total / 60u;
  uint16_t alarm_min  = total % 60u;

  rtc.clearAlarm();
  rtc.setAlarm(alarm_min, Alarm_hour, 99, 99);
  rtc.enableAlarm();
}
void Send_dane(){
  WiFi.mode(WIFI_STA);
  delay(200); //niech sie ustali
  esp_err_t err = esp_now_init();
  if(err != ESP_OK){delay(5000);esp_now_init();}
  esp_now_register_send_cb(OnDataSent);
  memcpy(peerInfo.peer_addr, peerAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK){
    Serial.println("Failed to add peer");
    WiFi.mode(WIFI_OFF);
    return;}

  esp_err_t result = esp_now_send(peerAddress, (uint8_t *) &Dane, sizeof(Dane));
  /*
    if (result == ESP_OK) {
      Serial.println("Sent with success");
    }
    else {
      Serial.println("Error sending the data");
    }
  */
  delay(500);
  WiFi.mode(WIFI_OFF);
}
