#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <SoftwareSerial.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <ArduinoJson.h>

// ===================== CONFIG =====================
// ---- Wi-Fi (STA) ----
#define WIFI_SSID "Beeline_307A"
#define WIFI_PASS "D30D307A"

// ---- Long-poll server ----
// Пример: "https://lasers.drawbridge.kz/api/v1/poll"
const char* POLL_BASE_URL = "https://lasers.drawbridge.kz/api/v1/poll";
const int   POLL_WAIT_SECS = 15;         // сколько сервер держит соединение
const bool  POLL_TLS_INSECURE = true;    // true = не проверять сертификат (для быстрых тестов)
const char* AUTH_BEARER = "";            // "Bearer xxx" (оставьте пустым, если не нужно)

// ---- Device identity ----
String DEVICE_ID; // формируется из ChipID в setup()

// ==== UART к Arduino Uno ====
// D5 -> Uno RX(10), D6 <- Uno TX(11)
SoftwareSerial unoSerial(D6, D5); // RX=D6 (GPIO12), TX=D5 (GPIO14)

// ==== UART к модему SIM808 ====
// D7 <- TX SIM808, D8 -> RX SIM808
SoftwareSerial simSerial(D7, D8);

// ==== SIM808 Power Key (софтверный open-drain, БЕЗ транзистора) ====
// WeMos D1 R1: D2 = GPIO4
#define SIM_PWRKEY_PIN       D2
#define SIM_PWRKEY_PULSE_MS  2500

// ==== Состояние/diag ====
String lastCommand = "OFF";
String lastCallNumber = "";
String lastCallStatus = "IDLE";

// ==== Кэш ширины ====
int cachedW   = -1;
int cachedMin = 0;
int cachedMax = 255;

// ---- SIM808 статусы ----
volatile uint32_t lastSimOKAt = 0; // millis() времени последнего OK/ответа

// ---- Long-poll state ----
String g_pollCursor = "";
unsigned long g_lastPollOkAt = 0;
int g_lastHttpCode = 0;
unsigned long g_nextPollAt = 0;
int g_backoffMs = 1000; // экспоненциальный откат при ошибках (до 30с)

// ---------- UCS2 HEX -> UTF-8 ----------
static inline bool isHexDigit(char c){
  return (c>='0'&&c<='9')||(c>='A'&&c<='F')||(c>='a'&&c<='f');
}
bool isLikelyUCS2Hex(const String& s){
  if (s.length()==0 || (s.length()%4)!=0) return false;
  for (size_t i=0;i<s.length();++i) if(!isHexDigit(s[i])) return false;
  return true;
}
uint8_t h2n(char c){ if(c>='0'&&c<='9')return c-'0'; if(c>='A'&&c<='F')return 10+c-'A'; return 10+c-'a'; }
String ucs2hex_to_utf8(const String& hex){
  if (!isLikelyUCS2Hex(hex)) return hex;
  String out; out.reserve(hex.length());
  for (size_t i=0;i<hex.length(); i+=4){
    uint16_t code = (h2n(hex[i])<<12)|(h2n(hex[i+1])<<8)|(h2n(hex[i+2])<<4)|h2n(hex[i+3]);
    if (code<=0x7F){ out += (char)code; }
    else if (code<=0x7FF){
      out += (char)(0xC0 | (code>>6));
      out += (char)(0x80 | (code & 0x3F));
    } else {
      out += (char)(0xE0 | (code>>12));
      out += (char)(0x80 | ((code>>6)&0x3F));
      out += (char)(0x80 | (code & 0x3F));
    }
  }
  return out;
}
String maybeDecodeUCS2(const String& s){
  return isLikelyUCS2Hex(s) ? ucs2hex_to_utf8(s) : s;
}

// ==== UNO I/O helpers ====
void sendToUno(const String& cmd){
  unoSerial.println(cmd);
  Serial.println("[UNO] >> " + cmd);
  lastCommand = cmd;
}

