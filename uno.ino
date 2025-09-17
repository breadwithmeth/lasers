#include <DmxSimple.h>
#include <SoftwareSerial.h>

// ---- UART к ESP ----
static const uint8_t ESP_RX = 10; // Uno RX  <- ESP TX (D1 R1)
static const uint8_t ESP_TX = 11; // Uno TX  -> ESP RX
SoftwareSerial bridge(ESP_RX, ESP_TX); // RX, TX

// ---- DMX ----
static const uint8_t DMX_PIN = 3;

// ---- Сцены ----
enum Scene {
  SCENE_NONE=0, SCENE_1, SCENE_2, SCENE_3, SCENE_4,
  SCENE_5, SCENE_6, SCENE_7, SCENE_8, SCENE_9, SCENE_10, SCENE_11,
  SCENE_12, SCENE_13, SCENE_14, SCENE_15, SCENE_16, SCENE_17, SCENE_18, SCENE_19, SCENE_20
};

Scene currentScene = SCENE_NONE;
unsigned long sceneStart = 0;
uint8_t scenePhase = 0; // номер фазы

// ---- Глобальная регулировка ширины (канал 6) ----
uint8_t widthMin  = 0;    // нижняя граница
uint8_t widthMax  = 255;  // верхняя граница
uint8_t widthNorm = 255;  // текущее «нормализованное» значение 0..255

// Преобразование нормализованного значения в реальный выход для канала 6
static inline uint8_t mapWidth(uint8_t n){
  if (n == 0) return 0; // ноль — всегда ноль
  uint8_t lo = widthMin, hi = widthMax;
  if (hi < lo) { uint8_t t = lo; lo = hi; hi = t; }
  uint16_t span = (uint16_t)hi - (uint16_t)lo;
  uint16_t out  = (uint16_t)lo + (uint16_t)((uint32_t)n * span / 255u);
  if (out > 255) out = 255;
  return (uint8_t)out;
}
static inline void setWidth(uint8_t normalized){ widthNorm = normalized; DmxSimple.write(6, mapWidth(normalized)); }


// ---- SCENE 5: фикс. каналы + дыхание 2 + мигание 40/50 ----
uint16_t scene5Blink40Ms = 1000;   // 40 мигает раз в 1s
uint16_t scene5Blink50Ms = 2000;   // 50 мигает раз в 2s

uint8_t  scene5C2Min = 100;        // пределы канала 2
uint8_t  scene5C2Max = 200;
uint8_t  scene5C2Val = 100;        // текущее значение канала 2
int8_t   scene5C2Dir = 1;          // 1 — вверх, -1 — вниз
uint8_t  scene5C2Step = 1;         // шаг «дыхания» канала 2
uint16_t scene5C2StepMs = 9000;      // период шага «дыхания» (настройка «скорости»)

unsigned long scene5T0C2  = 0;     // таймер дыхания канала 2
unsigned long scene5T0B40 = 0;     // таймер мигания 40
unsigned long scene5T0B50 = 0;     // таймер мигания 50
bool scene5On40 = false;
bool scene5On50 = false;




// ---- Параметры ----
uint8_t scene4Width = 100; // нормализованная ширина для SCENE 4 (0..255)

// ---- Настройки SCENE 9/10/11 ----
uint16_t scene9StepMs  = 2000;
uint16_t scene10StepMs = 1000;
uint16_t scene11StepMs = 1000;

// ---- Настройки SCENE 12 ("дыхание луча" + акценты 40/50) ----
uint8_t  scene12WidthMin   = 230;
uint8_t  scene12WidthMax   = 240;
uint8_t  scene12WidthVal   = 240; // нормализованно!
int8_t   scene12WidthDir   = 1;
uint8_t  scene12WidthStep  = 2;

uint16_t scene12WidthStepMs   = 2000;
uint16_t scene12AccentStepMs  = 20000;

uint8_t  scene12AccentPhase = 0;
unsigned long scene12T0Width  = 0;
unsigned long scene12T0Accent = 0;

// ---- SCENE 13 ----
uint16_t scene13Ams = 3000;
uint16_t scene13Bms = 500;
uint16_t scene13Wms = 80;
uint8_t  scene13Wmin = 180, scene13Wmax = 240, scene13Wval = 60, scene13Wstep = 2; // нормализованно
int8_t   scene13Wdir = 1;
unsigned long scene13T0A=0, scene13T0B=0, scene13T0W=0;
bool scene13A=false, scene13B=false;

// ---- SCENE 14 ----
uint16_t scene14AvgMs = 180;
uint8_t  scene14FlashMs = 60;
uint8_t  scene14EveryN  = 8;
unsigned long scene14T0 = 0, scene14NextDelta = 180, scene14FlashT0 = 0;
bool scene14Flashing=false;
uint8_t scene14UpdCnt=0;

// ---- SCENE 15 ----
uint16_t scene15StepMs   = 1000;
uint16_t scene15StrobeMs = 4000;
unsigned long scene15T0=0, scene15T1=0;
bool scene15St40=false, scene15St50=false;

// ---- SCENE 16 ----
uint16_t scene16StepMs = 1500;
bool scene16Toggle=false;
uint8_t scene16Color = 120;
unsigned long scene16T0=0;

// ---- SCENE 17 ----
uint16_t scene17StepMs = 2000;

