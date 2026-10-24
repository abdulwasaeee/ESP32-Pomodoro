
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <TM1637Display.h>
#include <DHT.h>
#include <math.h>

// ==========================================
// BUDDY V5 - POMODORO + ROOM SENSOR
// ESP32 DevKit / SSD1306 / TM1637 / DHT11
// ==========================================

// OLED - confirmed from Flappy Bird
#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_ADDRESS 0x3C

// Button - confirmed from Flappy Bird
#define BUTTON_PIN 18

// TM1637 4-digit timer
#define TM_CLK 25
#define TM_DIO 26

// DHT11 temperature/humidity
#define DHT_PIN 19
#define DHT_TYPE DHT11

Adafruit_SSD1306 oled(128, 64, &Wire, -1);
TM1637Display timerDisplay(TM_CLK, TM_DIO);
DHT dht(DHT_PIN, DHT_TYPE);

// ==========================================
// TIMING
// ==========================================

const uint32_t FRAME_MS = 50;
const uint32_t DEBOUNCE_MS = 35;
const uint32_t DOUBLE_CLICK_MS = 350;

const uint32_t WAKE_MS = 1600;
const uint32_t SLEEP_YAWN_MS = 1800;
const uint32_t GIGGLE_MS = 1000;
const uint32_t ANGRY_MS = 4000;

const uint32_t PRESS_WINDOW_MS = 10000;
const uint32_t SENSOR_INTERVAL_MS = 2500;
const uint32_t WEATHER_REACTION_MS = 1800;
const uint32_t WEATHER_INTERVAL_MS = 45000;

const uint8_t COLON = 0x40;

// ==========================================
// MODES
// ==========================================

enum BuddyMode {
  SELECT_MODE,
  WAKING_MODE,
  FOCUS_MODE,
  REST_MODE
};

BuddyMode mode = SELECT_MODE;

enum Reaction {
  REACTION_NONE,
  REACTION_GIGGLE,
  REACTION_ANGRY,
  REACTION_HOT,
  REACTION_HUMID
};

Reaction reaction = REACTION_NONE;

uint8_t selectedMinutes = 25;
uint32_t remainingSeconds = 25UL * 60UL;

uint32_t sessionStart = 0;
uint32_t stateStarted = 0;
uint32_t reactionStarted = 0;
uint32_t lastFrame = 0;

// ==========================================
// BUTTON
// ==========================================

bool lastRaw = HIGH;
bool stableButton = HIGH;

uint32_t buttonChangedAt = 0;
uint32_t firstClickAt = 0;
uint8_t pendingClicks = 0;

// ==========================================
// POKE TRACKING
// ==========================================

uint8_t pokeCount = 0;
uint32_t lastPokeAt = 0;

// ==========================================
// FACE ANIMATION
// ==========================================

uint32_t nextBlink = 0;
uint32_t blinkUntil = 0;
uint32_t nextLook = 0;

int eyeOffset = 0;

// ==========================================
// SENSOR
// ==========================================

float temperatureC = NAN;
float humidity = NAN;

uint32_t lastSensorRead = 0;
uint32_t lastWeatherReaction = 0;
bool sensorValid = false;

// ==========================================
// DISPLAY HELPERS
// ==========================================

void showTime(uint32_t seconds) {
  uint16_t mins = seconds / 60;
  uint8_t secs = seconds % 60;

  uint16_t digits = mins * 100 + secs;

  timerDisplay.showNumberDecEx(
    digits, COLON, true, 4, 0
  );
}

void centerText(const char *text, int y, int size = 1) {
  oled.setTextSize(size);
  oled.setTextColor(WHITE);

  int width = strlen(text) * 6 * size;
  oled.setCursor((128 - width) / 2, y);
  oled.print(text);
}

// ==========================================
// FACE HELPERS
// ==========================================

void closedEyes() {
  oled.drawLine(24, 27, 35, 34, WHITE);
  oled.drawLine(35, 34, 46, 27, WHITE);

  oled.drawLine(82, 27, 93, 34, WHITE);
  oled.drawLine(93, 34, 104, 27, WHITE);

  oled.drawLine(24, 28, 35, 35, WHITE);
  oled.drawLine(35, 35, 46, 28, WHITE);

  oled.drawLine(82, 28, 93, 35, WHITE);
  oled.drawLine(93, 35, 104, 28, WHITE);
}

void openEyes() {
  oled.fillRoundRect(
    30 + eyeOffset, 12, 15, 29, 7, WHITE
  );

  oled.fillRoundRect(
    83 + eyeOffset, 12, 15, 29, 7, WHITE
  );
}