bool sendToUnoAndRead(const String& cmd, String& reply, uint32_t timeoutMs=500){
  while (unoSerial.available()) unoSerial.read();
  unoSerial.println(cmd);
  Serial.println("[UNO] >> " + cmd);
  lastCommand = cmd;

  uint32_t t0 = millis();
  String line;
  while (millis() - t0 < timeoutMs){
    while (unoSerial.available()){
      char c = (char)unoSerial.read();
      if (c=='\r') continue;
      if (c=='\n'){
        line.trim();
        if (line.length()){
          reply = line;
          Serial.println("[UNO] << " + reply);
          return true;
        }
        line = "";
      } else {
        line += c;
      }
    }
    delay(1);
  }
  return false;
}

// разбор строки вида: "W=200 RANGE=[180..240] OUT=xxx"
void parseWReply(const String& rep, int& w, int& mn, int& mx){
  w = -1; mn = 0; mx = 255;
  int p = rep.indexOf("W=");
  if (p >= 0){
    int e = rep.indexOf(' ', p+2);
    String s = (e>p)? rep.substring(p+2, e) : rep.substring(p+2);
    w = s.toInt();
  }
  int r = rep.indexOf("RANGE=[");
  if (r >= 0){
    int d = rep.indexOf("..", r+7);
    int rb = rep.indexOf(']', r+7);
    if (d>0 && rb>0){
      mn = rep.substring(r+7, d).toInt();
      mx = rep.substring(d+2, rb).toInt();
    }
  }
}

// ==== SIM808 power helpers ====
static inline void simPwrkeyWrite(bool pressed){
  if (pressed) {
    // Нажатие: тянем к GND (только OUTPUT LOW)
    pinMode(SIM_PWRKEY_PIN, OUTPUT);
    digitalWrite(SIM_PWRKEY_PIN, LOW);
  } else {
    // Отпускание: Hi-Z, чтобы внутренняя подтяжка SIM808 подняла уровень
    pinMode(SIM_PWRKEY_PIN, INPUT);
  }
}

void sim808_pulse_pwrkey(uint16_t ms = SIM_PWRKEY_PULSE_MS){
  Serial.printf("[SIM] PWRKEY press %u ms\n", (unsigned)ms);
  simPwrkeyWrite(true);
  delay(ms);
  simPwrkeyWrite(false);
}

void sim808_flush_input(uint32_t ms=100){
  uint32_t t0 = millis();
  while (millis()-t0 < ms){ while (simSerial.available()) simSerial.read(); }
}

bool sim808_send_at_wait_ok(const String& cmd, uint32_t waitMs=800){
  simSerial.println(cmd);
  Serial.println("[SIM] << " + cmd);
  uint32_t t0 = millis();
  String line;
  while (millis()-t0 < waitMs){
    while (simSerial.available()){
      char c = (char)simSerial.read();
      if (c=='\r') continue;
      if (c=='\n'){
        line.trim();
        if (line.length()){
          Serial.println("[SIM] >> " + line);
          if (line == "OK") { lastSimOKAt = millis(); return true; }
          if (line.indexOf("NORMAL POWER DOWN")>=0) { lastCallStatus = "ENDED"; }
        }
        line = "";
      } else line += c;
    }
    delay(2);
  }
  return false;
}

bool sim808_is_alive(uint32_t waitMs=1000){
  sim808_flush_input(50);
  return sim808_send_at_wait_ok("AT", waitMs);
}

bool sim808_wait_ready(uint32_t fullTimeoutMs=15000){
  uint32_t t0 = millis();
  String line;
  while (millis() - t0 < fullTimeoutMs){
    if (sim808_send_at_wait_ok("AT", 200)) return true;
    uint32_t t1 = millis();
    while (millis()-t1 < 200){
      while (simSerial.available()){
        char c = (char)simSerial.read();
        if (c=='\r') continue;
        if (c=='\n'){
          line.trim();
          if (line.length()){
            Serial.println("[SIM] >> " + line);
            if (line.indexOf("RDY")>=0 || line.indexOf("SMS Ready")>=0 || line.indexOf("Call Ready")>=0) return true;
          }
          line = "";
        } else line += c;
      }
      delay(2);
    }
  }
  return false;
}

