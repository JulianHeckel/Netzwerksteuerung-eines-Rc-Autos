#define SerialMon Serial

#define SerialGPS Serial2


#define BOARD_GPS_TX_PIN                    21
#define BOARD_GPS_RX_PIN                    22
#define BOARD_GPS_PPS_PIN                   23
#define BOARD_GPS_WAKEUP_PIN                19


#include "utilities.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include <TinyGsm.h>
#include <TinyGPS++.h>
#include "Arduino.h"


// --- WiFi- und MQTT-Konfiguration ---
const char* ssid = "Bratiphone";
const char* password = "BratenDackel!";
const char* mqtt_server = "sv02.tobicloud.eu";
const char apn[] = "web.vodafone.de";  // APN für Vodafone Deutschland

#define TINY_GSM_USE_GPRS false
#define GSM_PIN "5579"  // PIN für SIM-Karte, falls erforderlich

// MQTT-Themen für Steuerbefehle und Telemetrie
const char* mqtt_topic_steering = "racecar/steering";
const char* mqtt_topic_drive = "racecar/drive";
const char* mqtt_topic_telemetry = "racecar/telemetry";
const char* mqtt_topic_control = "racecar/control";  // Neues Thema für Steuerbefehle
const char* topicLog = "racecar/log";  // Thema für Log-Nachrichten

// --- Servo-Konfiguration ---
// Ein Servo für die Lenkung, ein Servo für Gas/Bremse
const int steeringPin = 19;  // Lenkservo
const int drivePin = 18;     // Gas-/Bremservo

String location;  // GPS-Standort als String
String date_time;  // Datum und Uhrzeit als String
String gpsTime;  // Uhrzeit als String
String mqtt_message;  // MQTT-Nachricht als String

//tmp
int counter = 0;



// --- MQTT-Client ---
TinyGsm modem(SerialAT);
WiFiClient espClient;
PubSubClient mqttClient(espClient);
Servo servoSteering;
Servo servoDrive;
TinyGPSPlus gps;

// --- MQTT Callback ---
// Hier werden Befehle zum Steuern der Servos empfangen.
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = String((char*)payload).substring(0, length);
  Serial.print("MQTT Nachricht auf ");
  Serial.print(topic);
  Serial.print(": ");
  Serial.println(message);

  // Befehle für Steuerung aus textuellen Befehlen
  if (String(topic) == mqtt_topic_control) {
    message.trim();
    if (message == "w") {  // Vorwärts: Gas geben
      servoDrive.write(180);
      Serial.println("Drive Servo: Vorwärts (180°)");
    } else if (message == "s") {  // Rückwärts: Bremsen
      servoDrive.write(0);
      Serial.println("Drive Servo: Rückwärts/Bremsen (0°)");
    } else if (message == "a") {  // Links lenken
      servoSteering.write(0);
      Serial.println("Steering Servo: Links (0°)");
    } else if (message == "d") {  // Rechts lenken
      servoSteering.write(180);
      Serial.println("Steering Servo: Rechts (180°)");
    } else if (message == "stop") {
      servoDrive.write(90);
      servoSteering.write(90);
      Serial.println("Beide Servos: Neutral (90°)");
    }
  }
  // Bestehende Steuerung über numerische Werte (falls verwendet)
  else if (String(topic) == mqtt_topic_steering) {
    int value = message.toInt();
    int angle = map(value, 0, 100, 0, 180);
    servoSteering.write(angle);
    Serial.print("Lenkwinkel: ");
    Serial.println(angle);
  } else if (String(topic) == mqtt_topic_drive) {
    int value = message.toInt();
    int angle = map(value, -100, 100, 1000, 2000);  // Pulsbreite für Gas/Bremse
    servoDrive.write(angle);
    Serial.print("Drive Servo: ");
    Serial.println(angle);
  }
}

void logMessage(const String &msg) {
  SerialMon.println(msg);
  if (mqttClient.connected()) {
    mqttClient.publish(topicLog, msg.c_str());
  }
}