void happyMouth() {
  oled.drawLine(49, 47, 55, 52, WHITE);
  oled.drawLine(55, 52, 64, 55, WHITE);
  oled.drawLine(64, 55, 73, 52, WHITE);
  oled.drawLine(73, 52, 79, 47, WHITE);
}

void drawSleepingFace(uint32_t now) {
  oled.clearDisplay();

  closedEyes();
  oled.drawLine(59, 50, 69, 50, WHITE);

  uint8_t phase = (now / 650) % 4;

  oled.setTextSize(1);
  oled.setTextColor(WHITE);

  if (phase >= 1) {
    oled.setCursor(105, 25);
    oled.print("z");
  }

  if (phase >= 2) {
    oled.setCursor(113, 14);
    oled.print("z");
  }

  if (phase >= 3) {
    oled.setCursor(120, 3);
    oled.print("z");
  }

  oled.display();
}

void drawAwakeFace(uint32_t now) {
  oled.clearDisplay();

  if ((int32_t)(blinkUntil - now) > 0) {
    closedEyes();
  } else {
    openEyes();
  }

  happyMouth();
  oled.display();
}

void drawYawn(float progress) {
  oled.clearDisplay();

  closedEyes();

  progress = constrain(progress, 0.0f, 1.0f);

  float amount = progress < 0.5f
    ? progress * 2.0f
    : (1.0f - progress) * 2.0f;

  int h = 5 + (int)(amount * 17);
  int w = 8 + h / 2;

  oled.drawRoundRect(
    64 - w / 2,
    47 - h / 2,
    w, h, w / 2,
    WHITE
  );

  oled.display();
}

// ==========================================
// SQUISHY GIGGLE WITH BLUSH
// ==========================================

void drawGiggle(uint32_t now) {
  oled.clearDisplay();

  int bounce = ((now / 120) % 2) * 2;

  // Squishy crescent eyes
  oled.drawLine(20, 29+bounce, 27, 22+bounce, WHITE);
  oled.drawLine(27, 22+bounce, 35, 19+bounce, WHITE);
  oled.drawLine(35, 19+bounce, 43, 22+bounce, WHITE);
  oled.drawLine(43, 22+bounce, 50, 29+bounce, WHITE);

  oled.drawLine(78, 29+bounce, 85, 22+bounce, WHITE);
  oled.drawLine(85, 22+bounce, 93, 19+bounce, WHITE);
  oled.drawLine(93, 19+bounce, 101, 22+bounce, WHITE);
  oled.drawLine(101, 22+bounce, 108, 29+bounce, WHITE);

  // Slightly thicker eye curves
  oled.drawLine(27, 23+bounce, 35, 20+bounce, WHITE);
  oled.drawLine(35, 20+bounce, 43, 23+bounce, WHITE);

  oled.drawLine(85, 23+bounce, 93, 20+bounce, WHITE);
  oled.drawLine(93, 20+bounce, 101, 23+bounce, WHITE);

  // Blush marks
  for (int i = 0; i < 3; i++) {
    oled.drawLine(
      15 + i*6, 39+bounce,
      11 + i*6, 46+bounce,
      WHITE
    );

    oled.drawLine(
      105 + i*6, 39+bounce,
      101 + i*6, 46+bounce,
      WHITE
    );
  }

  // Squishy smile
  oled.drawLine(49, 43+bounce, 55, 49+bounce, WHITE);
  oled.drawLine(55, 49+bounce, 64, 52+bounce, WHITE);
  oled.drawLine(64, 52+bounce, 73, 49+bounce, WHITE);
  oled.drawLine(73, 49+bounce, 79, 43+bounce, WHITE);

  oled.display();
}

// ==========================================
// ANGRY FACE - NO CORNER SYMBOL
// ==========================================

void drawAngry() {
  oled.clearDisplay();

  oled.drawLine(22, 16, 48, 28, WHITE);
  oled.drawLine(22, 17, 48, 29, WHITE);

  oled.drawLine(80, 28, 106, 16, WHITE);
  oled.drawLine(80, 29, 106, 17, WHITE);

  oled.fillRoundRect(29, 31, 19, 9, 3, WHITE);
  oled.fillRoundRect(80, 31, 19, 9, 3, WHITE);

  oled.drawLine(49, 55, 56, 49, WHITE);
  oled.drawLine(56, 49, 72, 49, WHITE);
  oled.drawLine(72, 49, 79, 55, WHITE);

  oled.display();
}

// ==========================================
// HOT FACE
// ==========================================

