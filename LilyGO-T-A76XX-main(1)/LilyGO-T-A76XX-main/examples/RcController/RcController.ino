#define Serial Serial

#define SerialAT Serial1

#define SerialGPS Serial2

#define BOARD_GPS_TX_PIN                    21
#define BOARD_GPS_RX_PIN                    22
#define BOARD_GPS_PPS_PIN                   23
#define BOARD_GPS_WAKEUP_PIN                19

#include "utilities.h"
#include <WiFi.h>
#include <stdint.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include <TinyGsm.h>
#include <TinyGPS++.h>
#include "Arduino.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_now.h>   // Für ESPNow
#include <TinyGsmClient.h>

// --- WiFi- und MQTT-Konfiguration ---
const char* ssid = "ESP32Cam_AP";//"Bratiphone";//"ASUS-RCCAR"; //"Internetz"; //"HotspotTest"
const char* password = "Esp32CamPass";//"dhbwka04042025"; //"BratenDackel!";//"91369823410622158981"; //"NichtMartin"
const char* mqtt_server = "sv02.tobicloud.eu";
#define NETWORK_APN     "web.vodafone.de"    // APN für Vodafone Deutschland

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


// --- ESP32Cam Stream Server ---
const char* ESP32Cam_IP = "192.168.4.1";
const uint16_t ESP32Cam_PORT = 5001;
WiFiClient camStream; // Global client for the ESP32Cam stream

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

// --- MQTT-Client ---
TinyGsm modem(SerialAT);
WiFiClient espClient;
PubSubClient mqttClient(espClient);
Servo servoSteering;
Servo servoDrive;
TinyGPSPlus gps;

TinyGsmClient remoteClientControls(modem);  // Globaler TCP-Client, der sich über LTE zum Cloud-Server verbindet

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

// Globaler Puffer, um eingehende Daten vom CamStream zu sammeln
#define FORWARD_BUFFER_SIZE 16384    // Passen Sie die Größe ggf. an
uint8_t forwardBuffer[FORWARD_BUFFER_SIZE];
size_t forwardBufPos = 0;

// Hilfsfunktion, die in einem Buffer nach einem Pattern sucht
int findPattern(const uint8_t *buffer, size_t bufLen, const char *pattern) {
  size_t patLen = strlen(pattern);
  for (size_t i = 0; i <= bufLen - patLen; i++) {
    if (memcmp(buffer + i, pattern, patLen) == 0) {
      return i;
    }
  }
  return -1; // nicht gefunden
}

