const char MQTT_TOPIC_DATA[] = "home/house_1/sensor/data";
const char MQTT_TOPIC_EVENTS[] = "home/house_1/security/events";
const char MQTT_TOPIC_COMMANDS[] = "home/house_1/commands";
const char MQTT_TOPIC_ACTUATORS[] = "home/house_1/commands/actuators";
const char SENSOR_NAME[] = "MKR1010_WiFi";
const char HOUSE_ID[]   = "house_1";

enum AlarmState {
  ARMED,
  AUTH_COUNTDOWN,
  INTRUDER,
  NORMAL
};

enum EnvPage {
  PAGE_TEMP = 0,
  PAGE_HUM = 1,
  PAGE_PRESSURE = 2
};

const float GYRO_THRESHOLD = 50.0f;

const uint8_t USER_COUNT = 3;
const uint8_t PIN_LENGTH = 4;

const char USER_IDS[USER_COUNT][10] = {
  "owner_kid",
  "owner_mom",
  "owner_dad"
};

const char USER_DISPLAY_NAMES[USER_COUNT][8] = {
  "KID",
  "MOM",
  "DAD"
};

const uint8_t USER_PINS[USER_COUNT][PIN_LENGTH] = {
  {0, 1, 2, 3},
  {0, 2, 0, 2},
  {1, 1, 1, 1}
};

const uint8_t MAX_FAILED_ATTEMPTS = 3;

const unsigned long ENV_SAMPLE_INTERVAL = 5000UL;

const uint8_t AUTH_WINDOW_SECONDS = 30;