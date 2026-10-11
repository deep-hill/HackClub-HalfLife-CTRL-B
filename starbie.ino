#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <TM1637Display.h>
#include <PCF8574.h>

// ============================================================
// PIN DEFINITIONS
// ============================================================

// ESP32 native pins
#define DHT_PIN       2   // D1 - DHT11 data
#define BTN1_PIN      3   // D2 - Button 1
#define BTN2_PIN      4   // D3 - Button 2
#define BTN3_PIN      7   // D6 - Button 3 (ultrasonic trigger)
#define LDR_PIN       1   // D0 - LDR analog
#define RGB_R_PIN     8   // D7
#define RGB_G_PIN     9   // D8
#define RGB_B_PIN     11  // D10

// PCF8574T pins (via I2C expander at address 0x20)
#define PCF_DISP_CLK  0   // P0 - TM1637 CLK
#define PCF_DISP_DIO  1   // P1 - TM1637 DIO
#define PCF_TRIG      2   // P2 - HC-SR04 TRIG
#define PCF_ECHO      3   // P3 - HC-SR04 ECHO
#define PCF_BUZZER    4   // P4 - Passive buzzer
#define PCF_VIB       5   // P5 - Vibration motor

// ============================================================
// SETTINGS
// ============================================================

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_ADDR     0x3C
#define PCF_ADDR      0x20
#define DHT_TYPE      DHT11

// Pet stats limits
#define STAT_MAX      100
#define STAT_MIN      0

// ============================================================
// OBJECTS
// ============================================================

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
DHT dht(DHT_PIN, DHT_TYPE);
PCF8574 pcf(PCF_ADDR);

// TM1637 uses PCF8574 pins so we bit-bang it manually
// We'll use a software approach since PCF8574 can't hardware-PWM

// ============================================================
// PET STATE
// ============================================================

int hunger    = 80;
int happiness = 80;
int energy    = 80;
int health    = 100;

float temperature = 0;
float humidity    = 0;
int lightLevel    = 0;
int lastDistance  = 0;

unsigned long lastStatDecay   = 0;
unsigned long lastSensorRead  = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastBlink       = 0;

bool petAsleep  = false;
bool petHappy   = false;
String petMood  = "HAPPY";

// ============================================================
// HELPER: SET RGB COLOR
// ============================================================

void setRGB(int r, int g, int b) {
  // Common cathode - HIGH = on
  analogWrite(RGB_R_PIN, r);
  analogWrite(RGB_G_PIN, g);
  analogWrite(RGB_B_PIN, b);
}

// Mood colors
void moodColor() {
  if (health < 20)        setRGB(255, 0, 0);      // red = critical
  else if (hunger < 20)   setRGB(255, 100, 0);    // orange = hungry
  else if (happiness < 20) setRGB(0, 0, 255);     // blue = sad
  else if (energy < 20)   setRGB(128, 0, 128);    // purple = tired
  else if (petAsleep)     setRGB(0, 0, 50);        // dim blue = sleeping
  else                    setRGB(0, 255, 0);        // green = happy
}

// ============================================================
// HELPER: BUZZER (via PCF8574 - on/off beep only)
// ============================================================

void beep(int times, int duration) {
  for (int i = 0; i < times; i++) {
    pcf.write(PCF_BUZZER, LOW);   // buzzer on
    delay(duration);
    pcf.write(PCF_BUZZER, HIGH);  // buzzer off
    delay(duration);
  }
}

// ============================================================
// HELPER: VIBRATION
// ============================================================

void vibrate(int duration) {
  pcf.write(PCF_VIB, HIGH);
  delay(duration);
  pcf.write(PCF_VIB, LOW);
}

// ============================================================
// HELPER: ULTRASONIC DISTANCE (via PCF8574)
// ============================================================

long getDistance() {
  // Send 10us pulse on TRIG
  pcf.write(PCF_TRIG, LOW);
  delayMicroseconds(2);
  pcf.write(PCF_TRIG, HIGH);
  delayMicroseconds(10);
  pcf.write(PCF_TRIG, LOW);

  // Read ECHO - time how long it stays HIGH
  // PCF8574 is slow for timing so we poll it
  long timeout = micros() + 30000; // 30ms timeout
  while (pcf.read(PCF_ECHO) == LOW) {
    if (micros() > timeout) return -1;
  }
  long startTime = micros();
  timeout = micros() + 30000;
  while (pcf.read(PCF_ECHO) == HIGH) {
    if (micros() > timeout) return -1;
  }
  long duration = micros() - startTime;

  // Convert to inches: duration / 148
  return duration / 148;
}

