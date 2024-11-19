#include "wled.h"

/*
 * MQTT communication protocol for home automation
 *
 * Forked and modified by Wouter Gritter to change the topic structure to a more intuitive one.
 * <mqttDeviceTopic>/status => 1 or 0
 * <mqttDeviceTopic>/brightness => 0 - 255
 * <mqttDeviceTopic>/rgb => R,G,B[,W] where R,G,B,W 0 - 255
 *
 * <mqttGroupTopic> is unused.
 *
 * This fork of WLED both publishes and subscribes to these topics.
 */

#ifndef WLED_DISABLE_MQTT
#define MQTT_KEEP_ALIVE_TIME 60    // contact the MQTT broker every 60 seconds

bool ignoreStatusTopic = false;
bool ignoreBrightnessTopic = false;
bool ignoreRgbTopic = false;

static void parseMQTTStatusPayload(char* payload)
{
  if (ignoreStatusTopic) {
    ignoreStatusTopic = false;
    return;
  }

  if (strcmp(payload, "1") == 0) {
    bri = briLast > 0 ? briLast : 255; // Turn ON to last brightness or full if uninitialized
    stateUpdated(CALL_MODE_DIRECT_CHANGE);
  } else if (strcmp(payload, "0") == 0) {
    briLast = bri; // Save current brightness
    bri = 0;       // Turn OFF
    stateUpdated(CALL_MODE_DIRECT_CHANGE);
  }
}

static void parseMQTTBrightnessPayload(char* payload)
{
  if (ignoreBrightnessTopic) {
    ignoreBrightnessTopic = false;
    return;
  }

  uint8_t brightness = strtoul(payload, NULL, 10);
  if (brightness <= 255) {
    bri = brightness;
    stateUpdated(CALL_MODE_DIRECT_CHANGE);
  }
}

static void parseMQTTRgbPayload(char* payload)
{
  if (ignoreRgbTopic) {
    ignoreRgbTopic = false;
    return;
  }

  int r, g, b, w = -1; // White channel default to -1 (not provided)
  int fields = sscanf(payload, "%d,%d,%d,%d", &r, &g, &b, &w);

  if (fields >= 3) { // At least R, G, B must be provided
    col[0] = constrain(r, 0, 255);
    col[1] = constrain(g, 0, 255);
    col[2] = constrain(b, 0, 255);
    if (fields == 4) { // If white channel is provided
      col[3] = constrain(w, 0, 255);
    }
    colorUpdated(CALL_MODE_DIRECT_CHANGE);
  }
}

static void onMqttConnect(bool sessionPresent)
{
  //(re)subscribe to required topics
  char topicbuf[MQTT_MAX_TOPIC_LEN + 16];

  if (mqttDeviceTopic[0] != 0) {
    // Subscribe to device-specific topics
    strlcpy(topicbuf, mqttDeviceTopic, MQTT_MAX_TOPIC_LEN + 1);
    strcat_P(topicbuf, PSTR("/status"));
    mqtt->subscribe(topicbuf, 0);

    strlcpy(topicbuf, mqttDeviceTopic, MQTT_MAX_TOPIC_LEN + 1);
    strcat_P(topicbuf, PSTR("/brightness"));
    mqtt->subscribe(topicbuf, 0);

    strlcpy(topicbuf, mqttDeviceTopic, MQTT_MAX_TOPIC_LEN + 1);
    strcat_P(topicbuf, PSTR("/rgb"));
    mqtt->subscribe(topicbuf, 0);
  }

  UsermodManager::onMqttConnect(sessionPresent);

  DEBUG_PRINTLN(F("MQTT ready"));
  publishMqtt();
}

