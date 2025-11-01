#include <Wire.h>
#include <RTClib.h>
#include <DmxSimple.h>
#include <SPI.h>
#include <Ethernet.h>
#include <EthernetHttpClient.h>
#include <ArduinoJson.h>

// ===================== CONFIG =====================
String DEVICE_NAME = "mega001";
const char* SERVER_HOST = "lasers.drawbridge.kz";
const char* LOCAL_FALLBACK = "192.168.8.100";

// ---- DMX ----
static const uint8_t DMX_PIN = 18; // TX1 → MAX485 DE/RE

// ---- Ethernet ----
#define ETH_CS 10
byte mac[] = {0xDE,0xAD,0xBE,0xEF,0xFE,0xED};
IPAddress ip(192,168,8,2);
IPAddress dns(8,8,8,8);
IPAddress gateway(192,168,8,1);
IPAddress subnet(255,255,255,0);
EthernetClient ethClient;
EthernetHttpClient* http;

// ---- RTC DS3231 ----
RTC_DS3231 rtc;

// ---- UART ----
#define ESP_SERIAL Serial2

// ---- Сцены ----
enum Scene { 
  SCENE_NONE=0, SCENE_1, SCENE_2, SCENE_3, SCENE_4, SCENE_5, SCENE_6,
  SCENE_7, SCENE_8, SCENE_9, SCENE_10, SCENE_11, SCENE_12, SCENE_13, SCENE_14, SCENE_15
};
Scene currentScene = SCENE_NONE;

// ---- Фазы и интервалы ----
uint8_t scenePhase = 0;
unsigned long lastSceneSwitch = 0;

// ---- DMX width ----
uint8_t widthMin=0, widthMax=255, widthNorm=255;
bool serverOnline = true;

// ===================== DMX =====================
uint8_t mapWidth(uint8_t n){
  if(n==0)return 0;
  uint8_t lo=widthMin,hi=widthMax;
  if(hi<lo){uint8_t t=lo;lo=hi;hi=t;}
  uint16_t span=hi-lo;
  uint16_t out=lo+(uint32_t)n*span/255u;
  return out>255?255:(uint8_t)out;
}

void setWidth(uint8_t n){widthNorm=n;DmxSimple.write(6,mapWidth(n));}
void dmxSet(uint16_t ch,uint8_t v){if(ch>=1&&ch<=512)DmxSimple.write(ch,v);}
void dmxFill(uint8_t v){for(int i=1;i<=512;i++)DmxSimple.write(i,v);}

// ===================== СЦЕНЫ =====================

// --- Scene 1 (static) ---
void runScene1(){
  dmxSet(2, 60); setWidth(255); dmxSet(4, 10);
  dmxSet(1, 255); dmxSet(50, 255); dmxSet(40, 100);
}

// --- Scene 2 (5 сек) ---
void runScene2(){
  if (millis() - lastSceneSwitch > 1500) { scenePhase ^= 1; lastSceneSwitch = millis(); }
  if(scenePhase==0){ dmxSet(2,60); setWidth(255); dmxSet(4,10); dmxSet(1,255); dmxSet(50,255); dmxSet(40,0); }
  else{ dmxSet(2,0); setWidth(0); dmxSet(1,0); dmxSet(50,0); dmxSet(40,100); }
}

// --- Scene 3 (1.5 сек, другая яркость) ---
void runScene3(){
  if (millis() - lastSceneSwitch > 1500) { scenePhase ^= 1; lastSceneSwitch = millis(); }
  if(scenePhase==0){ dmxSet(2,60); setWidth(255); dmxSet(4,10); dmxSet(1,255); dmxSet(50,255); dmxSet(40,0); }
  else{ dmxSet(2,0); setWidth(0); dmxSet(1,0); dmxSet(50,0); dmxSet(40,120); }
}

// --- Scene 4 = Scene 2 ---
void runScene4(){ 
  if (millis() - lastSceneSwitch > 1000) { scenePhase ^= 1; lastSceneSwitch = millis(); }
  if(scenePhase==0){ dmxSet(2,60); setWidth(255); dmxSet(4,10); dmxSet(1,255); dmxSet(50,255); dmxSet(40,0); }
  else{ dmxSet(2,0); setWidth(0); dmxSet(1,0); dmxSet(50,255); dmxSet(40,40); }
 }

