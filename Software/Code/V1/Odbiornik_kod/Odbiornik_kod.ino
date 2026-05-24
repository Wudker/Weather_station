#include <WiFi.h>
#include <esp_now.h>
#include <WebServer.h>
#include <Preferences.h>

#include "PogodynkaRxTypes.h"

const char* ssid     = "UPC0772731_24Ghz";
const char* password = "sumyhfguw3zf5Hte";

WebServer server(80);
Preferences prefs;

uint8_t stationMac[] = {0x6C, 0xC8, 0x40, 0x54, 0xD7, 0x28};

DaneESP_V2 lastData{};
volatile bool hasData = false;

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static const uint16_t HIST_MAX = 120;
HistPoint hist[HIST_MAX];
volatile uint16_t histHead  = 0;
volatile uint16_t histCount = 0;

// ===================== CONFIG (NVS) =====================
uint16_t cfgSleepMin = 5;     // min
uint16_t cfgMeasSec  = 120;   // s
uint16_t cfgListenMs = 1800;  // ms
volatile bool stationSeen = false;

static void clampConfig() {
  if (cfgSleepMin < 1)   cfgSleepMin = 1;
  if (cfgSleepMin > 180) cfgSleepMin = 180;
  if (cfgMeasSec < 30)   cfgMeasSec = 30;
  if (cfgMeasSec > 300)  cfgMeasSec = 300;
  if (cfgListenMs < 200) cfgListenMs = 200;
  if (cfgListenMs > 5000) cfgListenMs = 5000;
}

static void loadConfigFromNVS() {
  prefs.begin("pogodynka", false);
  cfgSleepMin = prefs.getUShort("sleepMin", 5);
  cfgMeasSec  = prefs.getUShort("measSec", 120);
  cfgListenMs = prefs.getUShort("listenMs", 1800);
  clampConfig();
}

static void saveConfigToNVS() {
  prefs.putUShort("sleepMin", cfgSleepMin);
  prefs.putUShort("measSec",  cfgMeasSec);
  prefs.putUShort("listenMs", cfgListenMs);
}

static void sendConfigToStation() {
  CmdConfig c;
  c.magic = CFG_MAGIC;
  c.version = CFG_VERSION;
  c.sleep_minutes = cfgSleepMin;
  c.meas_seconds  = cfgMeasSec;
  c.listen_ms     = cfgListenMs;

  esp_err_t r = esp_now_send(stationMac, (uint8_t*)&c, sizeof(c));
  Serial.printf("CFG -> stacja: sleep=%u meas=%u listen=%u | %s\n",
                cfgSleepMin, cfgMeasSec, cfgListenMs, (r==ESP_OK?"OK":"FAIL"));
}

// ===================== HELPERY DANE/HIST =====================
static void saveLast(const DaneESP_V2& d) {
  portENTER_CRITICAL(&mux);
  lastData = d;
  hasData = true;
  portEXIT_CRITICAL(&mux);
}

static DaneESP_V2 getLast(bool &ok) {
  DaneESP_V2 d;
  portENTER_CRITICAL(&mux);
  ok = hasData;
  d = lastData;
  portEXIT_CRITICAL(&mux);
  return d;
}

static void pushHistory(const DaneESP_V2& d) {
  portENTER_CRITICAL(&mux);

  HistPoint p;
  p.temp10  = d.temperatura;
  p.pres    = d.cisnienie;
  p.hum     = d.wilgotnosc;

  p.wind10  = d.predkosc;
  p.rain10  = d.opady;

  p.sunmV   = d.poziom_slonca;
  p.battmV  = d.poziom_baterii;

  p.voc     = d.voc_index;

  p.h       = d.godzina;
  p.m       = d.minuta;

  p.day     = d.dzien;
  p.mon     = d.miesiac;
  p.yr      = d.rok;

  p.rain    = d.czy_pada ? 1 : 0;

  hist[histHead] = p;
  histHead = (histHead + 1) % HIST_MAX;
  if (histCount < HIST_MAX) histCount++;

  portEXIT_CRITICAL(&mux);
}