void drawHotFace() {
  oled.clearDisplay();

  // Tired eyes
  oled.drawLine(25, 26, 47, 29, WHITE);
  oled.drawLine(81, 29, 103, 26, WHITE);

  // Open uncomfortable mouth
  oled.drawRoundRect(57, 43, 14, 13, 6, WHITE);

  // Sweat droplet
  oled.drawLine(110, 13, 106, 22, WHITE);
  oled.drawLine(110, 13, 114, 22, WHITE);
  oled.drawLine(106, 22, 110, 27, WHITE);
  oled.drawLine(110, 27, 114, 22, WHITE);

  oled.display();
}

// ==========================================
// HUMID FACE
// ==========================================

void drawHumidFace() {
  oled.clearDisplay();

  closedEyes();

  // Small uncomfortable mouth
  oled.drawLine(55, 50, 62, 47, WHITE);
  oled.drawLine(62, 47, 69, 50, WHITE);
  oled.drawLine(69, 50, 76, 47, WHITE);

  // Droplets
  oled.drawCircle(12, 14, 3, WHITE);
  oled.drawCircle(116, 17, 3, WHITE);

  oled.display();
}

// ==========================================
// DHT11 READING
// ==========================================

void updateSensor(uint32_t now) {
  if (now - lastSensorRead < SENSOR_INTERVAL_MS) {
    return;
  }

  lastSensorRead = now;

  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (isnan(t) || isnan(h)) {
    sensorValid = false;
    Serial.println("DHT11 reading failed");
    return;
  }

  temperatureC = t;
  humidity = h;
  sensorValid = true;

  Serial.printf(
    "Room: %.1f C | Humidity: %.1f%%\n",
    temperatureC, humidity
  );
}

// ==========================================
// SESSION CONTROL
// ==========================================

void startFocus(uint32_t now) {
  remainingSeconds = selectedMinutes * 60UL;

  sessionStart = now;
  stateStarted = now;

  mode = WAKING_MODE;
  reaction = REACTION_NONE;

  pokeCount = 0;
  lastPokeAt = 0;

  showTime(remainingSeconds);

  Serial.printf(
    "Started %d-minute focus session\n",
    selectedMinutes
  );
}

void finishFocus(uint32_t now) {
  remainingSeconds = 0;

  mode = REST_MODE;
  stateStarted = now;

  reaction = REACTION_NONE;
  pokeCount = 0;

  showTime(0);

  Serial.println("Session complete. Rest!");
}

// ==========================================
// BUTTON REACTIONS
// ==========================================

void pokeBuddy(uint32_t now) {
  if (mode != FOCUS_MODE) return;

  if (reaction == REACTION_ANGRY &&
      now - reactionStarted < ANGRY_MS) {
    return;
  }

  if (lastPokeAt == 0 ||
      now - lastPokeAt > PRESS_WINDOW_MS) {
    pokeCount = 0;
  }

  pokeCount++;
  lastPokeAt = now;
  reactionStarted = now;

  if (pokeCount >= 6) {
    pokeCount = 0;
    reaction = REACTION_ANGRY;
    Serial.println("BUDDY: Stop poking me!");
  } else {
    reaction = REACTION_GIGGLE;
    Serial.println("BUDDY: Hehehe!");
  }
}

void singleClick(uint32_t now) {
  if (mode == SELECT_MODE) {
    selectedMinutes = selectedMinutes == 25 ? 45 : 25;
    remainingSeconds = selectedMinutes * 60UL;
    showTime(remainingSeconds);
    return;
  }

  if (mode == REST_MODE) {
    startFocus(now);
    return;
  }

  if (mode == FOCUS_MODE) {
    pokeBuddy(now);
  }
}

void doubleClick(uint32_t now) {
  if (mode == SELECT_MODE || mode == REST_MODE) {
    startFocus(now);
    return;
  }

  if (mode == FOCUS_MODE) {
    pokeBuddy(now);
    pokeBuddy(now);
  }
}

// ==========================================
// BUTTON DEBOUNCE + CLICK DETECTION
// ==========================================

void handleButton(uint32_t now) {
  bool raw = digitalRead(BUTTON_PIN);

  if (raw != lastRaw) {
    buttonChangedAt = now;
  }

  if (now - buttonChangedAt >= DEBOUNCE_MS &&
      raw != stableButton) {

    stableButton = raw;

    if (stableButton == HIGH) {
      if (pendingClicks == 0) {
        pendingClicks = 1;
        firstClickAt = now;
      } else if (now - firstClickAt <= DOUBLE_CLICK_MS) {
        pendingClicks = 0;
        doubleClick(now);
      } else {
        pendingClicks = 1;
        firstClickAt = now;
      }
    }
  }

  lastRaw = raw;

  if (pendingClicks == 1 &&
      stableButton == HIGH &&
      now - firstClickAt > DOUBLE_CLICK_MS) {

    pendingClicks = 0;
    singleClick(now);
  }
}

// ==========================================
// ANIMATION UPDATE
// ==========================================