// --- MQTT Reconnect-Funktion ---
void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Versuche MQTT-Verbindung...");
    if (mqttClient.connect("ESP32RaceCarClient")) {
      Serial.println("verbunden");
      mqttClient.subscribe(mqtt_topic_steering);
      mqttClient.subscribe(mqtt_topic_drive);
      mqttClient.subscribe(mqtt_topic_control);  // Abonniere zusätzlich das Control-Thema
    } else {
      Serial.print("Fehler, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" - Neuer Versuch in 5 Sekunden");
      delay(5000);
    }
  }
}

void setup() 
{
  pinMode(BOARD_POWERON_PIN, OUTPUT);
  digitalWrite(BOARD_POWERON_PIN, HIGH);

  pinMode(MODEM_RESET_PIN, OUTPUT);
  digitalWrite(MODEM_RESET_PIN, LOW);

  SerialMon.begin(115200);
  //Modem Serial port
  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  //GPS Serial port
  SerialGPS.begin(9600, SERIAL_8N1, BOARD_GPS_RX_PIN, BOARD_GPS_TX_PIN);

  delay(2000);

  while (!SerialAT) {
    SerialMon.print(".");
  }
  SerialMon.println("Modem ready");
  pinMode(BOARD_PWRKEY_PIN, OUTPUT);
  digitalWrite(BOARD_PWRKEY_PIN, LOW);
  delay(100);
  digitalWrite(BOARD_PWRKEY_PIN, HIGH);
  delay(1000);
  digitalWrite(BOARD_PWRKEY_PIN, LOW);

  while (!SerialGPS) {
    SerialMon.print(".");
  }
  SerialMon.println("GPS ready");

  String modemName = "UNKNOWN";
  while (true) {
    modemName = modem.getModemName();
    if (modemName != "UNKNOWN") {
      break;
    }
    SerialMon.print(".");
  }
  // Get model info
  modem.sendAT("+SIMCOMATI");
  modem.waitResponse();
  
#if TINY_GSM_USE_GPRS
    if(strlen(GSM_PIN) > 0 && modem.getSimStatus() != 3){
      logMessage("SIM card locked. Unlocking...");
      modem.simUnlock(GSM_PIN);
    }

  logMessage("waiting for network...");
  while (!modem.waitForNetwork(180000L, true)) {
    SerialMon.print(".");
  }
  SerialMon.println("Network connected");
  logMessage("Connecting to APN...");
  if (!modem.gprsConnect(apn)) {
    SerialMon.println("GPRS connection failed");
    while (true);    
  }  
  SerialMon.println("GPRS connected");
#endif




  

  // --- Servos initialisieren ---
  servoSteering.attach(steeringPin, 1000, 2000);  // Minimale und maximale Pulsbreite in µs
  servoDrive.attach(drivePin);
  // Beide Servos auf Neutral (90°) setzen
  servoSteering.write(90);
  servoDrive.writeMicroseconds(500);

  // --- WiFi verbinden ---
  Serial.print("Verbinde mit WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("IP-Adresse: ");
  Serial.println(WiFi.localIP());



  // --- MQTT einrichten ---
  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setCallback(mqttCallback);

  delay(500);

}

void loop() 
{
  // MQTT-Verbindung sicherstellen
  if (mqttClient.connected()) {
    mqttClient.loop();
  } else {
    reconnectMQTT();
  }

  // GPS-Daten einlesen
  int c = SerialGPS.read();
  // Serial.write(c);     // Debug gps nmae message output to serial
  if (gps.encode(c)) {
      displayInfo();
      mqttClient.publish(mqtt_topic_telemetry, mqtt_message.c_str());
  }



  if (SerialAT.available()) {
    Serial.write(SerialAT.read());
  }
  if (Serial.available()) {
    SerialAT.write(Serial.read());
  }
  delay(1);
  
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
  mqtt_message = "Location: " + location + "  Date/Time: " + date_time + " " + gpsTime;
  Serial.println(mqtt_message);
  return mqtt_message;
}