static void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  static char *payloadStr;

  DEBUG_PRINTF_P(PSTR("MQTT msg: %s\n"), topic);

  // paranoia check to avoid npe if no payload
  if (payload==nullptr) {
    DEBUG_PRINTLN(F("no payload -> leave"));
    return;
  }

  if (index == 0) {                       // start (1st partial packet or the only packet)
    if (payloadStr) delete[] payloadStr;  // fail-safe: release buffer
    payloadStr = new char[total+1];       // allocate new buffer
  }
  if (payloadStr == nullptr) return;      // buffer not allocated

  // copy (partial) packet to buffer and 0-terminate it if it is last packet
  char* buff = payloadStr + index;
  memcpy(buff, payload, len);
  if (index + len >= total) { // at end
    payloadStr[total] = '\0'; // terminate c style string
  } else {
    DEBUG_PRINTLN(F("MQTT partial packet received."));
    return; // process next packet
  }
  DEBUG_PRINTLN(payloadStr);

  size_t topicPrefixLen = strlen(mqttDeviceTopic);
  if (strncmp(topic, mqttDeviceTopic, topicPrefixLen) == 0) {
    topic += topicPrefixLen;
  } else {
    topicPrefixLen = strlen(mqttGroupTopic);
    if (strncmp(topic, mqttGroupTopic, topicPrefixLen) == 0) {
      topic += topicPrefixLen;
    } else {
      // Non-Wled Topic used here. Probably a usermod subscribed to this topic.
      UsermodManager::onMqttMessage(topic, payloadStr);
      delete[] payloadStr;
      payloadStr = nullptr;
      return;
    }
  }

  //Prefix is stripped from the topic at this point

  if (strcmp_P(topic, PSTR("/status")) == 0) {
    parseMQTTStatusPayload(payloadStr);
  } else if (strcmp_P(topic, PSTR("/brightness")) == 0) {
    parseMQTTBrightnessPayload(payloadStr);
  } else if (strcmp_P(topic, PSTR("/rgb")) == 0) {
    parseMQTTRgbPayload(payloadStr);
  } else if (strlen(topic) != 0) {
    // non standard topic, check with usermods
    UsermodManager::onMqttMessage(topic, payloadStr);
  } else {
    // topmost topic (ignore)
  }
  delete[] payloadStr;
  payloadStr = nullptr;
}

void publishMqtt()
{
  if (!WLED_MQTT_CONNECTED) return;
  DEBUG_PRINTLN(F("Publish MQTT"));

  char payloadbuf[32];
  char topicbuf[MQTT_MAX_TOPIC_LEN + 16];

  // Publish status
  ignoreStatusTopic = true;
  strlcpy(topicbuf, mqttDeviceTopic, MQTT_MAX_TOPIC_LEN + 1);
  strcat_P(topicbuf, PSTR("/status"));
  snprintf(payloadbuf, sizeof(payloadbuf), "%d", bri > 0 ? 1 : 0);
  mqtt->publish(topicbuf, 0, retainMqttMsg, payloadbuf);

  // Publish brightness
  ignoreBrightnessTopic = true;
  strlcpy(topicbuf, mqttDeviceTopic, MQTT_MAX_TOPIC_LEN + 1);
  strcat_P(topicbuf, PSTR("/brightness"));
  snprintf(payloadbuf, sizeof(payloadbuf), "%u", bri);
  mqtt->publish(topicbuf, 0, retainMqttMsg, payloadbuf);

  // Publish RGB(W)
  ignoreRgbTopic = true;
  strlcpy(topicbuf, mqttDeviceTopic, MQTT_MAX_TOPIC_LEN + 1);
  strcat_P(topicbuf, PSTR("/rgb"));
  if (col[3] > 0) { // White channel is present and non-zero
    snprintf(payloadbuf, sizeof(payloadbuf), "%d,%d,%d,%d", col[0], col[1], col[2], col[3]);
  } else { // No white channel
    snprintf(payloadbuf, sizeof(payloadbuf), "%d,%d,%d", col[0], col[1], col[2]);
  }
  mqtt->publish(topicbuf, 0, retainMqttMsg, payloadbuf);
}

bool initMqtt()
{
  if (!mqttEnabled || mqttServer[0] == 0 || !WLED_CONNECTED) return false;

  if (mqtt == nullptr) {
    mqtt = new AsyncMqttClient();
    if (!mqtt) return false;
    mqtt->onMessage(onMqttMessage);
    mqtt->onConnect(onMqttConnect);
  }
  if (mqtt->connected()) return true;

  DEBUG_PRINTLN(F("Reconnecting MQTT"));
  IPAddress mqttIP;
  if (mqttIP.fromString(mqttServer)) //see if server is IP or domain
  {
    mqtt->setServer(mqttIP, mqttPort);
  } else {
    mqtt->setServer(mqttServer, mqttPort);
  }
  mqtt->setClientId(mqttClientID);
  if (mqttUser[0] && mqttPass[0]) mqtt->setCredentials(mqttUser, mqttPass);

  mqtt->setKeepAlive(MQTT_KEEP_ALIVE_TIME);
  mqtt->connect();
  return true;
}
#endif
