#define Serial Serial

#define SerialAT Serial1

#define SerialGPS Serial2

#define BOARD_GPS_TX_PIN                    21
#define BOARD_GPS_RX_PIN                    22
#define BOARD_GPS_PPS_PIN                   23
#define BOARD_GPS_WAKEUP_PIN                19

// Transportauswahl: Nur ein Transport darf aktiv sein
#define TRANSPORT_WIFI     // Transport via WiFi
// #define TRANSPORT_LTE   // Transport via LTE

#if defined(TRANSPORT_WIFI) && defined(TRANSPORT_LTE)
  #error "Bitte nur einen Transport definieren (WiFi oder LTE)"
#endif

#include "utilities.h"
#include <WiFi.h>
#include <stdint.h>
#include <ESP32Servo.h>
#include <TinyGPS++.h>
#include "Arduino.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_now.h>   // Für ESPNow
#include <TinyGsmClient.h>
#include <TinyGsm.h>

// --- WiFi- und MQTT-Konfiguration ---
const char* ssid = "ASUS-RCCAR";//"Bratiphone";//"ASUS-RCCAR"; //"Internetz"; //"HotspotTest"
const char* password = "dhbwka04042025";//"dhbwka04042025"; //"BratenDackel!";//"91369823410622158981"; //"NichtMartin"
#define NETWORK_APN     "web.vodafone.de"    // APN für Vodafone Deutschland
TinyGsm modem(SerialAT);

#define TINY_GSM_USE_GPRS true
#define USE_Camera false
const char *SIMCARD_PIN_CODE = "5579"; // SIM card PIN code, if any

// MQTT-Themen für Steuerbefehle und Telemetrie
const char* mqtt_topic_steering = "racecar/steering";
const char* mqtt_topic_drive = "racecar/drive";
const char* mqtt_topic_telemetry = "racecar/telemetry";
const char* mqtt_topic_control = "racecar/control";  // Neues Thema für Steuerbefehle
const char* topicLog = "racecar/log";  // Thema für Log-Nachrichten

// --- Remote TCP-Server ---
const char* remoteServerControls = "157.90.228.207"; // Remote-Server-Adresse für Steuerbefehle
const char* remoteServerCam = "157.90.228.207"; // Remote-Server-Adresse für Kamerastream
const uint16_t remotePortControls = 12345; // Port für eingehende Verbindungen
const uint16_t remotePortCam = 12348; // Updated port for outbound forwarding

// --- Servo-Konfiguration ---
// Ein Servo für die Lenkung, ein Servo für Gas/Bremse
const int steeringPin = 32;  // Lenkservo
const int drivePin = 18;     // Gas-/Bremservo

String location;  // GPS-Standort als String
String date_time;  // Datum und Uhrzeit als String
String gpsTime;  // Uhrzeit als String
String mqtt_message;  // MQTT-Nachricht als String
String debug_message;  // Debug-GPS-Nachricht als String

//tmp
int counter = 0;
int lastDataTime = 0; // Variable to store the last data time

// Add these globals near the top (after your existing includes and constants)
const int gpsMessagesPerSecond = 1; // Adjust this value as needed
unsigned long lastGPSPublishTime = 0;
unsigned long gpsInterval = 1000 / gpsMessagesPerSecond;

unsigned long totalDelay = 0;
unsigned long countDelays = 0;

// Globale Variable zur Begrenzung der Send-FPS (z. B. 30 fps)
const int TARGET_SEND_FPS = 1; 
// Berechne das Intervall in Millisekunden, das zwischen den zu versendenden Frames liegen soll
const unsigned long FRAME_INTERVAL_MS = 1000 / TARGET_SEND_FPS;

// Variable zur Zeiterfassung des letzten versendeten Frames
unsigned long lastFrameSentTime = 0;

// --- Globaler Remote-Client ---
// Je nach Transport verwenden wir WiFiClient oder LTE-Client (TinyGsmClient)
#ifdef TRANSPORT_WIFI
WiFiClient remoteClientControls;
#else  // TRANSPORT_LTE
TinyGsmClient remoteClientControls(modem);
#endif