// Diese Funktion durchsucht den forwardBuffer nach einem kompletten Envelope.
// Erwartet wird folgender Aufbau:
//   Header: "FRAME_START <JPEG-Datenlänge>\n"
//   Direkt gefolgt von genau <JPEG-Datenlänge> Bytes JPEG-Daten
//   Trailer: "FRAME_END\n"
void processForwardBuffer() {
  const char *headerTag = "FRAME_START ";
  int headerPos = findPattern(forwardBuffer, forwardBufPos, headerTag);
  if (headerPos < 0) return; // Kein Header gefunden
  
  // Suche Ende der Headerzeile (erkennbar an '\n')
  int headerEnd = -1;
  for (size_t i = headerPos; i < forwardBufPos; i++) {
    if (forwardBuffer[i] == '\n') {
      headerEnd = i;
      break;
    }
  }
  if (headerEnd < 0) return; // Header noch nicht vollständig


  
  // Extrahiere Headerzeile als C-String
  char headerLine[64];
  size_t copyLen = min((size_t)sizeof(headerLine)-1, (size_t)(headerEnd - headerPos));
  memcpy(headerLine, forwardBuffer + headerPos, copyLen);
  headerLine[copyLen] = '\0';
  
  // Der Header sollte z.B. "FRAME_START 3496" enthalten. Parsen der erwarteten JPEG-Datenlänge:
  int expectedJPEGSize = atoi(headerLine + strlen(headerTag));
  if (expectedJPEGSize <= 0) {
    Serial.print("Ungültiger erwarteter Frame (");
    Serial.print(expectedJPEGSize);
    Serial.println(" Bytes)");
    // Falls der Header ungültig ist, verschiebe den Buffer weiter:
    size_t removeBytes = headerEnd + 1;
    memmove(forwardBuffer, forwardBuffer + removeBytes, forwardBufPos - removeBytes);
    forwardBufPos -= removeBytes;
    return;
  }
  
  Serial.print("Erwarte Frame mit ");
  Serial.print(expectedJPEGSize);
  Serial.println(" Bytes");
  
  // Daten beginnen direkt nach dem Header-Zeilenumbruch
  int dataStart = headerEnd + 1;
  // Trailer (als Zeichenkette) sollte exakt nach dem JPEG-Datenblock stehen:
  const char *trailerTag = "FRAME_END\n";
  int trailerLen = strlen(trailerTag);
  
  // Prüfen, ob bereits alle JPEG-Daten und der Trailer im Buffer vorhanden sind
  if (dataStart + expectedJPEGSize + trailerLen > (int)forwardBufPos) {
    return; // noch nicht alles angekommen
  }
  
  // Überprüfen, ob der Trailer an der erwarteten Stelle steht:
  if (memcmp(forwardBuffer + dataStart + expectedJPEGSize, trailerTag, trailerLen) != 0) {
    Serial.println("Trailer passt nicht.");
    // Falls nicht, entferne den Header bis zu headerPos+1 und versuche es erneut.
    size_t removeBytes = headerPos + 1;
    memmove(forwardBuffer, forwardBuffer + removeBytes, forwardBufPos - removeBytes);
    forwardBufPos -= removeBytes;
    return;
  }
  
  // Komplettpaket gefunden:
  int fullPacketSize = (dataStart + expectedJPEGSize + trailerLen) - headerPos;
  // Sende den gesamten Envelope über LTE
  if (remoteClientControls.connected()) {
    int sent = remoteClientControls.write(forwardBuffer + headerPos, fullPacketSize);
    Serial.print("LTE: Gesendetes komplettes Paket: ");
    Serial.print(sent);
    Serial.println(" Bytes");
    remoteClientControls.flush();
  } else {
    Serial.println("LTE-Client nicht verbunden, Paket nicht gesendet.");
    return;
  }
  
  // Entferne den gesendeten Frame aus dem Puffer
  size_t remainingBytes = forwardBufPos - (headerPos + fullPacketSize);
  memmove(forwardBuffer, forwardBuffer + headerPos + fullPacketSize, remainingBytes);
  forwardBufPos = remainingBytes;
}

void setup() 
{
  Serial.begin(115200); // Set console baud rate

  Serial.println("Start Sketch");

  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);

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

  //GPS Serial port
  SerialGPS.begin(9600, SERIAL_8N1, BOARD_GPS_RX_PIN, BOARD_GPS_TX_PIN);

  while (!SerialGPS) {
    Serial.print(".");
  }
  Serial.println("GPS ready");

  // --- Servos initialisieren ---
  servoSteering.attach(steeringPin, 1000, 2000);  // Minimale und maximale Pulsbreite in µs
  servoDrive.attach(drivePin);
  // Beide Servos auf Neutral (90°) setzen
  servoSteering.write(90);
  servoDrive.writeMicroseconds(500);

  // --- Remote TCP-Verbindung zum Cloud-Server aufbauen ---
  Serial.print("Verbinde zu Remote Control Server ");
  Serial.print(remoteServerControls);
  Serial.print(":");
  Serial.println(remotePortControls);
  if (remoteClientControls.connect(remoteServerControls, remotePortControls)) {
    Serial.println("TCP Verbindung zum Remote-Control-Server hergestellt");
  } else {
    Serial.println("TCP Verbindung zum Remote-Control-Server fehlgeschlagen");
  }

  #if USE_CAMERA
  // WiFi verbinden und Kamera-Stream aufbauen
  Serial.print("Verbinde mit WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("IP-Adresse: ");
  Serial.println(WiFi.localIP().toString());

  // Connect to the ESP32Cam stream as client
  Serial.print("Verbinde zum ESP32Cam Stream unter ");
  Serial.print(ESP32Cam_IP);
  Serial.print(":");
  Serial.println(ESP32Cam_PORT);
  if (camStream.connect(ESP32Cam_IP, ESP32Cam_PORT)) {
    Serial.println("ESP32Cam Stream verbunden.");
  } else {
    Serial.println("Verbindung zum ESP32Cam Stream fehlgeschlagen.");
  }
#endif

  // --- MQTT einrichten (wenn noch benötigt, z.B. für andere Aufgaben) ---
  //mqttClient.setServer(mqtt_server, 1883);
  //mqttClient.setCallback(mqttCallback);

  delay(2000);
}

