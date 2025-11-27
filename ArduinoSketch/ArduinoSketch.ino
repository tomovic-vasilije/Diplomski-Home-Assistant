#include <Arduino_MKRIoTCarrier.h>
#include <ArduinoMqttClient.h>
#include <ArduinoJson.h>
#include <WiFiNINA.h>
#include <math.h>
#include "visuals.h"
#include "pitches.h"
#include "secrets.h"
#include "consts.h"

MKRIoTCarrier carrier;
WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);

AlarmState alarmState = ARMED;
EnvPage currentPage = PAGE_TEMP;

unsigned long lastEnvSampleTime = 0;
bool normalStateJustEntered = false;

float Gx, Gy, Gz;

unsigned long authStartTime = 0;
int lastDisplayedSeconds = -1;
unsigned long lastBeepTime = 0;

int8_t currentUserIndex = -1;
const char* currentUserId = NULL;

uint8_t enteredPin[PIN_LENGTH];
uint8_t enteredLength = 0;

uint8_t failedAttempts = 0;

uint32_t colorRed, colorGreen, colorBlue, colorBlack;

// --- melodije ---

const uint16_t successMelody[] = {
  NOTE_E6, NOTE_G6, NOTE_C7, NOTE_G6
};

const uint8_t successDurations[] = {
  8, 8, 8, 8
};

const uint8_t SUCCESS_MELODY_LEN = sizeof(successMelody) / sizeof(successMelody[0]);

const uint16_t errorMelody[] = {
  NOTE_G5, NOTE_E5
};

const uint8_t errorDurations[] = {
  4, 4
};

const uint8_t ERROR_MELODY_LEN = sizeof(errorMelody) / sizeof(errorMelody[0]);

// ------------------------ Audio helperi ------------------------

void playMelody(const uint16_t *melody, const uint8_t *durations, uint8_t length) {
  for (uint8_t i = 0; i < length; i++) {
    uint16_t noteDuration = 1000 / durations[i];

    if (melody[i] != 0) {
      carrier.Buzzer.sound(melody[i]);
    }
    delay(noteDuration);

    uint16_t pauseBetweenNotes = noteDuration * 0.30f;
    delay(pauseBetweenNotes);

    carrier.Buzzer.noSound();
  }
}

// kratak beep za odbrojavanje
void playShortCountdownBeep() {
  uint16_t noteDuration = 1000 / 16;   // šesnaestinka
  carrier.Buzzer.sound(NOTE_A6);
  delay(noteDuration);
  uint16_t pauseBetweenNotes = noteDuration * 0.30f;
  delay(pauseBetweenNotes);
  carrier.Buzzer.noSound();
}

// ------------------------ Helperi za IMU / stanja ------------------------

bool detectMovement() {
  carrier.IMUmodule.readGyroscope(Gx, Gy, Gz);

  if (fabs(Gx) > GYRO_THRESHOLD ||
      fabs(Gy) > GYRO_THRESHOLD ||
      fabs(Gz) > GYRO_THRESHOLD) {

    Serial.print("Gyroscope:\tX: ");
    Serial.print(Gx);
    Serial.print("\tY: ");
    Serial.print(Gy);
    Serial.print("\tZ: ");
    Serial.println(Gz);
    Serial.println("IMU: POMERANJE DETEKTOVANO");
    Serial.println("------------------------");
    return true;
  }

  return false;
}

void drawArmedScreen() {
  carrier.display.fillScreen(ST77XX_BLACK);
  carrier.display.setTextColor(ST77XX_WHITE);
  carrier.display.setTextSize(3);
  carrier.display.setCursor(60, 90);
  carrier.display.print("ARMED");

  // ARMED = mrak na LED
  carrier.leds.fill(colorBlack, 0, 5);
  carrier.leds.show();
}