// --- MQTT-Client ---
Servo servoSteering;
Servo servoDrive;
TinyGPSPlus gps;

// Definiere Kameradatenparameter (Passe ggf. Auflösung an)
#define CAM_WIDTH   160
#define CAM_HEIGHT  120
#define CHUNK_SIZE 230
#define CAM_FRAME_SIZE (CAM_WIDTH*CAM_HEIGHT*2)  // RGB565: 2 Byte pro Pixel

// Globaler Frame-Puffer & Statusvariablen
uint8_t camFrameBuffer[CAM_FRAME_SIZE]; 
uint32_t currentCamFrameId = 0;
uint16_t packetsReceived = 0;
uint16_t totalPacketsExpected = 0;
volatile bool newCamFrameAvailable = false;

// Struktur für ein Frame-Fragment – identisch zum ESP32Cam Sender
typedef struct {
  uint32_t frameId;   // Eindeutige Frame-ID
  uint16_t seq;       // Sequenznummer des Fragments
  uint16_t total;     // Gesamtzahl der Fragmente
  uint16_t size;      // Anzahl der gültigen Bytes in diesem Fragment
  uint8_t data[CHUNK_SIZE]; // Bilddatenfragment
} __attribute__((packed)) FramePacket;

// Ergänzen Sie die globalen Variablen für den Camstream-Empfang:
#define FORWARD_BUFFER_SIZE 16384  // Puffergröße – ggf. anpassen
uint8_t forwardBuffer[FORWARD_BUFFER_SIZE];
size_t forwardBufPos = 0;

// Empfangs‑Callback für ESP‑NOW (neue Signatur)
void onCamStreamReceive(const uint8_t *mac, const uint8_t *data, int len) {
  Serial.printf("ESPNow: Empfange %d Bytes\n", len);
  if (forwardBufPos + len <= FORWARD_BUFFER_SIZE) {
    memcpy(forwardBuffer + forwardBufPos, data, len);
    forwardBufPos += len;
    Serial.printf("ForwardBuffer neu: %d Bytes gesamt\n", forwardBufPos);
  }
  else {
    Serial.println("Empfangspuffer überlaufen, Paket verworfen");
  }
}

// Diese Funktion sucht im Puffer nach einem vollständigen Frame-Envelope und sendet diesen.
void processForwardBuffer() {
  const char* headerTag = "FRAME_START ";
  int headerPos = -1;
  // Suchen nach Header
  for (size_t i = 0; i < forwardBufPos; i++) {
    if (strncmp((char*)(forwardBuffer + i), headerTag, strlen(headerTag)) == 0) {
      headerPos = i;
      break;
    }
  }
  if (headerPos < 0) {
    // Debug: Zeige aktuelle Puffergröße, wenn kein gültiger Header gefunden wurde
    Serial.printf("Kein Header gefunden. ForwardBuffer hat aktuell: %d Bytes\n", forwardBufPos);
    return;
  }
  
  // Suchen des Endes der Headerzeile (Zeilenumbruch)
  int headerEnd = -1;
  for (size_t i = headerPos; i < forwardBufPos; i++) {
    if (forwardBuffer[i] == '\n') {
      headerEnd = i;
      break;
    }
  }
  if (headerEnd < 0) return; // Warten auf einen kompletten Header
  
  // Extrahiere Headerzeile
  char headerLine[64];
  size_t copyLen = min(sizeof(headerLine)-1, (size_t)(headerEnd - headerPos));
  memcpy(headerLine, forwardBuffer + headerPos, copyLen);
  headerLine[copyLen] = '\0';
  
  int expectedJPEGSize = atoi(headerLine + strlen(headerTag));
  if (expectedJPEGSize <= 0) {
    Serial.printf("Ungültiger erwarteter JPEG-Frame (erwartet: %d Bytes)\n", expectedJPEGSize);
    size_t removeBytes = headerEnd + 1;
    memmove(forwardBuffer, forwardBuffer + removeBytes, forwardBufPos - removeBytes);
    forwardBufPos -= removeBytes;
    return;
  }
  
  Serial.printf("Frame erkannt: Erwarte %d Bytes JPEG-Daten\n", expectedJPEGSize);
  
  // Berechne den Start der JPEG-Daten
  int dataStart = headerEnd + 1;
  const char* trailerTag = "FRAME_END\n";
  int trailerLen = strlen(trailerTag);
  
  if (dataStart + expectedJPEGSize + trailerLen > (int)forwardBufPos) {
    Serial.println("Frame unvollständig. Warte auf mehr Daten...");
    return;
  }
  
  if (memcmp(forwardBuffer + dataStart + expectedJPEGSize, trailerTag, trailerLen) != 0) {
    Serial.println("Trailer stimmt nicht überein. Puffer wird angepasst.");
    size_t removeBytes = headerPos + 1;
    memmove(forwardBuffer, forwardBuffer + removeBytes, forwardBufPos - removeBytes);
    forwardBufPos -= removeBytes;
    return;
  }
  
  int fullPacketSize = (dataStart + expectedJPEGSize + trailerLen) - headerPos;
  Serial.printf("Vollständiger Frame im Puffer, Größe: %d Bytes\n", fullPacketSize);
  
  // Sende das Paket über remoteClientControls
  if (remoteClientControls.connected()) {
    int sent = remoteClientControls.write(forwardBuffer + headerPos, fullPacketSize);
    remoteClientControls.flush();
    Serial.printf("Frame veröffentlicht: %d Bytes gesendet\n", sent);
  } else {
    Serial.println("LTE: remoteClientControls nicht verbunden, Frame nicht gesendet.");
    return;
  }
  
  // Entferne das veröffentlichte Paket aus dem Puffer
  size_t remainingBytes = forwardBufPos - (headerPos + fullPacketSize);
  memmove(forwardBuffer, forwardBuffer + headerPos + fullPacketSize, remainingBytes);
  forwardBufPos = remainingBytes;
  Serial.printf("Puffer bereinigt. Verbleibende Byte: %d\n", forwardBufPos);
}