void loop() {

#if USE_CAMERA
  // Sicherstellen, dass der CamStream (WiFiClient) verbunden ist …
  if (!camStream.connected()) {
    Serial.print("Verbinde zum ESP32Cam Stream unter ");
    Serial.print(ESP32Cam_IP);
    Serial.print(":");
    Serial.println(ESP32Cam_PORT);
    if (camStream.connect(ESP32Cam_IP, ESP32Cam_PORT)) {
      Serial.println("ESP32Cam Stream verbunden.");
    } else {
      Serial.println("Verbindung zum ESP32Cam Stream fehlgeschlagen.");
      delay(2000);
      return;
    }
  }
#endif

  // Sicherstellen, dass der LTE-Client verbunden ist …
  if (!remoteClientControls.connected()) {
    Serial.println("Remote Control TCP Verbindung verloren. Versuche neu zu verbinden...");
    remoteClientControls.stop();
    if (remoteClientControls.connect(remoteServerControls, remotePortControls)) {
      Serial.println("Remote Control TCP Verbindung wiederhergestellt");
    } else {
      Serial.println("Reconnection fehlgeschlagen. Warte 5 Sekunden...");
      delay(5000);
      return;
    }
  }
  
 
// Empfange genau 3 Bytes: Startbyte, Steering, Drive
if (remoteClientControls.available() >= 3) {
  // Lese Startbyte
  uint8_t startByte = remoteClientControls.read();
  if (startByte != 0x02) {
    Serial.println("Fehlerhaftes Startbyte");
    return;
  }
  // Lese Steering und Drive Werte
  uint8_t steeringVal = remoteClientControls.read();
  uint8_t driveVal = remoteClientControls.read();

  driveVal = constrain(driveVal, 0, 200);    // Werte begrenzen
  steeringVal = constrain(steeringVal, 0, 100);  // Werte begrenzen
  
  // Debug-Ausgabe der rohen Werte:
  Serial.print("Raw Steering: ");
  Serial.print(steeringVal);
  Serial.print(" | Raw Drive: ");
  Serial.println(driveVal);

  // Umrechnung der Werte:
  int steeringAngle = map(steeringVal, 0, 100, 0, 180);
  int drivePulse = map(driveVal, 0, 100, 1000, 2000);
  
  Serial.print("Mapped Steering Angle: ");
  Serial.print(steeringAngle);
  Serial.print(" | Mapped Drive Pulse: ");
  Serial.println(drivePulse);
  
  // Sende Steuerbefehle an die Servos:
  servoSteering.write(steeringAngle);
  servoDrive.writeMicroseconds(drivePulse);
}

if (!remoteClientControls.connected()) {
  Serial.println("GSM TCP Verbindung verloren. Versuche neu zu verbinden...");
  remoteClientControls.stop();
  if (remoteClientControls.connect(remoteServerControls, remotePortControls)) {
    Serial.println("GSM TCP Verbindung wiederhergestellt");
  } else {
    Serial.println("GSM Reconnection fehlgeschlagen. Warte 5 Sekunden...");
    delay(5000);
    return;
  }
}

// Forward des Kamerastreams vom WiFiClient an den GSM/LTE TCP-Client
if (camStream.connected() && remoteClientControls.connected()) {
  if (camStream.available() > 0) {
        // Lies verfügbare Daten vom CamStream in unseren Puffer
  while (camStream.available() > 0 && forwardBufPos < FORWARD_BUFFER_SIZE) {
    int n = camStream.read(forwardBuffer + forwardBufPos, FORWARD_BUFFER_SIZE - forwardBufPos);
    if (n > 0) {
      forwardBufPos += n;
    }
  }
  
  // Versuche, einen kompletten Envelope zu verarbeiten und weiterzuleiten
  processForwardBuffer();
  
  delay(5);  // kurze Pause
}
}
}

String displayInfo()
{

  // Standortdaten abrufen
  if (gps.location.isValid()) {
    location = String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6);
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
  mqtt_message = location;
  Serial.println(gpsTime);
  return mqtt_message;
}