// ---- SCENE 18 ----
uint16_t scene18ColorMs = 5000;
uint8_t  scene18ColorStep = 1;
uint16_t scene18PulsePeriodMs = 10000;
uint16_t scene18PulseWidthMs  = 120;
unsigned long scene18TColor=0, scene18TPulse=0, scene18TPulseEnd=0;
bool scene18PulseOn=false;
bool scene18Which40=true;

// ---- SCENE 19 ----
uint16_t scene19OneMs = 1000;
uint16_t scene19MonoMs = 2000;
unsigned long scene19T1=0, scene19TMono=0;
bool scene19One=false;
bool scene19Which=false;

// ---- SCENE 20 ----
uint16_t scene20StepMs = 1500;
unsigned long scene20T0=0;

// --- DMX helpers ---
void dmxSet(uint16_t ch, uint8_t val){
  if (ch<1 || ch>512) return;
  DmxSimple.write(ch, val);
}
void dmxFill(uint8_t v){ for (int i=1;i<=512;i++) DmxSimple.write(i, v); }

String readline(Stream& s, uint16_t tout=20){
  String ln; unsigned long t0=millis();
  while (millis()-t0 < tout){
    while (s.available()){
      char c=(char)s.read();
      if (c=='\r') continue;
      if (c=='\n'){ ln.trim(); return ln; }
      ln+=c;
    }
  }
  return ln;
}

// ---------- Реализация сцен ----------
// SCENE 1
void runScene1Static(){
  dmxSet(2, 60);
  setWidth(255);
  dmxSet(4, 10);
  dmxSet(1, 255);
  dmxSet(50, 255);
  dmxSet(40, 100);
}

// SCENE 2
void runScene2Phase0(){ dmxSet(2,60); setWidth(255); dmxSet(4,10); dmxSet(1,255); dmxSet(50,255); dmxSet(40,0); }
void runScene2Phase1(){ dmxSet(2,0);  setWidth(0);   dmxSet(1,0);  dmxSet(50,0);   dmxSet(40,100); }

// SCENE 3
void runScene3PhaseA(){ dmxSet(2,60); setWidth(255); dmxSet(4,10); dmxSet(1,255); dmxSet(50,255); dmxSet(40,0); }
void runScene3PhaseB(){ dmxSet(2,0);  setWidth(0);   dmxSet(1,0);  dmxSet(50,0);   dmxSet(40,140); }

// SCENE 4 (ширина задаётся scene4Width, нормализованно)
void runScene4PhaseA(){
  setWidth(scene4Width);
  dmxSet(2,60); dmxSet(4,10); dmxSet(1,200); dmxSet(50,255); dmxSet(40,100);
}
void runScene4PhaseB(){
  setWidth(scene4Width);
  dmxSet(2,60); dmxSet(4,10); dmxSet(1,200); dmxSet(50,255); dmxSet(40,140);
}
// SCENE 5: 5=110, 4=10, 2 «дышит» 100..200, 40/50 мигают
void runScene5Base(){
  dmxSet(5, 110);              // фикс
  dmxSet(4, 10);    
  dmxSet(1, 100);           // фикс

  // канал 2 — старт с минимума
  scene5C2Val = scene5C2Min;
  scene5C2Dir = 1;
  dmxSet(2, scene5C2Val);

  // 40/50 — старт выключены
  scene5On40 = false; dmxSet(40, 0);
  scene5On50 = false; dmxSet(50, 0);
}


// SCENE 6
void runScene6Phase0(){ setWidth(255); dmxSet(2,60); dmxSet(4,10); dmxSet(1,200); dmxSet(50,255); }
void runScene6Phase1(){ setWidth(0);   dmxSet(2,0);  dmxSet(4,0);  dmxSet(1,0);   dmxSet(50,0);   dmxSet(40,100); }
void runScene6Phase2(){ setWidth(255); dmxSet(2,60); dmxSet(4,10); dmxSet(1,200); dmxSet(50,255); }
void runScene6Phase3(){ setWidth(0);   dmxSet(2,0);  dmxSet(4,0);  dmxSet(1,0);   dmxSet(50,0);   dmxSet(40,60);  }
void runScene6Phase4(){ setWidth(255); dmxSet(2,60); dmxSet(4,10); dmxSet(1,200); dmxSet(50,255); }
void runScene6Phase5(){ setWidth(0);   dmxSet(2,0);  dmxSet(4,0);  dmxSet(1,0);   dmxSet(50,0);   dmxSet(40,140); }

// SCENE 7
void runScene7Static(){
  dmxSet(2, 60);
  setWidth(255);
  dmxSet(4, 10);
  dmxSet(1, 255);
  dmxSet(50, 255);
  dmxSet(40, 140);
}

// SCENE 8
void runScene8Phase0(){ setWidth(255); dmxSet(2,60); dmxSet(4,10); dmxSet(1,200); dmxSet(50,255); }
void runScene8Phase1(){ dmxSet(40,160); }
void runScene8Phase2(){ setWidth(255); dmxSet(2,60); dmxSet(4,10); dmxSet(1,200); dmxSet(50,255); }
void runScene8Phase3(){ dmxSet(40,200); }
void runScene8Phase4(){ setWidth(255); dmxSet(2,60); dmxSet(4,10); dmxSet(1,200); dmxSet(50,255); }
void runScene8Phase5(){ dmxSet(40,250); }