// Callback-Funktion für empfangene ESP-NOW Pakete
void onDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.printf("ESP-NOW: Daten empfangen von %s, Länge: %d Bytes\n", macStr, len);
  
  // Hier kannst du die empfangenen Daten weiter verarbeiten
}

void setup() 
{
  Serial.begin(115200); // Set console baud rate

  Serial.println("Start Sketch");
  // LTE-Initialisierung (bestehender Code)
  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);

  //GPS Serial port
  SerialGPS.begin(9600, SERIAL_8N1, BOARD_GPS_RX_PIN, BOARD_GPS_TX_PIN);
  
  while (!SerialGPS) {
    Serial.print(".");
  }
  Serial.println("GPS ready");

  delay(5000);

#ifdef TRANSPORT_WIFI
  // WiFi-Verbindung aufbauen
  Serial.print("Verbinde mit WiFi: ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi verbunden, IP-Adresse: ");
  Serial.println(WiFi.localIP());
#else  // TRANSPORT_LTE


#ifdef BOARD_POWERON_PIN
  pinMode(BOARD_POWERON_PIN, OUTPUT);
  digitalWrite(BOARD_POWERON_PIN, HIGH);
#endif

  // Set modem reset pin ,reset modem
  pinMode(MODEM_RESET_PIN, OUTPUT);
  digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL); delay(100);
  digitalWrite(MODEM_RESET_PIN, MODEM_RESET_LEVEL); delay(2600);
  digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);

  pinMode(BOARD_PWRKEY_PIN, OUTPUT);
  digitalWrite(BOARD_PWRKEY_PIN, LOW);
  delay(100);
  digitalWrite(BOARD_PWRKEY_PIN, HIGH);
  delay(100);
  digitalWrite(BOARD_PWRKEY_PIN, LOW);

  // Check if the modem is online
  Serial.println("Start modem...");

  int retry = 0;
  while (!modem.testAT(1000)) {
      Serial.print(".");
      if (retry++ > 10) {
          digitalWrite(BOARD_PWRKEY_PIN, LOW);
          delay(100);
          digitalWrite(BOARD_PWRKEY_PIN, HIGH);
          delay(1000);
          digitalWrite(BOARD_PWRKEY_PIN, LOW);
          retry = 0;
      }
  }
  Serial.println();

  // Check if SIM card is online
  SimStatus sim = SIM_ERROR;
  while (sim != SIM_READY) {
      sim = modem.getSimStatus();
      switch (sim) {
      case SIM_READY:
          Serial.println("SIM card online");
          break;
      case SIM_LOCKED:
          Serial.println("The SIM card is locked. Please unlock the SIM card first.");
          modem.simUnlock(SIMCARD_PIN_CODE);
          break;
      case !SIM_LOCKED:
          Serial.println("SIM Unlocked");
          break;
      default:
          break;
      }
      delay(2000);
  }

  // Get model info
  modem.sendAT("+SIMCOMATI");
  modem.waitResponse();

  //SIM7672G Can't set network mode
