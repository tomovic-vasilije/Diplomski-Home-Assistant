#include <Arduino_MKRIoTCarrier.h>
#include <ArduinoMqttClient.h>
#include <ArduinoJson.h>
#include <WiFiNINA.h>
#include <math.h>
#include "visuals.h"
#include "pitches.h"
#include "secrets.h"
#include "consts.h"

// ------------------------ Globalne promenljive ------------------------

MKRIoTCarrier carrier;
WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);

void onMqttMessage(int messageSize);

// Stanje alarma i ekran stranice (TEMP/HUM/PRESS)
AlarmState alarmState = ARMED;
EnvPage currentPage = PAGE_TEMP;

// ENV sampling
unsigned long lastEnvSampleTime = 0;
bool normalStateJustEntered = false;

// IMU (žiroskop)
float Gx, Gy, Gz;

// Auth countdown (tajmer, beep, prikaz)
unsigned long authStartTime = 0;
int lastDisplayedSeconds = -1;
unsigned long lastBeepTime = 0;

// Informacije o korisniku (PIN)
int8_t currentUserIndex = -1;     // -1 znači: nijedan user
const char* currentUserId = NULL; // pokazivač na USER_IDS[x]

// Overlay za prikaz komandi sa actuators topica
bool actuatorOverlayActive = false;
unsigned long actuatorOverlayStart = 0;
char actuatorOverlayText[16];   // npr. "AC ON", "HUM OFF", "WND ON"

// Trenutno unet PIN (do 4 cifre)
uint8_t enteredPin[PIN_LENGTH];
uint8_t enteredLength = 0;

// Broj neuspešnih pokušaja PIN-a
uint8_t failedAttempts = 0;

// Boje za LED traku
uint32_t colorRed, colorGreen, colorBlue, colorBlack;

// ------------------------ Melodije (note + trajanje) ------------------------

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

// ======================================================================
//                            AUDIO HELPERI
// ======================================================================

// Puštanje melodije (uspeh/greška)
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

// Kratak beep za odbrojavanje (svake sekunde u AUTH_COUNTDOWN)
void playShortCountdownBeep() {
  uint16_t noteDuration = 1000 / 16;   // šesnaestinka
  carrier.Buzzer.sound(NOTE_A6);
  delay(noteDuration);
  uint16_t pauseBetweenNotes = noteDuration * 0.30f;
  delay(pauseBetweenNotes);
  carrier.Buzzer.noSound();
}

// ======================================================================
//                        MQTT + JSON HELPERI
// ======================================================================

// Helper koji JSON i ispiše na Serial i pošalje na MQTT
void publishJsonToMqtt(const char* topic, StaticJsonDocument<256>& doc) {
  Serial.print("MQTT PUBLISH [");
  Serial.print(topic);
  Serial.print("]: ");
  serializeJson(doc, Serial);
  Serial.println();

  mqttClient.beginMessage(topic);
  serializeJson(doc, mqttClient);
  mqttClient.endMessage();
}

// Slanje senzor podataka na DATA topic (NORMAL stanje)
void publishSensorData(float temperature, float humidity, float pressure) {
  StaticJsonDocument<256> doc;

  doc["sensor_name"] = SENSOR_NAME;
  doc["house_id"] = HOUSE_ID;

  if (currentUserId != NULL) {
    doc["user_id"] = currentUserId;
  } else {
    doc["user_id"] = nullptr; // JSON null
  }

  doc["state"] = "NORMAL";
  doc["temperature"] = temperature;
  doc["humidity"] = humidity;
  doc["pressure"] = pressure;
  doc["millis"] = millis();

  publishJsonToMqtt(MQTT_TOPIC_DATA, doc);
}

// Popunjavanje zajedničkih polja za EVENT JSON
void fillCommonEvent(StaticJsonDocument<256>& doc, const char* eventType) {
  doc["sensor_name"] = SENSOR_NAME;
  doc["house_id"] = HOUSE_ID;

  if (currentUserId != NULL) {
    doc["user_id"] = currentUserId;
  } else {
    doc["user_id"] = nullptr;
  }

  doc["event_type"] = eventType;
  doc["millis"] = millis();
}

// --------------- Pojedinačni event publish-eri ----------------

