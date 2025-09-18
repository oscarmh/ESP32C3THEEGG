#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Adafruit_NeoPixel.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>

/* =================== CONFIG =================== */
// OLED SSD1306 72x40 EastRising por I2C HW en GPIO 5/6
#define OLED_ADDR   0x3C
#define SDA_PIN     5
#define SCL_PIN     6

// WS2812
#define LED_PIN     7        // confirmado por ti
#define NUM_LEDS    1
#define BRIGHTNESS  60

// WiFi STA preferente + AP fallback
#ifndef STA_SSID
  #define STA_SSID "xxxxxxxxxxxxx"
#endif
#ifndef STA_PASS
  #define STA_PASS "xxxxxxxx"
#endif
const char* AP_SSID   = "xxxxxxx";
const char* AP_PASS   = "xxxxxx";
const char* MDNS_NAME = "marta";   // http://marta.local/

/* =================== OBJETOS =================== */
// SSD1306 72x40 ER en I2C HW
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
WebServer server(80);
Adafruit_BME680 bme;  // I2C

/* ============== ESTADO / MODOS ================ */
enum PlayMode   { MODE_STEADY, MODE_BLINK, MODE_EFFECT };
enum EffectName { E_NONE, E_BREATH, E_RAINBOW, E_POLICE, E_CYCLE };

PlayMode   gMode   = MODE_STEADY;
EffectName gEffect = E_NONE;

uint8_t baseR=0, baseG=0, baseB=0;          // Color base

// Intermitencia
bool     bRed=false, bGreen=false, bBlue=false;
uint16_t blinkMs=600;
uint8_t  sel[3];  uint8_t selCount=0;
uint8_t  blinkPhase=0;
uint32_t lastBlink=0;

// Efectos
uint32_t lastFx=0;
uint8_t  hue=0;                // Arcoíris
int16_t  breath=0;             // 0..255
int8_t   breathDir=4;          // paso de respiración
uint8_t  policeStep=0;         // 0..7
uint8_t  cycleStep=0;          // 0..5

// BME680 cache
bool     bmeOK=false;
float    envT=0, envH=0, envP=0, envGasK=0; // cache última lectura
uint32_t lastEnv=0;                           // ms
uint8_t  envPage=0;                           // 0: T/H | 1:P | 2:Gas

// Overlay de conexión en OLED
uint32_t overlayUntil=0; // millis hasta el que se muestra la IP