// ============================================================
// HELPER: DRAW PET FACE ON OLED
// ============================================================

void drawFace() {
  display.clearDisplay();

  if (petAsleep) {
    // Sleeping face - closed eyes
    display.fillCircle(64, 32, 20, WHITE);           // head
    display.drawLine(54, 30, 60, 30, BLACK);         // left closed eye
    display.drawLine(68, 30, 74, 30, BLACK);         // right closed eye
    display.drawCircle(64, 38, 4, BLACK);            // small mouth
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(80, 20);
    display.print("z");
    display.setCursor(88, 14);
    display.print("z");
    display.setCursor(96, 8);
    display.print("Z");
  } else if (health < 20 || hunger < 20) {
    // Sad/sick face
    display.fillCircle(64, 32, 20, WHITE);
    display.fillCircle(57, 28, 4, BLACK);            // left eye
    display.fillCircle(71, 28, 4, BLACK);
    // X eyes if critical
    if (health < 10) {
      display.drawLine(54, 25, 60, 31, BLACK);
      display.drawLine(60, 25, 54, 31, BLACK);
      display.drawLine(68, 25, 74, 31, BLACK);
      display.drawLine(74, 25, 68, 31, BLACK);
    }
    // sad mouth
    display.drawCircle(64, 42, 5, BLACK);
    display.fillRect(59, 37, 12, 6, WHITE);          // erase top half of circle
  } else if (happiness > 70) {
    // Happy face
    display.fillCircle(64, 32, 20, WHITE);
    display.fillCircle(57, 28, 4, BLACK);
    display.fillCircle(71, 28, 4, BLACK);
    // happy mouth
    display.drawCircle(64, 38, 6, BLACK);
    display.fillRect(58, 32, 13, 7, WHITE);          // erase top half
  } else {
    // Neutral face
    display.fillCircle(64, 32, 20, WHITE);
    display.fillCircle(57, 28, 3, BLACK);
    display.fillCircle(71, 28, 3, BLACK);
    display.drawLine(60, 38, 68, 38, BLACK);         // flat mouth
  }

  // Stats bar at bottom
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 56);
  display.print("H:");
  display.print(hunger);
  display.print(" E:");
  display.print(energy);
  display.print(" HP:");
  display.print(health);

  display.display();
}

// ============================================================
// HELPER: DISPLAY DISTANCE ON TM1637
// We bit-bang TM1637 through PCF8574
// ============================================================

// TM1637 bit-bang through PCF8574
void tm1637Start() {
  pcf.write(PCF_DISP_CLK, HIGH);
  pcf.write(PCF_DISP_DIO, HIGH);
  delayMicroseconds(2);
  pcf.write(PCF_DISP_DIO, LOW);
  delayMicroseconds(2);
  pcf.write(PCF_DISP_CLK, LOW);
}

void tm1637Stop() {
  pcf.write(PCF_DISP_CLK, LOW);
  pcf.write(PCF_DISP_DIO, LOW);
  delayMicroseconds(2);
  pcf.write(PCF_DISP_CLK, HIGH);
  delayMicroseconds(2);
  pcf.write(PCF_DISP_DIO, HIGH);
}

void tm1637WriteByte(uint8_t b) {
  for (int i = 0; i < 8; i++) {
    pcf.write(PCF_DISP_CLK, LOW);
    pcf.write(PCF_DISP_DIO, (b & 0x01));
    delayMicroseconds(2);
    pcf.write(PCF_DISP_CLK, HIGH);
    delayMicroseconds(2);
    b >>= 1;
  }
  // ACK
  pcf.write(PCF_DISP_CLK, LOW);
  pcf.write(PCF_DISP_DIO, HIGH);
  delayMicroseconds(2);
  pcf.write(PCF_DISP_CLK, HIGH);
  delayMicroseconds(2);
  pcf.write(PCF_DISP_CLK, LOW);
}

const uint8_t digitMap[] = {
  0x3F, // 0
  0x06, // 1
  0x5B, // 2
  0x4F, // 3
  0x66, // 4
  0x6D, // 5
  0x7D, // 6
  0x07, // 7
  0x7F, // 8
  0x6F  // 9
};