#ifndef TINY_GSM_MODEM_SIM7672
  if (!modem.setNetworkMode(MODEM_NETWORK_AUTO)) {
      Serial.println("Set network mode failed!");
  }
  String mode = modem.getNetworkModes();
  Serial.print("Current network mode : ");
  Serial.println(mode);
#endif

#ifdef NETWORK_APN
  Serial.printf("Set network apn : %s\n", NETWORK_APN);
  modem.sendAT(GF("+CGDCONT=1,\"IP\",\""), NETWORK_APN, "\"");
  if (modem.waitResponse() != 1) {
      Serial.println("Set network apn error !");
  }
#endif


  // Check network registration status and network signal status
  int16_t sq ;
  Serial.print("Wait for the modem to register with the network.");
  RegStatus status = REG_NO_RESULT;
  while (status == REG_NO_RESULT || status == REG_SEARCHING || status == REG_UNREGISTERED) {
      status = modem.getRegistrationStatus();
      switch (status) {
      case REG_UNREGISTERED:
      case REG_SEARCHING:
          sq = modem.getSignalQuality();
          Serial.printf("[%lu] Signal Quality:%d\n", millis() / 1000, sq);
          delay(1000);
          break;
      case REG_DENIED:
          Serial.println("Network registration was rejected, please check if the APN is correct");
          return ;
      case REG_OK_HOME:
          Serial.println("Online registration successful");
          break;
      case REG_OK_ROAMING:
          Serial.println("Network registration successful, currently in roaming mode");
          break;
      default:
          Serial.printf("Registration Status:%d\n", status);
          delay(1000);
          break;
      }
  }
  Serial.println();


  Serial.printf("Registration Status:%d\n", status);
  delay(1000);

  String ueInfo;
  if (modem.getSystemInformation(ueInfo)) {
      Serial.print("Inquiring UE system information:");
      Serial.println(ueInfo);
  }

  if (!modem.setNetworkActive()) {
      Serial.println("Enable network failed!");
  }

  delay(5000);

  String ipAddress = modem.getLocalIP();
  Serial.print("Network IP:"); Serial.println(ipAddress);


#endif

  // --- Servos initialisieren ---
  servoSteering.attach(steeringPin, 1000, 2000);  // Minimale und maximale Pulsbreite in µs
  servoDrive.attach(drivePin);
  // Beide Servos auf Neutral (90°) setzen
  servoSteering.write(90);
  servoDrive.writeMicroseconds(500);

  // Remote TCP-Verbindung zum Server aufbauen:
  Serial.print("Verbinde zu Remote Control Server ");
  Serial.print(remoteServerControls);
  Serial.print(":");
  Serial.println(remotePortControls);
  if (remoteClientControls.connect(remoteServerControls, remotePortControls)) {
    Serial.println("TCP Verbindung zum Remote-Control-Server hergestellt");
  } else {
    Serial.println("TCP Verbindung zum Remote-Control-Server fehlgeschlagen");
  }

  // Stellen Sie sicher, dass WiFi in STA-Modus ist – ESP‑NOW benötigt das.
  WiFi.mode(WIFI_STA);
  // Zeige die eigene MAC-Adresse im seriellen Monitor an:
  String mac = WiFi.macAddress();
  Serial.println("Eigene MAC-Adresse: " + mac);
  
  // Initialisieren Sie ESP‑NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Fehler bei der Initialisierung von ESP-NOW");
  }
  else {
    // esp_now_register_recv_cb(onCamStreamReceive);
    // Serial.println("ESP-NOW Camstream Empfang initialisiert");
  }
  esp_now_register_recv_cb(onDataRecv);
  int getTotalPeerCount();
  Serial.print("Total Peer Count: ");
  delay(2000);
}