// Pokret detektovan u ARMED → prelaz u AUTH_COUNTDOWN
void publishEventMotionDetectedArmed() {
  StaticJsonDocument<256> doc;
  fillCommonEvent(doc, "MOTION_DETECTED_ARMED");

  doc["prev_state"] = "ARMED";
  doc["new_state"] = "AUTH_COUNTDOWN";
  doc["reason"] = "IMU_MOVEMENT";

  publishJsonToMqtt(MQTT_TOPIC_EVENTS, doc);
}

// Uspešna autentifikacija PIN-om → NORMAL
void publishEventAuthSuccess() {
  StaticJsonDocument<256> doc;
  fillCommonEvent(doc, "AUTH_SUCCESS");

  doc["prev_state"] = "AUTH_COUNTDOWN";
  doc["new_state"] = "NORMAL";

  publishJsonToMqtt(MQTT_TOPIC_EVENTS, doc);
}

// Neuspešan PIN pokušaj (ali još nismo u INTRUDER)
void publishEventAuthFailed() {
  StaticJsonDocument<256> doc;
  fillCommonEvent(doc, "AUTH_FAILED");

  doc["state"] = "AUTH_COUNTDOWN";
  doc["failed_attempts"] = failedAttempts;
  doc["max_attempts"] = MAX_FAILED_ATTEMPTS;

  publishJsonToMqtt(MQTT_TOPIC_EVENTS, doc);
}

// Dostignut maksimum neuspešnih pokušaja → prelaz u INTRUDER
void publishEventAuthMaxFailed() {
  StaticJsonDocument<256> doc;
  fillCommonEvent(doc, "AUTH_MAX_FAILED");

  doc["prev_state"] = "AUTH_COUNTDOWN";
  doc["new_state"] = "INTRUDER";
  doc["failed_attempts"] = failedAttempts;

  publishJsonToMqtt(MQTT_TOPIC_EVENTS, doc);
}

// Istekao AUTH countdown → INTRUDER
void publishEventAuthTimeout() {
  StaticJsonDocument<256> doc;
  fillCommonEvent(doc, "AUTH_TIMEOUT");

  doc["prev_state"] = "AUTH_COUNTDOWN";
  doc["new_state"] = "INTRUDER";

  publishJsonToMqtt(MQTT_TOPIC_EVENTS, doc);
}

// Ušli smo u INTRUDER stanje (generalni event)
void publishEventIntruderAlarm() {
  StaticJsonDocument<256> doc;
  fillCommonEvent(doc, "INTRUDER_ALARM");

  doc["state"] = "INTRUDER";

  publishJsonToMqtt(MQTT_TOPIC_EVENTS, doc);
}

// Gesture DOWN iz NORMAL → ARMED (zaključavanje kuće od strane usera)
void publishEventArmedByUser() {
  StaticJsonDocument<256> doc;
  fillCommonEvent(doc, "ARMED_BY_USER");

  doc["prev_state"] = "NORMAL";
  doc["new_state"] = "ARMED";
  doc["reason"] = "GESTURE_DOWN";

  publishJsonToMqtt(MQTT_TOPIC_EVENTS, doc);
}

// ======================================================================
//                    HELPERI ZA IMU I EKRAN STANJA
// ======================================================================

// Detekcija pokreta preko žiroskopa
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

// Ekran za ARMED stanje
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

// Ekran za INTRUDER stanje
void drawIntruderScreen() {
  carrier.display.fillScreen(ST77XX_BLACK);
  carrier.display.setTextColor(ST77XX_RED);
  carrier.display.setTextSize(3);
  carrier.display.setCursor(30, 90);
  carrier.display.print("INTRUDER");
  carrier.display.setCursor(30, 130);
  carrier.display.print("DETECTED!");
}

// Prelazak u AUTH_COUNTDOWN stanje
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

void showWelcomeScreen() {
  if (currentUserIndex < 0 || currentUserIndex >= USER_COUNT) return;

  carrier.display.fillScreen(ST77XX_BLACK);
  carrier.display.setTextColor(ST77XX_WHITE);
  carrier.display.setTextSize(3);

  carrier.display.setCursor(20, 80);
  carrier.display.print("WELCOME");

  carrier.display.setCursor(40, 120);
  carrier.display.print(USER_DISPLAY_NAMES[currentUserIndex]);

  // 3 puta zablinka zeleno pa se ugasi
  for (uint8_t i = 0; i < 3; i++) {
    carrier.leds.fill(colorGreen, 0, 5);
    carrier.leds.show();
    delay(250);
    carrier.leds.fill(colorBlack, 0, 5);
    carrier.leds.show();
    delay(250);
  }

  delay(2000);  // ~2 sekunde welcome

  carrier.leds.fill(colorBlack, 0, 5);
  carrier.leds.show();
}