bool sim808_bootstrap(){
  if (sim808_is_alive()) return true;
  sim808_pulse_pwrkey();
  bool ok = sim808_wait_ready();
  if (!ok){
    sim808_pulse_pwrkey(SIM_PWRKEY_PULSE_MS + 700);
    ok = sim808_wait_ready(20000);
  }
  if (ok) lastSimOKAt = millis();
  return ok;
}

// ==== Веб-интерфейс (STA IP) ====
ESP8266WebServer server(80);

String htmlHeader(){
  return F(
"<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>DMX & Call Control</title>"
"<style>"
"body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;margin:20px}"
".card{max-width:560px;padding:16px;border:1px solid #ddd;border-radius:12px;box-shadow:0 1px 3px rgba(0,0,0,.06);margin:14px 0}"
"h2{margin:0 0 10px}"
".row{display:flex;align-items:center;gap:12px;margin:10px 0}"
"input[type=range]{flex:1}"
"input[type=number]{width:90px}"
".val{min-width:40px;text-align:right;font-variant-numeric:tabular-nums}"
"button{padding:6px 10px;border:1px solid #ccc;border-radius:8px;background:#f6f6f6;cursor:pointer}"
".ok{color:#0a0} .err{color:#a00}"
"a{color:#06c;text-decoration:none} a:hover{text-decoration:underline}"
"</style></head><body>"
  );
}
String htmlFooter(){ return F("</body></html>"); }