// SCENE 9
void runScene9Phase0(){ dmxSet(4,10); setWidth(200); dmxSet(2,125); dmxSet(1,255); dmxSet(40,0); dmxSet(50,0); }
void runScene9Phase1(){ dmxSet(40,100); dmxSet(1,0); }
void runScene9Phase2(){ dmxSet(50,255); }
void runScene9Phase3(){ dmxSet(40,0); }
void runScene9Phase4(){ dmxSet(1,0); dmxSet(2,100); dmxSet(40,0); dmxSet(50,255); }

// SCENE 10
void runScene10Phase0(){ dmxSet(4,10); setWidth(200); dmxSet(2,115); dmxSet(1,255); dmxSet(40,0); dmxSet(50,0); }
void runScene10Phase1(){ dmxSet(2,120); dmxSet(40,255); }
void runScene10Phase2(){ dmxSet(2,125); dmxSet(50,255); }
void runScene10Phase3(){ dmxSet(2,130); dmxSet(50,0); }
void runScene10Phase4(){ dmxSet(2,135); dmxSet(40,0); }

// SCENE 11
void runScene11Base(){ dmxSet(4,10); setWidth(200); dmxSet(2,125); dmxSet(1,255); }
void runScene11Phase0(){ dmxSet(50,100); dmxSet(40,0); }
void runScene11Phase1(){ dmxSet(40,100); }
void runScene11Phase2(){ dmxSet(50,0); }

// SCENE 12
void runScene12Base(){
  dmxSet(4,10);
  dmxSet(1,230);
  dmxSet(2,120);
}
static inline void scene12ApplyWidth(){
  setWidth(scene12WidthVal);
}
static inline void scene12ApplyAccent(uint8_t ph){
  switch (ph & 0x03){
    case 0: dmxSet(40, 0);   dmxSet(50, 0);   break;
    case 1: dmxSet(40, 255); dmxSet(50, 0);   break;
    case 2: dmxSet(40, 255); dmxSet(50, 255); break;
    case 3: dmxSet(40, 255); dmxSet(50, 0);   break;
  }
}

// SCENE 13
void runScene13Base(){
  dmxSet(4,10);
  dmxSet(1,255);
  dmxSet(2,120);
  dmxSet(40,0); dmxSet(50,0);
  setWidth(scene13Wval);
}

// SCENE 14
void runScene14Base(){
  dmxSet(4,10);
  dmxSet(1,200);
  dmxSet(2,120);
  dmxSet(40,0); dmxSet(50,0);
}

// SCENE 15
void runScene15Base(){
  dmxSet(4,10);
  dmxSet(1,220); dmxSet(2,120);
  setWidth(200);
  dmxSet(40,0); dmxSet(50,0);
}

// SCENE 16
void runScene16Base(){
  dmxSet(4,10);
  dmxSet(1,255);
  dmxSet(2,scene16Color);
  setWidth(200);
  dmxSet(40,255); dmxSet(50,0);
}

// SCENE 17
void runScene17Phase(uint8_t ph){
  dmxSet(4,10);
  switch (ph & 0x03){
    case 0: dmxSet(1,255); dmxSet(40,0);   dmxSet(50,0);   break;
    case 1: dmxSet(1,255); dmxSet(40,255); dmxSet(50,0);   break;
    case 2: dmxSet(1,255); dmxSet(40,255); dmxSet(50,255); break;
    case 3: dmxSet(1,0);   dmxSet(40,0);   dmxSet(50,0);   break;
  }
}

// SCENE 18
void runScene18Base(){
  dmxSet(4,10);
  dmxSet(1,0);
  dmxSet(2,0);
  setWidth(200);
  dmxSet(40,0); dmxSet(50,0);
}

// SCENE 19
void runScene19Base(){
  dmxSet(4,10);
  dmxSet(2,120);
  setWidth(200);
  dmxSet(1,0); dmxSet(40,255); dmxSet(50,0);
}

// SCENE 20
void runScene20Phase(uint8_t ph){
  dmxSet(4,10);
  switch (ph & 0x03){
    case 0: dmxSet(40,255); dmxSet(50,0);   dmxSet(1,0);   break;
    case 1: dmxSet(40,0);   dmxSet(50,255); dmxSet(1,0);   break;
    case 2: dmxSet(40,0);   dmxSet(50,0);   dmxSet(1,255); break;
    case 3: dmxSet(40,0);   dmxSet(50,0);   dmxSet(1,0);   break;
  }
}