static uint16_t getHistorySnapshot(HistPoint* out, uint16_t maxOut) {
  portENTER_CRITICAL(&mux);

  uint16_t n = histCount;
  if (n > maxOut) n = maxOut;

  uint16_t start = (histHead + HIST_MAX - n) % HIST_MAX;
  for (uint16_t i = 0; i < n; i++) out[i] = hist[(start + i) % HIST_MAX];

  portEXIT_CRITICAL(&mux);
  return n;
}

static void printV2(const DaneESP_V2& d) {
  Serial.println("\n===== DANE ZE STACJI (V2) =====");
  Serial.printf("Temperatura : %.1f C\n", d.temperatura / 10.0f);
  Serial.printf("Cisnienie   : %u hPa\n", d.cisnienie);
  Serial.printf("Wilgotnosc  : %u %%\n", d.wilgotnosc);
  Serial.printf("Opady       : %.1f\n", d.opady / 10.0f);
  Serial.printf("Predkosc    : %.1f m/s\n", d.predkosc / 10.0f);
  Serial.printf("Slonce      : %.2f V\n", d.poziom_slonca / 1000.0f);

  float vbatt = d.poziom_baterii / 1000.0f;
  Serial.printf("Bateria     : %.2f V\n", vbatt);

  float battPct = (vbatt / 4.2f) * 100.0f;
  if (battPct < 0) battPct = 0;
  if (battPct > 100) battPct = 100;
  Serial.printf("Bateria %%   : %.0f %%\n", battPct);

  Serial.printf("VOC raw avg : %u\n", d.voc_raw);
  Serial.printf("VOC index   : %u (0..500)\n", d.voc_index);
  Serial.printf("Czas        : %02u:%02u:%02u\n", d.godzina, d.minuta, d.sekunda);
  Serial.printf("Data        : %02u-%02u-20%02u\n", d.dzien, d.miesiac, d.rok);
  Serial.printf("Czy pada    : %s\n", d.czy_pada ? "TAK" : "NIE");
  Serial.println("===============================");
}

// ===================== API JSON =====================
static void handleLatest() {
  bool ok;
  DaneESP_V2 d = getLast(ok);

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json; charset=utf-8", "");

  if (!ok) { server.sendContent("{\"ok\":false}"); return; }

  String s; s.reserve(420);
  s  = "{\"ok\":true";
  s += ",\"temp10\":" + String(d.temperatura);
  s += ",\"pres\":"   + String(d.cisnienie);
  s += ",\"hum\":"    + String(d.wilgotnosc);
  s += ",\"wind10\":" + String(d.predkosc);
  s += ",\"rain10\":" + String(d.opady);
  s += ",\"sunmV\":"  + String(d.poziom_slonca);
  s += ",\"battmV\":" + String(d.poziom_baterii);
  s += ",\"voc\":"    + String(d.voc_index);
  s += ",\"h\":"      + String(d.godzina);
  s += ",\"m\":"      + String(d.minuta);
  s += ",\"sec\":"    + String(d.sekunda);
  s += ",\"day\":"    + String(d.dzien);
  s += ",\"mon\":"    + String(d.miesiac);
  s += ",\"yr\":"     + String(d.rok);
  s += ",\"rain\":"   + String(d.czy_pada ? 1 : 0);
  s += "}";
  server.sendContent(s);
}

static void handleHistory() {
  static HistPoint tmp[HIST_MAX];
  uint16_t n = getHistorySnapshot(tmp, HIST_MAX);

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json; charset=utf-8", "");

  if (n == 0) { server.sendContent("{\"ok\":false,\"count\":0,\"points\":[]}"); return; }

  server.sendContent("{\"ok\":true,\"count\":");
  server.sendContent(String(n));
  server.sendContent(",\"points\":[");
  for (uint16_t i = 0; i < n; i++) {
    if (i) server.sendContent(",");
    server.sendContent("{\"h\":");      server.sendContent(String(tmp[i].h));
    server.sendContent(",\"m\":");      server.sendContent(String(tmp[i].m));
    server.sendContent(",\"day\":");    server.sendContent(String(tmp[i].day));
    server.sendContent(",\"mon\":");    server.sendContent(String(tmp[i].mon));
    server.sendContent(",\"yr\":");     server.sendContent(String(tmp[i].yr));
    server.sendContent(",\"temp10\":"); server.sendContent(String(tmp[i].temp10));
    server.sendContent(",\"pres\":");   server.sendContent(String(tmp[i].pres));
    server.sendContent(",\"hum\":");    server.sendContent(String(tmp[i].hum));
    server.sendContent(",\"wind10\":"); server.sendContent(String(tmp[i].wind10));
    server.sendContent(",\"rain10\":"); server.sendContent(String(tmp[i].rain10));
    server.sendContent(",\"sunmV\":");  server.sendContent(String(tmp[i].sunmV));
    server.sendContent(",\"battmV\":"); server.sendContent(String(tmp[i].battmV));
    server.sendContent(",\"voc\":");    server.sendContent(String(tmp[i].voc));
    server.sendContent(",\"rain\":");   server.sendContent(String(tmp[i].rain ? 1 : 0));
    server.sendContent("}");
  }
  server.sendContent("]}");
}

