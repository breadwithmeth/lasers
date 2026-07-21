#include <SPI.h>
#include <EthernetENC.h>
#include <RF24.h>
#include <Wire.h>
#include <RTClib.h>
#include <time.h>
#include <WiFi.h>

// ================= CONFIG =================
#define DEVICE_NAME "laser-26-023"
#define DEVICE_SECRET "0cdd3acf48a440d7188c73684fbad226"

const char* MAC_STR = "DE:AD:BE:EF:FE:ED"; 
byte mac[6];

// --- НАСТРОЙКИ СТАТИЧЕСКОГО IP ---
IPAddress localIp(192, 168, 100, 200);
IPAddress dnsIp(8, 8, 8, 8);
IPAddress gatewayIp(192, 168, 100, 1);
IPAddress mySubnetMask(255, 255, 255, 0);

const char* serverHost = "213.148.10.233";
#define SERVER_PORT 3000

// Пины ENC28J60 для ESP32 (VSPI)
#define ETH_CS     2
#define ETH_SCK    18
#define ETH_MISO   19
#define ETH_MOSI   23

// Пины nRF24L01 для ESP32 (HSPI)
#define RF_SCK   12
#define RF_MOSI  13
#define RF_MISO  14
#define RF_CSN   15
#define RF_CE    27

// Пины I2C для DS3231
#define I2C_SDA  21
#define I2C_SCL  22

// Пины UART и RS485
#define UART_RX     16
#define UART_TX     17
#define RS485_DIR   4  

#define OFFLINE_TIMEOUT_MS 600000UL
#define RF_BROADCAST_MS 20000UL 

// ================= NTP CONFIG =================
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3 * 3600;
const int   daylightOffset_sec = 0;

EthernetClient client;

// ================= DS3231 =================
RTC_DS3231 rtc;

// ================= RF24 CONFIG & STRUCT =================
SPIClass hspi(HSPI);
RF24 radio(RF_CE, RF_CSN);

const byte oledAddress[6] = "00001"; // Совпадает с адресом приемника
const byte espAddress[6]  = "ESP01";

// Единая структура данных (только message)
struct DataPacket {
  char message[32];
};

DataPacket txMessage = {};
DataPacket rxMessage = {};

bool rfIsBroadcasting = false;
unsigned long rfBroadcastStart = 0;
unsigned long lastRfSendTime = 0;
const unsigned long rfSendInterval = 200;

char rfBroadcastMsg[32] = {0}; 

// ================= STATE & NETWORK =================
bool useOfflineMode = false;
bool offlineSceneActive = false;
unsigned long offlineStartMs = 0;

enum NetState { NET_IDLE, NET_CONNECT, NET_SEND, NET_READ };
NetState netState = NET_IDLE;

unsigned long lastPoll = 0;
unsigned long netTimer = 0;
String responseBuffer = "";

unsigned long lastLinkCheck = 0;
const unsigned long linkCheckInterval = 30000UL;
bool lastLinkUp = true;
const unsigned long pollInterval = 30000UL;

unsigned long lastPingCheck = 0;
const unsigned long pingCacheTtl = 180000UL;
bool lastPingResult = false;
bool pingCacheValid = false;

unsigned long dbgT = 0;