void drawNormalScreen() {
  carrier.display.fillScreen(ST77XX_BLACK);
  carrier.display.setTextColor(ST77XX_WHITE);
  carrier.display.setTextSize(3);
  carrier.display.setCursor(50, 90);
  carrier.display.print("NORMAL");

  // 3 puta zablinka zeleno pa se ugasi
  for (uint8_t i = 0; i < 3; i++) {
    carrier.leds.fill(colorGreen, 0, 5);
    carrier.leds.show();
    delay(250);
    carrier.leds.fill(colorBlack, 0, 5);
    carrier.leds.show();
    delay(250);
  }
}

void drawIntruderScreen() {
  carrier.display.fillScreen(ST77XX_BLACK);
  carrier.display.setTextColor(ST77XX_RED);
  carrier.display.setTextSize(3);
  carrier.display.setCursor(30, 90);
  carrier.display.print("INTRUDER");
  carrier.display.setCursor(30, 130);
  carrier.display.print("DETECTED!");
}

void startAuthCountdown() {
  alarmState = AUTH_COUNTDOWN;
  authStartTime = millis();
  lastDisplayedSeconds = -1;
  lastBeepTime = 0;
  enteredLength = 0;
  failedAttempts = 0;
  currentUserIndex = -1;
  currentUserId = NULL;

  Serial.println("Prelaz u AUTH_COUNTDOWN");

  carrier.leds.fill(colorRed, 0, 5);
  carrier.leds.show();

  carrier.display.fillScreen(ST77XX_BLACK);
}

// ------------------------ PIN logika ------------------------
int8_t findUserIndexForPin(const uint8_t pinDigits[PIN_LENGTH]) {
  for (uint8_t userIdx = 0; userIdx < USER_COUNT; userIdx++) {
    bool match = true;
    for (uint8_t i = 0; i < PIN_LENGTH; i++) {
      if (pinDigits[i] != USER_PINS[userIdx][i]) {
        match = false;
        break;
      }
    }
    if (match) {
      return (int8_t)userIdx;   // našli smo user-a
    }
  }
  return -1;                    // nijedan se ne poklapa
}

void addDigit(uint8_t d) {
  if (enteredLength >= PIN_LENGTH) return;

  enteredPin[enteredLength] = d;
  enteredLength++;

  Serial.print("Unet broj: ");
  Serial.print(d);
  Serial.print("  (duzina: ");
  Serial.print(enteredLength);
  Serial.println(")");

  if (enteredLength == PIN_LENGTH) {
    // pokušaj da nađeš korisnika za ovaj PIN
    int8_t userIdx = findUserIndexForPin(enteredPin);
    delay(1000);

    if (userIdx >= 0) {
      // uspešna autentifikacija
      currentUserIndex = userIdx;
      currentUserId = USER_IDS[userIdx];   // pokazivač na string u USER_IDS tabeli

      Serial.print("PIN tacan -> NORMAL stanje, korisnik: ");
      Serial.print("index=");
      Serial.print(currentUserIndex);
      Serial.print(", id=");
      Serial.println(currentUserId);

      alarmState = NORMAL;
      normalStateJustEntered = true;
      carrier.Buzzer.noSound();
      playMelody(successMelody, successDurations, SUCCESS_MELODY_LEN);
      drawNormalScreen();
    } else {
      // nijedan user nema ovaj PIN
      Serial.println("PIN pogresan!");
      failedAttempts++;

      playMelody(errorMelody, errorDurations, ERROR_MELODY_LEN);

      Serial.print("Broj neuspesnih pokusaja: ");
      Serial.println(failedAttempts);

      if (failedAttempts >= MAX_FAILED_ATTEMPTS) {
        Serial.println("Previse pogresnih pokusaja -> INTRUDER!");
        alarmState = INTRUDER;
        drawIntruderScreen();
      }

      enteredLength = 0;
    }
  }
}

