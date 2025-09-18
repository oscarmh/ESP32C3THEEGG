#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Adafruit_NeoPixel.h>
#include <U8g2lib.h>
#include <Wire.h>

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
const char* STA_SSID  = "xxxxxxxx";
const char* STA_PASS  = "xxxxxxxxx";
const char* AP_SSID   = "MARTALED";
const char* AP_PASS   = "12345678";
const char* MDNS_NAME = "marta";   // http://marta.local/

/* =================== OBJETOS =================== */
// SSD1306 72x40 ER en I2C HW
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
WebServer server(80);

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

/* ============== HTML embebido ================== */
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="es"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>MARTALED</title>
<style>
:root{color-scheme:light dark}
body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;margin:0;display:grid;place-items:center;min-height:100dvh}
.w{width:min(520px,94%);padding:10px}
h1{margin:.2rem 0 1rem;font-size:1.35rem}
section{border:1px solid #8883;border-radius:12px;padding:12px;margin:12px 0}
button{padding:12px 14px;margin:6px;border:0;border-radius:10px;color:#fff;cursor:pointer}
.btn{min-width:120px}
.r{background:#d32f2f}.g{background:#388e3c}.b{background:#1976d2}.k{background:#424242}
.e{background:#7b1fa2}.c{background:#009688}.o{background:#ef6c00}
label{margin-right:14px}
input[type="range"]{width:220px}
#status{opacity:.85;margin-top:8px}
</style></head><body>
<div class="w">
  <h1>Control LED</h1>

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

void oledText(const char* l1,const char* l2){
  // Pensado para 72x40: dos líneas legibles
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);   // o u8g2_font_5x7_tf si necesitas más margen
  u8g2.drawStr(0, 8,  l1);
  u8g2.drawStr(0, 24, l2);
  u8g2.sendBuffer();
}

void oledShowMode(){
  char line2[32];
  if(gMode==MODE_STEADY){
    const char* name=(baseR||baseG||baseB)?((baseR&& !baseG&& !baseB)?"Rojo":(baseG&& !baseR&& !baseB)?"Verde":(baseB&& !baseR&& !baseG)?"Azul":"Mix"):"OFF";
    snprintf(line2,sizeof(line2),"Color: %s",name);
    oledText("marta.local",line2);
  }else if(gMode==MODE_BLINK){
    String s; if(bRed)s+="R"; if(bGreen)s+="G"; if(bBlue)s+="B";
    snprintf(line2,sizeof(line2),"Blink %s @%ums",s.length()?s.c_str():"-",blinkMs);
    oledText("marta.local",line2);
  }else{
    const char* fx=(gEffect==E_BREATH)?"Respir.":(gEffect==E_RAINBOW)?"Arcoiris":(gEffect==E_POLICE)?"Policia":(gEffect==E_CYCLE)?"Ciclo RGB":"Ninguno";
    snprintf(line2,sizeof(line2),"Efecto: %s",fx);
    oledText("marta.local",line2);
  }
}

/* Mostrar estado de red en OLED */
void oledNetSTA(){
  String ip = WiFi.localIP().toString();
  oledText("WiFi: STA", ip.c_str());
}
void oledNetAP(){
  String ip = WiFi.softAPIP().toString();
  oledText("WiFi: AP", ip.c_str());
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
  oledShowMode();
  sendOk(String("Color: ")+c);
}

void handleOff(){
  baseR=baseG=baseB=0;
  gMode=MODE_STEADY; gEffect=E_NONE;
  showColor(0,0,0);
  oledShowMode();
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
    oledShowMode(); sendOk("Blink ON");
  }else{
    gMode=MODE_STEADY; gEffect=E_NONE; showColor(baseR,baseG,baseB); oledShowMode(); sendOk("Blink OFF");
  }
}

void handleEffect(){
  if(!server.hasArg("name")){ sendErr("Falta name"); return; }
  String name=server.arg("name"); name.toLowerCase();

  if(name=="breath"){ gEffect=E_BREATH; gMode=MODE_EFFECT; lastFx=nowMs(); breath=0; breathDir=4; if(baseR==0&&baseG==0&&baseB==0){ baseR=0;baseG=64;baseB=255; } }
  else if(name=="rainbow"){ gEffect=E_RAINBOW; gMode=MODE_EFFECT; lastFx=nowMs(); hue=0; }
  else if(name=="police"){  gEffect=E_POLICE;  gMode=MODE_EFFECT; lastFx=nowMs(); policeStep=0; }
  else if(name=="cycle"){   gEffect=E_CYCLE;   gMode=MODE_EFFECT; lastFx=nowMs(); cycleStep=0; }
  else if(name=="none"){    gEffect=E_NONE;    gMode=MODE_STEADY; showColor(baseR,baseG,baseB); oledShowMode(); sendOk("Efecto parado"); return; }
  else { sendErr("Efecto invalido"); return; }

  oledShowMode(); sendOk(String("Efecto: ")+name);
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
  oledText("MARTALED","Arrancando...");

  // --- WiFi: intentamos STA y, si falla, AP ---
  WiFi.mode(WIFI_STA);
  WiFi.begin(STA_SSID, STA_PASS);
  oledText("WiFi STA","Conectando...");
  const uint32_t T0 = millis();
  const uint32_t TIMEOUT = 10000; // 10 s
  while (WiFi.status() != WL_CONNECTED && (millis() - T0) < TIMEOUT) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    // Conectado en STA
    oledNetSTA();
    if (MDNS.begin(MDNS_NAME)) {
      MDNS.addService("http","tcp",80);
    }
  } else {
    // Fallback a AP
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    oledNetAP();
    if (MDNS.begin(MDNS_NAME)) { // mDNS para clientes del AP
      MDNS.addService("http","tcp",80);
    }
  }

  // Web
  server.on("/",HTTP_GET,handleRoot);
  server.on("/set",HTTP_GET,handleSet);
  server.on("/off",HTTP_GET,handleOff);
  server.on("/blinkset",HTTP_GET,handleBlinkSet);
  server.on("/effect",HTTP_GET,handleEffect);
  server.begin();

  // Señal de arranque
  showColor(0,0,64); delay(200); showColor(0,0,0);
}

void loop(){
  server.handleClient();
  uint32_t t=nowMs();
  if(gMode==MODE_BLINK)       tickBlink(t);
  else if(gMode==MODE_EFFECT) tickEffect(t);
}