void handleRoot(){
  String html = htmlHeader();
  html += "<div class='card'><h2>Сеть</h2>";
  html += "<p>Mode: STA · IP: <code>" + WiFi.localIP().toString() + "</code></p>";
  html += "<p>DeviceID: <code>" + DEVICE_ID + "</code></p>";
  html += "<p>Poll URL: <code>" + String(POLL_BASE_URL) + "</code></p>";
  html += "<p>Last poll code: <code>" + String(g_lastHttpCode) + "</code>, okAt=" + String(g_lastPollOkAt) + " ms </p>";
  html += "</div>";

  html += "<div class='card'><h2>DMX Scene Controller</h2>";
  html += "<p>Last DMX command: " + lastCommand + "</p>";
  for (int i=1; i<=20; i++){
    html += "<p><a href=\"/scene" + String(i) + "\">SCENE " + String(i) + "</a></p>";
  }
  html += "<p><a href=\"/off\">OFF</a></p>";

  html += "<details><summary>Параметры сцен</summary>";
  html += "<ul>"
          "<li>SCENE 9 шаг 3с: <code>/scene9?q=3000</code></li>"
          "<li>SCENE 12 width step 200 мс: <code>/scene12?q=200</code></li>"
          "<li>SCENE 13 periods: <code>/scene13?q=300 500 80</code></li>"
          "<li>SCENE 14: <code>/scene14?q=180 60 8</code></li>"
          "<li>SCENE 15: <code>/scene15?q=120 400</code></li>"
          "<li>SCENE 16: <code>/scene16?q=600</code></li>"
          "<li>SCENE 17: <code>/scene17?q=700</code></li>"
          "<li>SCENE 18: <code>/scene18?q=120 1000 120</code></li>"
          "<li>SCENE 19: <code>/scene19?q=250 900</code></li>"
          "<li>SCENE 20 BPM: <code>/scene20?q=128</code></li>"
          "</ul></details></div>";

  // Блок ширины
  html += "<div class='card'><h2>Ширина луча (канал 6)</h2>"
          "<div class='row'><input id='rng' type='range' min='0' max='255' step='1'>"
          "<div class='val'><span id='val'>-</span></div>"
          "<button id='btnSet'>Установить</button></div>"
          "<div class='row'>"
          "Мин: <input id='min' type='number' min='0' max='255' step='1'>"
          "Макс: <input id='max' type='number' min='0' max='255' step='1'>"
          "<button id='btnLim'>Применить лимиты</button>"
          "</div>"
          "<div id='st' class='row'></div>"
          "<p><small>API: <code>/api/width</code>, <code>/api/width?val=NN</code>, <code>/api/width/limits?min=AA&max=BB</code></small></p>"
          "</div>";

  // Блок SIM808
  html += "<div class='card'><h2>SIM808 Power & Call</h2>";
  html += "<p>SIM status: <code>" + String(lastSimOKAt ? "alive? (ping /sim/ping)" : "unknown") + "</code></p>";
  html += "<p><a href=\"/sim/on\">⚡ Включить (PWRKEY)</a> · <a href=\"/sim/off\">⏻ Выкл (toggle)</a> · <a href=\"/sim/ping\">Ping</a></p>";

  html += "<p>Last call: " + (lastCallNumber.length()? lastCallNumber : String("-")) + " | Status: " + lastCallStatus + "</p>";
  html += "<p><a href=\"/call?num=%2B77081541739\">📞 Позвонить на +77081541739</a></p>";
  html += "<form action='/call' method='get'>"
          "<input type='text' name='num' placeholder='+770XXXXXXXXX' style='width:220px'>"
          "<button type='submit'>Позвонить</button></form>";
  html += "<p><a href=\"/hang\">🛑 Положить трубку</a></p></div>";

  // JS
  html += F(
"<script>"
"async function loadW(){"
"  try{ let r=await fetch('/api/width'); let j=await r.json();"
"    if('width'in j){ rng.value=j.width; document.getElementById('val').textContent=j.width; }"
"    if('min'in j){ document.getElementById('min').value=j.min; }"
"    if('max'in j){ document.getElementById('max').value=j.max; }"
"  }catch(e){}"
"}"
"rng&&rng.addEventListener('input',()=>{ document.getElementById('val').textContent=rng.value; });"
"btnSet&&btnSet.addEventListener('click',async()=>{"
"  st.textContent='...'; st.className='';"
"  try{ let r=await fetch('/api/width?val='+rng.value); let j=await r.json();"
"    if(j.ok){ st.textContent='OK width='+j.width+' ['+j.min+'..'+j.max+']'; st.className='ok'; }"
"    else{ st.textContent='Ошибка'; st.className='err'; }"
"  }catch(e){ st.textContent='Нет связи'; st.className='err'; }"
"});"
"btnLim&&btnLim.addEventListener('click',async()=>{"
"  st.textContent='...'; st.className='';"
"  try{ let mn=min.value, mx=max.value; "
"       let r=await fetch('/api/width/limits?min='+mn+'&max='+mx); let j=await r.json();"
"       if(j.ok){ st.textContent='Лимиты применены: ['+j.min+'..'+j.max+'] width='+j.width; st.className='ok'; }"
"       else{ st.textContent='Ошибка'; st.className='err'; }"
"  }catch(e){ st.textContent='Нет связи'; st.className='err'; }"
"});"
"loadW();"
"</script>"
  );

  html += htmlFooter();
  server.send(200, "text/html", html);
}