// --- Scene 5 (1.5 сек, слабее) ---
void runScene5(){
  if (millis() - lastSceneSwitch > 1500) { scenePhase ^= 1; lastSceneSwitch = millis(); }
  if(scenePhase==0){ dmxSet(2,60); setWidth(255); dmxSet(4,10); dmxSet(1,255); dmxSet(50,255); dmxSet(40,0); }
  else{ dmxSet(2,0); setWidth(0); dmxSet(1,0); dmxSet(50,0); dmxSet(40,40); }
}

// --- Scene 6 (1.5 сек, ярче) ---
void runScene6(){
  if (millis() - lastSceneSwitch > 1500) { scenePhase ^= 1; lastSceneSwitch = millis(); }
  if(scenePhase==0){ dmxSet(2,60); setWidth(255); dmxSet(4,10); dmxSet(1,255); dmxSet(50,255); dmxSet(40,0); }
  else{ dmxSet(2,0); setWidth(0); dmxSet(1,0); dmxSet(50,0); dmxSet(40,240); }
}

// --- Scene 7–15 (идентичны Scene 6) ---
void runScene7(){ runScene6(); }
void runScene8(){ runScene6(); }
void runScene9(){ runScene6(); }
void runScene10(){ runScene6(); }
void runScene11(){ runScene6(); }
void runScene12(){ runScene6(); }
void runScene13(){ runScene6(); }
void runScene14(){ runScene6(); }
void runScene15(){ runScene6(); }

// --- Scene switcher ---
void updateScenes(){
  switch(currentScene){
    case SCENE_1: runScene1(); break;
    case SCENE_2: runScene2(); break;
    case SCENE_3: runScene3(); break;
    case SCENE_4: runScene4(); break;
    case SCENE_5: runScene5(); break;
    case SCENE_6: runScene6(); break;
    case SCENE_7: runScene7(); break;
    case SCENE_8: runScene8(); break;
    case SCENE_9: runScene9(); break;
    case SCENE_10: runScene10(); break;
    case SCENE_11: runScene11(); break;
    case SCENE_12: runScene12(); break;
    case SCENE_13: runScene13(); break;
    case SCENE_14: runScene14(); break;
    case SCENE_15: runScene15(); break;
    default: break;
  }
}

// ===================== COMMANDS =====================
void handleLine(const String& ln){
  if(!ln.length())return;
  String cmd=ln; cmd.trim();
  Serial.println(cmd);

  if(cmd.equalsIgnoreCase("OFF")){
    currentScene=SCENE_NONE;
    dmxFill(0);
    Serial.println(F("[OK] OFF"));
    return;
  }

  if(cmd.startsWith("W ")){
    int v=-1; sscanf(cmd.c_str(),"W %d",&v);
    v = constrain(v,0,255);
    setWidth(v);
    Serial.print(F("[OK] W=")); Serial.println(widthNorm);
    return;
  }

  // запуск сцен
  for (int i=1; i<=15; i++) {
    String s = "SCENE " + String(i);
    if (cmd.equalsIgnoreCase(s)) {
      currentScene = (Scene)i;
      scenePhase = 0;
      lastSceneSwitch = millis();
      Serial.print(F("[OK] ")); Serial.println(s + " started");
      return;
    }
  }

  // управление каналом вручную
  uint16_t ch; int val;
  if(2 == sscanf(cmd.c_str(), "%hu %d", &ch, &val)){
    currentScene = SCENE_NONE;
    val = constrain(val, 0, 255);
    if (ch == 6) setWidth(val);
    else dmxSet(ch, val);
    Serial.print(F("[OK] SET CH=")); Serial.print(ch);
    Serial.print(F(" VAL=")); Serial.println(val);
    return;
  }

  Serial.println(F("[ERR] Unknown command"));
}

// ===================== NETWORK =====================
void checkNetwork(){
  Serial.println(F("=== Проверка сети ==="));
  EthernetLinkStatus link = Ethernet.linkStatus();
  if (link == LinkON) Serial.println(F("✅ LINK: активен"));
  else if (link == LinkOFF) Serial.println(F("❌ LINK: нет кабеля"));
  else Serial.println(F("⚠️ LINK: неизвестен"));
  Serial.print(F("IP: ")); Serial.println(Ethernet.localIP());

  EthernetClient test;
  Serial.print(F("Проверка TCP lasers.drawbridge.kz:80 ... "));
  if (test.connect(SERVER_HOST, 80)) {
    Serial.println(F("✅ OK"));
    test.stop();
    http = new EthernetHttpClient(ethClient, SERVER_HOST, 80);
  } else {
    Serial.println(F("❌ fallback"));
    http = new EthernetHttpClient(ethClient, LOCAL_FALLBACK, 80);
  }
}

