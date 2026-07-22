#include <Wire.h>
#include <SPI.h>
#include <RF24.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BNO055.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

// --- ПИНЫ ---
#define RELAY_PIN A2
#define BUTTON_PIN 5 // PD5 / D5
#define OLED_ADDR 0x3C
#define EEPROM_ADDR 0x50
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

const float TOLERANCE_DEF = 1.5;
const uint32_t EEPROM_MAGIC = 0xB0052026;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_BNO055 bno = Adafruit_BNO055(55, 0x28);
RF24 radio(PIN_PB1, PIN_PD4); // CE, CSN

// --- АДРЕСА ДЛЯ RF24 (ЗЕРКАЛЬНО ОТНОСИТЕЛЬНО ESP32) ---
const byte oledAddress[6] = "00001"; // Модуль СЛУШАЕТ этот адрес
const byte espAddress[6]  = "ESP01"; // Модуль ОТПРАВЛЯЕТ на этот адрес

// --- СТРУКТУРА ДЛЯ ПРИЕМА / ОТПРАВКИ ---
struct DataPacket {
  char message[32];
} myData;

struct CalibData {
  uint32_t magic;
  float zeroY;
  float zeroZ;
  uint8_t crc;
};

float zeroY = 0.0;
float zeroZ = -90.0;
bool isZeroSet = false; // Флаг активности установки нуля

bool oledOK = false;
bool bnoOK = false;
bool radioOK = false;
bool eepromOK = false;

bool lastButton = HIGH;
unsigned long btnTimer = 0;
bool btnLatch = false;

// --- ТАЙМЕР ДЛЯ ОТПРАВКИ ОШИБКИ ---
unsigned long lastErrorSendTime = 0;

// ---------- CRC ----------
uint8_t calcCRC(uint8_t *data, uint8_t len) {
  uint8_t crc = 0;
  for (uint8_t i = 0; i < len; i++) crc ^= data[i];
  return crc;
}

// ---------- I2C CHECK ----------
bool i2cDeviceExists(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// ---------- EEPROM ----------
bool eepromWriteByte(uint8_t memAddr, uint8_t value) {
  Wire.beginTransmission(EEPROM_ADDR);
  Wire.write(memAddr);
  Wire.write(value);
  if (Wire.endTransmission() != 0) return false;
  delay(7);
  return true;
}

bool eepromReadByte(uint8_t memAddr, uint8_t &value) {
  Wire.beginTransmission(EEPROM_ADDR);
  Wire.write(memAddr);
  if (Wire.endTransmission() != 0) return false;
  uint8_t count = Wire.requestFrom(EEPROM_ADDR, 1);
  if (count != 1) return false;
  value = Wire.read();
  return true;
}

bool eepromWriteBlock(uint8_t memAddr, uint8_t *data, uint8_t len) {
  for (uint8_t i = 0; i < len; i++) {
    if (!eepromWriteByte(memAddr + i, data[i])) return false;
  }
  return true;
}

bool eepromReadBlock(uint8_t memAddr, uint8_t *data, uint8_t len) {
  for (uint8_t i = 0; i < len; i++) {
    if (!eepromReadByte(memAddr + i, data[i])) return false;
  }
  return true;
}

bool saveZeroToEEPROM() {
  if (!eepromOK) return false;
  CalibData data;
  data.magic = EEPROM_MAGIC;
  data.zeroY = zeroY;
  data.zeroZ = zeroZ;
  data.crc = calcCRC((uint8_t*)&data, sizeof(CalibData) - 1);
  return eepromWriteBlock(0, (uint8_t*)&data, sizeof(CalibData));
}

bool loadZeroFromEEPROM() {
  if (!eepromOK) return false;
  CalibData data;
  if (!eepromReadBlock(0, (uint8_t*)&data, sizeof(CalibData))) return false;
  if (data.magic != EEPROM_MAGIC) return false;
  uint8_t crc = calcCRC((uint8_t*)&data, sizeof(CalibData) - 1);
  if (crc != data.crc) return false;
  zeroY = data.zeroY;
  zeroZ = data.zeroZ;
  return true;
}

// Разница углов через 180/-180
float angleDiff(float current, float target) {
  float diff = current - target;
  while (diff > 180.0) diff -= 360.0;
  while (diff < -180.0) diff += 360.0;
  return diff;
}

void showText(const char* txt) {
  if (!oledOK) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(txt);
  display.display();
}

void showSaved() {
  if (!oledOK) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print("ZERO SET");
  display.setTextSize(1);
  display.setCursor(0, 30);
  display.print("Y0: ");
  display.print(zeroY, 1);
  display.setCursor(0, 42);
  display.print("Z0: ");
  display.print(zeroZ, 1);
  display.setCursor(0, 55);
  if (eepromOK) display.print("EEPROM SAVED");
  else display.print("EEPROM ERR");
  display.display();
  delay(800);
}

void showReset() {
  if (!oledOK) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print("ZERO RESET");
  display.setTextSize(1);
  display.setCursor(0, 30);
  display.print("Zero cleared.");
  display.setCursor(0, 45);
  display.print("Relay is locked LOW");
  display.display();
  delay(800);
}

// ---------- ИСПРАВЛЕННАЯ ФУНКЦИЯ ОТПРАВКИ "ERROR" ----------
void sendErrorMessage() {
  if (!radioOK) return;
  
  radio.stopListening();               // Останавливаем приём
  radio.openWritingPipe(espAddress);    // ВАЖНО: Отправляем на адрес ESP32 ("ESP01")
  
  DataPacket errPacket;
  memset(&errPacket, 0, sizeof(errPacket));
  
  if (!bnoOK) {
    strncpy(errPacket.message, "BNO ERR", sizeof(errPacket.message) - 1);
  } else {
    strncpy(errPacket.message, "ERROR", sizeof(errPacket.message) - 1);
  }
  
  radio.write(&errPacket, sizeof(errPacket)); // Отправляем пакет
  
  radio.openReadingPipe(1, oledAddress); // ВАЖНО: Возвращаем прослушивание своего адреса ("00001")
  radio.startListening();              // Возвращаемся в режим приёма
}

void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Wire.begin();
  Wire.setClock(100000);

  oledOK = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  showText("BOOT...");

  eepromOK = i2cDeviceExists(EEPROM_ADDR);
  bnoOK = bno.begin();

  if (bnoOK) {
    delay(500);
    bno.setExtCrystalUse(true);
  } else {
    showText("BNO055 ERROR");
    delay(1000);
  }

  if (eepromOK) {
    if (loadZeroFromEEPROM()) {
      isZeroSet = true; 
    } else {
      zeroY = 0.0;
      zeroZ = -90.0;
      isZeroSet = false;
      saveZeroToEEPROM();
    }
  }

  // --- НАСТРОЙКА РАДИО ---
  radioOK = radio.begin();
  if (radioOK) {
    radio.setChannel(100);                    // Канал 100
    radio.setDataRate(RF24_250KBPS);          // Скорость 250kbps
    radio.setAutoAck(false);                  // Без подтверждения
    radio.setPayloadSize(sizeof(DataPacket)); // 32 байта
    radio.openReadingPipe(1, oledAddress);    // Слушаем трубу "00001"
    radio.setPALevel(RF24_PA_LOW);
    radio.startListening();                   // Режим приёма
  }

  myData.message[0] = '\0';
  showText("READY");
  delay(500);
}

