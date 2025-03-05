#define LILYGO_T_A7670
#include <WiFi.h>  
#include <PubSubClient.h>  
#include "utilities.h"
#include <ESP32Servo.h>  
#include <TinyGPS++.h>

//Definition für den servo
Servo servo1;
int pos = 0;
const int servFrq = 50;
const int servPin = 19;
// Definieren Sie die Pulsbreite für die Positionen des Servo 
const int pulseNeutral = 1500; // Mittelstellung des Servos (in Mikrosekunden)  
const int pulseLeft = 1000;    // Pulsbreite für die äußerste linke Position  
const int pulseRight = 2000;   // Pulsbreite für die äußerste rechte Position 

// Definieren Sie die GPS-Modul-Kommunikationspins  
static const int RXPin = 22, TXPin = 21;  
static const uint32_t GPSBaud = 9600;  // Definieren Sie die Baudrate für das GPS-Modul  
TinyGPSPlus gps;  // Initialisieren Sie das GPS-Modul  
HardwareSerial gpsSerial(1);  // Erstellen Sie eine HardwareSerial-Instanz namens "gpsSerial"  


// Definitionen für den ESC
const int pwmPin = 18; // Pin for PWM signal  
const int freq = 50; // Frequency in Hz (typical for servos and ESCs)  
const int channel = 1; // PWM channel  
const int resolution = 16; // Resolution in bits (0-65535 for 16-bit resolution)  
const int maxDutyCycle = 65535; // Maximum duty cycle for 16-bit resolution  
const float timerPeriod = (1.0 / freq) * 1000.0; // PWM period in milliseconds  
const int dutyCycle1ms = 32768; // PWM duty cycles for 1ms, 1.5ms, and 2ms  
const int dutyCycle1_5ms = 49152;  
const int dutyCycle2ms = 65535;  

// WiFi credentials  
const char* ssid = "HUAWEI MediaPad M5";  
const char* password = "1d1f476775c5";  
  
// MQTT Broker  
const char* mqtt_username = "yourMQTTusername"; // If the broker requires authentication  
const char* mqtt_password = "yourMQTTpassword"; // If the broker requires authentication  
const char* mqtt_sendGps = "gps/data";  
const char* mqtt_recESC = "ESC/steering";
const char* mqtt_server = "192.168.43.145";

WiFiClient espClient;  
PubSubClient client(espClient);  
unsigned long signalEndTime = 0; // Variable zum Speichern des Endzeitpunkts für das Signal  
int currentDutyCycle = dutyCycle1_5ms; // Starten Sie mit der neutralen Pulsbreite  

void setup() {  
  Serial.begin(115200);

  servo1.attach(servPin, pulseLeft, pulseRight);
  centerServo();

  ledcAttachChannel(pwmPin, freq, resolution, channel);
  
  // Initialize with neutral signal  
  ledcWrite(channel, dutyCycle1_5ms);  
  WiFi.begin(ssid, password);  
  while (WiFi.status() != WL_CONNECTED) {  
    delay(500);  
    Serial.println("Connecting to WiFi...");  
  }  
  Serial.println("Connected to WiFi");   
  Serial.println("Neutral");  

  client.setServer(mqtt_server, 1883);  
  client.setCallback(callback); 

  while (!client.connected()) {  
    Serial.println("Connecting to MQTT...");  
    if (client.connect("ESP32Client")) {  
      Serial.println("Connected to MQTT Broker!");  
      if (client.subscribe(mqtt_recESC)){
        Serial.println("Client subscribed to:");
        Serial.println(mqtt_recESC);
      }
    } else {  
      Serial.print("Failed with state ");  
      Serial.print(client.state());  
      delay(2000);  
    }  
  }   
}

// Callback function to receive messages  
void callback(char* topic, byte* payload, unsigned int length) {  
  Serial.print("Message received: ");  
  String message;  
  for (int i = 0; i < length; i++) {  
    message += (char)payload[i];  
  }  
  Serial.println(message);  
  
  // Convert the message to an integer value    
  int value = message.toInt();  
  
  // Check the value and set the motor direction and speed  
  // Map the value to the PWM duty cycle range (1ms to 2ms)  
  int currentDutyCycle = map(60, -100, 100, dutyCycle1ms, dutyCycle2ms);    
  ledcWrite(1, currentDutyCycle); 


  int servoPos = map(value, 0, 100, pulseLeft, pulseRight);
  servo1.write(servoPos);
  
  // Output the current motor state  
  if (value == 0) {    
      Serial.println("Neutral");    
  } else if (value < 0) {    
      Serial.print("Reverse at ");    
      Serial.print(map(value, -100, 1, 100, 0));  
      Serial.println("% power");    
  } else if (value > 0) {    
      Serial.print("Forward at ");    
      Serial.print(map(value, 1, 100, 0, 100));  
      Serial.println("% power");    
  }
}
    
void centerServo(){
  servo1.write(90);
}

void reconnect() {
  while (!client.connected()) {
    if (client.connect("ESP32Client")) {
      client.subscribe(mqtt_recESC);
    } else {
    }
  }
} 
  
void loop() {  
  while (gpsSerial.available() > 0) {  
    char c = gpsSerial.read();  
    if (gps.encode(c)) { // Wenn ein neues Satz von GPS-Daten verfügbar ist  
      if (gps.location.isValid()) {  
        String payload = "Latitude: " + String(gps.location.lat(), 6) + ", Longitude: " + String(gps.location.lng(), 6);  
        if (!client.connected()) {  
          reconnect();  
        }  
        client.publish(mqtt_sendGps, payload.c_str()); // Veröffentlichen Sie die GPS-Daten  
      }  
    }
  
  }
  //Servo sweep for testing
  for (pos = 0; pos <= 180; pos +=1) {
    servo1.write(pos);
    delay(15);
  }
  for (pos = 180; pos >= 0; pos -=1) {
    servo1.write(pos);
    delay(15);
  }

  // Überprüfen Sie, ob die Zeit für das gehaltene Signal abgelaufen ist  
  if (millis() >= signalEndTime && signalEndTime != 0) {  
    ledcWrite(channel, dutyCycle1_5ms); // Setze den PWM-Duty-Cycle auf Neutral  
    signalEndTime = 0; // Setzen Sie die Endzeit zurück, um zu signalisieren, dass kein Signal mehr gehalten wird  
  } 
  delay(500);
  client.loop();
}