void handleScene(int n){
  String cmd = "SCENE " + String(n);
  String extras;
  if (server.hasArg("q")) {
    extras = server.arg("q");
  } else {
    String tmp;
    if (server.hasArg("ms"))  { tmp += server.arg("ms");  tmp += ' '; }
    if (server.hasArg("a"))   { tmp += server.arg("a");   tmp += ' '; }
    if (server.hasArg("b"))   { tmp += server.arg("b");   tmp += ' '; }
    if (server.hasArg("c"))   { tmp += server.arg("c");   tmp += ' '; }
    if (server.hasArg("bpm")) { tmp += server.arg("bpm"); tmp += ' '; }
    tmp.trim();
    extras = tmp;
  }
  if (extras.length()) cmd += " " + extras;

  sendToUno(cmd);
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleOff(){ sendToUno("OFF"); server.sendHeader("Location", "/"); server.send(303); }

// ---- WIDTH API ----
void handleWidthAPI(){
  String rep;
  bool ok;
  if (server.hasArg("val")) {
    int v = server.arg("val").toInt();
    if (v<0) v=0; if (v>255) v=255;
    ok = sendToUnoAndRead(String("W ")+v, rep, 500);
  } else {
    ok = sendToUnoAndRead("GET W", rep, 500);
  }

  int w, mn, mx;
  if (ok) {
    parseWReply(rep, w, mn, mx);
    if (w>=0) cachedW = w;
    cachedMin = mn; cachedMax = mx;
    String out = String("{\"ok\":true,\"width\":") + (cachedW>=0?cachedW:0) +
                 ",\"min\":" + cachedMin + ",\"max\":" + cachedMax +
                 ",\"raw\":\"" + rep + "\"}";
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", out);
  } else {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(504, "application/json", "{\"ok\":false,\"error\":\"uno_timeout\"}");
  }
}

void handleWidthLimits(){
  String rep;
  if (server.hasArg("min")) { sendToUnoAndRead(String("WMIN ") + server.arg("min"), rep, 300); }
  if (server.hasArg("max")) { sendToUnoAndRead(String("WMAX ") + server.arg("max"), rep, 300); }
  bool ok = sendToUnoAndRead("GET W", rep, 500);
  int w, mn, mx; parseWReply(rep, w, mn, mx);
  if (w>=0) cachedW = w; cachedMin = mn; cachedMax = mx;
  String out = String("{\"ok\":") + (ok?"true":"false") +
               ",\"width\":" + (cachedW>=0?cachedW:0) +
               ",\"min\":" + cachedMin + ",\"max\":" + cachedMax +
               ",\"raw\":\"" + rep + "\"}";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(ok?200:504, "application/json", out);
}

// Шорткаты
void handleW(){ if (server.hasArg("val")) { String r; sendToUnoAndRead(String("W ")+server.arg("val"), r, 400); } server.sendHeader("Location","/"); server.send(303); }
void handleWmin(){ if (server.hasArg("val")) { String r; sendToUnoAndRead(String("WMIN ")+server.arg("val"), r, 300); } server.sendHeader("Location","/"); server.send(303); }
void handleWmax(){ if (server.hasArg("val")) { String r; sendToUnoAndRead(String("WMAX ")+server.arg("val"), r, 300); } server.sendHeader("Location","/"); server.send(303); }

// [CALL] Телефонные вызовы через SIM808
void startCall(const String& number){
  lastCallNumber = number;
  lastCallStatus = "DIALING";
  simSerial.print("ATD"); simSerial.print(number); simSerial.println(";");
  Serial.println("[CALL] DIAL " + number);
}
void hangupCall(){ simSerial.println("ATH"); Serial.println("[CALL] HANGUP"); lastCallStatus = "ENDED"; }

void handleCall(){
  String num = server.hasArg("num") ? server.arg("num") : String("+77081541739");
  num.trim();
  if (num.length() == 0){ server.send(400, "text/plain", "Number is empty"); return; }
  String normalized = num; normalized.replace(" ", "");
  if (normalized[0] != '+'){ if (isdigit(normalized[0])) normalized = "+" + normalized; }
  startCall(normalized);
  server.sendHeader("Location", "/"); server.send(303);
}
void handleHang(){ hangupCall(); server.sendHeader("Location","/"); server.send(303); }

// ==== SIM808 web handlers ====
void handleSimOn(){ bool ok = sim808_bootstrap(); server.send(200, "text/plain", ok?"SIM808 ON (AT ok)":"SIM808: no response"); }
void handleSimOff(){ sim808_pulse_pwrkey(); server.send(200, "text/plain", "SIM808 toggled (power off/on pulse)"); }
void handleSimPing(){ bool ok = sim808_is_alive(); server.send(200, "application/json", ok?"{\"ok\":true}":"{\"ok\":false}"); }
void handleSimPulse(){ int ms = server.hasArg("ms") ? server.arg("ms").toInt() : SIM_PWRKEY_PULSE_MS; if (ms<200) ms=200; if (ms>5000) ms=5000; sim808_pulse_pwrkey(ms); server.send(200, "text/plain", "PWRKEY pulsed"); }

// ==== Проверка SMS/URC ====
void checkSMS(){
  while (simSerial.available()){
    String line = simSerial.readStringUntil('\n'); line.trim();
    if (line.length() == 0) continue;

    // статусы звонка
    if (line.indexOf("BUSY") >= 0 || line.indexOf("NO CARRIER") >= 0){ lastCallStatus = "ENDED"; }
    else if (line.indexOf("NO DIALTONE") >= 0 || line.indexOf("NO ANSWER") >= 0){ lastCallStatus = "ENDED"; }
    else if (line.indexOf("VOICE CALL: BEGIN") >= 0){ lastCallStatus = "ACTIVE"; }
    else if (line.indexOf("VOICE CALL: END") >= 0){ lastCallStatus = "ENDED"; }
    else if (line.indexOf("RDY")>=0 || line.indexOf("SMS Ready")>=0 || line.indexOf("Call Ready")>=0){ lastSimOKAt = millis(); }

    if (line.startsWith("+CMT:")){
      String body = simSerial.readStringUntil('\n'); body.trim();
      String decoded = maybeDecodeUCS2(body);
      Serial.println("[SMS RAW] " + body);
      Serial.println("[SMS TXT] " + decoded);

      String up = decoded; up.trim(); up.toUpperCase();
      if (up == "OFF") sendToUno("OFF");
      else if (up.startsWith("SCENE")) sendToUno(up);
      else if (up.startsWith("WMIN ")) { String v = up.substring(5); v.trim(); String r; sendToUnoAndRead("WMIN "+v, r, 300); }
      else if (up.startsWith("WMAX ")) { String v = up.substring(5); v.trim(); String r; sendToUnoAndRead("WMAX "+v, r, 300); }
      else if (up.startsWith("W "))    { String v = up.substring(2); v.trim(); String r; sendToUnoAndRead("W "+v, r, 400); }
      else if (up == "GET W" || up == "W?") { String r; sendToUnoAndRead("GET W", r, 400); }
      else if (up.startsWith("CALL ")) { String smsNum = up.substring(5); smsNum.trim(); if (smsNum.length()){ if (smsNum[0] != '+') smsNum = "+" + smsNum; startCall(smsNum); } }
      else if (up == "HANG" || up == "HANGUP") hangupCall();
      else if (up == "SIM ON")  sim808_bootstrap();
      else if (up == "SIM OFF") sim808_pulse_pwrkey();
    }
  }
}

// ===================== Wi-Fi helpers =====================
void wifiConnectIfNeeded(){
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.print("[WiFi] Connecting to "); Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000){
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED){
    Serial.print("[WiFi] Connected. IP: "); Serial.println(WiFi.localIP());
  } else {
    Serial.println("[WiFi] Failed. Will retry in loop.");
  }
}

