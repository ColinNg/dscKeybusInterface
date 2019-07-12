/*
 *  OpenHAB-MQTT 1.0 (esp32)
 *
 *  Processes the security system status and allows for control using OpenHAB.  This uses MQTT to
 *  interface with OpenHAB and the MQTT binding and demonstrates using the armed states as OpenHAB
 *  switches, and the alarm and zones states as OpenHAB contacts.  Also see https://github.com/jimtng/dscalarm-mqtt
 *  for an integration using the Homie convention for OpenHAB's Homie MQTT component.
 *
 *  OpenHAB: https://www.openhab.org
 *  OpenHAB MQTT Binding: https://www.openhab.org/addons/bindings/mqtt/
 *  Mosquitto MQTT broker: https://mosquitto.org
 *
 *  Usage:
 *    1. Set the WiFi SSID and password in the sketch.
 *    2. Set the security system access code to permit disarming through HomeKit.
 *    3. Set the MQTT server address in the sketch.
 *    4. Upload the sketch.
 *    5. Install the OpenHAB MQTT binding.
 *    6. Copy the example configuration to OpenHAB and customize.
 *
 *  Release notes:
 *    1.0 - Initial release
 *
 *  Example OpenHAB configuration:
 *
 *  1. Create a "things" file for the MQTT broker as (OpenHAB configuration directory)/things/mymqtt.things:
 *     - https://www.openhab.org/docs/configuration/things.html

Bridge mqtt:broker:mymqtt "My MQTT" [host="MQTT broker IP address or hostname"]

 *  2. Create a "things" file for the security system as (OpenHAB configuration directory)/things/dsc.things:

Thing mqtt:topic:mymqtt:dsc "DSC Security System" (mqtt:broker:mymqtt) @ "Home" {
    Channels:
        Type switch : partition1_armed_away "Partition 1 Armed Away" [stateTopic="dsc/Get/Partition1", commandTopic="dsc/Set", on="1A", off="1D"]
        Type switch : partition1_armed_stay "Partition 1 Armed Stay" [stateTopic="dsc/Get/Partition1", commandTopic="dsc/Set", on="1S", off="1D"]
        Type contact: partition1_triggered "Partition 1 Alarm Triggered" [stateTopic="dsc/Get/Partition1", on="1T", off="1D"]
        Type contact : zone1 "Zone 1" [stateTopic="dsc/Get/Zone1", on="1", off="0"]
        Type contact : zone2 "Zone 2" [stateTopic="dsc/Get/Zone2", on="1", off="0"]
}

*   3. Create an "items" file for the security system as (OpenHAB configuration directory)/items/dsc.items:
*      - https://www.openhab.org/docs/configuration/items.html

Switch partition1_armed_away "Partition 1 Armed Away" {channel="mqtt:topic:mymqtt:dsc:partition1_armed_away"}
Switch partition1_armed_stay "Partition 1 Armed Stay" {channel="mqtt:topic:mymqtt:dsc:partition1_armed_stay"}
Contact partition1_triggered "Partition 1 Alarm Triggered" {channel="mqtt:topic:mymqtt:dsc:partition1_triggered"}
Contact zone1 "Zone 1" {channel="mqtt:topic:mymqtt:dsc:zone1"}
Contact zone2 "Zone 2" {channel="mqtt:topic:mymqtt:dsc:zone2"}


 *  The commands to set the alarm state are setup in OpenHAB with the partition number (1-8) as a prefix to the command:
 *    Partition 1 stay arm: "1S"
 *    Partition 1 away arm: "1A"
 *    Partition 2 disarm: "2D"
 *
 *  The interface listens for commands in the configured mqttSubscribeTopic, and publishes partition status in a
 *  separate topic per partition with the configured mqttPartitionTopic appended with the partition number:
 *    Partition 1 stay arm: "1S"
 *    Partition 1 away arm: "1A"
 *    Partition 1 disarm: "1D"
 *    Partition 2 alarm tripped: "2T"
 *
 *  Zone states are published as an integer in a separate topic per zone with the configured mqttZoneTopic appended
 *  with the zone number:
 *    Open: "1"
 *    Closed: "0"
 *
 *  Fire states are published as an integer in a separate topic per partition with the configured mqttFireTopic
 *  appended with the partition number:
 *    Fire alarm: "1"
 *    Fire alarm restored: "0"
 *
 *  Wiring:
 *      DSC Aux(+) --- 5v voltage regulator --- esp32 development board 5v pin
 *
 *      DSC Aux(-) --- esp32 Ground
 *
 *                                         +--- dscClockPin (esp32: 4,13,16-39)
 *      DSC Yellow --- 33k ohm resistor ---|
 *                                         +--- 10k ohm resistor --- Ground
 *
 *                                         +--- dscReadPin (esp32: 4,13,16-39)
 *      DSC Green ---- 33k ohm resistor ---|
 *                                         +--- 10k ohm resistor --- Ground
 *
 *  Virtual keypad (optional):
 *      DSC Green ---- NPN collector --\
 *                                      |-- NPN base --- 1k ohm resistor --- dscWritePin (esp32: 4,13,16-33)
 *            Ground --- NPN emitter --/
 *
 *  Virtual keypad uses an NPN transistor to pull the data line low - most small signal NPN transistors should
 *  be suitable, for example:
 *   -- 2N3904
 *   -- BC547, BC548, BC549
 *
 *  Issues and (especially) pull requests are welcome:
 *  https://github.com/taligentx/dscKeybusInterface
 *
 *  This example code is in the public domain.
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <dscKeybusInterface.h>

// Settings
const char* wifiSSID = "";
const char* wifiPassword = "";
const char* accessCode = "";    // An access code is required to disarm/night arm and may be required to arm based on panel configuration.
const char* mqttServer = "";    // MQTT server domain name or IP address
const int mqttPort = 1883;      // MQTT server port
const char* mqttUsername = "";  // Optional, leave blank if not required
const char* mqttPassword = "";  // Optional, leave blank if not required

// MQTT topics - match to Homebridge's config.json
const char* mqttClientName = "dscKeybusInterface";
const char* mqttPartitionTopic = "dsc/Get/Partition";  // Sends armed and alarm status per partition: dsc/Get/Partition1 ... dsc/Get/Partition8
const char* mqttZoneTopic = "dsc/Get/Zone";            // Sends zone status per zone: dsc/Get/Zone1 ... dsc/Get/Zone64
const char* mqttFireTopic = "dsc/Get/Fire";            // Sends fire status per partition: dsc/Get/Fire1 ... dsc/Get/Fire8
const char* mqttSubscribeTopic = "dsc/Set";            // Receives messages to write to the panel

// Configures the Keybus interface with the specified pins - dscWritePin is optional, leaving it out disables the
// virtual keypad.
#define dscClockPin 18  // esp32: 4,13,16-39
#define dscReadPin 19   // esp32: 4,13,16-39
#define dscWritePin 21  // esp32: 4,13,16-33

// Initialize components
dscKeybusInterface dsc(dscClockPin, dscReadPin, dscWritePin);
WiFiClient wifiClient;
PubSubClient mqtt(mqttServer, mqttPort, wifiClient);
unsigned long mqttPreviousTime;


void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println();

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID, wifiPassword);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  Serial.print("WiFi connected: ");
  Serial.println(WiFi.localIP());

  mqtt.setCallback(mqttCallback);
  if (mqttConnect()) mqttPreviousTime = millis();
  else mqttPreviousTime = 0;

  // Starts the Keybus interface and optionally specifies how to print data.
  // begin() sets Serial by default and can accept a different stream: begin(Serial1), etc.
  dsc.begin();

  Serial.println(F("DSC Keybus Interface is online."));
}


void loop() {
  mqttHandle();

  dsc.loop();

  if (dsc.statusChanged) {      // Checks if the security system status has changed
    dsc.statusChanged = false;  // Reset the status tracking flag

    // If the Keybus data buffer is exceeded, the sketch is too busy to process all Keybus commands.  Call
    // loop() more often, or increase dscBufferSize in the library: src/dscKeybusInterface.h
    if (dsc.bufferOverflow) {
      Serial.println(F("Keybus buffer overflow"));
      dsc.bufferOverflow = false;
    }

    // Sends the access code when needed by the panel for arming
    if (dsc.accessCodePrompt) {
      dsc.accessCodePrompt = false;
      dsc.write(accessCode);
    }

    // Publishes status per partition
    for (byte partition = 0; partition < dscPartitions; partition++) {

      // Publishes armed/disarmed status
      if (dsc.armedChanged[partition]) {
        dsc.armedChanged[partition] = false;  // Resets the partition armed status flag

        if (dsc.armed[partition]) {

          // Armed away
          if (dsc.armedAway[partition]) {
            publishState(mqttPartitionTopic, partition, "A");
          }

          // Armed stay
          else if (dsc.armedStay[partition]) {
            publishState(mqttPartitionTopic, partition, "S");
          }
        }

        // Disarmed
        else publishState(mqttPartitionTopic, partition, "D");
      }

      // Checks exit delay status
      if (dsc.exitDelayChanged[partition]) {
        dsc.exitDelayChanged[partition] = false;  // Resets the exit delay status flag

        // Exit delay in progress
        if (dsc.exitDelay[partition]) {
            publishState(mqttPartitionTopic, partition, "P");
        }

        // Disarmed during exit delay
        else if (!dsc.armed[partition]) {
          publishState(mqttPartitionTopic, partition, "D");
        }
      }

      // Publishes alarm triggered status
      if (dsc.alarmChanged[partition]) {
        dsc.alarmChanged[partition] = false;  // Resets the partition alarm status flag
        if (dsc.alarm[partition]) {
          publishState(mqttPartitionTopic, partition, "T");
        }
      }

      // Publishes fire alarm status
      if (dsc.fireChanged[partition]) {
        dsc.fireChanged[partition] = false;  // Resets the fire status flag

        if (dsc.fire[partition]) {
          publishState(mqttFireTopic, partition, "1");  // Fire alarm tripped
        }
        else {
          publishState(mqttFireTopic, partition, "0");  // Fire alarm restored
        }
      }
    }

    // Publishes zones 1-64 status in a separate topic per zone
    // Zone status is stored in the openZones[] and openZonesChanged[] arrays using 1 bit per zone, up to 64 zones:
    //   openZones[0] and openZonesChanged[0]: Bit 0 = Zone 1 ... Bit 7 = Zone 8
    //   openZones[1] and openZonesChanged[1]: Bit 0 = Zone 9 ... Bit 7 = Zone 16
    //   ...
    //   openZones[7] and openZonesChanged[7]: Bit 0 = Zone 57 ... Bit 7 = Zone 64
    if (dsc.openZonesStatusChanged) {
      dsc.openZonesStatusChanged = false;                           // Resets the open zones status flag
      for (byte zoneGroup = 0; zoneGroup < dscZones; zoneGroup++) {
        for (byte zoneBit = 0; zoneBit < 8; zoneBit++) {
          if (bitRead(dsc.openZonesChanged[zoneGroup], zoneBit)) {  // Checks an individual open zone status flag
            bitWrite(dsc.openZonesChanged[zoneGroup], zoneBit, 0);  // Resets the individual open zone status flag

            // Appends the mqttZoneTopic with the zone number
            char zonePublishTopic[strlen(mqttZoneTopic) + 2];
            char zone[3];
            strcpy(zonePublishTopic, mqttZoneTopic);
            itoa(zoneBit + 1 + (zoneGroup * 8), zone, 10);
            strcat(zonePublishTopic, zone);

            if (bitRead(dsc.openZones[zoneGroup], zoneBit)) {
              mqtt.publish(zonePublishTopic, "1", true);            // Zone open
            }
            else mqtt.publish(zonePublishTopic, "0", true);         // Zone closed
          }
        }
      }
    }

    mqtt.subscribe(mqttSubscribeTopic);
  }
}


// Handles messages received in the mqttSubscribeTopic
void mqttCallback(char* topic, byte* payload, unsigned int length) {

  // Handles unused parameters
  (void)topic;
  (void)length;

  byte partition = 0;
  byte payloadIndex = 0;

  // Checks if a partition number 1-8 has been sent and sets the second character as the payload
  if (payload[0] >= 0x31 && payload[0] <= 0x38) {
    partition = payload[0] - 49;
    payloadIndex = 1;
  }


  // Arm stay
  if (payload[payloadIndex] == 'S' && !dsc.armed[partition] && !dsc.exitDelay[partition]) {
    dsc.writePartition = partition + 1;    // Sets writes to the partition number
    dsc.write('s');  // Keypad arm stay
    return;
  }

  // Arm away
  if (payload[payloadIndex] == 'A' && !dsc.armed[partition] && !dsc.exitDelay[partition]) {
    dsc.writePartition = partition + 1;    // Sets writes to the partition number
    dsc.write('w');  // Keypad arm away
    return;
  }

  // Disarm
  if (payload[payloadIndex] == 'D' && (dsc.armed[partition] || dsc.exitDelay[partition])) {
    dsc.writePartition = partition + 1;    // Sets writes to the partition number
    dsc.write(accessCode);
    return;
  }
}


void mqttHandle() {
  if (!mqtt.connected()) {
    unsigned long mqttCurrentTime = millis();
    if (mqttCurrentTime - mqttPreviousTime > 5000) {
      mqttPreviousTime = mqttCurrentTime;
      if (mqttConnect()) {
        Serial.println(F("MQTT disconnected, successfully reconnected."));
        mqttPreviousTime = 0;
      }
      else Serial.println(F("MQTT disconnected, failed to reconnect."));
    }
  }
  else mqtt.loop();
}


bool mqttConnect() {
  if (mqtt.connect(mqttClientName, mqttUsername, mqttPassword)) {
    Serial.print(F("MQTT connected: "));
    Serial.println(mqttServer);
    dsc.resetStatus();  // Resets the state of all status components as changed to get the current status
  }
  else {
    Serial.print(F("MQTT connection failed: "));
    Serial.println(mqttServer);
  }
  return mqtt.connected();
}


// Publishes the current states with partition numbers
void publishState(const char* sourceTopic, byte partition, const char* sourceSuffix) {
  char publishTopic[strlen(sourceTopic) + 1];
  char partitionNumber[2];

  // Appends the sourceTopic with the partition number
  itoa(partition + 1, partitionNumber, 10);
  strcpy(publishTopic, sourceTopic);
  strcat(publishTopic, partitionNumber);

  // Prepends the sourceSuffix with the partition number
  char currentState[strlen(sourceSuffix) + 1];
  strcpy(currentState, partitionNumber);
  strcat(currentState, sourceSuffix);

  // Publishes the current state
  mqtt.publish(publishTopic, currentState, true);
}