void updateBuddy(uint32_t now) {

  // ----------------------------------------
  // SELECT 25 OR 45 MINUTES
  // ----------------------------------------
  if (mode == SELECT_MODE) {
    showTime(selectedMinutes * 60UL);

    oled.clearDisplay();
    centerText("BUDDY POMODORO", 2);

    char label[16];
    snprintf(
      label, sizeof(label),
      "%d MIN", selectedMinutes
    );

    centerText(label, 20, 2);
    centerText("1 CLICK: CHANGE", 47);
    centerText("2 CLICKS: START", 57);

    oled.display();
    return;
  }

  // ----------------------------------------
  // WAKING UP
  // ----------------------------------------
  if (mode == WAKING_MODE) {
    uint32_t elapsed = now - stateStarted;

    if (elapsed < WAKE_MS) {
      drawYawn((float)elapsed / WAKE_MS);
    } else {
      mode = FOCUS_MODE;

      nextBlink = now + 2200;
      nextLook = now + 1800;
      blinkUntil = 0;
      eyeOffset = 0;
    }

    return;
  }

  // ----------------------------------------
  // FOCUS SESSION
  // ----------------------------------------
  if (mode == FOCUS_MODE) {
    uint32_t elapsed = now - sessionStart;
    uint32_t total = selectedMinutes * 60000UL;

    if (elapsed >= total) {
      finishFocus(now);
      return;
    }

    uint32_t leftMs = total - elapsed;

    remainingSeconds = (leftMs + 999UL) / 1000UL;
    showTime(remainingSeconds);

    // Button reactions
    if (reaction != REACTION_NONE) {
      uint32_t reactionElapsed =
        now - reactionStarted;

      if (reaction == REACTION_GIGGLE &&
          reactionElapsed < GIGGLE_MS) {
        drawGiggle(now);
        return;
      }

      if (reaction == REACTION_ANGRY &&
          reactionElapsed < ANGRY_MS) {
        drawAngry();
        return;
      }

      if (reaction == REACTION_HOT &&
          reactionElapsed < WEATHER_REACTION_MS) {
        drawHotFace();
        return;
      }

      if (reaction == REACTION_HUMID &&
          reactionElapsed < WEATHER_REACTION_MS) {
        drawHumidFace();
        return;
      }

      reaction = REACTION_NONE;
    }

    // Occasional temperature reaction
    if (sensorValid &&
        now - lastWeatherReaction >= WEATHER_INTERVAL_MS) {

      lastWeatherReaction = now;

      if (temperatureC >= 30.0f) {
        reaction = REACTION_HOT;
        reactionStarted = now;
        drawHotFace();
        return;
      }

      if (humidity >= 75.0f) {
        reaction = REACTION_HUMID;
        reactionStarted = now;
        drawHumidFace();
        return;
      }
    }

    // Blink
    if ((int32_t)(now - nextBlink) >= 0) {
      blinkUntil = now + 150;
      nextBlink = now + random(2200, 4500);
    }

    // Look around
    if ((int32_t)(now - nextLook) >= 0) {
      int positions[] = {-5, 0, 5, 0};
      eyeOffset = positions[random(0, 4)];
      nextLook = now + random(1200, 2800);
    }

    drawAwakeFace(now);
    return;
  }

  // ----------------------------------------
  // REST MODE
  // ----------------------------------------
  if (mode == REST_MODE) {
    showTime(0);

    uint32_t elapsed = now - stateStarted;

    if (elapsed < SLEEP_YAWN_MS) {
      drawYawn(
        (float)elapsed / SLEEP_YAWN_MS
      );
    } else {
      drawSleepingFace(now);
    }
  }
}

// ==========================================
// SETUP
// ==========================================

void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Wire.begin(OLED_SDA, OLED_SCL);

  if (!oled.begin(
        SSD1306_SWITCHCAPVCC,
        OLED_ADDRESS
      )) {
    Serial.println("OLED initialization failed!");
    while (true) delay(1000);
  }

  dht.begin();

  timerDisplay.setBrightness(4);
  timerDisplay.clear();

  randomSeed(micros());

  oled.clearDisplay();
  oled.setTextColor(WHITE);

  centerText("BUDDY", 13, 2);
  centerText("Focus Companion", 43);
  oled.display();

  delay(1200);

  mode = SELECT_MODE;
  lastSensorRead = millis() - SENSOR_INTERVAL_MS;
  lastWeatherReaction = millis();
}

// ==========================================
// MAIN LOOP
// ==========================================

void loop() {
  uint32_t now = millis();

  handleButton(now);
  updateSensor(now);

  if (now - lastFrame >= FRAME_MS) {
    lastFrame = now;
    updateBuddy(now);
  }

  delay(2);
}
