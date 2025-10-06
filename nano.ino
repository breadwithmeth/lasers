// Arduino Nano — отправка статуса по DMX через MAX485
// Кодирует:
//  CH1 — 0 (OK) или 255 (DEVIATION)
//  CH2 — deviation*100 MSB
//  CH3 — deviation*100 LSB
//  CH4 — heartbeat (бегущая «жизнь» узла)

#include <DmxSimple.h>
#include <string.h>
#include <strings.h>  // для strncasecmp
#include <stdlib.h>   // atof
#include <ctype.h>
#include <stdint.h>

#define DMX_ENABLE_PIN 2     // Пин, объединённый DE+RE MAX485 (высоко = передача)
#define DMX_TX_PIN     3     // DmxSimple по умолчанию на пине 3 (Timer2)

const uint8_t CH_STATE  = 1;
const uint8_t CH_DEVMSB = 2;
const uint8_t CH_DEVLSB = 3;
const uint8_t CH_HEART  = 4;

uint8_t heartbeat = 0;

static inline long roundToCenti(float v) {
  // Избегаем lround; вручную округляем в сотых
  float x = v * 100.0f;
  if (x >= 0) return (long)(x + 0.5f);
  return (long)(x - 0.5f);
}

void setDmxFromStatus(bool deviation, float devVal){
  long centi = roundToCenti(devVal);
  if (centi < 0) centi = 0;
  if (centi > 65535) centi = 65535;

  uint8_t msb = (centi >> 8) & 0xFF;
  uint8_t lsb = (centi      ) & 0xFF;

  DmxSimple.write(CH_STATE, deviation ? 255 : 0);
  DmxSimple.write(CH_DEVMSB, msb);
  DmxSimple.write(CH_DEVLSB, lsb);
  DmxSimple.write(CH_HEART, heartbeat++);
}

bool iequals(const char* a, const char* b){
  while (*a && *b){
    char ca = *a, cb = *b;
    if (ca >= 'a' && ca <= 'z') ca -= 32;
    if (cb >= 'a' && cb <= 'z') cb -= 32;
    if (ca != cb) return false;
    ++a; ++b;
  }
  return *a == 0 && *b == 0;
}

void parseLine(char* line){
  // trim left
  while (*line==' ' || *line=='\t') ++line;
  size_t n = strlen(line);
  // trim right
  while (n>0 && (line[n-1]=='\r' || line[n-1]=='\n' || line[n-1]==' ' || line[n-1]=='\t')) line[--n]=0;

  if (n==0) return;

  // Вариант 1: простые команды
  if (iequals(line, "OK")){
    setDmxFromStatus(false, 0.0f);
    return;
  }
  if (strncasecmp(line, "DEVIATION", 9)==0){
    // "DEVIATION 1.23"
    float v = 0.0f;
    if (n > 10) v = atof(line+9);
    setDmxFromStatus(true, v);
    return;
  }

  // Вариант 2: очень простой JSON-парсер
  bool dev = false;
  const char* ps = strstr(line, "\"state\"");
  if (ps){
    const char* pd = strstr(ps, "DEVIATION");
    const char* po = strstr(ps, "OK");
    if (pd && (!po || pd < po)) dev = true;
  }
  float val = 0.0f;
  const char* pv = strstr(line, "\"deviation\"");
  if (pv){
    const char* pc = strchr(pv, ':');
    if (pc) val = atof(pc+1);
  }
  setDmxFromStatus(dev, val);
}

void setup(){
  pinMode(DMX_ENABLE_PIN, OUTPUT);
  digitalWrite(DMX_ENABLE_PIN, HIGH); // включаем передатчик MAX485

  DmxSimple.usePin(DMX_TX_PIN);
  DmxSimple.maxChannel(8);

  Serial.begin(115200);

  // начальное состояние — OK
  setDmxFromStatus(false, 0.0f);
}

void loop(){
  static char buf[128];
  static size_t len = 0;

  // Чтение строк из Serial
  while (Serial.available()){
    char c = (char)Serial.read();
    if (c=='\n' || c=='\r'){
      if (len){
        buf[len]=0;
        parseLine(buf);
        len=0;
      }
    } else if (len < sizeof(buf)-1){
      buf[len++] = c;
    }
  }

  // Пульс хартбита (обновляется на шину)
  static unsigned long t0=0;
  if (millis() - t0 > 200){
    t0 = millis();
    DmxSimple.write(CH_HEART, heartbeat++);
  }
}