void showDistance(int inches) {
  // Display up to 4 digits
  uint8_t segs[4];
  segs[0] = (inches / 1000) % 10;
  segs[1] = (inches / 100)  % 10;
  segs[2] = (inches / 10)   % 10;
  segs[3] = inches % 10;

  // Leading zero suppression
  bool leading = true;
  for (int i = 0; i < 4; i++) {
    if (leading && segs[i] == 0 && i < 3) {
      segs[i] = 0x00; // blank
    } else {
      leading = false;
      segs[i] = digitMap[segs[i]];
    }
  }

  tm1637Start();
  tm1637WriteByte(0x40); // auto increment address
  tm1637Stop();

  tm1637Start();
  tm1637WriteByte(0xC0); // start at address 0
  for (int i = 0; i < 4; i++) tm1637WriteByte(segs[i]);
  tm1637Stop();

  tm1637Start();
  tm1637WriteByte(0x8F); // brightness max
  tm1637Stop();
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);

  // Pin modes
  pinMode(BTN1_PIN, INPUT_PULLUP);
  pinMode(BTN2_PIN, INPUT_PULLUP);
  pinMode(BTN3_PIN, INPUT_PULLUP);
  pinMode(LDR_PIN,  INPUT);
  pinMode(RGB_R_PIN, OUTPUT);
  pinMode(RGB_G_PIN, OUTPUT);
  pinMode(RGB_B_PIN, OUTPUT);

  // I2C
  Wire.begin();

  // PCF8574T init
  pcf.begin();
  pcf.write(PCF_BUZZER, HIGH);  // buzzer off by default
  pcf.write(PCF_VIB,    LOW);   // motor off
  pcf.write(PCF_TRIG,   LOW);   // ultrasonic trig low
  pcf.write(PCF_DISP_CLK, HIGH);
  pcf.write(PCF_DISP_DIO, HIGH);

  // OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED failed");
    while (1);
  }
  display.clearDisplay();
  display.display();

  // DHT11
  dht.begin();

  // Startup beep and vibrate
  beep(2, 100);
  vibrate(200);
  setRGB(0, 255, 0);

  Serial.println("Starbie booting up!");
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop() {
  unsigned long now = millis();

  // ---- Read sensors every 3 seconds ----
  if (now - lastSensorRead > 3000) {
    temperature = dht.readTemperature();
    humidity    = dht.readHumidity();
    lightLevel  = analogRead(LDR_PIN);
    lastSensorRead = now;

    // Affect pet based on environment
    if (temperature > 35) {
      health -= 2;
      Serial.println("Too hot!");
    }
    if (lightLevel < 500) {
      // Dark — pet gets sleepy
      energy -= 1;
    }
  }

  // ---- Stat decay every 30 seconds ----
  if (now - lastStatDecay > 30000) {
    hunger    = max(STAT_MIN, hunger - 3);
    happiness = max(STAT_MIN, happiness - 2);
    energy    = max(STAT_MIN, energy - 1);
    lastStatDecay = now;

    // Health drops if hunger is critical
    if (hunger < 10) health = max(0, health - 5);

    // Update mood
    if      (health < 20)    petMood = "SICK";
    else if (hunger < 20)    petMood = "HUNGRY";
    else if (happiness < 20) petMood = "SAD";
    else if (energy < 20)    petMood = "TIRED";
    else                     petMood = "HAPPY";

    // Alert beep if any stat critical
    if (hunger < 20 || happiness < 20 || health < 20) {
      beep(3, 80);
      vibrate(300);
    }
  }

  // ---- Sleep if dark and low energy ----
  if (lightLevel < 300 && energy < 30) {
    petAsleep = true;
  } else if (lightLevel > 600) {
    petAsleep = false;
  }

  // ---- Button 1 — Feed pet ----
  if (digitalRead(BTN1_PIN) == LOW) {
    hunger    = min(STAT_MAX, hunger + 20);
    happiness = min(STAT_MAX, happiness + 5);
    beep(1, 100);
    vibrate(100);
    petAsleep = false;
    delay(300); // debounce
  }

  // ---- Button 2 — Play with pet ----
  if (digitalRead(BTN2_PIN) == LOW) {
    happiness = min(STAT_MAX, happiness + 20);
    energy    = max(STAT_MIN, energy - 10);
    beep(2, 80);
    vibrate(200);
    petAsleep = false;
    delay(300);
  }

  // ---- Button 3 — Measure distance and show on display ----
  if (digitalRead(BTN3_PIN) == LOW) {
    long inches = getDistance();
    if (inches > 0) {
      lastDistance = inches;
      showDistance(inches);
      Serial.print("Distance: ");
      Serial.print(inches);
      Serial.println(" inches");
    }
    delay(300);
  }

  // ---- Update display every 500ms ----
  if (now - lastDisplayUpdate > 500) {
    drawFace();
    moodColor();
    lastDisplayUpdate = now;
  }
}