// ================= HELPERS =================
void parseMAC(const char* str, byte* outMac) {
  int v[6];
  sscanf(str, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
  for (byte i = 0; i < 6; i++) outMac[i] = (byte)v[i];
}

String getRTCTimeString() {
  if (!rtc.begin()) return "NO_RTC";
  DateTime now = rtc.now();
  char buf[] = "YYYY-MM-DD hh:mm:ss";
  return now.toString(buf);
}

void syncTimeFromNTP() {
  if (!rtc.begin()) return;
  Serial.println(F("[NTP] Requesting time..."));
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  time_t now = time(nullptr);
  int attempts = 0;
  while (now < 1700000000 && attempts < 10) {
    delay(500); 
    now = time(nullptr); 
    attempts++;
  }
  if (now > 1700000000) {
    rtc.adjust(DateTime(now));
    Serial.print(F("[NTP] Synced OK: "));
    Serial.println(getRTCTimeString());
  } else {
    Serial.println(F("[NTP] Failed."));
  }
}

bool applyStaticIP() {
  Serial.println(F("[NET] Waiting for link..."));
  int attempts = 0;
  while (Ethernet.linkStatus() == LinkOFF && attempts < 20) {
    delay(500); 
    Serial.print("."); 
    attempts++;
  }
  if (Ethernet.linkStatus() == LinkOFF) return false;
  
  Serial.println(F("\n[NET] Link UP. Applying Static IP..."));
  Ethernet.begin(mac, localIp, dnsIp, gatewayIp, mySubnetMask);
  delay(300);
  return (Ethernet.localIP() == localIp);
}

void printNetDiag(const __FlashStringHelper* tag) {
  Serial.println();
  Serial.print(F("[NET] === ")); Serial.print(tag); Serial.println(F(" ==="));
  Serial.print(F("[NET] IP : ")); Serial.println(Ethernet.localIP());
  Serial.print(F("[NET] SN : ")); Serial.println(Ethernet.subnetMask());
  Serial.print(F("[NET] GW : ")); Serial.println(Ethernet.gatewayIP());
  Serial.print(F("[NET] DNS: ")); Serial.println(Ethernet.dnsServerIP());
  Serial.println(F("[NET] ====================="));
}

void sendToUART(const String& cmd) {
  digitalWrite(RS485_DIR, HIGH); 
  Serial2.println(cmd);          
  Serial2.flush();               
  digitalWrite(RS485_DIR, LOW);  
}

// ================= NETWORK LOGIC =================
bool pingServerRaw() {
  EthernetClient pingClient;
  bool ok = pingClient.connect(serverHost, SERVER_PORT);
  pingClient.stop();
  return ok;
}

bool pingServerCached() {
  unsigned long now = millis();
  if (pingCacheValid && (now - lastPingCheck < pingCacheTtl)) return lastPingResult;
  lastPingResult = pingServerRaw();
  lastPingCheck = now;
  pingCacheValid = true;
  return lastPingResult;
}

void invalidatePingCache() { 
  pingCacheValid = false; 
}

void markOffline() {
  if (!useOfflineMode) {
    useOfflineMode = true;
    offlineStartMs = millis();
    offlineSceneActive = false;
  }
}

void markOnline() {
  useOfflineMode = false;
  offlineSceneActive = false;
  offlineStartMs = 0;
}

void processNetwork() {
  switch (netState) {
    case NET_IDLE:
      if (millis() - lastPoll > pollInterval) {
        lastPoll = millis();
        netState = NET_CONNECT;
      }
      break;

    case NET_CONNECT:
      client.stop();
      if (!pingServerCached()) { 
        markOffline(); 
        netState = NET_IDLE; 
        break; 
      }
      if (client.connect(serverHost, SERVER_PORT)) netState = NET_SEND;
      else { 
        markOffline(); 
        netState = NET_IDLE; 
      }
      break;

    case NET_SEND:
      client.println(F("GET /device-agent/commands/pending HTTP/1.1"));
      client.print(F("Host: ")); 
      client.print(serverHost); 
      client.print(F(":")); 
      client.println(SERVER_PORT);
      client.print(F("x-device-unique-name: ")); 
      client.println(F(DEVICE_NAME));
      client.print(F("x-device-secret-key: ")); 
      client.println(F(DEVICE_SECRET));
      client.print(F("x-device-time: ")); 
      client.println(getRTCTimeString());
      client.println(F("Connection: close"));
      client.println();
      responseBuffer = "";
      netTimer = millis();
      netState = NET_READ;
      break;

    case NET_READ:
      while (client.available()) responseBuffer += (char)client.read();
      if (!client.connected()) {
        client.stop();
        int bodyStart = responseBuffer.indexOf("\r\n\r\n");
        if (bodyStart != -1) {
          String body = responseBuffer.substring(bodyStart + 4);
          body.trim(); 
          if (body.length() > 0) {
            if (body == "P") {
              rfIsBroadcasting = true;
              rfBroadcastStart = millis();
              strncpy(rfBroadcastMsg, "S_P", sizeof(rfBroadcastMsg) - 1);
              Serial.println(F("[RF] START BROADCAST S_P FOR 20s"));
            } else if (body == "R") {
              rfIsBroadcasting = true;
              rfBroadcastStart = millis();
              strncpy(rfBroadcastMsg, "R_P", sizeof(rfBroadcastMsg) - 1);
              Serial.println(F("[RF] START BROADCAST R_P FOR 20s"));
            } else {
              sendToUART(body);
            }
          }
          markOnline();
        } else { 
          markOffline(); 
        }
        responseBuffer = "";
        netTimer = 0;
        netState = NET_IDLE;
      }
      if (millis() - netTimer > 15000) {
        client.stop(); 
        markOffline();
        responseBuffer = ""; 
        netTimer = 0; 
        netState = NET_IDLE;
      }
      break;
  }
}

void ensureNetwork() {
  if (millis() - lastLinkCheck < linkCheckInterval) return;
  lastLinkCheck = millis();
  
  bool linkUp = (Ethernet.linkStatus() != LinkOFF);

  if (!linkUp) {
    client.stop(); 
    sendToUART("O"); 
    offlineSceneActive = false; 
    responseBuffer = "";
    netTimer = 0; 
    netState = NET_IDLE; 
    lastLinkUp = false;
    invalidatePingCache(); 
    markOffline(); 
    return;
  }

  if (!lastLinkUp && linkUp) {
    client.stop();
    if (applyStaticIP()) {
      printNetDiag(F("RELINK OK"));
      syncTimeFromNTP(); 
      markOnline();
      responseBuffer = ""; 
      netTimer = 0; 
      netState = NET_IDLE;
      lastPoll = 0; 
      invalidatePingCache();
    } else {
      printNetDiag(F("RELINK FAIL"));
      responseBuffer = ""; 
      netTimer = 0; 
      netState = NET_IDLE;
      invalidatePingCache(); 
      markOffline();
    }
  }
  
  if (linkUp && useOfflineMode) netState = NET_IDLE;
  lastLinkUp = linkUp;
}

// ================= RF24 LOGIC =================
void handleRF() {
  if (rfIsBroadcasting) {
    if (millis() - rfBroadcastStart >= RF_BROADCAST_MS) {
      rfIsBroadcasting = false;
      Serial.println(F("[RF] STOP BROADCAST"));
      radio.startListening();
    } else {
      if (millis() - lastRfSendTime >= rfSendInterval) {
        lastRfSendTime = millis();
        
        // Очищаем и заполняем структуру новым сообщением
        memset(txMessage.message, 0, sizeof(txMessage.message));
        strncpy(txMessage.message, rfBroadcastMsg, sizeof(txMessage.message) - 1);

        radio.stopListening();
        radio.openWritingPipe(oledAddress);
        
        if (radio.write(&txMessage, sizeof(txMessage))) {
          Serial.print(F("[RF] TX OK: "));
          Serial.println(txMessage.message);
        } else {
          Serial.println(F("[RF] TX FAIL"));
        }

        radio.openReadingPipe(1, espAddress);
        radio.startListening();
      }
    }
  }

  // Чтение сообщений от внешних узлов
  if (radio.available()) {
    radio.read(&rxMessage, sizeof(rxMessage));
    Serial.print(F("[RF] RX Message: \""));
    Serial.print(rxMessage.message);
    Serial.println(F("\""));

    if (strcmp(rxMessage.message, "ERROR") == 0 || strcmp(rxMessage.message, "BNO ERR") == 0) {
      Serial.println(F("[RF] !!! ALARM: ERROR RECEIVED FROM NODE !!!"));
    }
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(100); 

  WiFi.mode(WIFI_OFF);
  btStop();

  Serial2.begin(115200, SERIAL_8N1, UART_RX, UART_TX);
  pinMode(RS485_DIR, OUTPUT);
  digitalWrite(RS485_DIR, LOW); 

  Serial.println(F(DEVICE_NAME));
  parseMAC(MAC_STR, mac);

  Wire.begin(I2C_SDA, I2C_SCL);
  if (!rtc.begin()) {
    Serial.println(F("[RTC] DS3231 NOT FOUND!"));
  } else {
    Serial.println(F("[RTC] DS3231 OK"));
  }

  SPI.begin(ETH_SCK, ETH_MISO, ETH_MOSI, ETH_CS);
  Ethernet.init(ETH_CS);

  hspi.begin(RF_SCK, RF_MISO, RF_MOSI, RF_CSN);
  if (!radio.begin(&hspi) || !radio.isChipConnected()) {
    Serial.println(F("[RF] nRF24L01 NOT FOUND!"));
  } else {
    radio.setChannel(100);
    radio.setDataRate(RF24_250KBPS);
    radio.setPALevel(RF24_PA_LOW); 
    radio.setAutoAck(false);
    radio.setPayloadSize(sizeof(DataPacket)); // Фиксированный размер структура DataPacket
    radio.openReadingPipe(1, espAddress);
    radio.startListening();
    Serial.println(F("[RF] nRF24L01 READY"));
  }

  printNetDiag(F("BEFORE APPLY"));
  bool ok = false;
  for (byte i = 0; i < 3; i++) {
    if (applyStaticIP()) { ok = true; break; }
    delay(1000);
  }
  printNetDiag(F("AFTER APPLY"));

  if (ok) {
    syncTimeFromNTP();
  } else {
    Serial.println(F("[NET] STATIC CONFIG FAIL"));
    markOffline();
  }

  lastLinkUp = (Ethernet.linkStatus() != LinkOFF);
  invalidatePingCache();
}

// ================= LOOP =================
void loop() {
  ensureNetwork();
  processNetwork();
  handleRF();

  if (useOfflineMode && !offlineSceneActive) {
    if (millis() - offlineStartMs >= OFFLINE_TIMEOUT_MS) {
      Serial.println(F("[OFFLINE] START SCENE 99"));
      sendToUART("S|99|10|1500");
      offlineSceneActive = true;
    }
  }

  if (!useOfflineMode && offlineSceneActive) {
    sendToUART("O");
    offlineSceneActive = false;
  }

  if (millis() - dbgT > 10000UL) {
    dbgT = millis();
    Serial.print(F("[HEALTH] time="));
    Serial.print(getRTCTimeString());
    Serial.print(F(" link="));
    Serial.print(Ethernet.linkStatus() == LinkOFF ? F("OFF") : F("ON"));
    Serial.print(F(" offline="));
    Serial.print(useOfflineMode ? F("1") : F("0"));
    Serial.print(F(" offlineSec="));
    Serial.println(useOfflineMode ? (millis() - offlineStartMs) / 1000UL : 0UL);
  }
}
