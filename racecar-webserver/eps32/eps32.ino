#include <WiFi.h>
#include <PubSubClient.h>
#include <Servo.h>
#include <TinyGPS++.h>
#include "utilities.h"  // Board-spezifische Definitionen für LILYGO_T_A7670

// --- WiFi- und MQTT-Konfiguration ---
const char* ssid = "HUAWEI MediaPad M5";
const char* password = "1d1f476775c5";
const char* mqtt_server = "192.168.43.145";

// MQTT-Themen für Steuerbefehle und Telemetrie
const char* mqtt_topic_steering = "racecar/steering";
const char* mqtt_topic_drive    = "racecar/drive";
const char* mqtt_topic_telemetry = "racecar/telemetry";
const char* mqtt_topic_control  = "racecar/control";  // Neues Thema für Steuerbefehle

// --- Servo-Konfiguration ---
// Ein Servo für die Lenkung, ein Servo für Gas/Bremse
const int steeringPin = 19;  // Lenkservo
const int drivePin    = 18;  // Gas-/Bremservo

Servo servoSteering;
Servo servoDrive;

// --- GPS-Konfiguration ---
static const int RXPin = 22, TXPin = 21;
static const uint32_t GPSBaud = 9600;
TinyGPSPlus gps;
HardwareSerial gpsSerial(1);

// --- MQTT-Client ---
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Intervall für Telemetrie-Publikation (in ms)
unsigned long lastTelemetryPublish = 0;
const unsigned long telemetryInterval = 1000; // z. B. alle 1 Sekunde

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
    if (message == "w") {          // Vorwärts: Gas geben
      servoDrive.write(180);
      Serial.println("Drive Servo: Vorwärts (180°)");
    } else if (message == "s") {   // Rückwärts: Bremsen
      servoDrive.write(0);
      Serial.println("Drive Servo: Rückwärts/Bremsen (0°)");
    } else if (message == "a") {   // Links lenken
      servoSteering.write(0);
      Serial.println("Steering Servo: Links (0°)");
    } else if (message == "d") {   // Rechts lenken
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
  }
  else if (String(topic) == mqtt_topic_drive) {
    int value = message.toInt();
    int angle = map(value, 0, 100, 0, 180);
    servoDrive.write(angle);
    Serial.print("Drive Servo Winkel: ");
    Serial.println(angle);
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

void setup() {
  Serial.begin(115200);
  delay(1000);

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

  // --- Servos initialisieren ---
  servoSteering.attach(steeringPin, 1000, 2000);  // Minimale und maximale Pulsbreite in µs
  servoDrive.attach(drivePin, 1000, 2000);
  // Beide Servos auf Neutral (90°) setzen
  servoSteering.write(90);
  servoDrive.write(90);

  // --- MQTT einrichten ---
  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setCallback(mqttCallback);

  // --- GPS initialisieren ---
  gpsSerial.begin(GPSBaud, SERIAL_8N1, RXPin, TXPin);
}

void loop() {
  // MQTT-Verbindung sicherstellen
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();

  // GPS-Daten einlesen
  while (gpsSerial.available() > 0) {
    char c = gpsSerial.read();
    gps.encode(c);
  }

  // Telemetrie-Daten (z. B. Standort und Geschwindigkeit) periodisch veröffentlichen
  if (millis() - lastTelemetryPublish >= telemetryInterval) {
    if (gps.location.isValid()) {
      float lat = gps.location.lat();
      float lng = gps.location.lng();
      float speed = gps.speed.kmph();  // Geschwindigkeit in km/h

      // Erstelle einen JSON-String für die Telemetrie
      String telemetry = "{\"lat\":";
      telemetry += String(lat, 6);
      telemetry += ",\"lng\":";
      telemetry += String(lng, 6);
      telemetry += ",\"speed\":";
      telemetry += String(speed, 2);
      telemetry += "}";
      
      mqttClient.publish(mqtt_topic_telemetry, telemetry.c_str());
      Serial.print("Telemetrie: ");
      Serial.println(telemetry);
    }
    lastTelemetryPublish = millis();
  }
}