void loop() {
  float y = 0.0;
  float z = 0.0;

  if (bnoOK) {
    sensors_event_t event;
    bno.getEvent(&event);
    y = event.orientation.y;
    z = event.orientation.z;
  }

  // --- КНОПКА PD5 / D5 ---
  bool btn = digitalRead(BUTTON_PIN);
  if (btn != lastButton) {
    btnTimer = millis();
  }

  if ((millis() - btnTimer) > 60) {
    if (btn == LOW && !btnLatch) {
      btnLatch = true;
      zeroY = y;
      zeroZ = z;
      isZeroSet = true;
      saveZeroToEEPROM();
      showSaved();
    }
    if (btn == HIGH) {
      btnLatch = false;
    }
  }
  lastButton = btn;

  // --- ПРИЕМ ДАННЫХ ПО RADIO И ОБРАБОТКА КОМАНД ---
  if (radioOK && radio.available()) {
    radio.read(&myData, sizeof(myData));

    if (strcmp(myData.message, "S_P") == 0) {
      zeroY = y;
      zeroZ = z;
      isZeroSet = true;
      saveZeroToEEPROM();
      showSaved();
    }
    else if (strcmp(myData.message, "R_P") == 0) {
      zeroY = 0.0;
      zeroZ = -90.0;
      isZeroSet = false;
      saveZeroToEEPROM();
      showReset();
    }
  }

  float diffY = angleDiff(y, zeroY);
  float diffZ = angleDiff(z, zeroZ);

  // Условие нормальной работы
  bool ok = bnoOK &&
            isZeroSet &&
            fabs(diffY) <= TOLERANCE_DEF &&
            fabs(diffZ) <= TOLERANCE_DEF;

  digitalWrite(RELAY_PIN, ok ? HIGH : LOW);

  // --- ОТПРАВКА ОШИБКИ РАЗ В СЕКУНДУ ---
  if (!ok) {
    if (millis() - lastErrorSendTime >= 1000) {
      lastErrorSendTime = millis();
      sendErrorMessage();
    }
  }

  // --- OLED ---
  if (oledOK) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(0, 0);
    display.print("Y:");
    display.print(diffY, 1);

    display.setCursor(0, 18);
    display.print("Z:");
    display.print(diffZ, 1);

    display.drawFastHLine(0, 37, 128, SSD1306_WHITE);

    if (!bnoOK) {
      display.setTextSize(2);
      display.setCursor(0, 42);
      display.print("BNO ERR");
    } else if (ok) {
      display.setTextSize(2);
      display.setCursor(0, 40);
      display.print("NORM");
      display.setTextSize(1);
      display.setCursor(0, 56);
      display.print("RX: ");
      display.print(myData.message);
    } else {
      display.setTextSize(1);
      display.setCursor(0, 42);
      display.print("ZERO: ");
      display.print(isZeroSet ? "OK" : "NO");

      display.setCursor(0, 54);
      display.print("RX: ");
      display.print(myData.message);
    }
    display.display();
  }

  delay(30);
}
