#include <WiFi.h>
#include <esp_now.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <SPI.h>
#include <SD.h>
#include <math.h>
#include "PogodynkaRxTypes.h"

const char* ssid     = "UPC0772731_24Ghz";
const char* password = "sumyhfguw3zf5Hte";

WebServer server(80);

static const int History_size = 32;
static const int Forecast_size = 4;

// Poprawka czasu: +1 h po zmianie czasu zimowego na letni.
// Korekta jest robiona tylko po stronie odbiornika, bez zmiany danych wysyłanych przez stację.
static const int TIME_OFFSET_HOURS = 1;

// Proponowane piny karty SD dla klasycznego ESP32 / ESP32 DevKit:
// CS  -> GPIO5
// SCK -> GPIO18
// MISO-> GPIO19
// MOSI-> GPIO23
// Zasilanie modułu SD: 3.3 V, wspólna masa GND.
static const int SD_CS_PIN   = 5;
static const int SD_SCK_PIN  = 18;
static const int SD_MISO_PIN = 19;
static const int SD_MOSI_PIN = 23;
// Pliki logow sa tworzone osobno dla kazdego miesiaca, np. /pogodynka_2026_04.csv.
// Jeden plik obejmuje dni od 1 do konca danego miesiaca.

Dane_ESP Dane;

static float Historia_Temperatury[History_size];
static float Prognoza_Temperatury[Forecast_size];
static float Historia_Bateri[History_size];
static float Historia_Opady[History_size];

static uint32_t lastRxMs = 0;
static uint16_t historia_ilosc = 0;

static bool sd_ok = false;
static volatile bool sd_log_pending = false;
static Dane_ESP Dane_do_zapisu;

static const uint32_t WIFI_CHECK_PERIOD_MS = 14UL * 60UL * 1000UL;
static uint32_t next_wifi_check_ms = 0;

static bool wifi_laczenie = false;
static uint32_t wifi_start_ms = 0;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 5000;