static void handleConfigGet() {
  String s; s.reserve(160);
  s  = "{\"ok\":true";
  s += ",\"sleepMin\":" + String(cfgSleepMin);
  s += ",\"measSec\":"  + String(cfgMeasSec);
  s += ",\"listenMs\":" + String(cfgListenMs);
  s += ",\"stationSeen\":" + String(stationSeen ? 1 : 0);
  s += "}";
  server.send(200, "application/json; charset=utf-8", s);
}

static void handleConfigSet() {
  if (server.hasArg("sleep"))  cfgSleepMin = (uint16_t)server.arg("sleep").toInt();
  if (server.hasArg("meas"))   cfgMeasSec  = (uint16_t)server.arg("meas").toInt();
  if (server.hasArg("listen")) cfgListenMs = (uint16_t)server.arg("listen").toInt();

  clampConfig();
  saveConfigToNVS();
  sendConfigToStation();

  server.sendHeader("Location", "/");
  server.send(303);
}

// ===================== WWW (krótko: korzysta z Twojego /api/latest i /api/history) =====================
static const char INDEX_HTML[] = R"rawliteral(
<!doctype html><html lang="pl"><head>
<meta charset="utf-8"/><meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>Pogodynka</title>
<style>
body{font-family:'Courier New',monospace;text-align:center;background:#000;color:#fff;margin:0;padding:16px 8px;}
.wrap{max-width:980px;margin:0 auto;}
table{border-collapse:collapse;width:100%;margin:10px auto;}
th{padding:10px;background:#EFBF04;color:#000;}
td{padding:8px;border:1px solid #333;background:#0b0b0b;}
td.val{text-align:right;}
.box{border:1px solid #333;background:#060606;border-radius:12px;padding:12px;margin:12px 0;text-align:left;}
.grid{display:grid;grid-template-columns:1fr 140px;gap:8px;align-items:center;}
input,button,select{background:#111;color:#fff;border:1px solid #444;padding:6px 8px;border-radius:8px;font-family:'Courier New',monospace;}
button{cursor:pointer}
.muted{color:#bbb;font-size:12px;}
.warn{color:#ff6b6b;font-weight:bold;}
.chartBox{border:1px solid #333;background:#050505;border-radius:12px;padding:10px;margin:10px 0;}
canvas{width:100%;height:280px;display:block;}
</style></head><body>
<div class="wrap">
<h2 style="margin:0;text-align:left;">POGODYNKA</h2>
<div class="muted" style="text-align:left;margin-top:6px;">Ostatni pomiar: <b id="last">—</b></div>
<div class="muted" style="text-align:left;" id="status">Ładowanie...</div>
<div class="muted" style="text-align:left;" id="cfgStatus">Konfiguracja: —</div>

<div class="box">
  <b>Ustawienia stacji</b>
  <form method="POST" action="/setcfg" style="margin-top:10px;">
    <div class="grid">
      <label for="sleep">Interwał wybudzeń [min]</label>
      <input id="sleep" name="sleep" type="number" min="1" max="180" value="5">
      <label for="meas">Okno pomiaru [s]</label>
      <input id="meas" name="meas" type="number" min="30" max="300" value="120">
      <label for="listen">Nasłuch po wysyłce [ms]</label>
      <input id="listen" name="listen" type="number" min="200" max="5000" value="1800">
    </div>
    <div style="margin-top:10px;">
      <button type="submit">Zapisz i wyślij</button>
      <span class="muted">Stacja zastosuje przy następnym pomiarze.</span>
    </div>
  </form>
</div>

<table>
<tr><th>Pomiar</th><th>Wartość</th></tr>
<tr><td>Temperatura</td><td class="val" id="temp">—</td></tr>
<tr><td>Ciśnienie</td><td class="val" id="pres">—</td></tr>
<tr><td>Wilgotność</td><td class="val" id="hum">—</td></tr>
</table>

<div style="display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap;">
  <div style="text-align:left;">
    <b>Wykres trendu</b>
    <div class="muted" id="chartInfo">—</div>
  </div>
  <div>
    <label class="muted" for="metric">Dane:</label>
    <select id="metric">
      <option value="temp">Temperatura</option>
      <option value="batt">Bateria (V)</option>
    </select>
  </div>
</div>

<div class="chartBox"><canvas id="chart"></canvas></div>

<table>
<tr><th>Pomiar</th><th>Wartość</th></tr>
<tr><td>Opady</td><td class="val" id="rain">—</td></tr>
<tr><td>Prędkość wiatru</td><td class="val" id="wind">—</td></tr>
<tr><td>VOC index</td><td class="val" id="voc">—</td></tr>
<tr><td>Bateria</td><td class="val" id="battV">—</td></tr>
<tr><td>Panel</td><td class="val" id="sunV">—</td></tr>
</table>

</div>
<script>
const $=id=>document.getElementById(id);
const pad2=n=>String(n).padStart(2,'0');
const setText=(id,txt)=>{const e=$(id); if(e) e.textContent=txt;};
const clamp=(x,a,b)=>Math.max(a,Math.min(b,x));

const metricMap={
  temp:{key:'temp10',scale:0.1,unit:'°C',digits:1,name:'Temperatura'},
  batt:{key:'battmV',scale:0.001,unit:'V',digits:3,name:'Bateria'},
};

async function refreshConfig(){
  try{
    const r=await fetch('/api/config',{cache:'no-store'});
    const c=await r.json();
    if(!c.ok) return;
    $('sleep').value=c.sleepMin;
    $('meas').value=c.measSec;
    $('listen').value=c.listenMs;
    setText('cfgStatus', c.stationSeen ? 'Stacja widoczna (ostatnia paczka OK)' : 'Czekam na pierwszą paczkę...');
  }catch(e){}
}

async function refreshLatest(){
  try{
    const r=await fetch('/api/latest',{cache:'no-store'});
    const d=await r.json();
    if(!d.ok){ $('status').innerHTML='<span class="warn">Brak danych z ESP-NOW</span>'; return; }
    $('status').textContent='OK';
    setText('last', `${pad2(d.h)}:${pad2(d.m)}:${pad2(d.sec)} | ${pad2(d.day)}.${pad2(d.mon)}.20${pad2(d.yr)}`);
    setText('temp',(d.temp10*0.1).toFixed(1)+' °C');
    setText('pres',d.pres+' hPa');
    setText('hum',d.hum+' %');
    setText('wind',(d.wind10*0.1).toFixed(1)+' m/s');
    setText('rain',(d.rain10*0.1).toFixed(1));
    setText('voc',d.voc);
    setText('battV',(d.battmV/1000).toFixed(3)+' V');
    setText('sunV',(d.sunmV/1000).toFixed(3)+' V');
  }catch(e){
    $('status').innerHTML='<span class="warn">Błąd HTTP</span>';
  }
}

function setupCanvas(canvas){
  const dpr=window.devicePixelRatio||1;
  const rect=canvas.getBoundingClientRect();
  canvas.width=Math.max(300,Math.floor(rect.width*dpr));
  canvas.height=Math.max(200,Math.floor(rect.height*dpr));
  const ctx=canvas.getContext('2d');
  ctx.setTransform(dpr,0,0,dpr,0,0);
  return ctx;
}

function drawChart(points, metricKey){
  const canvas=$('chart');
  const ctx=setupCanvas(canvas);
  const m=metricMap[metricKey];
  const W=canvas.getBoundingClientRect().width;
  const H=canvas.getBoundingClientRect().height;
  ctx.clearRect(0,0,W,H);

  const L=48,R=18,T=18,B=30;
  const w=W-L-R, h=H-T-B;

  if(!points || points.length<2){
    ctx.fillStyle='#bbb'; ctx.font='12px Courier New';
    ctx.fillText('Za mało danych do wykresu', L, T+20);
    setText('chartInfo','Za mało danych do wykresu');
    return;
  }

  const ys=points.map(p=>((p[m.key]??0)*m.scale));
  let yMin=Math.min(...ys), yMax=Math.max(...ys);
  if(yMin===yMax){yMin-=1;yMax+=1;}
  const pad=(yMax-yMin)*0.10; yMin-=pad; yMax+=pad;

  ctx.strokeStyle='#222'; ctx.lineWidth=1;
  for(let i=0;i<=4;i++){
    const yy=T+(h*i/4);
    ctx.beginPath(); ctx.moveTo(L,yy); ctx.lineTo(L+w,yy); ctx.stroke();
  }
  ctx.strokeStyle='#555';
  ctx.beginPath(); ctx.moveTo(L,T); ctx.lineTo(L,T+h); ctx.lineTo(L+w,T+h); ctx.stroke();

  ctx.fillStyle='#bbb'; ctx.font='12px Courier New';
  ctx.fillText(yMax.toFixed(m.digits)+' '+m.unit, 6, T+10);
  ctx.fillText(yMin.toFixed(m.digits)+' '+m.unit, 6, T+h);

  ctx.strokeStyle='#EFBF04'; ctx.lineWidth=2;
  for(let i=0;i<ys.length;i++){
    const x=L+(w*i/(ys.length-1));
    const y=T+(h*(1-(ys[i]-yMin)/(yMax-yMin)));
    if(i===0){ctx.beginPath();ctx.moveTo(x,y);} else ctx.lineTo(x,y);
  }
  ctx.stroke();

  const last=ys[ys.length-1];
  setText('chartInfo', `${m.name}: ostatnia=${last.toFixed(m.digits)} ${m.unit} | punkty=${points.length}`);
}

async function refreshChart(){
  try{
    const metricKey=$('metric').value;
    const r=await fetch('/api/history',{cache:'no-store'});
    const j=await r.json();
    if(!j.ok){ setText('chartInfo','Brak historii'); drawChart([],metricKey); return; }
    drawChart(j.points, metricKey);
  }catch(e){ setText('chartInfo','Błąd wykresu'); }
}

$('metric').addEventListener('change', refreshChart);

(async ()=>{
  await refreshConfig();
  await refreshLatest();
  await refreshChart();
  setInterval(refreshConfig, 5000);
  setInterval(refreshLatest, 4000);
  setInterval(refreshChart, 15000);
})();
</script>
</body></html>
)rawliteral";

static void handleRoot() {
  server.send(200, "text/html; charset=utf-8", INDEX_HTML);
}

// ===================== ESP-NOW recv callback =====================
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
  (void)info;
#else
void OnDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  (void)mac;
#endif
  if (len == (int)sizeof(DaneESP_V2)) {
    DaneESP_V2 d;
    memcpy(&d, incomingData, sizeof(d));
    saveLast(d);
    pushHistory(d);
    stationSeen = true;
    printV2(d);

    // klucz: odsyłamy config od razu po odbiorze
    sendConfigToStation();
  } else {
    Serial.printf("Błędny rozmiar pakietu: %d (oczek. %u)\n", len, (unsigned)sizeof(DaneESP_V2));
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  loadConfigFromNVS();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid, password);

  Serial.print("Receiver STA MAC: ");
  Serial.println(WiFi.macAddress());

  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.print("\nIP: ");
  Serial.println(WiFi.localIP());
  Serial.print("WiFi channel: ");
  Serial.println(WiFi.channel());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init FAILED");
    return;
  }
  esp_now_register_recv_cb(OnDataRecv);
  Serial.println("ESP-NOW OK");

  // dodaj peer stacji (żeby móc wysyłać CFG)
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, stationMac, 6);
  peerInfo.channel = 0;      // 0 = bieżący kanał WiFi
  peerInfo.encrypt = false;

  esp_now_add_peer(&peerInfo); // jeśli już był, zignoruje/zwroci błąd - nie szkodzi

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/latest", HTTP_GET, handleLatest);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/setcfg", HTTP_POST, handleConfigSet);

  server.begin();
  Serial.println("HTTP server started");

  // spróbuj wysłać config na start
  sendConfigToStation();
}

void loop() {
  server.handleClient();
}