// ===================== Long-poll =====================
String urlEncode(const String& s){
  String out; out.reserve(s.length()*3);
  const char *hex="0123456789ABCDEF";
  for (size_t i=0;i<s.length();++i){
    char c=s[i];
    if (isalnum(c) || c=='-'||c=='_'||c=='.'||c=='~') out += c;
    else if (c==' ') out += "%20";
    else { out += '%'; out += hex[(c>>4)&0xF]; out += hex[c&0xF]; }
  }
  return out;
}

void applyEvent(JsonVariant ev){
  if (!ev.is<JsonObject>()) return;
  JsonObject o = ev.as<JsonObject>();
  String cmd = o.containsKey("cmd") ? String((const char*)o["cmd"]) : String();
  cmd.trim(); cmd.toUpperCase();

  if (cmd == "OFF"){
    sendToUno("OFF");
  } else if (cmd == "SCENE"){
    String args = o.containsKey("args") ? String((const char*)o["args"]) : String();
    args.trim();
    String full = "SCENE";
    if (args.length()) { full += " "; full += args; }
    sendToUno(full);
  } else if (cmd == "UNO"){
    String raw = o.containsKey("raw") ? String((const char*)o["raw"]) : String();
    raw.trim(); if (raw.length()) sendToUno(raw);
  } else if (cmd == "W" || cmd == "WIDTH"){
    int v = o.containsKey("val") ? (int)o["val"] : -1;
    if (v>=0){ v = constrain(v, 0, 255); String rep; sendToUnoAndRead(String("W ")+v, rep, 500); }
  } else if (cmd == "WMIN"){
    if (o.containsKey("val")){ String rep; sendToUnoAndRead(String("WMIN ")+String((int)o["val"]), rep, 300); }
  } else if (cmd == "WMAX"){
    if (o.containsKey("val")){ String rep; sendToUnoAndRead(String("WMAX ")+String((int)o["val"]), rep, 300); }
  } else if (cmd == "CALL"){
    String num = o.containsKey("num") ? String((const char*)o["num"]) : String();
    num.trim(); if (num.length()){ if (num[0] != '+') num = "+" + num; startCall(num); }
  } else if (cmd == "HANG"){
    hangupCall();
  } else if (cmd == "SIM_ON"){
    sim808_bootstrap();
  } else if (cmd == "SIM_TOGGLE"){
    int ms = o.containsKey("pulse_ms") ? (int)o["pulse_ms"] : SIM_PWRKEY_PULSE_MS;
    ms = constrain(ms, 200, 5000);
    sim808_pulse_pwrkey(ms);
  }
}