// ======================================================================
//                          PIN LOGIKA
// ======================================================================

// Traži user-a čiji PIN se poklapa sa unetim
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

// Dodaj jednu cifru u PIN i proveri kada ima 4 cifre
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
    int8_t userIdx = findUserIndexForPin(enteredPin);

    if (userIdx >= 0) {
      // uspešna autentifikacija
      currentUserIndex = userIdx;
      currentUserId = USER_IDS[userIdx];   // pokazivač na string u USER_IDS tabeli

      Serial.print("PIN tacan -> NORMAL stanje, korisnik: ");
      Serial.print("index=");
      Serial.print(currentUserIndex);
      Serial.print(", id=");
      Serial.println(currentUserId);

      publishEventAuthSuccess();           // MQTT event AUTH_SUCCESS

      alarmState = NORMAL;
      normalStateJustEntered = true;
      carrier.Buzzer.noSound();
      playMelody(successMelody, successDurations, SUCCESS_MELODY_LEN);

      showWelcomeScreen();
    } else {
      // nijedan user nema ovaj PIN
      Serial.println("PIN pogresan!");
      failedAttempts++;

      publishEventAuthFailed();            // MQTT event AUTH_FAILED

      playMelody(errorMelody, errorDurations, ERROR_MELODY_LEN);

      Serial.print("Broj neuspesnih pokusaja: ");
      Serial.println(failedAttempts);

      if (failedAttempts >= MAX_FAILED_ATTEMPTS) {
        Serial.println("Previse pogresnih pokusaja -> INTRUDER!");

        publishEventAuthMaxFailed();       // MQTT event AUTH_MAX_FAILED
        publishEventIntruderAlarm();       // MQTT event INTRUDER_ALARM

        alarmState = INTRUDER;
        drawIntruderScreen();
      }

      enteredLength = 0;
    }
  }
}

