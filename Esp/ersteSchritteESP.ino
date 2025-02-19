#define LILYGO_T_A7670
#include <WiFi.h>  
#include <PubSubClient.h>  
#include "utilities.h"
#include <ESP32Servo.h>  

Servo servo1;

const int servFrq = 50;
const int servPin = 19;

// Definieren Sie die Pulsbreite für die Positionen  
const int pulseNeutral = 1500; // Mittelstellung des Servos (in Mikrosekunden)  
const int pulseLeft = 1000;    // Pulsbreite für die äußerste linke Position  
const int pulseRight = 2000;   // Pulsbreite für die äußerste rechte Position 

const int pwmPin = 18; // Pin for PWM signal  
const int freq = 50; // Frequency in Hz (typical for servos and ESCs)  
const int channel = 0; // PWM channel  
const int resolution = 16; // Resolution in bits (0-65535 for 16-bit resolution)  
const int maxDutyCycle = 65535; // Maximum duty cycle for 16-bit resolution  
const float timerPeriod = (1.0 / freq) * 1000.0; // PWM period in milliseconds  
  
// PWM duty cycles for 1ms, 1.5ms, and 2ms  
const int dutyCycle1ms = (int)((1.0 / timerPeriod) * maxDutyCycle);  
const int dutyCycle1_5ms = (int)((1.5 / timerPeriod) * maxDutyCycle);  
const int dutyCycle2ms = (int)((2.0 / timerPeriod) * maxDutyCycle);  

// WiFi credentials  
const char* ssid = "yourSSID";  
const char* password = "yourPASSWORD";  
  
// MQTT Broker  
const char* mqtt_broker = "broker.hivemq.com";  
const char* mqtt_username = "yourMQTTusername"; // If the broker requires authentication  
const char* mqtt_password = "yourMQTTpassword"; // If the broker requires authentication  
const char* mqtt_topic = "esp32/controls";  

WiFiClient espClient;  
PubSubClient client(espClient);  

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
  int dutyCycle = map(value, 0, 100, dutyCycle1ms, dutyCycle2ms);    
  ledcWrite(channel, dutyCycle); 

  int servoPos = map(value, 0, 100, 0, 180);
  servo1.writeMicroseconds(servoPos);
  
  // Output the current motor state  
  if (value == 50) {    
      Serial.println("Neutral");    
  } else if (value < 50) {    
      Serial.print("Reverse at ");    
      Serial.print(map(value, 0, 49, 0, 100));  
      Serial.println("% power");    
  } else if (value > 50) {    
      Serial.print("Forward at ");    
      Serial.print(map(value, 51, 100, 0, 100));  
      Serial.println("% power");    
  }
}
    
void setup() {  
  Serial.begin(115200);

  servo1.setPeriodHertz(servFrq);
  servo1.attach(servPin, pulseLeft, pulseRight);
  centerServo(servo1);

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
  client.setServer(mqtt_broker, 1883);  
  client.setCallback(callback);  
  while (!client.connected()) {  
    Serial.println("Connecting to MQTT...");  
    if (client.connect("ESP32Client", mqtt_username, mqtt_password)) {  
      Serial.println("Connected to MQTT Broker!");  
    } else {  
      Serial.print("Failed with state ");  
      Serial.print(client.state());  
      delay(2000);  
    }  
  }  
  client.subscribe(mqtt_topic);  
}  
  
void loop() {  
  client.loop(); // Maintain MQTT connection  
}  