// ===================== POLL =====================
unsigned long lastPollAt = 0;
void pollServer(){
  String url = "/api/v1/poll?device=" + DEVICE_NAME;
  Serial.println("[HTTP] GET " + url);
  http->get(url);
  int status = http->responseStatusCode();

  if (status == 200) {
    serverOnline = true;
    String body = http->responseBody();
    Serial.println("[HTTP] body=" + body);

    DynamicJsonDocument doc(1024);
    if (deserializeJson(doc, body) == DeserializationError::Ok) {
      if (doc.containsKey("cmd")) {
        String cmd = (const char*)doc["cmd"];
        if (doc.containsKey("args")) { cmd += " "; cmd += (const char*)doc["args"]; }
        handleLine(cmd);
      } else if (doc.containsKey("events")) {
        for (JsonVariant v : doc["events"].as<JsonArray>()) {
          if (v["cmd"]) {
            String cmd = (const char*)v["cmd"];
            if (v["args"]) { cmd += " "; cmd += (const char*)v["args"]; }
            handleLine(cmd);
          }
        }
      }
    }
  } else {
    Serial.print("[HTTP] code="); Serial.println(status);
    if (status < 0) {
      Serial.println(F("❌ Нет соединения с сервером"));
      serverOnline = false;
    }
  }
  http->stop();
}

// ===================== NIGHT MODE =====================
void checkNightMode(DateTime now){
  if (serverOnline) return;
  int h = now.hour();
  bool night = (h >= 19 || h < 4);
  if (night && currentScene != SCENE_2) {
    Serial.println(F("🌙 Сервер недоступен, ночь — SCENE 2"));
    currentScene = SCENE_1;
    scenePhase = 0;
    lastSceneSwitch = millis();
  }
}

// ===================== SERIAL READER =====================
String readline(HardwareSerial &s, uint16_t tout=20){
  String ln; unsigned long t0=millis();
  while(millis()-t0<tout){
    while(s.available()){
      char c=(char)s.read();
      if(c=='\r')continue;
      if(c=='\n'){ln.trim();return ln;}
      ln+=c;
    }
  }
  return ln;
}

// ===================== SETUP / LOOP =====================
void setup(){
  Serial.begin(115200);
  ESP_SERIAL.begin(9600);
  Wire.begin();
  pinMode(53,OUTPUT);digitalWrite(53,HIGH);
  pinMode(ETH_CS,OUTPUT);digitalWrite(ETH_CS,HIGH);

  if(!rtc.begin())Serial.println(F("❌ RTC не найден"));
  else if(rtc.lostPower()){rtc.adjust(DateTime(F(__DATE__),F(__TIME__)));Serial.println(F("RTC восстановлен"));}

  DmxSimple.usePin(DMX_PIN);DmxSimple.maxChannel(512);dmxFill(0);
  Ethernet.init(ETH_CS);Ethernet.begin(mac,ip,dns,gateway,subnet);delay(2000);
  checkNetwork();

  Serial.println(F("=== MEGA DMX Controller ==="));
  Serial.print(F("Device: "));Serial.println(DEVICE_NAME);
  Serial.print(F("IP: "));Serial.println(Ethernet.localIP());
}

void loop(){
  DateTime now = rtc.now();

  String ln1 = readline(ESP_SERIAL, 5); if(ln1.length()) handleLine(ln1);
  String ln2 = readline(Serial, 5);     if(ln2.length()) handleLine(ln2);

  updateScenes();

  if (millis() - lastPollAt > 5000) {
    pollServer();
    lastPollAt = millis();
  }

  checkNightMode(now);

  static unsigned long t=0;
  if(millis()-t>10000){
    Serial.print("RTC ");Serial.print(now.hour());
    Serial.print(":");Serial.print(now.minute());
    Serial.print(":");Serial.println(now.second());
    t=millis();
  }
}