/* ============== HTML embebido ================== */
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="es"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>MARTALED</title>
<style>
:root{color-scheme:light dark}
body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;margin:0;display:grid;place-items:center;min-height:100dvh}
.w{width:min(540px,94%);padding:10px}
h1{margin:.2rem 0 1rem;font-size:1.35rem}
section{border:1px solid #8883;border-radius:12px;padding:12px;margin:12px 0}
button{padding:12px 14px;margin:6px;border:0;border-radius:10px;color:#fff;cursor:pointer}
.btn{min-width:120px}
.r{background:#d32f2f}.g{background:#388e3c}.b{background:#1976d2}.k{background:#424242}
.e{background:#7b1fa2}.c{background:#009688}.o{background:#ef6c00}
label{margin-right:14px}
input[type="range"]{width:220px}
#status{opacity:.85;margin-top:8px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:10px}
.card{padding:10px;border:1px solid #8883;border-radius:10px}
.val{font-weight:600}
</style></head><body>
<div class="w">
  <h1>MARTALED</h1>

  <section>
    <div>
      <button class="btn r" onclick="setColor('red')">Rojo</button>
      <button class="btn g" onclick="setColor('green')">Verde</button>
      <button class="btn b" onclick="setColor('blue')">Azul</button>
      <button class="btn k" onclick="off()">OFF</button>
    </div>
  </section>

  <section>
    <h3>Intermitencia</h3>
    <div style="display:flex;flex-wrap:wrap;align-items:center;gap:10px">
      <label><input type="checkbox" id="blinkR"> Rojo</label>
      <label><input type="checkbox" id="blinkG"> Verde</label>
      <label><input type="checkbox" id="blinkB"> Azul</label>
      <label>Velocidad (ms):
        <input type="range" id="blinkMs" min="100" max="2000" step="50" value="600"
               oninput="bmv.value=this.value"><output id="bmv">600</output>
      </label>
    </div>
    <div>
      <button class="btn c" onclick="blink(true)">Activar intermitente</button>
      <button class="btn k" onclick="blink(false)">Detener intermitente</button>
    </div>
  </section>

  <section>
    <h3>Efectos</h3>
    <div>
      <button class="btn e" onclick="fx('breath')">Respiración</button>
      <button class="btn o" onclick="fx('rainbow')">Arcoíris</button>
      <button class="btn r" onclick="fx('police')">Policía</button>
      <button class="btn b" onclick="fx('cycle')">Ciclo RGB</button>
      <button class="btn k" onclick="fx('none')">Parar</button>
    </div>
  </section>

  <section>
    <h3>Ambiente (BME680)</h3>
    <div class="grid">
      <div class="card">Temp<br><span id="t" class="val">--.- °C</span></div>
      <div class="card">Humedad<br><span id="h" class="val">-- %</span></div>
      <div class="card">Presión<br><span id="p" class="val">---- hPa</span></div>
      <div class="card">Gas<br><span id="g" class="val">--- kΩ</span></div>
    </div>
  </section>

  <p id="status">Listo</p>
</div>

<script>
async function call(u){
  const s=document.getElementById('status'); s.textContent='Procesando...';
  try{
    const r=await fetch(u,{cache:'no-store'});
    const j=await r.json();
    s.textContent=j.ok?(j.msg||'OK'):(j.msg||'Error');
  }catch(e){ s.textContent='Error de red'; }
}
function setColor(c){ call('/set?c='+c); }
function off(){ call('/off'); }
function blink(enable){
  const r=+document.getElementById('blinkR').checked;
  const g=+document.getElementById('blinkG').checked;
  const b=+document.getElementById('blinkB').checked;
  const ms=document.getElementById('blinkMs').value;
  call(`/blinkset?enable=${enable?1:0}&red=${r}&green=${g}&blue=${b}&ms=${ms}`);
}
function fx(name){ call('/effect?name='+name); }

async function pollEnv(){
  try{
    const r=await fetch('/env',{cache:'no-store'});
    const j=await r.json();
    if(j && j.ok){
      document.getElementById('t').textContent = j.t.toFixed(1)+ ' °C';
      document.getElementById('h').textContent = Math.round(j.h)+' %';
      document.getElementById('p').textContent = Math.round(j.p)+' hPa';
      document.getElementById('g').textContent = Math.round(j.g)+' kΩ';
    }
  }catch(e){ /* ignore */ }
}
setInterval(pollEnv, 5000);
pollEnv();
</script>
</body></html>
)HTML";

/* ============== UTILIDADES ===================== */
static inline void showColor(uint8_t r,uint8_t g,uint8_t b){
  strip.setPixelColor(0, strip.Color(r,g,b)); strip.show();
}
static inline void showScaled(uint8_t r,uint8_t g,uint8_t b,uint8_t s){
  showColor((uint16_t)r*s/255,(uint16_t)g*s/255,(uint16_t)b*s/255);
}
static inline uint32_t nowMs(){ return millis(); }

/* ===== OLED helpers ===== */
void buildLedLine(char* out, size_t n){
  // Máximo ~14-16 caracteres por línea con font 5x7
  if(gMode==MODE_STEADY){
    if(!baseR && !baseG && !baseB) snprintf(out,n,"LED Off");
    else if(baseR && !baseG && !baseB) snprintf(out,n,"LED R");
    else if(baseG && !baseR && !baseB) snprintf(out,n,"LED G");
    else if(baseB && !baseR && !baseG) snprintf(out,n,"LED B");
    else snprintf(out,n,"LED Mix");
  }else if(gMode==MODE_BLINK){
    char c[4]={0}; uint8_t p=0; if(bRed)c[p++]='R'; if(bGreen)c[p++]='G'; if(bBlue)c[p++]='B'; c[p]=0;
    snprintf(out,n,"BLK %s %u", p?c:"-", (unsigned)blinkMs);
  }else{
    const char* fx=(gEffect==E_BREATH)?"FxBreath":(gEffect==E_RAINBOW)?"FxRbow":(gEffect==E_POLICE)?"FxPol":(gEffect==E_CYCLE)?"FxCyc":"FxNone";
    snprintf(out,n,"%s",fx);
  }
}

void buildEnvLine(char* out, size_t n){
  if(!bmeOK){ snprintf(out,n,"BME680 -"); return; }
  switch(envPage%3){
    case 0: snprintf(out,n,"T %.1f H %u", envT, (unsigned)(envH+0.5f)); break;
    case 1: snprintf(out,n,"P %u", (unsigned)(envP+0.5f)); break;          // hPa
    default: snprintf(out,n,"Gas %.0fk", envGasK); break;                  // kΩ
  }
}

void oledDrawCombined(){
  char l1[20], l2[20];
  buildLedLine(l1,sizeof(l1));
  buildEnvLine(l2,sizeof(l2));
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tf);   // compacto para 72x40
  u8g2.drawStr(0, 8,  l1);
  u8g2.drawStr(0, 24, l2);
  u8g2.sendBuffer();
}

void oledDrawConn(){
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tf);
  if(WiFi.getMode() & WIFI_MODE_STA && WiFi.status()==WL_CONNECTED){
    u8g2.drawStr(0,8,  "WiFi: STA");
    String ip=WiFi.localIP().toString(); u8g2.drawStr(0,24, ip.c_str());
  } else if(WiFi.getMode() & WIFI_MODE_AP){
    u8g2.drawStr(0,8,  "WiFi: AP");
    String ip=WiFi.softAPIP().toString(); u8g2.drawStr(0,24, ip.c_str());
  } else {
    u8g2.drawStr(0,8,  "WiFi: -");
  }
  u8g2.sendBuffer();
}

void oledRefresh(){
  if(overlayUntil && nowMs() < overlayUntil) oledDrawConn();
  else oledDrawCombined();
}

/* ===== Lectura BME680 cada 5s ===== */
void readBME680(){
  if(!bmeOK) return;
  if(!bme.performReading()) return; // lectura con espera interna
  envT = bme.temperature;                   // °C
  envH = bme.humidity;                      // %RH
  envP = bme.pressure / 100.0;              // hPa
  envGasK = (bme.gas_resistance/1000.0);    // kΩ
}

/* ============== ARCOÍRIS (una sola vez) ======= */
uint32_t wheelColor(uint8_t p){
  p=255-p; uint8_t r,g,b;
  if(p<85){ r=255-p*3; g=0; b=p*3; }
  else if(p<170){ p-=85; r=0; g=p*3; b=255-p*3; }
  else { p-=170; r=p*3; g=255-p*3; b=0; }
  return ((uint32_t)r<<16)|((uint32_t)g<<8)|b;
}
void applyWheel(uint8_t pos){
  uint32_t c=wheelColor(pos);
  showColor((c>>16)&0xFF,(c>>8)&0xFF,c&0xFF);
}

/* ============== HTTP =========================== */
void sendOk(const String& msg){
  server.sendHeader("Cache-Control","no-store");
  server.send(200,"application/json",String("{\"ok\":true,\"msg\":\"")+msg+"\"}");
}
void sendErr(const String& msg){
  server.sendHeader("Cache-Control","no-store");
  server.send(400,"application/json",String("{\"ok\":false,\"msg\":\"")+msg+"\"}");
}

void handleRoot(){ server.send_P(200,"text/html; charset=utf-8",INDEX_HTML); }

void handleSet(){
  if(!server.hasArg("c")){ sendErr("Falta parametro c"); return; }
  String c=server.arg("c"); c.toLowerCase();
  if(c=="red"){ baseR=255;baseG=0;baseB=0; }
  else if(c=="green"){ baseR=0;baseG=255;baseB=0; }
  else if(c=="blue"){ baseR=0;baseG=0;baseB=255; }
  else { sendErr("Color invalido"); return; }

  gMode=MODE_STEADY; gEffect=E_NONE;
  showColor(baseR, baseG, baseB);
  oledRefresh();
  sendOk(String("Color: ")+c);
}

void handleOff(){
  baseR=baseG=baseB=0;
  gMode=MODE_STEADY; gEffect=E_NONE;
  showColor(0,0,0);
  oledRefresh();
  sendOk("OFF");
}

void handleBlinkSet(){
  if(!server.hasArg("enable")){ sendErr("Falta enable"); return; }
  bool enable=(server.arg("enable")=="1");
  bRed  =(server.hasArg("red")   && server.arg("red") =="1");
  bGreen=(server.hasArg("green") && server.arg("green")=="1");
  bBlue =(server.hasArg("blue")  && server.arg("blue") =="1");
  if(server.hasArg("ms")){
    int ms=server.arg("ms").toInt();
    if(ms<50)ms=50; if(ms>5000)ms=5000;
    blinkMs=(uint16_t)ms;
  }

  if(enable){
    selCount=0; if(bRed)sel[selCount++]=0; if(bGreen)sel[selCount++]=1; if(bBlue)sel[selCount++]=2;
    if(selCount==0){ sendErr("Selecciona al menos un color"); return; }
    gMode=MODE_BLINK; gEffect=E_NONE; blinkPhase=0; lastBlink=nowMs();
    oledRefresh(); sendOk("Blink ON");
  }else{
    gMode=MODE_STEADY; gEffect=E_NONE; showColor(baseR,baseG,baseB); oledRefresh(); sendOk("Blink OFF");
  }
}

void handleEffect(){
  if(!server.hasArg("name")){ sendErr("Falta name"); return; }
  String name=server.arg("name"); name.toLowerCase();

  if(name=="breath"){ gEffect=E_BREATH; gMode=MODE_EFFECT; lastFx=nowMs(); breath=0; breathDir=4; if(baseR==0&&baseG==0&&baseB==0){ baseR=0;baseG=64;baseB=255; } }
  else if(name=="rainbow"){ gEffect=E_RAINBOW; gMode=MODE_EFFECT; lastFx=nowMs(); hue=0; }
  else if(name=="police"){  gEffect=E_POLICE;  gMode=MODE_EFFECT; lastFx=nowMs(); policeStep=0; }
  else if(name=="cycle"){   gEffect=E_CYCLE;   gMode=MODE_EFFECT; lastFx=nowMs(); cycleStep=0; }
  else if(name=="none"){    gEffect=E_NONE;    gMode=MODE_STEADY; showColor(baseR,baseG,baseB); oledRefresh(); sendOk("Efecto parado"); return; }
  else { sendErr("Efecto invalido"); return; }

  oledRefresh(); sendOk(String("Efecto: ")+name);
}

void handleEnv(){
  String json = "{";
  json += "\"ok\":true,";
  json += "\"t\":" + String(envT,1) + ",";
  json += "\"h\":" + String(envH,1) + ",";
  json += "\"p\":" + String(envP,1) + ",";
  json += "\"g\":" + String(envGasK,0);
  json += "}";
  server.sendHeader("Cache-Control","no-store");
  server.send(200, "application/json", json);
}

/* ============== Tickers ======================== */
void tickBlink(uint32_t now){
  if(selCount==0) return;
  const uint32_t half=(uint32_t)blinkMs/2;
  if(now-lastBlink>=half){
    lastBlink=now;
    if((blinkPhase%2)==0){
      uint8_t idx=sel[(blinkPhase/2)%selCount];
      if(idx==0)      showColor(255,0,0);
      else if(idx==1) showColor(0,255,0);
      else            showColor(0,0,255);
    }else{
      showColor(0,0,0);
    }
    blinkPhase=(blinkPhase+1)%(selCount*2);
  }
}

void tickEffect(uint32_t now){
  switch(gEffect){
    case E_BREATH:
      if(now-lastFx>=12){
        lastFx=now; breath+=breathDir;
        if(breath>=255){ breath=255; breathDir=-breathDir; }
        if(breath<=0){   breath=0;   breathDir=-breathDir; }
        showScaled(baseR,baseG,baseB,(uint8_t)breath);
      }
      break;
    case E_RAINBOW:
      if(now-lastFx>=15){ lastFx=now; applyWheel(hue++); }
      break;
    case E_POLICE: {
      const uint16_t stepMs=90;
      if(now-lastFx>=stepMs){
        lastFx=now;
        switch(policeStep&0x07){
          case 0: showColor(255,0,0); break;
          case 1: showColor(0,0,0);   break;
          case 2: showColor(255,0,0); break;
          case 3: showColor(0,0,0);   break;
          case 4: showColor(0,0,255); break;
          case 5: showColor(0,0,0);   break;
          case 6: showColor(0,0,255); break;
          case 7: showColor(0,0,0);   break;
        }
        policeStep=(policeStep+1)&0x07;
      }
      break; }
    case E_CYCLE: {
      const uint16_t stepMs=350;
      if(now-lastFx>=stepMs){
        lastFx=now;
        switch(cycleStep%6){
          case 0: showColor(255,0,0); break;
          case 1: showColor(0,0,0);   break;
          case 2: showColor(0,255,0); break;
          case 3: showColor(0,0,0);   break;
          case 4: showColor(0,0,255); break;
          case 5: showColor(0,0,0);   break;
        }
        cycleStep=(cycleStep+1)%6;
      }
      break; }
    default: break;
  }
}

/* ============== SETUP / LOOP =================== */
void setup(){
  // LED
  strip.begin(); strip.setBrightness(BRIGHTNESS); showColor(0,0,0);

  // OLED: I2C HW remapeado + ajustes
  Wire.begin(SDA_PIN, SCL_PIN);
  u8g2.begin();
  u8g2.setI2CAddress(OLED_ADDR * 2);
  u8g2.setBusClock(400000);   // 400 kHz
  u8g2.setPowerSave(0);
  u8g2.setContrast(220);

  // BME680 init (0x77 -> 0x76)
  if (bme.begin(0x77) || bme.begin(0x76)){
    bmeOK=true;
    // Oversampling recomendado
    bme.setTemperatureOversampling(BME680_OS_8X);
    bme.setHumidityOversampling(BME680_OS_4X);
    bme.setPressureOversampling(BME680_OS_4X);
    bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
    bme.setGasHeater(320, 150); // 320°C durante 150 ms
  }

  // Pantalla de arranque
  oledDrawCombined();

  // --- WiFi: intentamos STA y, si falla, AP ---
  WiFi.mode(WIFI_STA);
  WiFi.begin(STA_SSID, STA_PASS);
  {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0,8,"WiFi STA");
    u8g2.drawStr(0,24,"Conectando...");
    u8g2.sendBuffer();
  }
  const uint32_t T0 = millis();
  const uint32_t TIMEOUT = 10000; // 10 s
  while (WiFi.status() != WL_CONNECTED && (millis() - T0) < TIMEOUT) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    // Conectado en STA
    if (MDNS.begin(MDNS_NAME)) {
      MDNS.addService("http","tcp",80);
    }
    overlayUntil = nowMs() + 4000;   // mostrar IP STA 4s
    oledDrawConn();
  } else {
    // Fallback a AP
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    if (MDNS.begin(MDNS_NAME)) { // mDNS para clientes del AP
      MDNS.addService("http","tcp",80);
    }
    overlayUntil = nowMs() + 4000;   // mostrar IP AP 4s
    oledDrawConn();
  }

  // Web
  server.on("/",HTTP_GET,handleRoot);
  server.on("/set",HTTP_GET,handleSet);
  server.on("/off",HTTP_GET,handleOff);
  server.on("/blinkset",HTTP_GET,handleBlinkSet);
  server.on("/effect",HTTP_GET,handleEffect);
  server.on("/env",HTTP_GET,handleEnv);
  server.begin();

  // Señal de arranque
  showColor(0,0,64); delay(200); showColor(0,0,0);
}

void loop(){
  server.handleClient();
  const uint32_t t=nowMs();

  // Tickers de LED
  if(gMode==MODE_BLINK)       tickBlink(t);
  else if(gMode==MODE_EFFECT) tickEffect(t);

  // Lectura BME680 y refresco OLED cada 5 s
  if(t - lastEnv >= 5000){
    lastEnv = t;
    readBME680();
    envPage = (envPage + 1) % 3;   // rota páginas T/H, P y Gas
    oledRefresh();
  }

  // Si sigue activo el overlay, refrescar por si cambia IP (raro) y para mantenerlo visible
  if(overlayUntil && t < overlayUntil){
    static uint32_t lastOverlay=0;
    if(t - lastOverlay > 1000){ lastOverlay=t; oledDrawConn(); }
  }
}