// ---------- Тикер сцен ----------
void tickScene(){
  if (currentScene == SCENE_NONE) return;
  unsigned long now = millis();

  if (currentScene == SCENE_1) { runScene1Static(); return; }

  if (currentScene == SCENE_2) {
    if (scenePhase==0){ runScene2Phase0(); if (now-sceneStart>=2000){scenePhase=1; sceneStart=now;} }
    else { runScene2Phase1(); if (now-sceneStart>=2000){scenePhase=0; sceneStart=now;} }
    return;
  }

  if (currentScene == SCENE_3) {
    if (scenePhase==0){ runScene3PhaseA(); if (now-sceneStart>=2000){scenePhase=1; sceneStart=now;} }
    else { runScene3PhaseB(); if (now-sceneStart>=2000){scenePhase=0; sceneStart=now;} }
    return;
  }

  if (currentScene == SCENE_4) {
    if (scenePhase==0){ runScene4PhaseA(); if (now-sceneStart>=2000){scenePhase=1; sceneStart=now;} }
    else { runScene4PhaseB(); if (now-sceneStart>=2000){scenePhase=0; sceneStart=now;} }
    return;
  }

  if (currentScene == SCENE_5) {
  // дыхание канала 2
  if (now - scene5T0C2 >= scene5C2StepMs){
    scene5T0C2 = now;
    int16_t next = (int16_t)scene5C2Val + (int16_t)scene5C2Dir * (int16_t)scene5C2Step;
    if (next >= scene5C2Max){ next = scene5C2Max; scene5C2Dir = -1; }
    else if (next <= scene5C2Min){ next = scene5C2Min; scene5C2Dir = 1; }
    scene5C2Val = (uint8_t)next;
    dmxSet(2, scene5C2Val);
  }

  // мигание 40 (каждую секунду)
  if (now - scene5T0B40 >= scene5Blink40Ms){
    scene5T0B40 = now;
    scene5On40 = !scene5On40;
    dmxSet(40, scene5On40 ? 255 : 0);
  }

  // мигание 50 (каждые 2 секунды)
  if (now - scene5T0B50 >= scene5Blink50Ms){
    scene5T0B50 = now;
    scene5On50 = !scene5On50;
    dmxSet(50, scene5On50 ? 255 : 0);
  }

  return;
}


  if (currentScene == SCENE_6) {
    switch(scenePhase){
      case 0: runScene6Phase0(); if (now-sceneStart>=2000){scenePhase=1; sceneStart=now;} break;
      case 1: runScene6Phase1(); if (now-sceneStart>=2000){scenePhase=2; sceneStart=now;} break;
      case 2: runScene6Phase2(); if (now-sceneStart>=2000){scenePhase=3; sceneStart=now;} break;
      case 3: runScene6Phase3(); if (now-sceneStart>=2000){scenePhase=4; sceneStart=now;} break;
      case 4: runScene6Phase4(); if (now-sceneStart>=2000){scenePhase=5; sceneStart=now;} break;
      case 5: runScene6Phase5(); if (now-sceneStart>=2000){scenePhase=0; sceneStart=now;} break;
    }
    return;
  }

  if (currentScene == SCENE_7) { runScene7Static(); return; }

  if (currentScene == SCENE_8) {
    switch(scenePhase){
      case 0: runScene8Phase0(); if (now - sceneStart >= 2000){ scenePhase = 1; sceneStart = now; } break;
      case 1: runScene8Phase1(); if (now - sceneStart >= 2000){ scenePhase = 2; sceneStart = now; } break;
      case 2: runScene8Phase2(); if (now - sceneStart >= 2000){ scenePhase = 3; sceneStart = now; } break;
      case 3: runScene8Phase3(); if (now - sceneStart >= 2000){ scenePhase = 4; sceneStart = now; } break;
      case 4: runScene8Phase4(); if (now - sceneStart >= 2000){ scenePhase = 5; sceneStart = now; } break;
      case 5: runScene8Phase5(); if (now - sceneStart >= 2000){ scenePhase = 0; sceneStart = now; } break;
    }
    return;
  }

  // SCENE 9
  if (currentScene == SCENE_9) {
    switch(scenePhase){
      case 0: runScene9Phase0(); if (now - sceneStart >= scene9StepMs){ scenePhase = 1; sceneStart = now; } break;
      case 1: runScene9Phase1(); if (now - sceneStart >= scene9StepMs){ scenePhase = 2; sceneStart = now; } break;
      case 2: runScene9Phase2(); if (now - sceneStart >= scene9StepMs){ scenePhase = 3; sceneStart = now; } break;
      case 3: runScene9Phase3(); if (now - sceneStart >= scene9StepMs){ scenePhase = 4; sceneStart = now; } break;
      case 4: runScene9Phase4(); if (now - sceneStart >= scene9StepMs){ scenePhase = 0; sceneStart = now; } break;
    }
    return;
  }

  // SCENE 10
  if (currentScene == SCENE_10) {
    switch(scenePhase){
      case 0: runScene10Phase0(); if (now - sceneStart >= scene10StepMs){ scenePhase = 1; sceneStart = now; } break;
      case 1: runScene10Phase1(); if (now - sceneStart >= scene10StepMs){ scenePhase = 2; sceneStart = now; } break;
      case 2: runScene10Phase2(); if (now - sceneStart >= scene10StepMs){ scenePhase = 3; sceneStart = now; } break;
      case 3: runScene10Phase3(); if (now - sceneStart >= scene10StepMs){ scenePhase = 4; sceneStart = now; } break;
      case 4: runScene10Phase4(); if (now - sceneStart >= scene10StepMs){ scenePhase = 0; sceneStart = now; } break;
    }
    return;
  }

  // SCENE 11
  if (currentScene == SCENE_11) {
    switch (scenePhase) {
      case 0: runScene11Phase0(); if (now - sceneStart >= scene11StepMs){ scenePhase = 1; sceneStart = now; } break;
      case 1: runScene11Phase1(); if (now - sceneStart >= scene11StepMs){ scenePhase = 2; sceneStart = now; } break;
      case 2: runScene11Phase2(); if (now - sceneStart >= scene11StepMs){ scenePhase = 0; sceneStart = now; } break;
    }
    return;
  }

  // SCENE 12
  if (currentScene == SCENE_12) {
    // дыхание канала 6 (нормализованно)
    if (now - scene12T0Width >= scene12WidthStepMs){
      scene12T0Width = now;
      int16_t next = (int16_t)scene12WidthVal + (int16_t)scene12WidthDir * (int16_t)scene12WidthStep;
      if (next >= scene12WidthMax){ next = scene12WidthMax; scene12WidthDir = -1; }
      else if (next <= scene12WidthMin){ next = scene12WidthMin; scene12WidthDir = 1; }
      scene12WidthVal = (uint8_t)next;
      scene12ApplyWidth();
    }
    // акценты 40/50
    if (now - scene12T0Accent >= scene12AccentStepMs){
      scene12T0Accent = now;
      scene12AccentPhase = (scene12AccentPhase + 1) & 0x03;
      scene12ApplyAccent(scene12AccentPhase);
    }
    return;
  }

  // SCENE 13
  if (currentScene == SCENE_13) {
    // дыхание ширины
    if (now - scene13T0W >= scene13Wms){
      scene13T0W = now;
      int16_t next = (int16_t)scene13Wval + (int16_t)scene13Wdir * (int16_t)scene13Wstep;
      if (next >= scene13Wmax){ next = scene13Wmax; scene13Wdir = -1; }
      else if (next <= scene13Wmin){ next = scene13Wmin; scene13Wdir = 1; }
      scene13Wval = (uint8_t)next; setWidth(scene13Wval);
    }
    // тумблеры
    if (now - scene13T0A >= scene13Ams){ scene13T0A = now; scene13A = !scene13A; dmxSet(40, scene13A?255:0); }
    if (now - scene13T0B >= scene13Bms){ scene13T0B = now; scene13B = !scene13B; dmxSet(50, scene13B?255:0); }
    return;
  }

  // SCENE 14
  if (currentScene == SCENE_14) {
    if (scene14Flashing && (now - scene14FlashT0 >= scene14FlashMs)){
      scene14Flashing=false; dmxSet(1,200);
    }
    if (now - scene14T0 >= scene14NextDelta){
      scene14T0 = now;
      if ((scene14UpdCnt++ & 1) == 0) dmxSet(40, random(0,2) ? 255 : 0);
      else                            dmxSet(50, random(0,2) ? 255 : 0);
      if (scene14UpdCnt % scene14EveryN == 0){
        dmxSet(1,255); scene14Flashing=true; scene14FlashT0 = now;
      }
      uint16_t lo = (scene14AvgMs/3u < 30u) ? 30u : (scene14AvgMs/3u);
      uint32_t hi32 = (uint32_t)scene14AvgMs * 2u; if (hi32 > 60000u) hi32 = 60000u;
      scene14NextDelta = (uint16_t)random((long)lo, (long)hi32 + 1);
    }
    return;
  }

  // SCENE 15
  if (currentScene == SCENE_15) {
    if (now - scene15T0 >= scene15StepMs){ scene15T0 = now; scene15St40 = !scene15St40; dmxSet(40, scene15St40?255:0); }
    if (now - scene15T1 >= scene15StrobeMs){ scene15T1 = now; scene15St50 = !scene15St50; dmxSet(50, scene15St50?255:0); }
    return;
  }

  // SCENE 16
  if (currentScene == SCENE_16) {
    if (now - scene16T0 >= scene16StepMs){
      scene16T0 = now;
      scene16Toggle = !scene16Toggle;
      dmxSet(40, scene16Toggle?0:255);
      dmxSet(50, scene16Toggle?255:0);
      scene16Color = (uint8_t)(scene16Color + 5);
      dmxSet(2, scene16Color);
    }
    return;
  }

  // SCENE 17
  if (currentScene == SCENE_17) {
    switch (scenePhase){
      case 0: runScene17Phase(0); if (now - sceneStart >= scene17StepMs){ scenePhase=1; sceneStart=now; } break;
      case 1: runScene17Phase(1); if (now - sceneStart >= scene17StepMs){ scenePhase=2; sceneStart=now; } break;
      case 2: runScene17Phase(2); if (now - sceneStart >= scene17StepMs){ scenePhase=3; sceneStart=now; } break;
      case 3: runScene17Phase(3); if (now - sceneStart >= scene17StepMs){ scenePhase=0; sceneStart=now; } break;
    }
    return;
  }

  // SCENE 18
  if (currentScene == SCENE_18) {
    // цвет
    if (now - scene18TColor >= scene18ColorMs){
      scene18TColor = now;
      uint8_t c = (uint8_t)random(0, 256);
      dmxSet(2, c);
    }
    // пульс
    if (!scene18PulseOn && (now - scene18TPulse >= scene18PulsePeriodMs)){
      scene18TPulse = now; scene18PulseOn = true;
      if (scene18Which40){ dmxSet(40,255); } else { dmxSet(50,255); }
      scene18Which40 = !scene18Which40;
      scene18TPulseEnd = now + scene18PulseWidthMs;
    }
    if (scene18PulseOn && ((long)(now - scene18TPulseEnd) >= 0)){
      scene18PulseOn = false; dmxSet(40,0); dmxSet(50,0);
    }
    return;
  }

  // SCENE 19
  if (currentScene == SCENE_19) {
    if (now - scene19T1 >= scene19OneMs){
      scene19T1 = now; scene19One = !scene19One; dmxSet(1, scene19One?255:0);
    }
    if (now - scene19TMono >= scene19MonoMs){
      scene19TMono = now; scene19Which = !scene19Which;
      dmxSet(40, scene19Which?0:255);
      dmxSet(50, scene19Which?255:0);
    }
    return;
  }

  // SCENE 20
  if (currentScene == SCENE_20) {
    uint8_t ph = (uint8_t)((now - scene20T0) / scene20StepMs) & 0x03;
    runScene20Phase(ph);
    if (now - scene20T0 >= (unsigned long)scene20StepMs * 4u){
      scene20T0 = now;
    }
    return;
  }
}

