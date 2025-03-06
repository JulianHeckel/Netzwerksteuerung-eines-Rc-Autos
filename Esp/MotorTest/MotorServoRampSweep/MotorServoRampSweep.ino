#define LILYGO_T_A7670
#include <ESP32Servo.h>
#include <ESC.h>

#define ESC_PIN (18)
#define SPEED_MIN (1500)                                  // Set the Minimum Speed in microseconds
#define SPEED_MAX (2000)                                  // Set the Minimum Speed in microseconds
ESC myESC (ESC_PIN, SPEED_MIN, SPEED_MAX, 500);
int oESC; 

static const int servoPin = 19;
Servo myservo;


void setup() {
  Serial.begin(115200);

  myESC.arm(); // ESC arming sequence
  delay(5000); // Wartezeit zum Arming

  myservo.setPeriodHertz(100);
  myservo.attach(servoPin, 1000, 2000);
}

void loop() {
  for (oESC = SPEED_MIN; oESC <= SPEED_MAX; oESC += 1) {  // goes from 1000 microseconds to 2000 microseconds
    myESC.speed(oESC);                                    // tell ESC to go to the oESC speed value
    delay(10);                                            // waits 10ms for the ESC to reach speed
  }
  delay(100);
  for (oESC = SPEED_MAX; oESC >= SPEED_MIN; oESC -= 1) {  // goes from 2000 microseconds to 1000 microseconds
    myESC.speed(oESC);                                    // tell ESC to go to the oESC speed value
    delay(10);                                            // waits 10ms for the ESC to reach speed  
   }
  delay(500);  
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