void updateAuthCountdown() {
  unsigned long now = millis();

  int elapsedSeconds = (now - authStartTime) / 1000;
  int remaining = AUTH_WINDOW_SECONDS - elapsedSeconds;

  if (alarmState == INTRUDER) {
    return;
  }

  if (remaining <= 0 && alarmState == AUTH_COUNTDOWN) {
    alarmState = INTRUDER;
    Serial.println("Isteklo vreme -> INTRUDER");
    drawIntruderScreen();
    return;
  }

  if (now - lastBeepTime >= 1000) {
    lastBeepTime = now;
    playShortCountdownBeep();
  }

  if (remaining != lastDisplayedSeconds) {
    lastDisplayedSeconds = remaining;

    carrier.display.fillScreen(ST77XX_BLACK);
    carrier.display.setTextColor(ST77XX_WHITE);

    carrier.display.setTextSize(4);
    carrier.display.setCursor(100, 60);
    carrier.display.print(remaining);

    carrier.display.setTextSize(3);
    carrier.display.setCursor(15, 130);
    carrier.display.print("PIN: ");
    for (uint8_t i = 0; i < enteredLength; i++) {
      carrier.display.print('*');
    }

    carrier.display.setTextSize(2);
    carrier.display.setCursor(15, 160);
    carrier.display.print("Attempts left: ");
    carrier.display.print(MAX_FAILED_ATTEMPTS - failedAttempts);
  }

  carrier.Buttons.update();

  if (carrier.Buttons.onTouchDown(TOUCH0)) {
    addDigit(0);
  }
  else if (carrier.Buttons.onTouchDown(TOUCH1)) {
    addDigit(1);
  }
  else if (carrier.Buttons.onTouchDown(TOUCH2)) {
    addDigit(2);
  }
  else if (carrier.Buttons.onTouchDown(TOUCH3)) {
    addDigit(3);
  }
  else if (carrier.Buttons.onTouchDown(TOUCH4)) {
    addDigit(4);
  }
}

// ------------------------ INTRUDER alarm ------------------------

void updateIntruderAlarm() {
  static bool ledsOn = false;
  static unsigned long lastToggle = 0;
  unsigned long now = millis();

  if (now - lastToggle >= 300) {
    lastToggle = now;
    ledsOn = !ledsOn;

    if (ledsOn) {
      carrier.leds.fill(colorRed, 0, 5);
      carrier.leds.show();
      carrier.Buzzer.sound(NOTE_C7);
    } else {
      carrier.leds.fill(colorBlack, 0, 5);
      carrier.leds.show();
      carrier.Buzzer.noSound();
    }
  }
}

// ------------------------ ENV ekran: static deo + value deo ------------------------

// TEMP

void drawTemperatureStatic() {
  carrier.display.fillScreen(ST77XX_BLACK);
  carrier.display.setTextSize(3);
  carrier.display.setTextColor(ST77XX_WHITE);
  carrier.display.setCursor(25, 60);
  carrier.display.print("Temperature");

  carrier.display.drawBitmap(80, 80, temperature_logo, 100, 100, 0xDAC9);
}

void drawTemperatureValue(int16_t tempInt) {
  // obriši samo donji deo
  carrier.display.fillRect(40, 170, 200, 60, ST77XX_BLACK);

  carrier.display.setTextSize(3);
  carrier.display.setTextColor(ST77XX_WHITE);
  carrier.display.setCursor(60, 180);
  carrier.display.print(tempInt);
  carrier.display.print(" C");
}

// HUMIDITY

void drawHumidityStatic() {
  carrier.display.fillScreen(ST77XX_BLACK);
  carrier.display.setTextSize(3);
  carrier.display.setTextColor(ST77XX_WHITE);
  carrier.display.setCursor(54, 40);
  carrier.display.print("Humidity");

  carrier.display.drawBitmap(70, 70, humidity_logo, 100, 100, 0x0D14);
}