static bool mdns_ok = false;

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
  <!DOCTYPE html>
  <html lang="pl">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>Pogodynka</title>
    <style>
      :root{ --maxw: 1100px; --gap: 16px; --yellow: #EFBF04; }
      *{ box-sizing: border-box; }
      body{ margin:0; padding:12px; font-family:"Courier New", monospace; text-align:center; background:#000; color:#fff; }
      .page{ width:min(100%, var(--maxw)); margin:0 auto; padding:16px; }
      .top{ display:flex; align-items:flex-start; justify-content:space-between; gap:var(--gap); flex-wrap:nowrap; }
      .logo{ flex:0 0 260px; text-align:left; }
      .logo h2{ margin:0 0 6px 0; font-size:clamp(20px, 3vw, 32px); line-height:1.1; }
      .logo h5{ margin:0; font-size:clamp(12px, 1.6vw, 16px); line-height:1.25; font-weight:600; }
      .summary{ flex:1 1 auto; min-width:0; text-align:right; }
      .badges{ display:flex; gap:8px; justify-content:flex-end; flex-wrap:wrap; overflow-x:auto; -webkit-overflow-scrolling:touch; padding-bottom:4px; }
      .badge{ display:inline-block; padding:6px 10px; border-radius:999px; font-size:clamp(11px, 1.2vw, 13px); line-height:1; white-space:nowrap; flex:0 0 auto; color:#000; background:#ddd; }
      .b-yellow{ background:#EFBF04; }
      .b-red{ background:#ff3b30; color:#fff; }
      .b-blue{ background:#1e5eff; color:#fff; }
      .b-green{ background:#18c964; }
      .b-gray{ background:#9aa0a6; }
      .b-orange{ background:#ff9f0a; }
      .b-lightblue{ background:#7dd3fc; }
      .hidden{ display:none; }
      .battery{ margin-top:10px; font-size:clamp(12px, 1.4vw, 15px); text-align:right; }
      table{ width:100%; border-collapse:collapse; margin:16px auto; }
      th{ padding:10px; background:var(--yellow); color:#000; font-weight:700; }
      td{ padding:9px; border:1px solid #333; font-size:15px; }
      .chart{ width:100%; margin:10px auto 18px auto; padding:10px 0; }
      .chart h1{ margin:0 0 10px 0; font-size:clamp(18px, 2.4vw, 26px); font-weight:700; }
      canvas{ width:100%; height:200px; border:1px solid #333; border-radius:8px; display:block; }
      @media (min-width:900px){ .badges{ flex-wrap:wrap; overflow-x:visible; } }
      @media (max-width:520px){
        body{ padding:10px; }
        .page{ padding:12px; }
        .top{ flex-direction:column; align-items:stretch; }
        .logo{ flex:1 1 auto; }
        .summary{ text-align:left; }
        .badges{ justify-content:flex-start; }
        .battery{ text-align:left; }
        td{ font-size:14px; }
      }
    </style>
  </head>
  <body>
    <div class="page">
      <div class="top">
        <div class="logo">
          <h2>POGODYNKA</h2>
          <h5>Ostatni pomiar:<br><span id="last">--:-- | --.--.----</span></h5>
        </div>
        <div class="summary">
          <div class="badges">
            <span class="badge b-yellow hidden" id="bd_sun">Słonecznie</span>
            <span class="badge b-blue hidden" id="bd_rain">Deszcz</span>
            <span class="badge b-gray hidden" id="bd_cloud">Pochmurnie</span>
            <span class="badge b-orange hidden" id="bd_wind">Wietrznie</span>
            <span class="badge b-lightblue hidden" id="bd_cold">Zimno</span>

            <span class="badge b-blue hidden" id="bd_humid">Wilgotno</span>
            <span class="badge b-gray hidden" id="bd_dry">Sucho</span>

            <span class="badge b-orange hidden" id="bd_batt_low">Niski stan baterii</span>
            <span class="badge b-red hidden" id="bd_batt_crit">Krytyczna bateria</span>

            <span class="badge b-gray hidden" id="bd_pres_low">Niskie ciśnienie</span>
            <span class="badge b-yellow hidden" id="bd_pres_high">Wysokie ciśnienie</span>
          </div>
          <div class="battery">Naładowanie baterii: <span id="Batt_procent">--</span>%</div>
        </div>
      </div>

      <table>
        <tr><th>Pomiar</th><th>Wartość</th></tr>
        <tr><td>Temperatura</td><td><span id="temp">--</span> °C</td></tr>
        <tr><td>Ciśnienie</td><td><span id="pres">--</span> hPa</td></tr>
        <tr><td>Wilgotność</td><td><span id="hum">--</span> %</td></tr>
        <tr><td>Stan opadów</td><td><span id="rainState">--</span></td></tr>
        <tr><td>Prędkość wiatru</td><td><span id="wind">--</span> m/s</td></tr>
        <tr><td>Temp odczuwalna</td><td><span id="feels">--</span> °C</td></tr>
      </table>

      <div class="chart">
        <h1>Wykres temperatury</h1>
        <canvas id="chartTemp"></canvas>
      </div>

      <div class="chart">
        <h1>Wykres baterii</h1>
        <canvas id="chartBatt"></canvas>
      </div>
    </div>

  <script>
    const DT_MIN = 30;

    let rainDryMV = 200;
    let rainWetMV = 1200;

    function $(id){ return document.getElementById(id); }
    function pad2(x){ x = Number(x)||0; return (x<10?'0':'') + x; }
    function showBadge(id, show){
      const el = $(id);
      if(!el) return;
      el.classList.toggle('hidden', !show);
    }

    function resizeCanvas(c, h){
      const w = c.clientWidth || 300;
      const dpr = window.devicePixelRatio || 1;
      c.width = Math.floor(w * dpr);
      c.height = Math.floor(h * dpr);
      c.style.height = h + 'px';
      const ctx = c.getContext('2d');
      ctx.setTransform(dpr,0,0,dpr,0,0);
      return {ctx, w, h};
    }

    function fmtTime(mins){
      if(mins === 0) return "0";
      const sign = mins > 0 ? "+" : "-";
      const a = Math.abs(mins);
      if(a >= 120){
        const h = Math.round(a/60);
        return sign + h + "h";
      }
      return sign + a + "m";
    }

    function clamp(x, a, b){ return Math.max(a, Math.min(b, x)); }

    function calcApparentTempC(Tc, RH, windMs){
      const e = (RH/100.0) * 6.105 * Math.exp((17.27*Tc)/(237.7+Tc));
      return Tc + 0.33*e - 0.70*windMs - 4.00;
    }

    function calcWindChillC(Tc, windMs){
      const v = windMs * 3.6;
      const p = Math.pow(v, 0.16);
      return 13.12 + 0.6215*Tc - 11.37*p + 0.3965*Tc*p;
    }

    function calcHeatIndexC(Tc, RH){
      const Tf = Tc * 9/5 + 32;
      const HI =
        -42.379 +
        2.04901523*Tf +
        10.14333127*RH +
        -0.22475541*Tf*RH +
        -0.00683783*Tf*Tf +
        -0.05481717*RH*RH +
        0.00122874*Tf*Tf*RH +
        0.00085282*Tf*RH*RH +
        -0.00000199*Tf*Tf*RH*RH;
      return (HI - 32) * 5/9;
    }

    function calcFeelsLikeC(Tc, RH, windMs){
      if(Tc <= 10 && windMs >= 1.34) return calcWindChillC(Tc, windMs);
      if(Tc >= 27 && RH >= 40) return calcHeatIndexC(Tc, RH);
      return calcApparentTempC(Tc, RH, windMs);
    }

    function movingAverage(data, windowSize){
      if(!Array.isArray(data)) return [];
      const n = Math.max(1, Number(windowSize) || 1);
      const out = [];

      for(let i=0; i<data.length; i++){
        let sum = 0;
        let count = 0;

        for(let j=Math.max(0, i-n+1); j<=i; j++){
          const v = Number(data[j]);
          if(isFinite(v)){
            sum += v;
            count++;
          }
        }

        out.push(count ? (sum / count) : null);
      }

      return out;
    }

    function drawLineChart(canvasId, data, yUnit, dtMin){
      const c = $(canvasId);
      if(!c || !data || data.length < 2) return;

      const H = 200;
      const {ctx, w, h} = resizeCanvas(c, H);

      ctx.clearRect(0,0,w,h);

      const arr = data.map(v => (v===null || v===undefined) ? NaN : Number(v));
      const valid = arr.filter(v => isFinite(v));
      if(valid.length < 2) return;

      let min = Math.min(...valid);
      let max = Math.max(...valid);
      let pad = (max - min) * 0.12;
      if(pad === 0) pad = 1;
      min -= pad; max += pad;

      ctx.fillStyle = "#fff";
      ctx.font = "12px Courier New";

      const yLabels = [];
      for(let i=0;i<=4;i++){
        const val = (max - (max-min) * (i/4));
        const txt = (Math.abs(val) >= 100 ? val.toFixed(0) : val.toFixed(1)) + " " + yUnit;
        yLabels.push(txt);
      }
      const maxLabelW = Math.max(...yLabels.map(t => ctx.measureText(t).width));

      const mL = Math.max(48, Math.ceil(maxLabelW) + 12);
      const mR = 12, mT = 10, mB = 30;

      const plotW = Math.max(10, w - mL - mR);
      const plotH = Math.max(10, h - mT - mB);

      const n = arr.length;

      function mapX(i){ return mL + plotW * (i/(n-1)); }
      function mapY(v){ return mT + plotH - plotH * ((v - min) / (max - min)); }

      ctx.strokeStyle = "#333";
      ctx.lineWidth = 1;
      for(let i=0;i<=4;i++){
        const y = mT + (plotH * i / 4);
        ctx.beginPath(); ctx.moveTo(mL,y); ctx.lineTo(mL+plotW,y); ctx.stroke();
      }
      for(let i=0;i<=4;i++){
        const x = mL + (plotW * i / 4);
        ctx.beginPath(); ctx.moveTo(x,mT); ctx.lineTo(x,mT+plotH); ctx.stroke();
      }

      ctx.fillStyle = "#fff";
      ctx.font = "12px Courier New";

      ctx.textAlign = "right";
      ctx.textBaseline = "middle";
      for(let i=0;i<=4;i++){
        const y = mT + (plotH * i / 4);
        ctx.fillText(yLabels[i], mL - 6, y);
      }

      ctx.textAlign = "center";
      ctx.textBaseline = "top";
      for(let i=0;i<=4;i++){
        const idx = Math.round((n-1) * (i/4));
        const x = mapX(idx);
        const mins = (idx - (n-1)) * (dtMin || DT_MIN);
        ctx.fillText(fmtTime(mins), x, mT + plotH + 6);
      }

      ctx.textAlign = "center";
      ctx.textBaseline = "bottom";
      ctx.fillText("czas", mL + plotW/2, h - 2);

      ctx.strokeStyle = "#fff";
      ctx.lineWidth = 2;
      let started = false;
      ctx.beginPath();
      for(let i=0;i<n;i++){
        const v = arr[i];
        if(!isFinite(v)) continue;
        const x = mapX(i);
        const y = mapY(v);
        if(!started){ ctx.moveTo(x,y); started = true; }
        else ctx.lineTo(x,y);
      }
      ctx.stroke();

      ctx.fillStyle = "#fff";
      for(let i=0;i<n;i++){
        const v = arr[i];
        if(!isFinite(v)) continue;
        const x = mapX(i);
        const y = mapY(v);
        ctx.beginPath(); ctx.arc(x,y,2.2,0,Math.PI*2); ctx.fill();
      }
    }

    function drawLineChartForecast(canvasId, history, forecast, yUnit, dtMin){
      const c = $(canvasId);
      if(!c) return;

      const hist = Array.isArray(history) ? history : [];
      const fore = Array.isArray(forecast) ? forecast : [];
      const combined = hist.concat(fore);
      if(combined.length < 2) return;

      const H = 200;
      const {ctx, w, h} = resizeCanvas(c, H);
      ctx.clearRect(0,0,w,h);

      const arr = combined.map(v => (v===null || v===undefined) ? NaN : Number(v));
      const valid = arr.filter(v => isFinite(v));
      if(valid.length < 2) return;

      let min = Math.min(...valid);
      let max = Math.max(...valid);
      let pad = (max - min) * 0.12;
      if(pad === 0) pad = 1;
      min -= pad; max += pad;

      ctx.fillStyle = "#fff";
      ctx.font = "12px Courier New";

      const yLabels = [];
      for(let i=0;i<=4;i++){
        const val = (max - (max-min) * (i/4));
        const txt = (Math.abs(val) >= 100 ? val.toFixed(0) : val.toFixed(1)) + " " + yUnit;
        yLabels.push(txt);
      }
      const maxLabelW = Math.max(...yLabels.map(t => ctx.measureText(t).width));

      const mL = Math.max(48, Math.ceil(maxLabelW) + 12);
      const mR = 12, mT = 10, mB = 30;

      const plotW = Math.max(10, w - mL - mR);
      const plotH = Math.max(10, h - mT - mB);

      const n = arr.length;
      const zeroIndex = Math.max(0, hist.length - 1);

      function mapX(i){ return mL + plotW * (i/(n-1)); }
      function mapY(v){ return mT + plotH - plotH * ((v - min) / (max - min)); }

      const ticks = [];
      ticks.push(0);
      ticks.push(Math.round(zeroIndex/2));
      ticks.push(zeroIndex);
      ticks.push(Math.round((zeroIndex + (n-1))/2));
      ticks.push(n-1);
      const uniq = Array.from(new Set(ticks)).filter(i => i>=0 && i<=n-1).sort((a,b)=>a-b);

      ctx.strokeStyle = "#333";
      ctx.lineWidth = 1;
      for(let i=0;i<=4;i++){
        const y = mT + (plotH * i / 4);
        ctx.beginPath(); ctx.moveTo(mL,y); ctx.lineTo(mL+plotW,y); ctx.stroke();
      }
      for(const idx of uniq){
        const x = mapX(idx);
        ctx.beginPath(); ctx.moveTo(x,mT); ctx.lineTo(x,mT+plotH); ctx.stroke();
      }

      ctx.fillStyle = "#fff";
      ctx.font = "12px Courier New";

      ctx.textAlign = "right";
      ctx.textBaseline = "middle";
      for(let i=0;i<=4;i++){
        const y = mT + (plotH * i / 4);
        ctx.fillText(yLabels[i], mL - 6, y);
      }

      ctx.textAlign = "center";
      ctx.textBaseline = "top";
      for(const idx of uniq){
        const x = mapX(idx);
        const mins = (idx - zeroIndex) * (dtMin || DT_MIN);
        ctx.fillText(fmtTime(mins), x, mT + plotH + 6);
      }

      ctx.textAlign = "center";
      ctx.textBaseline = "bottom";
      ctx.fillText("czas", mL + plotW/2, h - 2);

      const x0 = mapX(zeroIndex);
      ctx.strokeStyle = "#666";
      ctx.lineWidth = 1;
      ctx.setLineDash([4,4]);
      ctx.beginPath(); ctx.moveTo(x0, mT); ctx.lineTo(x0, mT + plotH); ctx.stroke();
      ctx.setLineDash([]);

      ctx.strokeStyle = "#EFBF04";
      ctx.lineWidth = 2;
      let started = false;
      ctx.beginPath();
      for(let i=0;i<=zeroIndex;i++){
        const v = arr[i];
        if(!isFinite(v)) continue;
        const x = mapX(i);
        const y = mapY(v);
        if(!started){ ctx.moveTo(x,y); started = true; }
        else ctx.lineTo(x,y);
      }
      ctx.stroke();

      let lastHistIdx = -1;
      for(let i=zeroIndex;i>=0;i--){
        if(isFinite(arr[i])) { lastHistIdx = i; break; }
      }

      if(lastHistIdx >= 0){
        ctx.strokeStyle = "#1e5eff";
        ctx.lineWidth = 2;
        ctx.setLineDash([6,4]);
        ctx.beginPath();
        ctx.moveTo(mapX(lastHistIdx), mapY(arr[lastHistIdx]));
        for(let i=lastHistIdx+1;i<n;i++){
          const v = arr[i];
          if(!isFinite(v)) continue;
          ctx.lineTo(mapX(i), mapY(v));
        }
        ctx.stroke();
        ctx.setLineDash([]);
      }

      for(let i=0;i<n;i++){
        const v = arr[i];
        if(!isFinite(v)) continue;
        const x = mapX(i);
        const y = mapY(v);
        ctx.fillStyle = (i <= zeroIndex) ? "#EFBF04" : "#1e5eff";
        ctx.beginPath(); ctx.arc(x,y,2.2,0,Math.PI*2); ctx.fill();
      }
    }

    async function refreshLatest(){
      try{
        const r = await fetch('/api/latest', {cache:'no-store'});
        if(!r.ok) return;
        const d = await r.json();
        if(!d.ok){
          [
            'bd_sun','bd_cloud','bd_cold','bd_wind','bd_rain',
            'bd_humid','bd_dry',
            'bd_batt_low','bd_batt_crit',
            'bd_pres_low','bd_pres_high'
          ].forEach(id => showBadge(id,false));
          return;
        }

        $('last').textContent = `${pad2(d.h)}:${pad2(d.m)} | ${pad2(d.day)}.${pad2(d.mon)}.20${pad2(d.yr)}`;

        const temp = Number(d.temp) || 0;
        const presPa = Number(d.presPa) || 0;
        const hum = Number(d.hum) || 0;
        const wind = Number(d.wind) || 0;
        const rainmV = Number(d.rainmV) || 0;
        const Batt_procent = Number(d.Batt_procent) || 0;
        const sunV = Number(d.sunV) || 0;

        $('temp').textContent = temp.toFixed(1);
        $('pres').textContent = (presPa/100.0).toFixed(0);
        $('hum').textContent  = hum.toFixed(0);

        const feels = calcFeelsLikeC(temp, hum, wind);
        $('feels').textContent = (isFinite(feels) ? feels : temp).toFixed(1);

        $('wind').textContent = wind.toFixed(1);
        $('Batt_procent').textContent = String(Math.round(Batt_procent));

        const TH_SUN_V = 4.0;
        const TH_CLOUD_V = 2.0;
        const TH_COLD_C = 5.0;
        const TH_WIND_MS = 6.0;

        const TH_RAIN_START = 100.0;
        const TH_RAIN_LIGHT = 300.0;
        const TH_RAIN_MED = 500.0;
        const TH_RAIN_HEAVY = 1200.0;

        const TH_HUM_HI = 70.0;
        const TH_HUM_LO = 30.0;

        const TH_BATT_LOW = 35.0;
        const TH_BATT_CRIT = 20.0;

        const TH_PRES_LOW = 1000.0;
        const TH_PRES_HIGH = 1020.0;

        const isSunny = sunV > TH_SUN_V;
        const isCloud = (!isSunny) && (sunV > TH_CLOUD_V);
        const isCold = temp < TH_COLD_C;
        const isWind = wind > TH_WIND_MS;
        const isRain = rainmV > TH_RAIN_START;

        const isHumid = hum >= TH_HUM_HI;
        const isDry = hum <= TH_HUM_LO;

        const isBattCrit = Batt_procent <= TH_BATT_CRIT;
        const isBattLow = (Batt_procent <= TH_BATT_LOW) && !isBattCrit;

        const presHpa = presPa / 100.0;
        const isPresLow = presHpa < TH_PRES_LOW;
        const isPresHigh = presHpa > TH_PRES_HIGH;

        let wetPct = 0;
        const denom = (rainWetMV - rainDryMV);
        if(denom > 10){
          wetPct = 100.0 * (rainmV - rainDryMV) / denom;
        }else{
          wetPct = 100.0 * (rainmV - TH_RAIN_START) / (TH_RAIN_HEAVY - TH_RAIN_START);
        }
        wetPct = clamp(wetPct, 0, 100);

        let rainState = "Brak";
        if(rainmV > TH_RAIN_START){
          if(rainmV > TH_RAIN_HEAVY) rainState = "Ulewa";
          else if(rainmV > TH_RAIN_MED) rainState = "Silny deszcz";
          else if(rainmV > TH_RAIN_LIGHT) rainState = "Deszcz";
          else rainState = "Mżawka";
        }
        $('rainState').textContent = `${rainState} (${wetPct.toFixed(0)}%)`;

        showBadge('bd_sun', isSunny);
        showBadge('bd_cloud', isCloud);
        showBadge('bd_cold', isCold);
        showBadge('bd_wind', isWind);
        showBadge('bd_rain', isRain);

        showBadge('bd_humid', isHumid);
        showBadge('bd_dry', isDry);

        showBadge('bd_batt_crit', isBattCrit);
        showBadge('bd_batt_low', isBattLow);

        showBadge('bd_pres_low', isPresLow);
        showBadge('bd_pres_high', isPresHigh);

      }catch(e){
        console.error(e);
      }
    }

    async function refreshHistory(){
      try{
        const r = await fetch('/api/history', {cache:'no-store'});
        if(!r.ok) return;
        const d = await r.json();
        if(!d.ok) return;

        drawLineChartForecast('chartTemp', d.temp || [], d.tempForecast || [], "°C", DT_MIN);

        // Wygładzenie dotyczy wyłącznie danych pokazywanych na wykresie baterii.
        // Wartość w tabeli i progi alarmowe nadal korzystają z aktualnego, nieprzefiltrowanego pomiaru.
        const BATT_MA_WINDOW = 5;
        drawLineChart('chartBatt', movingAverage(d.Batt_procent || [], BATT_MA_WINDOW), "%", DT_MIN);

      }catch(e){
        console.error(e);
      }
    }

    refreshLatest();
    refreshHistory();
    setInterval(refreshLatest, 2000);
    setInterval(refreshHistory, 10000);
  </script>
  </body>
  </html>
  )rawliteral";


struct PoprawionyCzas{
  int Day;
  int Month;
  int Year;
  int Hour;
  int Minute;
};

static bool CzyRokPrzestepny(int year){
  int y = year;
  if(y < 100) y += 2000;
  return ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
}

static int DniWMiesiacu(int month, int year){
  switch(month){
    case 1:  return 31;
    case 2:  return CzyRokPrzestepny(year) ? 29 : 28;
    case 3:  return 31;
    case 4:  return 30;
    case 5:  return 31;
    case 6:  return 30;
    case 7:  return 31;
    case 8:  return 31;
    case 9:  return 30;
    case 10: return 31;
    case 11: return 30;
    case 12: return 31;
    default: return 31;
  }
}

static PoprawionyCzas Czas_po_korekcie(const Dane_ESP& d){
  PoprawionyCzas t;
  t.Day = d.Day;
  t.Month = d.Month;
  t.Year = d.Year;
  t.Hour = d.Hour;
  t.Minute = d.Minute;

  t.Hour += TIME_OFFSET_HOURS;

  while(t.Hour >= 24){
    t.Hour -= 24;
    t.Day++;

    if(t.Month >= 1 && t.Month <= 12){
      int dim = DniWMiesiacu(t.Month, t.Year);
      if(t.Day > dim){
        t.Day = 1;
        t.Month++;
        if(t.Month > 12){
          t.Month = 1;
          t.Year++;
          if(t.Year >= 100) t.Year = 0;
        }
      }
    }
  }

  while(t.Hour < 0){
    t.Hour += 24;
    t.Day--;

    if(t.Day < 1 && t.Month >= 1 && t.Month <= 12){
      t.Month--;
      if(t.Month < 1){
        t.Month = 12;
        t.Year--;
        if(t.Year < 0) t.Year = 99;
      }
      t.Day = DniWMiesiacu(t.Month, t.Year);
    }
  }

  return t;
}

static String DwaZnaki(int v){
  String s;
  if(v < 10) s += "0";
  s += String(v);
  return s;
}

static String ZnacznikCzasuCSV(const PoprawionyCzas& t){
  String ts;
  ts.reserve(20);
  ts += "20";
  ts += DwaZnaki(t.Year);
  ts += "-";
  ts += DwaZnaki(t.Month);
  ts += "-";
  ts += DwaZnaki(t.Day);
  ts += " ";
  ts += DwaZnaki(t.Hour);
  ts += ":";
  ts += DwaZnaki(t.Minute);
  ts += ":00";
  return ts;
}

static void History_update(float array[], float Nowa_wartosc){
  for(int i=0; i<=(History_size-2); i++){
    array[i] = array[i+1];
  }
  array[History_size-1] = Nowa_wartosc;
}

static void Forecast_update_temp(const float hist[], uint16_t ilosc, float outForecast[], uint16_t k){
  for(uint16_t i=0;i<k;i++) outForecast[i] = NAN;
  if(ilosc < 2) return;

  uint16_t empty = History_size - ilosc;
  uint16_t wantN = (ilosc >= 4) ? 4 : ilosc;
  uint16_t start = (History_size > wantN) ? (History_size - wantN) : 0;
  if(start < empty) start = empty;

  uint16_t n = History_size - start;
  if(n < 2) return;

  float sx=0, sy=0, sxx=0, sxy=0;
  for(uint16_t i=0;i<n;i++){
    float x = (float)i;
    float y = hist[start + i];
    sx += x;
    sy += y;
    sxx += x*x;
    sxy += x*y;
  }

  float denom = (float)n * sxx - sx*sx;
  float a = 0.0f;
  if(fabsf(denom) > 1e-6f){
    a = ((float)n * sxy - sx*sy) / denom;
  }
  float b = (sy - a*sx) / (float)n;

  float lastX = (float)(n - 1);
  for(uint16_t step=1; step<=k; step++){
    outForecast[step-1] = a*(lastX + (float)step) + b;
  }
}

static float Battery_state(float mV){
  //ogniwo nomilanie ma 3.7V ale praca ogranicza sie od ~3.2V do 4.2V, stąd offsety
  float dolny_prog = 3.2f, gorny_prog=4.2f;
  float vbatt = (mV / 1000.0f)-dolny_prog;
  float pct = (vbatt / (gorny_prog-dolny_prog)) * 100.0f;
  if(pct < 0) pct = 0;
  if(pct > 100) pct = 100;
  return pct;
}

static String SD_sciezka_logu_miesiecznego(const PoprawionyCzas& t){
  String path;
  path.reserve(28);
  path += "/pogodynka_20";
  path += DwaZnaki(t.Year);
  path += "_";
  path += DwaZnaki(t.Month);
  path += ".csv";
  return path;
}


static bool SD_czy_nazwa_logu_miesiecznego(String name){
  if(!name.startsWith("/")) name = "/" + name;
  return name.startsWith("/pogodynka_20") && name.endsWith(".csv");
}

static String SD_najnowszy_plik_logu(){
  if(!sd_ok) return "";

  File root = SD.open("/");
  if(!root){
    Serial.println("SD: nie moge otworzyc katalogu glownego");
    return "";
  }

  String newest = "";

  File entry = root.openNextFile();
  while(entry){
    if(!entry.isDirectory()){
      String name = entry.name();
      if(!name.startsWith("/")) name = "/" + name;

      // Nazwa zawiera rok i miesiac, dlatego porownanie tekstowe wybiera najnowszy plik.
      // Przyklad: /pogodynka_2026_05.csv > /pogodynka_2026_04.csv
      if(SD_czy_nazwa_logu_miesiecznego(name)){
        if(newest.length() == 0 || name.compareTo(newest) > 0){
          newest = name;
        }
      }
    }
    entry.close();
    entry = root.openNextFile();
  }

  root.close();
  return newest;
}

static bool CSV_pobierz_pole(const String& line, int column, String& out){
  int start = 0;
  int current = 0;
  int len = line.length();

  for(int i=0; i<=len; i++){
    if(i == len || line.charAt(i) == ';'){
      if(current == column){
        out = line.substring(start, i);
        out.trim();
        return true;
      }
      current++;
      start = i + 1;
    }
  }

  out = "";
  return false;
}

static bool CSV_pobierz_float(const String& line, int column, float& value){
  String field;
  if(!CSV_pobierz_pole(line, column, field)) return false;
  if(field.length() == 0) return false;

  // Gdyby plik byl kiedys edytowany w Excelu z przecinkiem dziesietnym.
  field.replace(',', '.');
  value = field.toFloat();
  return true;
}

static bool SD_wczytaj_historie_z_pliku(const String& path){
  if(!sd_ok || path.length() == 0) return false;

  File f = SD.open(path.c_str(), FILE_READ);
  if(!f){
    Serial.print("SD: nie moge wczytac historii z pliku: ");
    Serial.println(path);
    return false;
  }

  uint16_t loaded = 0;

  while(f.available()){
    String line = f.readStringUntil('\n');
    line.trim();

    if(line.length() == 0) continue;
    if(line.startsWith("sep=")) continue;
    if(line.startsWith("timestamp")) continue;

    float temp = NAN;
    float battMv = NAN;
    float rainMv = NAN;

    // Kolumny CSV:
    // 0 timestamp, 1 date, 2 time, 3 temp_C, 8 rain_mV, 9 battery_mV.
    bool okTemp = CSV_pobierz_float(line, 3, temp);
    bool okRain = CSV_pobierz_float(line, 8, rainMv);
    bool okBatt = CSV_pobierz_float(line, 9, battMv);

    if(okTemp && okRain && okBatt){
      History_update(Historia_Temperatury, temp);
      History_update(Historia_Bateri, battMv);
      History_update(Historia_Opady, rainMv);
      if(loaded < History_size) loaded++;
    }
  }

  f.close();

  historia_ilosc = loaded;
  Forecast_update_temp(Historia_Temperatury, historia_ilosc, Prognoza_Temperatury, Forecast_size);

  Serial.print("SD: wczytano historie z pliku ");
  Serial.print(path);
  Serial.print(", probek: ");
  Serial.println(historia_ilosc);

  return loaded > 0;
}

static bool SD_wczytaj_historie_z_najnowszego_pliku(){
  String path = SD_najnowszy_plik_logu();
  if(path.length() == 0){
    Serial.println("SD: brak miesiecznych plikow historii do wczytania");
    return false;
  }
  return SD_wczytaj_historie_z_pliku(path);
}

static bool SD_zapisz_naglowek_jezeli_trzeba(const String& path){
  bool needHeader = true;

  if(SD.exists(path.c_str())){
    File f = SD.open(path.c_str(), FILE_READ);
    if(f){
      needHeader = (f.size() == 0);
      f.close();
    }
  }

  if(!needHeader) return true;

  File f = SD.open(path.c_str(), FILE_WRITE);
  if(!f){
    Serial.print("SD: nie moge utworzyc pliku CSV: ");
    Serial.println(path);
    return false;
  }

  // sep=; pomaga Excelowi od razu rozpoznac separator kolumn.
  f.println("sep=;");
  f.println("timestamp;date;time;temp_C;pressure_Pa;pressure_hPa;humidity_pct;wind_m_s;rain_mV;battery_mV;battery_pct;sun_mV;sun_V");
  f.close();
  return true;
}

static bool SD_init_logger(){
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

  if(!SD.begin(SD_CS_PIN, SPI, 4000000)){
    Serial.println("SD: brak karty albo blad inicjalizacji");
    return false;
  }

  uint8_t cardType = SD.cardType();
  if(cardType == CARD_NONE){
    Serial.println("SD: nie wykryto karty");
    return false;
  }

  // Naglowek pliku CSV jest tworzony dopiero przy pierwszym zapisie pomiaru,
  // bo dopiero wtedy wiadomo, do ktorego miesiaca nalezy rekord.
  Serial.println("SD: logger CSV aktywny");
  return true;
}

static void SD_zapisz_pomiar(const Dane_ESP& d){
  if(!sd_ok) return;

  PoprawionyCzas t = Czas_po_korekcie(d);
  String timestamp = ZnacznikCzasuCSV(t);
  String logPath = SD_sciezka_logu_miesiecznego(t);

  if(!SD_zapisz_naglowek_jezeli_trzeba(logPath)){
    Serial.println("SD: blad przygotowania pliku miesiecznego");
    sd_ok = false;
    return;
  }

  File f = SD.open(logPath.c_str(), FILE_APPEND);
  if(!f){
    Serial.print("SD: blad otwarcia pliku do dopisania: ");
    Serial.println(logPath);
    sd_ok = false;
    return;
  }

  float Batt_procent = Battery_state(d.Battery_level);
  float pressure_hPa = d.Pressure / 100.0f;
  float sunV = d.Sun_level / 1000.0f;

  f.print(timestamp); f.print(';');
  f.print("20"); f.print(DwaZnaki(t.Year)); f.print('-'); f.print(DwaZnaki(t.Month)); f.print('-'); f.print(DwaZnaki(t.Day)); f.print(';');
  f.print(DwaZnaki(t.Hour)); f.print(':'); f.print(DwaZnaki(t.Minute)); f.print(';');
  f.print(d.Temperature, 2); f.print(';');
  f.print(d.Pressure, 0); f.print(';');
  f.print(pressure_hPa, 2); f.print(';');
  f.print(d.Humility, 2); f.print(';');
  f.print(d.Wind_speed, 2); f.print(';');
  f.print(d.Rain, 0); f.print(';');
  f.print(d.Battery_level, 0); f.print(';');
  f.print(Batt_procent, 1); f.print(';');
  f.print(d.Sun_level, 0); f.print(';');
  f.println(sunV, 3);

  f.close();
}

static void SD_logger_loop(){
  if(!sd_log_pending) return;

  Dane_ESP d;
  noInterrupts();
  d = Dane_do_zapisu;
  sd_log_pending = false;
  interrupts();

  SD_zapisz_pomiar(d);
}

static void Internet_check_start(){
  if(WiFi.status() == WL_CONNECTED) return;
  if(wifi_laczenie) return;
  WiFi.begin(ssid, password);
  wifi_start_ms = millis();
  wifi_laczenie = true;
}

static void Internet_check_loop(){
  wl_status_t st = WiFi.status();

  if(st == WL_CONNECTED){
    if(wifi_laczenie){
      wifi_laczenie = false;
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
    }
    if(!mdns_ok){
      if(MDNS.begin("pogodynka")){
        MDNS.addService("http", "tcp", 80);
        mdns_ok = true;
        Serial.println("mDNS: pogodynka.local");
      }
    }
    return;
  }

  if(wifi_laczenie){
    if(millis() - wifi_start_ms > WIFI_CONNECT_TIMEOUT_MS){
      wifi_laczenie = false;
      mdns_ok = false;
      WiFi.disconnect(false);
    }
  }
}

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len){
  (void)info;
#else
void OnDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len){
  (void)mac;
#endif
  if(len == (int)sizeof(Dane_ESP)){
    memcpy(&Dane, incomingData, sizeof(Dane));
    lastRxMs = millis();
    next_wifi_check_ms = lastRxMs + WIFI_CHECK_PERIOD_MS;
    if(historia_ilosc < History_size) historia_ilosc++;

    History_update(Historia_Temperatury, Dane.Temperature);
    History_update(Historia_Bateri, Dane.Battery_level);
    History_update(Historia_Opady, Dane.Rain);

    Dane_do_zapisu = Dane;
    sd_log_pending = true;

    Forecast_update_temp(Historia_Temperatury, historia_ilosc, Prognoza_Temperatury, Forecast_size);
  }
}

void handleRoot(){
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleLatest(){
  server.sendHeader("Cache-Control", "no-store");

  const uint32_t STALE_MS = 60UL * 60UL * 1000UL;
  bool ok = (lastRxMs != 0) && (millis() - lastRxMs < STALE_MS);

  Dane_ESP d;
  noInterrupts();
  d = Dane;
  interrupts();

  PoprawionyCzas t = Czas_po_korekcie(d);

  float Batt_procent = Battery_state(d.Battery_level);
  float sunV = d.Sun_level / 1000.0f;

  String json;
  json.reserve(320);
  json += "{";
  json += "\"ok\":"; json += (ok ? "true" : "false");
  json += ",\"temp\":"; json += String(d.Temperature, 1);
  json += ",\"presPa\":"; json += String(d.Pressure, 0);
  json += ",\"hum\":"; json += String(d.Humility, 0);
  json += ",\"rainmV\":"; json += String(d.Rain, 0);
  json += ",\"wind\":"; json += String(d.Wind_speed, 1);
  json += ",\"Batt_procent\":"; json += String(Batt_procent, 0);
  json += ",\"sunV\":"; json += String(sunV, 2);
  json += ",\"h\":"; json += String(t.Hour);
  json += ",\"m\":"; json += String(t.Minute);
  json += ",\"day\":"; json += String(t.Day);
  json += ",\"mon\":"; json += String(t.Month);
  json += ",\"yr\":"; json += String(t.Year);
  json += "}";
  server.send(200, "application/json; charset=utf-8", json);
}

void handleHistory(){
  server.sendHeader("Cache-Control", "no-store");

  float tempCopy[History_size];
  float battCopy[History_size];
  float rainCopy[History_size];
  float foreCopy[Forecast_size];
  uint16_t ilosc;

  noInterrupts();
  for(int i=0;i<History_size;i++){
    tempCopy[i] = Historia_Temperatury[i];
    battCopy[i] = Historia_Bateri[i];
    rainCopy[i] = Historia_Opady[i];
  }
  for(int i=0;i<Forecast_size;i++){
    foreCopy[i] = Prognoza_Temperatury[i];
  }
  ilosc = historia_ilosc;
  interrupts();

  int empty = History_size - (int)ilosc;
  if(empty < 0) empty = 0;

  String json;
  json.reserve(History_size * 18 * 3 + Forecast_size * 18 + 260);
  json += "{";
  json += "\"ok\":true";

  json += ",\"temp\":[";
  for(int i=0;i<History_size;i++){
    if(i) json += ",";
    if(i < empty) json += "null";
    else json += String(tempCopy[i], 1);
  }
  json += "]";

  json += ",\"tempForecast\":[";
  for(int i=0;i<Forecast_size;i++){
    if(i) json += ",";
    if(!isfinite(foreCopy[i])) json += "null";
    else json += String(foreCopy[i], 1);
  }
  json += "]";

  json += ",\"Batt_procent\":[";
  for(int i=0;i<History_size;i++){
    if(i) json += ",";
    if(i < empty) json += "null";
    else json += String(Battery_state(battCopy[i]), 0);
  }
  json += "]";

  json += ",\"rainmV\":[";
  for(int i=0;i<History_size;i++){
    if(i) json += ",";
    if(i < empty) json += "null";
    else json += String(rainCopy[i], 0);
  }
  json += "]";

  json += "}";
  server.send(200, "application/json; charset=utf-8", json);
}

void handleLogDownload(){
  if(!sd_ok){
    server.send(503, "text/plain; charset=utf-8", "Karta SD nie jest dostepna");
    return;
  }

  String logPath;

  if(lastRxMs == 0){
    // Po restarcie, zanim przyjdzie nowy pakiet ESP-NOW, pobieramy najnowszy plik z karty.
    logPath = SD_najnowszy_plik_logu();
    if(logPath.length() == 0){
      server.send(404, "text/plain; charset=utf-8", "Brak plikow logu na karcie SD");
      return;
    }
  }else{
    Dane_ESP d;
    noInterrupts();
    d = Dane;
    interrupts();

    PoprawionyCzas t = Czas_po_korekcie(d);
    logPath = SD_sciezka_logu_miesiecznego(t);
  }

  File f = SD.open(logPath.c_str(), FILE_READ);
  if(!f){
    server.send(404, "text/plain; charset=utf-8", "Nie znaleziono pliku logu dla aktualnego miesiaca");
    return;
  }

  String filename = logPath;
  if(filename.startsWith("/")) filename.remove(0, 1);

  server.sendHeader("Content-Disposition", "attachment; filename=" + filename);
  server.streamFile(f, "text/csv");
  f.close();
}

void setup(){
  Serial.begin(9600);
  delay(300);

  sd_ok = SD_init_logger();

  for(int i=0;i<History_size;i++){
    Historia_Temperatury[i] = 0.0f;
    Historia_Bateri[i] = 0.0f;
    Historia_Opady[i] = 0.0f;
  }
  for(int i=0;i<Forecast_size;i++){
    Prognoza_Temperatury[i] = NAN;
  }

  // Po restarcie odbiornika odtwarzamy ostatnie punkty wykresow z karty SD.
  // Dzieki temu wykresy nie startuja od zera po kazdym odcieciu zasilania.
  if(sd_ok){
    SD_wczytaj_historie_z_najnowszego_pliku();
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  Internet_check_start();

  if(esp_now_init() != ESP_OK){
    Serial.println("ESP-NOW init FAILED");
    return;
  }
  esp_now_register_recv_cb(OnDataRecv);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/latest", HTTP_GET, handleLatest);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/log.csv", HTTP_GET, handleLogDownload);
  server.onNotFound([](){ server.send(404, "text/plain; charset=utf-8", "404: Nie znaleziono"); });
  server.begin();
}

void loop(){
  server.handleClient();
  Internet_check_loop();
  SD_logger_loop();

  if(next_wifi_check_ms != 0){
    if((int32_t)(millis() - next_wifi_check_ms) >= 0){
      Internet_check_start();
      next_wifi_check_ms += WIFI_CHECK_PERIOD_MS;
    }
  }

  delay(2);
}