// Update AUTH_COUNTDOWN ekrana (odbrojavanje + beep + PIN zvezdice)
void updateAuthCountdown() {
  unsigned long now = millis();

  int elapsedSeconds = (now - authStartTime) / 1000;
  int remaining = AUTH_WINDOW_SECONDS - elapsedSeconds;

  if (alarmState == INTRUDER) {
    return;
  }

  // Countdown istekao → INTRUDER
  if (remaining <= 0 && alarmState == AUTH_COUNTDOWN) {
    publishEventAuthTimeout();     // MQTT AUTH_TIMEOUT
    publishEventIntruderAlarm();   // MQTT INTRUDER_ALARM

    alarmState = INTRUDER;
    Serial.println("Isteklo vreme -> INTRUDER");
    drawIntruderScreen();
    return;
  }

  // Svake sekunde beep
  if (now - lastBeepTime >= 1000) {
    lastBeepTime = now;
    playShortCountdownBeep();
  }

  // Osvežavanje prikaza samo kad se promeni broj sekundi
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

  // Čitanje touch dugmića za unos PIN-a
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

// ======================================================================
//                     INTRUDER ALARM (blink + sirena)
// ======================================================================

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

// ======================================================================
//             ENV EKRAN: STATIC (logo + naslov) + VALUE (broj)
// ======================================================================

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
  // obriši samo donji deo (gde piše broj + C)
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

// Izbor statičke strane na osnovu currentPage
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

// Izbor value dela na osnovu currentPage
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

void showActuatorOverlay(const char* text) {
  // upamti tekst
  strncpy(actuatorOverlayText, text, sizeof(actuatorOverlayText) - 1);
  actuatorOverlayText[sizeof(actuatorOverlayText) - 1] = '\0';

  actuatorOverlayActive = true;
  actuatorOverlayStart = millis();

  // ekran
  carrier.display.fillScreen(ST77XX_BLACK);
  carrier.display.setTextColor(ST77XX_WHITE);
  carrier.display.setTextSize(4);
  carrier.display.setCursor(40, 90);
  carrier.display.print(actuatorOverlayText);

  // plava LED traka dok prikaz traje
  carrier.leds.fill(colorBlue, 0, 5);
  carrier.leds.show();
}

void updateActuatorOverlay() {
  if (!actuatorOverlayActive) return;

  if (millis() - actuatorOverlayStart >= 3000UL) {  // 3 sekunde
    actuatorOverlayActive = false;

    // ugasi plavu traku
    carrier.leds.fill(colorBlack, 0, 5);
    carrier.leds.show();

    // vrati se na "normalan" prikaz, zavisi od stanja
    switch (alarmState) {
      case ARMED:
        drawArmedScreen();
        break;
      case AUTH_COUNTDOWN:
        // countdown ekran će se osvežiti u updateAuthCountdown()
        break;
      case NORMAL:
        // neka NORMAL izgradi env ekran ispočetka
        normalStateJustEntered = true;
        break;
      case INTRUDER:
        drawIntruderScreen();
        break;
    }
  }
}

// ======================================================================
//                 NORMAL STANJE: merenje + gestovi
// ======================================================================

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

    // float za log / MQTT, int za prikaz
    tempInt  = (int16_t)roundf(lastTemp);
    humInt   = (int16_t)roundf(lastHum);
    pressInt = (int16_t)roundf(lastPress);

    // Slanje na MQTT DATA topic + ispis JSON-a u Serial
    publishSensorData(lastTemp, lastHum, lastPress);

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

    // Ponovo slanje na MQTT DATA topic + Serial
    publishSensorData(lastTemp, lastHum, lastPress);

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

  // čitanje gestova LEFT/RIGHT/DOWN
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

      // pošalji event ko je "zaključao" kuću
      publishEventArmedByUser();

      // resetuj trenutnog user-a
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

// ======================================================================
//                           setup / loop
// ======================================================================

void onMqttMessage(int messageSize) {
  String topic = mqttClient.messageTopic();

  Serial.print("MQTT IN [");
  Serial.print(topic);
  Serial.print("] bytes=");
  Serial.println(messageSize);

  String payload;
  while (mqttClient.available()) {
    char c = (char)mqttClient.read();
    payload += c;
  }
  Serial.print("Payload: ");
  Serial.println(payload);

  // Zanima nas samo actuators topic
  if (topic != MQTT_TOPIC_ACTUATORS) {
    return;
  }

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("Actuator JSON parse error: ");
    Serial.println(err.c_str());
    return;
  }

  const char* device = doc["device"];          // "AC", "HUMIDIFIER", "VENT_FAN"
  const char* desired = doc["desired_state"];  // "ON" / "OFF"

  if (!device || !desired) {
    Serial.println("Actuator message missing device/desired_state");
    return;
  }

  char text[16] = "";

  if (strcmp(device, "AC") == 0) {
    strcpy(text, "AC ");
  } else if (strcmp(device, "HUMIDIFIER") == 0) {
    strcpy(text, "HUM ");
  } else if (strcmp(device, "VENT_FAN") == 0) {
    strcpy(text, "WND ");
  } else {
    strcpy(text, "DEV ");
  }

  if (strcmp(desired, "ON") == 0) {
    strcat(text, "ON");
  } else if (strcmp(desired, "OFF") == 0) {
    strcat(text, "OFF");
  } else {
    strcat(text, "?");
  }

  // Aktiviraj overlay
  showActuatorOverlay(text);
}


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

  // MQTT callback + subscribe za actuators topic
  mqttClient.onMessage(onMqttMessage);
  mqttClient.subscribe(MQTT_TOPIC_ACTUATORS);

  drawArmedScreen();
}

void loop() {
  // Održava MQTT konekciju (ping, keep-alive)
  mqttClient.poll();

  // Ako je aktivan overlay za komandu, drži ga 3 sekunde i pauziraj ostalo
  if (actuatorOverlayActive) {
    updateActuatorOverlay();
    delay(20);
    return;
  }

  switch (alarmState) {
    case ARMED:
      if (detectMovement()) {
        // imamo pokret → šaljemo event + ulazimo u AUTH_COUNTDOWN
        publishEventMotionDetectedArmed();
        startAuthCountdown();
      } else {
        // nema pokreta: mali delay da ne vrti loop prebrzo
        delay(20);
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