void drawHumidityValue(int16_t humInt) {
  carrier.display.fillRect(40, 170, 200, 60, ST77XX_BLACK);

  carrier.display.setTextSize(3);
  carrier.display.setTextColor(ST77XX_WHITE);
  carrier.display.setCursor(60, 180);
  carrier.display.print(humInt);
  carrier.display.print(" %");
}

// PRESSURE

void drawPressureStatic() {
  carrier.display.fillScreen(ST77XX_BLACK);
  carrier.display.setTextSize(3);
  carrier.display.setTextColor(ST77XX_WHITE);
  carrier.display.setCursor(54, 40);
  carrier.display.print("Pressure");

  carrier.display.drawBitmap(70, 60, pressure_logo, 100, 100, 0xF621);
}

void drawPressureValue(int16_t pressInt) {
  carrier.display.fillRect(20, 150, 210, 60, ST77XX_BLACK);

  carrier.display.setTextSize(3);
  carrier.display.setTextColor(ST77XX_WHITE);
  carrier.display.setCursor(40, 160);
  carrier.display.print(pressInt);
  carrier.display.setCursor(150, 160);
  carrier.display.print("kPa");
}

// helperi za izbor stranice

void drawEnvStaticPage(EnvPage page) {
  switch (page) {
    case PAGE_TEMP:
      drawTemperatureStatic();
      break;
    case PAGE_HUM:
      drawHumidityStatic();
      break;
    case PAGE_PRESSURE:
      drawPressureStatic();
      break;
  }
}

void drawEnvValue(EnvPage page, int16_t tempInt, int16_t humInt, int16_t pressInt) {
  switch (page) {
    case PAGE_TEMP:
      drawTemperatureValue(tempInt);
      break;
    case PAGE_HUM:
      drawHumidityValue(humInt);
      break;
    case PAGE_PRESSURE:
      drawPressureValue(pressInt);
      break;
  }
}

// ------------------------ Logovanje ENV u JSON ------------------------

void logEnvToSerial(float temperature, float humidity, float pressure) {
  StaticJsonDocument<256> doc;

  doc["state"] = "NORMAL";
  doc["temperature"] = temperature;
  doc["humidity"] = humidity;
  doc["pressure"] = pressure;
  doc["millis"] = millis();  // opcionalno, vreme rada

  serializeJson(doc, Serial);
  Serial.println(); // novi red
}

// ------------------------ NORMAL stanje: merenje + gestovi ------------------------