bool processPollPayload(const String& body){
  // Поддерживаем 3 формы:
  // 1) {"events":[...], "cursor":"..."}
  // 2) [ {...}, {...} ]
  // 3) { "cmd":"...", ... }
  DynamicJsonDocument doc(4096);
  DeserializationError e = deserializeJson(doc, body);
  if (e){
    Serial.print("[POLL] JSON parse error: "); Serial.println(e.f_str());
    return false;
  }
  if (doc.is<JsonObject>()){
    JsonObject o = doc.as<JsonObject>();
    if (o.containsKey("events")){
      JsonArray arr = o["events"].as<JsonArray>();
      for (JsonVariant v: arr) applyEvent(v);
    } else if (o.containsKey("cmd")){
      applyEvent(o);
    }
    if (o.containsKey("cursor")) g_pollCursor = String((const char*)o["cursor"]);
  } else if (doc.is<JsonArray>()){
    JsonArray arr = doc.as<JsonArray>();
    for (JsonVariant v: arr) applyEvent(v);
  }
  return true;
}

bool longPollOnce(){
  if (WiFi.status() != WL_CONNECTED) return false;

  // Сборка URL
  String url = String(POLL_BASE_URL);
  url += "?device=" + urlEncode(DEVICE_ID);
  if (g_pollCursor.length()) url += "&cursor=" + urlEncode(g_pollCursor);
  url += "&wait=" + String(POLL_WAIT_SECS);

  std::unique_ptr<BearSSL::WiFiClientSecure> httpsClient(new BearSSL::WiFiClientSecure);
  WiFiClient *clientPtr = nullptr;
  if (String(POLL_BASE_URL).startsWith("https://")){
    if (POLL_TLS_INSECURE) httpsClient->setInsecure();
    httpsClient->setTimeout((POLL_WAIT_SECS + 5) * 1000);
    clientPtr = httpsClient.get();
  } else {
    clientPtr = new WiFiClient();
    ((WiFiClient*)clientPtr)->setTimeout((POLL_WAIT_SECS + 5) * 1000);
  }

  HTTPClient http;
  http.setReuse(true);
  http.setTimeout((POLL_WAIT_SECS + 5) * 1000);
  Serial.println("[POLL] GET " + url);

  bool began = http.begin(*clientPtr, url);
  if (!began){
    Serial.println("[POLL] http.begin() failed");
    if (!String(POLL_BASE_URL).startsWith("https://")) delete clientPtr;
    return false;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("X-Device", DEVICE_ID);
  if (AUTH_BEARER && strlen(AUTH_BEARER)>0){
    http.addHeader("Authorization", String("Bearer ") + AUTH_BEARER);
  }

  int code = http.GET();
  g_lastHttpCode = code;

  bool ok = false;
  if (code > 0){
    if (code == HTTP_CODE_OK){
      String payload = http.getString();
      Serial.printf("[POLL] 200 bytes=%u\n", (unsigned)payload.length());
      ok = processPollPayload(payload);
      if (ok) g_lastPollOkAt = millis();
    } else if (code == HTTP_CODE_NO_CONTENT){
      Serial.println("[POLL] 204 (no content)");
      ok = true; // соединение ок, просто без событий
      g_lastPollOkAt = millis();
    } else {
      Serial.printf("[POLL] HTTP %d\n", code);
    }
  } else {
    Serial.printf("[POLL] http error: %s\n", http.errorToString(code).c_str());
  }

  http.end();
  if (!String(POLL_BASE_URL).startsWith("https://")) delete clientPtr;
  return ok;
}

void longPollTick(){
  unsigned long now = millis();
  if (now < g_nextPollAt) return;

  if (WiFi.status() != WL_CONNECTED){
    wifiConnectIfNeeded();
    g_nextPollAt = millis() + 1000;
    return;
  }

  bool ok = longPollOnce();
  if (ok){
    g_backoffMs = 1000;
    g_nextPollAt = millis() + 50; // почти сразу следующий запрос
  } else {
    g_backoffMs = min(g_backoffMs * 2, 30000);
    g_nextPollAt = millis() + g_backoffMs;
  }
}

// ===================== setup/loop =====================
void setup(){
  Serial.begin(115200);
  unoSerial.begin(9600);   // должна совпадать с UNO
  simSerial.begin(9600);     // SIM808 обычно 9600 бод

  pinMode(SIM_PWRKEY_PIN, INPUT); // Hi-Z

  // Формируем идентификатор устройства
  DEVICE_ID = "wemos-" + String(ESP.getChipId(), HEX);

  // Wi-Fi STA
  WiFi.mode(WIFI_STA);
  WiFi.hostname(DEVICE_ID);
  wifiConnectIfNeeded();

  // Веб-маршруты (на STA IP)
  server.on("/", handleRoot);
  server.on("/off", handleOff);
  for (int i=1;i<=20;i++){
    String path = "/scene" + String(i);
    server.on(path.c_str(), [i](){ handleScene(i); });
  }
  // Width API
  server.on("/api/width", HTTP_GET, handleWidthAPI);
  server.on("/api/width/limits", HTTP_GET, handleWidthLimits);
  server.on("/w", HTTP_GET, handleW);
  server.on("/wmin", HTTP_GET, handleWmin);
  server.on("/wmax", HTTP_GET, handleWmax);

  // CALL
  server.on("/call", handleCall);
  server.on("/hang", handleHang);

  // SIM808 power/ping endpoints
  server.on("/sim/on", HTTP_GET, handleSimOn);
  server.on("/sim/off", HTTP_GET, handleSimOff);
  server.on("/sim/pulse", HTTP_GET, handleSimPulse);
  server.on("/sim/ping", HTTP_GET, handleSimPing);

  server.begin();
  Serial.print("[HTTP] Server on STA IP: "); Serial.println(WiFi.localIP());

  // Старт SIM808
  delay(500);
  bool simOK = sim808_bootstrap();

  // Базовая инициализация модема
  auto sendAT = [&](const String& cmd){
    simSerial.println(cmd);
    Serial.println("[SIM] << " + cmd);
    delay(250);
    while (simSerial.available()){
      String line = simSerial.readStringUntil('\n'); line.trim();
      if (line.length()) Serial.println("[SIM] >> " + line);
    }
  };
  if (simOK){
    sendAT("ATE0");
    sendAT("AT+CMGF=1");
    sendAT("AT+CNMI=2,2,0,0,0");
    sendAT("AT+CLVL=90");
    sendAT("AT+CRSL=90");
    sendAT("AT+CMIC=0,12");
  } else {
    Serial.println("[SIM] not responding after bootstrap");
  }

  Serial.println("Ready (STA + long-poll).");
}

void loop(){
  server.handleClient();
  checkSMS();
  longPollTick();
}