// ---------- Парсер команд ----------
void handleLine(const String& ln){
  if (!ln.length()) return;
  String cmd = ln; cmd.trim();

  // Power OFF
  if (cmd.equalsIgnoreCase("OFF")){
    currentScene = SCENE_NONE;
    dmxFill(0);
    Serial.println(F("[OK] OFF"));
    return;
  }

  // === Регулировка ширины ===
  if (cmd.startsWith("WMIN")) {
    int v=-1; if (sscanf(cmd.c_str(), "WMIN %d", &v)==1){ widthMin = (uint8_t)constrain(v,0,255); setWidth(widthNorm); }
    Serial.print(F("[OK] WMIN=")); Serial.println(widthMin); return;
  }
  if (cmd.startsWith("WMAX")) {
    int v=-1; if (sscanf(cmd.c_str(), "WMAX %d", &v)==1){ widthMax = (uint8_t)constrain(v,0,255); setWidth(widthNorm); }
    Serial.print(F("[OK] WMAX=")); Serial.println(widthMax); return;
  }
  if (cmd.startsWith("W=") || cmd.startsWith("W ")) {
    int v=-1; if (sscanf(cmd.c_str(), "W %d", &v)!=1) sscanf(cmd.c_str(), "W=%d", &v);
    if (v<0) v=0; if (v>255) v=255; setWidth((uint8_t)v);
    Serial.print(F("[OK] W=")); Serial.print(widthNorm);
    Serial.print(F(" OUT="));  Serial.println(mapWidth(widthNorm));
    return;
  }
  if (cmd.equalsIgnoreCase("GET W") || cmd.equalsIgnoreCase("W?")) {
    Serial.print(F("W=")); Serial.print(widthNorm);
    Serial.print(F(" RANGE=[")); Serial.print(widthMin); Serial.print(F("..")); Serial.print(widthMax);
    Serial.print(F("] OUT="));  Serial.println(mapWidth(widthNorm));
    return;
  }

  // === Сцены ===
  if (cmd.equalsIgnoreCase("SCENE 1")){ currentScene=SCENE_1; Serial.println(F("[OK] SCENE 1 started")); return; }
  if (cmd.equalsIgnoreCase("SCENE 2")){ currentScene=SCENE_2; scenePhase=0; sceneStart=millis(); Serial.println(F("[OK] SCENE 2 started")); return; }
  if (cmd.equalsIgnoreCase("SCENE 3")){ currentScene=SCENE_3; scenePhase=0; sceneStart=millis(); Serial.println(F("[OK] SCENE 3 started")); return; }

  if (cmd.startsWith("SCENE 4")){
    int v = -1;
    if (sscanf(cmd.c_str(), "SCENE 4 %d", &v) == 1){
      scene4Width = (uint8_t)constrain(v, 0, 255);
      Serial.print(F("[OK] SCENE 4 width set to ")); Serial.println(scene4Width);
    } else {
      Serial.print(F("[OK] SCENE 4 keep width ")); Serial.println(scene4Width);
    }
    currentScene = SCENE_4;
    scenePhase = 0;
    sceneStart = millis();
    Serial.println(F("[OK] SCENE 4 started"));
    return;
  }

if (cmd.startsWith("SCENE 5")){
  // опционально: можно передать скорость дыхания канала 2 в мс
  // пример: "SCENE 5 60" (шаг дыхания каждые 60 мс)
  int ms = -1;
  if (sscanf(cmd.c_str(), "SCENE 5 %d", &ms) == 1){
    scene5C2StepMs = (uint16_t)constrain(ms, 10, 60000);
  } else {
    scene5C2StepMs = 800; // дефолт
  }

  currentScene = SCENE_5;
  scenePhase = 0;
  unsigned long now = millis();
  scene5T0C2 = scene5T0B40 = scene5T0B50 = now;
  sceneStart = now;

  runScene5Base();
  Serial.print(F("[OK] SCENE 5 started (step="));
  Serial.print(scene5C2StepMs);
  Serial.println(F(" ms, 40=1s, 50=2s)"));
  return;
}
  if (cmd.equalsIgnoreCase("SCENE 6")){ currentScene=SCENE_6; scenePhase=0; sceneStart=millis(); Serial.println(F("[OK] SCENE 6 started")); return; }
  if (cmd.equalsIgnoreCase("SCENE 7")){ currentScene=SCENE_7; Serial.println(F("[OK] SCENE 7 started")); return; }

  if (cmd.equalsIgnoreCase("SCENE 8")){
    currentScene = SCENE_8; scenePhase = 0; sceneStart = millis();
    Serial.println(F("[OK] SCENE 8 started"));
    return;
  }

  // SCENE 9 [ms_per_step]
  if (cmd.startsWith("SCENE 9")){
    int ms = -1;
    if (sscanf(cmd.c_str(), "SCENE 9 %d", &ms) == 1){
      if (ms < 50) ms = 50; if (ms > 60000) ms = 60000;
      scene9StepMs = (uint16_t)ms;
    } else scene9StepMs = 1000;
    currentScene = SCENE_9; scenePhase = 0; sceneStart = millis();
    Serial.print(F("[OK] SCENE 9 started, step=")); Serial.print(scene9StepMs); Serial.println(F(" ms"));
    return;
  }

  // SCENE 10 [ms_per_step]
  if (cmd.startsWith("SCENE 10")){
    int ms = -1;
    if (sscanf(cmd.c_str(), "SCENE 10 %d", &ms) == 1){
      if (ms < 50) ms = 50; if (ms > 60000) ms = 60000;
      scene10StepMs = (uint16_t)ms;
    } else scene10StepMs = 1000;
    currentScene = SCENE_10; scenePhase = 0; sceneStart = millis();
    dmxFill(0); runScene10Phase0();
    Serial.print(F("[OK] SCENE 10 started, step=")); Serial.print(scene10StepMs); Serial.println(F(" ms"));
    return;
  }

  // SCENE 11 [ms_per_step]
  if (cmd.startsWith("SCENE 11")){
    int ms = -1;
    if (sscanf(cmd.c_str(), "SCENE 11 %d", &ms) == 1){
      if (ms < 50) ms = 50; if (ms > 60000) ms = 60000;
      scene11StepMs = (uint16_t)ms;
    } else scene11StepMs = 1000;
    currentScene = SCENE_11; scenePhase = 0; sceneStart = millis();
    dmxFill(0); runScene11Base(); runScene11Phase0();
    Serial.print(F("[OK] SCENE 11 started, step=")); Serial.print(scene11StepMs); Serial.println(F(" ms"));
    return;
  }

  // SCENE 12 [ms_per_width_step]
  if (cmd.startsWith("SCENE 12")){
    int ms = -1;
    if (sscanf(cmd.c_str(), "SCENE 12 %d", &ms) == 1){
      if (ms < 20) ms = 20; if (ms > 60000) ms = 60000;
      scene12WidthStepMs  = (uint16_t)ms;
      uint32_t acc = (uint32_t)ms * 10u;
      if (acc < 200)   acc = 200;
      if (acc > 60000) acc = 60000;
      scene12AccentStepMs = (uint16_t)acc;
    } else {
      scene12WidthStepMs  = 2000;
      scene12AccentStepMs = 20000;
    }
    // подтянуть глобальные границы
    scene12WidthMin = widthMin;
    scene12WidthMax = widthMax;

    currentScene = SCENE_12; scenePhase = 0;
    runScene12Base();
    dmxSet(40,0); dmxSet(50,0);
    scene12WidthVal = scene12WidthMin;
    scene12WidthDir = 1;
    scene12ApplyWidth();

    scene12AccentPhase = 0;
    scene12ApplyAccent(scene12AccentPhase);

    unsigned long now = millis();
    scene12T0Width  = now;
    scene12T0Accent = now;
    sceneStart = now;

    Serial.print(F("[OK] SCENE 12 started, width_step="));
    Serial.print(scene12WidthStepMs);
    Serial.print(F(" ms, accent_step="));
    Serial.print(scene12AccentStepMs);
    Serial.println(F(" ms"));
    return;
  }

  // SCENE 13 [period40_ms] [period50_ms] [width_step_ms]
  if (cmd.startsWith("SCENE 13")){
    int a=-1,b=-1,w=-1;
    int n = sscanf(cmd.c_str(), "SCENE 13 %d %d %d", &a,&b,&w);
    if (n>=1) scene13Ams = (uint16_t)constrain(a, 50, 60000);
    if (n>=2) scene13Bms = (uint16_t)constrain(b, 50, 60000);
    if (n>=3) scene13Wms = (uint16_t)constrain(w, 20, 60000);
    // использовать глобальные границы
    scene13Wmin = widthMin; scene13Wmax = widthMax;

    currentScene = SCENE_13; scenePhase=0;
    runScene13Base();
    unsigned long now=millis(); scene13T0A=scene13T0B=scene13T0W=now;
    sceneStart = now;
    Serial.println(F("[OK] SCENE 13 started"));
    return;
  }

  // SCENE 14 [avg_ms] [flash_ms] [everyN]
  if (cmd.startsWith("SCENE 14")){
    int a=-1,f=-1,nEvery=-1;
    int n = sscanf(cmd.c_str(), "SCENE 14 %d %d %d", &a,&f,&nEvery);
    if (n>=1) scene14AvgMs   = (uint16_t)constrain(a, 30, 60000);
    if (n>=2) scene14FlashMs = (uint8_t)constrain(f, 10, 1000);
    if (n>=3) scene14EveryN  = (uint8_t)constrain(nEvery, 2, 50);

    currentScene = SCENE_14; scenePhase=0;
    runScene14Base();
    randomSeed(analogRead(A0) ^ micros());
    scene14T0 = millis(); scene14NextDelta = scene14AvgMs;
    scene14UpdCnt = 0; scene14Flashing=false; sceneStart=scene14T0;
    Serial.println(F("[OK] SCENE 14 started"));
    return;
  }

  // SCENE 15 [step40_ms] [strobe50_ms]
  if (cmd.startsWith("SCENE 15")){
    int s=-1, st=-1;
    int n = sscanf(cmd.c_str(), "SCENE 15 %d %d", &s,&st);
    if (n>=1) scene15StepMs   = (uint16_t)constrain(s, 40, 60000);
    if (n>=2) scene15StrobeMs = (uint16_t)constrain(st, 40, 60000);

    currentScene = SCENE_15; scenePhase=0;
    runScene15Base();
    unsigned long now=millis(); scene15T0=scene15T1=now; sceneStart=now;
    scene15St40=false; scene15St50=false;
    Serial.println(F("[OK] SCENE 15 started"));
    return;
  }

  // SCENE 16 [step_ms]
  if (cmd.startsWith("SCENE 16")){
    int ms=-1;
    if (sscanf(cmd.c_str(), "SCENE 16 %d", &ms) == 1){
      scene16StepMs = (uint16_t)constrain(ms, 60, 60000);
    } else scene16StepMs = 600;

    currentScene = SCENE_16; scenePhase=0;
    scene16Toggle=false; scene16Color=120;
    runScene16Base();
    scene16T0 = millis(); sceneStart=scene16T0;
    Serial.println(F("[OK] SCENE 16 started"));
    return;
  }

  // SCENE 17 [ms_per_step]
  if (cmd.startsWith("SCENE 17")){
    int ms=-1;
    if (sscanf(cmd.c_str(), "SCENE 17 %d", &ms) == 1){
      scene17StepMs = (uint16_t)constrain(ms, 80, 60000);
    } else scene17StepMs = 700;
    currentScene = SCENE_17; scenePhase=0; sceneStart=millis();
    runScene17Phase(0);
    Serial.println(F("[OK] SCENE 17 started"));
    return;
  }

  // SCENE 18 [color_ms] [pulse_period_ms] [pulse_width_ms]
  if (cmd.startsWith("SCENE 18")){
    int cm=-1, pm=-1, pw=-1;
    int n = sscanf(cmd.c_str(), "SCENE 18 %d %d %d", &cm,&pm,&pw);
    if (n>=1) scene18ColorMs       = (uint16_t)constrain(cm, 30, 60000);
    if (n>=2) scene18PulsePeriodMs = (uint16_t)constrain(pm, 80, 60000);
    if (n>=3) scene18PulseWidthMs  = (uint16_t)constrain(pw, 20, 5000);

    currentScene = SCENE_18; scenePhase=0;
    runScene18Base();
    unsigned long now=millis();
    scene18TColor = scene18TPulse = now;
    scene18PulseOn=false; scene18Which40=true; sceneStart=now;
    Serial.println(F("[OK] SCENE 18 started"));
    return;
  }

  // SCENE 19 [one_ms] [mono_ms]
  if (cmd.startsWith("SCENE 19")){
    int a=-1,b=-1;
    int n = sscanf(cmd.c_str(), "SCENE 19 %d %d", &a,&b);
    if (n>=1) scene19OneMs  = (uint16_t)constrain(a, 50, 60000);
    if (n>=2) scene19MonoMs = (uint16_t)constrain(b, 80, 60000);

    currentScene = SCENE_19; scenePhase=0;
    scene19One=false; scene19Which=false;
    runScene19Base();
    unsigned long now=millis(); scene19T1=scene19TMono=now; sceneStart=now;
    Serial.println(F("[OK] SCENE 19 started"));
    return;
  }

  // SCENE 20 [BPM]
  if (cmd.startsWith("SCENE 20")){
    int bpm=-1;
    if (sscanf(cmd.c_str(), "SCENE 20 %d", &bpm) == 1){
      if (bpm < 20) bpm = 20;
      if (bpm > 300) bpm = 300;
      scene20StepMs = (uint16_t)(60000 / bpm);
    } else {
      scene20StepMs = 500; // 120 BPM
    }
    currentScene = SCENE_20; scenePhase=0;
    dmxSet(4,10); dmxSet(1,0); dmxSet(40,0); dmxSet(50,0);
    scene20T0 = millis(); sceneStart=scene20T0;
    Serial.print(F("[OK] SCENE 20 started at ")); Serial.print(60000/scene20StepMs); Serial.println(F(" BPM"));
    return;
  }

  // Ручной ввод: "<канал> <значение>"
  uint16_t ch=0; int val=0;
  if (2==sscanf(cmd.c_str(),"%hu %d",&ch,&val)){
    currentScene=SCENE_NONE;
    if (ch == 6) { setWidth((uint8_t)constrain(val,0,255)); }
    else         { dmxSet(ch, (uint8_t)constrain(val,0,255)); }
    Serial.print(F("[OK] SET ")); Serial.print(ch);
    Serial.print(F(" = ")); Serial.println((int)constrain(val,0,255));
    return;
  }

  Serial.println(F("[ERR] Unknown command"));
}

// ---------- Setup / Loop ----------
void setup(){
  DmxSimple.usePin(DMX_PIN);
  DmxSimple.maxChannel(512);
  dmxFill(0);

  bridge.begin(115200);
  Serial.begin(115200);

  Serial.println(F("UNO DMX ready."));
  Serial.println(F("Commands: OFF | <channel> <value> | SCENE 1..20 | SCENE 4 <width>"));
  Serial.println(F("Width control: W <0..255> | WMIN <0..255> | WMAX <0..255> | GET W"));
  setWidth(widthNorm); // применить начальные значения
}

void loop(){
  String ln1 = readline(bridge, 5);
  if (ln1.length()) handleLine(ln1);

  String ln2 = readline(Serial, 5);
  if (ln2.length()) handleLine(ln2);

  tickScene();
}