void handleNormalState() {

  static float lastTemp, lastHum, lastPress;
  static int16_t tempInt, humInt, pressInt;

  unsigned long now = millis();

  // prvi ulazak u NORMAL – očitaj senzore i prikaži temperaturu
  if (normalStateJustEntered) {
    currentPage = PAGE_TEMP; // kreni od Temperature

    lastTemp  = carrier.Env.readTemperature();
    lastHum   = carrier.Env.readHumidity();
    lastPress = carrier.Pressure.readPressure();

    // float za log, int za prikaz
    tempInt  = (int16_t)roundf(lastTemp);
    humInt   = (int16_t)roundf(lastHum);
    pressInt = (int16_t)roundf(lastPress);

    logEnvToSerial(lastTemp, lastHum, lastPress);

    drawEnvStaticPage(currentPage);
    drawEnvValue(currentPage, tempInt, humInt, pressInt);

    lastEnvSampleTime = now;
    normalStateJustEntered = false;
  }

  // periodično očitavanje svakih ENV_SAMPLE_INTERVAL ms
  if (now - lastEnvSampleTime >= ENV_SAMPLE_INTERVAL) {
    lastEnvSampleTime = now;

    lastTemp  = carrier.Env.readTemperature();
    lastHum   = carrier.Env.readHumidity();
    lastPress = carrier.Pressure.readPressure();

    logEnvToSerial(lastTemp, lastHum, lastPress);

    int16_t newTempInt  = (int16_t)roundf(lastTemp);
    int16_t newHumInt   = (int16_t)roundf(lastHum);
    int16_t newPressInt = (int16_t)roundf(lastPress);

    bool needRedrawValue = false;

    if (newTempInt != tempInt) {
      tempInt = newTempInt;
      if (currentPage == PAGE_TEMP) needRedrawValue = true;
    }
    if (newHumInt != humInt) {
      humInt = newHumInt;
      if (currentPage == PAGE_HUM) needRedrawValue = true;
    }
    if (newPressInt != pressInt) {
      pressInt = newPressInt;
      if (currentPage == PAGE_PRESSURE) needRedrawValue = true;
    }

    // ako se promenio int na trenutno prikazanoj stranici, update samo broja
    if (needRedrawValue) {
      drawEnvValue(currentPage, tempInt, humInt, pressInt);
    }
  }

  // čitanje gestova LEFT/RIGHT za promenu stranice
  if (carrier.Light.gestureAvailable()) {
    uint8_t gesture = carrier.Light.readGesture();

    if (gesture == LEFT) {
      // TEMP <- HUM <- PRESS <- TEMP ...
      if (currentPage == PAGE_TEMP) {
        currentPage = PAGE_PRESSURE;
      } else {
        currentPage = (EnvPage)((uint8_t)currentPage - 1);
      }
      drawEnvStaticPage(currentPage);
      drawEnvValue(currentPage, tempInt, humInt, pressInt);
    }
    else if (gesture == RIGHT) {
      // TEMP -> HUM -> PRESS -> TEMP ...
      if (currentPage == PAGE_PRESSURE) {
        currentPage = PAGE_TEMP;
      } else {
        currentPage = (EnvPage)((uint8_t)currentPage + 1);
      }
      drawEnvStaticPage(currentPage);
      drawEnvValue(currentPage, tempInt, humInt, pressInt);
    }
    else if (gesture == DOWN) {
      Serial.println("Gesture DOWN -> ARMED");

      currentUserIndex = -1;
      currentUserId = NULL;

      alarmState = ARMED;
      drawArmedScreen();
      // pošto smo promenili stanje, nema više posla u NORMAL u ovoj iteraciji
      return;
    }
  }

  // mali delay da ne cepa CPU
  delay(50);
}

// ------------------------ setup / loop ------------------------

void setup() {
  Serial.begin(9600);
  delay(1000);

  if (!carrier.begin()) {
    Serial.println("Carrier not connected, check connections");
    while (1);
  }

  CARRIER_CASE = false;

  carrier.display.setRotation(0);

  colorRed   = carrier.leds.Color(255, 0, 0);
  colorGreen = carrier.leds.Color(0, 255, 0);
  colorBlue  = carrier.leds.Color(0, 0, 255);
  colorBlack = carrier.leds.Color(0, 0, 0);

  delay(1000);

  Serial.print("Povezivanje na WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nWiFi povezan!");
  Serial.print("IP adresa: ");
  Serial.println(WiFi.localIP());

  Serial.print("Povezivanje na MQTT broker: ");
  Serial.print(MQTT_BROKER);
  Serial.print(":");
  Serial.println(MQTT_PORT);

  if (!mqttClient.connect(MQTT_BROKER, MQTT_PORT)) {
    Serial.print("MQTT konekcija neuspesna! Code: ");
    Serial.println(mqttClient.connectError());
    while (1);
  }
  Serial.println("MQTT povezan!");
  mqttClient.setId(SENSOR_NAME);

  drawArmedScreen();
}

void loop() {
  switch (alarmState) {
    case ARMED:
      if (detectMovement()) {
        startAuthCountdown();
      }
      break;

    case AUTH_COUNTDOWN:
      updateAuthCountdown();
      break;

    case NORMAL:
      handleNormalState();
      break;

    case INTRUDER:
      updateIntruderAlarm();
      break;
  }
}