void loop() {
  // Verarbeiten Sie periodisch die im Puffer gesammelten Camstream-Daten
  if (forwardBufPos > 0) {
    processForwardBuffer();
  }

  // Sende Steuerbefehle etc. per Remote-Client (bestehender Code):
  if (remoteClientControls.available() >= 3) {
    uint8_t startByte = remoteClientControls.read();
    if (startByte != 0x02) {
      Serial.println("Fehlerhaftes Startbyte");
      return;
    }
    uint8_t steeringVal = remoteClientControls.read();
    uint8_t driveVal = remoteClientControls.read();
    
    driveVal = constrain(driveVal, 0, 200);
    steeringVal = constrain(steeringVal, 0, 100);
    
    Serial.print("Raw Steering: ");
    Serial.print(steeringVal);
    Serial.print(" | Raw Drive: ");
    Serial.println(driveVal);

    int steeringAngle = map(steeringVal, 0, 100, 0, 180);
    int drivePulse = map(driveVal, 0, 100, 1000, 2000);
  
    Serial.print("Mapped Steering Angle: ");
    Serial.print(steeringAngle);
    Serial.print(" | Mapped Drive Pulse: ");
    Serial.println(drivePulse);
  
    servoSteering.write(steeringAngle);
    servoDrive.writeMicroseconds(drivePulse);
  }

  // Falls Remote Verbindung getrennt -> Wieder verbinden:
  if (!remoteClientControls.connected()) {
    Serial.println("TCP Verbindung verloren. Versuche neu zu verbinden...");
    remoteClientControls.stop();
    if (remoteClientControls.connect(remoteServerControls, remotePortControls)) {
      Serial.println("TCP Verbindung wiederhergestellt");
    } else {
      Serial.println("Reconnection fehlgeschlagen. Warte 5 Sekunden...");
      delay(5000);
      return;
    }
  }

  // Periodisch zusätzlich die mqtt_message (aus displayInfo) senden:
  static unsigned long lastMsgSend = 0;
  if(millis() - lastMsgSend > 1000) { // z.B. jede Sekunde
    if (SerialGPS.available()) {
      int c = SerialGPS.read();
      // Serial.write(c);     // Debug gps nmae message output to serial
      if (gps.encode(c)) {
        String infoMsg = displayInfo();  // liefert mqtt_message
        remoteClientControls.print(infoMsg);
        lastMsgSend = millis();
      } 
    
  }

}
}

String displayInfo(){

  // Standortdaten abrufen
  if (gps.location.isValid()) {
    location = String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6);
    mqtt_message = location;
  } else {
    location = "INVALID";
  }
  // Datum und Uhrzeit abrufen
  if (gps.date.isValid()) {
    date_time = String(gps.date.year()) + "-" + String(gps.date.month()) + "-" + String(gps.date.day());
  } else {
    date_time = "INVALID";
    }
    if (gps.time.isValid()) {
    int hr = gps.time.hour();
    int min = gps.time.minute();
    int sec = gps.time.second();
    int csec = gps.time.centisecond();

    String hrStr = (hr < 10 ? "0" : "") + String(hr);
    String minStr = (min < 10 ? "0" : "") + String(min);
    String secStr = (sec < 10 ? "0" : "") + String(sec);
    String csStr  = (csec < 10 ? "0" : "") + String(csec);

    gpsTime = hrStr + ":" + minStr + ":" + secStr + "." + csStr;
    } else {
    gpsTime = "INVALID";
    }
  // MQTT-Nachricht erstellen
  mqtt_message = "";
  debug_message = "Location: " + location + "  Date/Time: " + date_time + " " + gpsTime;
  
  return mqtt_message;
}