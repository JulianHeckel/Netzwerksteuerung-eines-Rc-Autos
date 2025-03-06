#include <ESP32Servo.h>

static const int servoPin = 18;

Servo myservo;  // create servo object to control a servo

void setup() {
  Serial.begin(115200);
	myservo.attach(servoPin);
}

void loop() {
  for(int posDegrees = 0; posDegrees <= 180; posDegrees++) {
    myservo.write(posDegrees);
    Serial.println(posDegrees);
    delay(20);
  }

  for(int posDegrees = 180; posDegrees >= 0; posDegrees--) {
    myservo.write(posDegrees);
    Serial.println(posDegrees);
    delay(20);
  }
}

